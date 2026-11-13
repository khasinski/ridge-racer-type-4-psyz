/* The link cable's network side: one TCP connection to the other PsyZ,
 * carrying the serial port's bytes and line reports (link_wire.h) in
 * order. A thread listens (host) or connects (join), checks the other
 * end's HELLO and feeds what arrives to the port model (libcomb.c). */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* getaddrinfo, select under -std=c11 */
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET LinkSocket;
#define LINK_BAD_SOCKET INVALID_SOCKET
#define LinkClose closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int LinkSocket;
#define LINK_BAD_SOCKET (-1)
#define LinkClose close
#endif
#include <stdio.h>
#include <string.h>
#include <psyz/link.h>
#include <psyz/log.h>
#include "os.h"
#include "link_wire.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum {
    PROTOCOL_VERSION = 1,
    OUT_MAX = 8192,      // messages held back by the test delay
    HELLO_TIMEOUT_US = 5000000,
    PING_US = 1000000,
    CONNECT_TIMEOUT_US = 2000000,
};

static const char kMagic[4] = {'R', 'R', 'L', 'K'};

/* Psyz_LinkSetDelay's test latency in ms. A plain global so that a test can
 * also change it in the middle of a run (the game's RR4_POKE). */
volatile unsigned Psyz_LinkDelayMs;

typedef struct {
    uint64_t due_us;
    int len;
    uint8_t frame[2 + 255];
} OutFrame;

static struct {
    void* lock;      // configuration, status, held-back messages
    void* send_lock; // writes on the connection, and closing it
    int started;
    /* configuration */
    PsyzLinkMode mode;
    char address[256];
    unsigned port;
    char identity[128];
    unsigned generation;
    /* status */
    PsyzLinkState state;
    char status[256];
    unsigned rtt_us;
    unsigned long sent, received;
    /* connection */
    LinkSocket conn;
    int open; // the other end said HELLO: the cable carries data
    OutFrame out[OUT_MAX];
    int out_head, out_len;
} net = {.conn = LINK_BAD_SOCKET};

static void SetStatus(PsyzLinkState state, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#include <stdarg.h>
static void SetStatus(PsyzLinkState state, const char* fmt, ...) {
    char text[sizeof(net.status)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    Psyz_OsLock(net.lock);
    if (net.state != state || strcmp(net.status, text)) {
        net.state = state;
        snprintf(net.status, sizeof(net.status), "%s", text);
        INFOF("link: %s", text);
    }
    Psyz_OsUnlock(net.lock);
}

static const char* SocketError(void) {
    static char text[64];
#ifdef _WIN32
    snprintf(text, sizeof(text), "error %d", WSAGetLastError());
    return text;
#else
    snprintf(text, sizeof(text), "%s", strerror(errno));
    return text;
#endif
}

static void SetBlocking(LinkSocket s, int blocking) {
#ifdef _WIN32
    u_long nb = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
#endif
}

static void Tune(LinkSocket s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&one, sizeof(one));
#endif
}

/* select() on one or two sockets; returns a mask of the readable ones. */
static int WaitReadable(LinkSocket a, LinkSocket b, uint64_t timeout_us) {
    fd_set set;
    struct timeval tv;
    int n;
    FD_ZERO(&set);
    if (a != LINK_BAD_SOCKET)
        FD_SET(a, &set);
    if (b != LINK_BAD_SOCKET)
        FD_SET(b, &set);
    tv.tv_sec = (long)(timeout_us / 1000000);
    tv.tv_usec = (long)(timeout_us % 1000000);
    n = select((int)((a > b || b == LINK_BAD_SOCKET ? a : b) + 1), &set, NULL, NULL, &tv);
    if (n <= 0)
        return 0;
    return (a != LINK_BAD_SOCKET && FD_ISSET(a, &set) ? 1 : 0) |
           (b != LINK_BAD_SOCKET && FD_ISSET(b, &set) ? 2 : 0);
}

static void NetSleep(uint64_t us) {
    WaitReadable(LINK_BAD_SOCKET, LINK_BAD_SOCKET, us);
}

static int SendAll(LinkSocket s, const uint8_t* data, int len) {
    while (len > 0) {
        int n = (int)send(s, (const char*)data, len, MSG_NOSIGNAL);
        if (n <= 0)
            return 0;
        data += n;
        len -= n;
    }
    return 1;
}

static int Frame(uint8_t* out, int type, const void* payload, int len) {
    out[0] = (uint8_t)type;
    out[1] = (uint8_t)len;
    if (len)
        memcpy(out + 2, payload, (size_t)len);
    return 2 + len;
}

/* A message from the network thread (handshake, pings). */
static int SendNow(int type, const void* payload, int len) {
    uint8_t frame[2 + 255];
    int ok;
    int n = Frame(frame, type, payload, len);
    Psyz_OsLock(net.send_lock);
    ok = net.conn != LINK_BAD_SOCKET && SendAll(net.conn, frame, n);
    Psyz_OsUnlock(net.send_lock);
    return ok;
}

int Psyz_LinkSend(int type, const void* payload, int len) {
    uint8_t frame[2 + 255];
    int n, ok;
    if (len < 0 || len > 255 || !net.lock)
        return 0;
    Psyz_OsLock(net.lock);
    if (!net.open) {
        Psyz_OsUnlock(net.lock);
        return 0;
    }
    if (type == LINK_MSG_DATA)
        net.sent += (unsigned long)len;
    if (Psyz_LinkDelayMs || net.out_len) {
        if (net.out_len < OUT_MAX) {
            OutFrame* f = &net.out[(net.out_head + net.out_len++) % OUT_MAX];
            f->due_us = Psyz_OsNowUs() + (uint64_t)Psyz_LinkDelayMs * 1000;
            f->len = Frame(f->frame, type, payload, len);
        }
        Psyz_OsUnlock(net.lock);
        return 1;
    }
    Psyz_OsUnlock(net.lock);
    n = Frame(frame, type, payload, len);
    Psyz_OsLock(net.send_lock);
    ok = net.conn != LINK_BAD_SOCKET && SendAll(net.conn, frame, n);
    Psyz_OsUnlock(net.send_lock);
    return ok;
}

/* Sends the held-back messages that are due; returns the time until the
 * next one (or `idle`). */
static uint64_t FlushHeld(uint64_t idle) {
    for (;;) {
        OutFrame f;
        uint64_t now = Psyz_OsNowUs();
        Psyz_OsLock(net.lock);
        if (!net.out_len) {
            Psyz_OsUnlock(net.lock);
            return idle;
        }
        if (net.out[net.out_head].due_us > now) {
            uint64_t left = net.out[net.out_head].due_us - now;
            Psyz_OsUnlock(net.lock);
            return left < idle ? left : idle;
        }
        f = net.out[net.out_head];
        net.out_head = (net.out_head + 1) % OUT_MAX;
        net.out_len--;
        Psyz_OsUnlock(net.lock);
        Psyz_OsLock(net.send_lock);
        if (net.conn != LINK_BAD_SOCKET)
            SendAll(net.conn, f.frame, f.len);
        Psyz_OsUnlock(net.send_lock);
    }
}

static void Disconnect(void) {
    int was_open;
    Psyz_OsLock(net.lock);
    was_open = net.open;
    net.open = 0;
    net.out_len = 0;
    Psyz_OsUnlock(net.lock);
    Psyz_OsLock(net.send_lock);
    if (net.conn != LINK_BAD_SOCKET)
        LinkClose(net.conn);
    net.conn = LINK_BAD_SOCKET;
    Psyz_OsUnlock(net.send_lock);
    if (was_open)
        Psyz_SioWireDown();
}

static LinkSocket Listen(unsigned port) {
    static const int families[2] = {AF_INET6, AF_INET};
    for (int i = 0; i < 2; i++) {
        struct addrinfo hints, *res = NULL;
        char service[16];
        LinkSocket s;
        int one = 1, zero = 0;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = families[i];
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        snprintf(service, sizeof(service), "%u", port);
        if (getaddrinfo(NULL, service, &hints, &res) != 0 || !res)
            continue;
        s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == LINK_BAD_SOCKET) {
            freeaddrinfo(res);
            continue;
        }
#ifndef _WIN32
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
#endif
        (void)one;
        if (families[i] == AF_INET6)
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&zero, sizeof(zero));
        if (bind(s, res->ai_addr, (int)res->ai_addrlen) == 0 && listen(s, 2) == 0) {
            freeaddrinfo(res);
            return s;
        }
        freeaddrinfo(res);
        LinkClose(s);
    }
    return LINK_BAD_SOCKET;
}

static LinkSocket Connect(const char* address, unsigned port, char* why, size_t why_size) {
    struct addrinfo hints, *res = NULL, *ai;
    char service[16];
    int rc;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", port);
    rc = getaddrinfo(address, service, &hints, &res);
    if (rc != 0 || !res) {
        snprintf(why, why_size, "cannot find %s", address);
        return LINK_BAD_SOCKET;
    }
    snprintf(why, why_size, "no answer");
    for (ai = res; ai; ai = ai->ai_next) {
        LinkSocket s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        fd_set w;
        struct timeval tv = {CONNECT_TIMEOUT_US / 1000000, CONNECT_TIMEOUT_US % 1000000};
        int err = 0;
        socklen_t len = sizeof(err);
        if (s == LINK_BAD_SOCKET)
            continue;
        SetBlocking(s, 0);
        connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        FD_ZERO(&w);
        FD_SET(s, &w);
        if (select((int)s + 1, NULL, &w, NULL, &tv) == 1 &&
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0) {
            SetBlocking(s, 1);
            freeaddrinfo(res);
            return s;
        }
        if (err) {
#ifdef _WIN32
            snprintf(why, why_size, "error %d", err);
#else
            snprintf(why, why_size, "%s", strerror(err));
#endif
        }
        LinkClose(s);
    }
    freeaddrinfo(res);
    return LINK_BAD_SOCKET;
}

static void PeerName(LinkSocket s, char* out, size_t size) {
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    char host[64], port[16];
    if (getpeername(s, (struct sockaddr*)&addr, &len) != 0 ||
        getnameinfo((struct sockaddr*)&addr, len, host, sizeof(host), port, sizeof(port),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        snprintf(out, size, "the other player");
        return;
    }
    if (!strncmp(host, "::ffff:", 7))
        memmove(host, host + 7, strlen(host + 7) + 1);
    snprintf(out, size, "%s", host);
}

static void SendHello(void) {
    uint8_t hello[255];
    int len;
    memcpy(hello, kMagic, 4);
    hello[4] = PROTOCOL_VERSION;
    Psyz_OsLock(net.lock);
    len = (int)strlen(net.identity);
    if (len > 255 - 5)
        len = 255 - 5;
    memcpy(hello + 5, net.identity, (size_t)len);
    Psyz_OsUnlock(net.lock);
    SendNow(LINK_MSG_HELLO, hello, 5 + len);
}

/* Handles one message; returns 0 when the connection has to go. */
static int Receive(int type, const uint8_t* p, int len, const char* peer, char* refusal,
                   size_t refusal_size) {
    switch (type) {
    case LINK_MSG_HELLO: {
        char identity[256];
        int mine_matches;
        if (len < 5 || memcmp(p, kMagic, 4) || p[4] != PROTOCOL_VERSION) {
            snprintf(refusal, refusal_size, "%s does not speak this link protocol", peer);
            SendNow(LINK_MSG_BYE, "different link protocol", 23);
            return 0;
        }
        memcpy(identity, p + 5, (size_t)(len - 5));
        identity[len - 5] = '\0';
        Psyz_OsLock(net.lock);
        mine_matches = !strcmp(identity, net.identity);
        Psyz_OsUnlock(net.lock);
        if (!mine_matches) {
            snprintf(refusal, refusal_size, "%s runs a different game: %s", peer, identity);
            SendNow(LINK_MSG_BYE, "different game version or video timing", 38);
            return 0;
        }
        Psyz_OsLock(net.lock);
        net.open = 1;
        net.sent = net.received = 0;
        net.rtt_us = 0;
        Psyz_OsUnlock(net.lock);
        Psyz_SioWireUp();
        SetStatus(PSYZ_LINK_CONNECTED, "Connected to %s", peer);
        return 1;
    }
    case LINK_MSG_LINES: {
        LinkLines lines;
        if (net.open && LinkLinesDecode(&lines, p, len))
            Psyz_SioWireLines(&lines);
        return 1;
    }
    case LINK_MSG_DATA:
        if (net.open) {
            Psyz_OsLock(net.lock);
            net.received += (unsigned long)len;
            Psyz_OsUnlock(net.lock);
            Psyz_SioWireData(p, len);
        }
        return 1;
    case LINK_MSG_PING:
        SendNow(LINK_MSG_PONG, p, len);
        return 1;
    case LINK_MSG_PONG:
        if (len == 8) {
            uint64_t sent = 0;
            for (int i = 0; i < 8; i++)
                sent |= (uint64_t)p[i] << (8 * i);
            Psyz_OsLock(net.lock);
            net.rtt_us = (unsigned)(Psyz_OsNowUs() - sent);
            Psyz_OsUnlock(net.lock);
        }
        return 1;
    case LINK_MSG_BYE: {
        char reason[256];
        memcpy(reason, p, (size_t)len);
        reason[len] = '\0';
        snprintf(refusal, refusal_size, "%s refused the link: %s", peer, reason);
        return 0;
    }
    default:
        return 1;
    }
}

static void NetThread(void) {
    unsigned generation = ~0u;
    LinkSocket listener = LINK_BAD_SOCKET;
    uint64_t next_try = 0, connected_at = 0, last_ping = 0;
    uint8_t buffer[8192];
    int buffered = 0;
    char peer[96] = "", refusal[256] = "";

    for (;;) {
        PsyzLinkMode mode;
        char address[256];
        unsigned port, now_generation, delay;
        uint64_t now = Psyz_OsNowUs();

        Psyz_OsLock(net.lock);
        mode = net.mode;
        port = net.port;
        delay = Psyz_LinkDelayMs;
        now_generation = net.generation;
        snprintf(address, sizeof(address), "%s", net.address);
        Psyz_OsUnlock(net.lock);
        if (now_generation != generation) {
            generation = now_generation;
            Disconnect();
            if (listener != LINK_BAD_SOCKET)
                LinkClose(listener);
            listener = LINK_BAD_SOCKET;
            next_try = 0;
            refusal[0] = '\0';
        }
        if (mode == PSYZ_LINK_OFF) {
            SetStatus(PSYZ_LINK_IDLE, "Off: no cable");
            NetSleep(100000);
            continue;
        }

        if (net.conn == LINK_BAD_SOCKET) {
            LinkSocket s = LINK_BAD_SOCKET;
            if (now < next_try) {
                NetSleep(next_try - now < 100000 ? next_try - now : 100000);
                continue;
            }
            if (mode == PSYZ_LINK_HOST) {
                if (listener == LINK_BAD_SOCKET) {
                    listener = Listen(port);
                    if (listener == LINK_BAD_SOCKET) {
                        SetStatus(PSYZ_LINK_ERROR, "Cannot use port %u: %s", port, SocketError());
                        next_try = now + 2000000;
                        continue;
                    }
                }
                if (refusal[0])
                    SetStatus(PSYZ_LINK_WAITING, "%s. Waiting on port %u", refusal, port);
                else
                    SetStatus(PSYZ_LINK_WAITING, "Waiting for the other player on port %u", port);
                if (WaitReadable(listener, LINK_BAD_SOCKET, 100000) & 1)
                    s = accept(listener, NULL, NULL);
            } else {
                char why[128];
                SetStatus(PSYZ_LINK_WAITING, "Connecting to %s port %u", address, port);
                s = Connect(address, port, why, sizeof(why));
                if (s == LINK_BAD_SOCKET) {
                    if (refusal[0])
                        SetStatus(PSYZ_LINK_ERROR, "%s", refusal);
                    else
                        SetStatus(PSYZ_LINK_WAITING, "Connecting to %s port %u (%s)", address, port, why);
                    next_try = now + (refusal[0] ? 5000000 : 1000000);
                    continue;
                }
            }
            if (s == LINK_BAD_SOCKET)
                continue;
            Tune(s);
            Psyz_OsLock(net.send_lock);
            net.conn = s;
            Psyz_OsUnlock(net.send_lock);
            PeerName(s, peer, sizeof(peer));
            buffered = 0;
            connected_at = Psyz_OsNowUs();
            last_ping = 0;
            SendHello();
            continue;
        }

        /* connected: read, answer, refuse a third console */
        {
            uint64_t wait = FlushHeld(delay ? 1000 : 50000);
            int ready = WaitReadable(net.conn, listener, wait ? wait : 1);
            int drop = 0;
            refusal[0] = '\0';
            if (ready & 2) {
                LinkSocket extra = accept(listener, NULL, NULL);
                if (extra != LINK_BAD_SOCKET) {
                    uint8_t bye[64];
                    int n = Frame(bye, LINK_MSG_BYE, "a link is already in use", 24);
                    SendAll(extra, bye, n);
                    LinkClose(extra);
                }
            }
            if (ready & 1) {
                int n = (int)recv(net.conn, (char*)buffer + buffered, (int)sizeof(buffer) - buffered, 0);
                if (n <= 0) {
                    drop = 1;
                    snprintf(refusal, sizeof(refusal), "%s left", peer);
                } else {
                    int at = 0;
                    buffered += n;
                    while (!drop && buffered - at >= 2 && buffered - at >= 2 + buffer[at + 1]) {
                        if (!Receive(buffer[at], buffer + at + 2, buffer[at + 1], peer, refusal,
                                     sizeof(refusal)))
                            drop = 1;
                        at += 2 + buffer[at + 1];
                    }
                    memmove(buffer, buffer + at, (size_t)(buffered - at));
                    buffered -= at;
                }
            }
            now = Psyz_OsNowUs();
            if (!drop && !net.open && now - connected_at > HELLO_TIMEOUT_US) {
                drop = 1;
                snprintf(refusal, sizeof(refusal), "%s did not answer", peer);
            }
            if (!drop && net.open && now - last_ping >= PING_US) {
                uint8_t stamp[8];
                for (int i = 0; i < 8; i++)
                    stamp[i] = (uint8_t)(now >> (8 * i));
                SendNow(LINK_MSG_PING, stamp, 8);
                last_ping = now;
            }
            if (drop) {
                Disconnect();
                SetStatus(PSYZ_LINK_WAITING, "%s", refusal);
                /* a refused join waits before it asks again */
                next_try = now + (mode == PSYZ_LINK_JOIN ? 2000000 : 0);
            }
        }
    }
}

int Psyz_LinkConfigure(PsyzLinkMode mode, const char* address, unsigned port,
                       const char* identity) {
    if (mode < PSYZ_LINK_OFF || mode > PSYZ_LINK_JOIN || port == 0 || port > 65535)
        return -1;
    if (mode == PSYZ_LINK_JOIN && (!address || !*address))
        return -1;
    if (!net.lock) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        net.lock = Psyz_OsLockCreate();
        net.send_lock = Psyz_OsLockCreate();
        Psyz_SioWireInit();
    }
    Psyz_OsLock(net.lock);
    if (net.mode == mode && net.port == port &&
        !strcmp(net.address, address ? address : "") &&
        !strcmp(net.identity, identity ? identity : "")) {
        Psyz_OsUnlock(net.lock);
        return 0;
    }
    net.mode = mode;
    net.port = port;
    snprintf(net.address, sizeof(net.address), "%s", address ? address : "");
    snprintf(net.identity, sizeof(net.identity), "%s", identity ? identity : "");
    net.generation++;
    if (mode == PSYZ_LINK_OFF) {
        net.state = PSYZ_LINK_IDLE;
        snprintf(net.status, sizeof(net.status), "Off: no cable");
    }
    Psyz_OsUnlock(net.lock);
    if (mode != PSYZ_LINK_OFF && !net.started) {
        net.started = 1;
        if (Psyz_OsThreadStart(NetThread, "psyz_link") != 0)
            ERRORF("link: cannot start the network thread");
    }
    return 0;
}

void Psyz_LinkSetDelay(unsigned ms) {
    if (!net.lock)
        Psyz_LinkConfigure(PSYZ_LINK_OFF, "", PSYZ_LINK_DEFAULT_PORT, "");
    Psyz_OsLock(net.lock);
    Psyz_LinkDelayMs = ms;
    Psyz_OsUnlock(net.lock);
}

PsyzLinkState Psyz_LinkStatus(char* text, size_t size) {
    PsyzLinkState state;
    if (!net.lock) {
        if (text && size)
            snprintf(text, size, "Off: no cable");
        return PSYZ_LINK_IDLE;
    }
    Psyz_OsLock(net.lock);
    state = net.state;
    if (text && size) {
        if (state == PSYZ_LINK_CONNECTED && net.rtt_us)
            snprintf(text, size, "%s, round trip %.1f ms", net.status, net.rtt_us / 1000.0);
        else
            snprintf(text, size, "%s", net.status[0] ? net.status : "Off: no cable");
    }
    Psyz_OsUnlock(net.lock);
    return state;
}

void Psyz_LinkStats(unsigned* rtt_us, unsigned long* sent, unsigned long* received) {
    if (!net.lock) {
        if (rtt_us)
            *rtt_us = 0;
        if (sent)
            *sent = 0;
        if (received)
            *received = 0;
        return;
    }
    Psyz_OsLock(net.lock);
    if (rtt_us)
        *rtt_us = net.rtt_us;
    if (sent)
        *sent = net.sent;
    if (received)
        *received = net.received;
    Psyz_OsUnlock(net.lock);
}
