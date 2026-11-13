/* libcomb's serial port over the network link: a fake other console on a
 * TCP socket speaks the link protocol (src/psyz/link_wire.h) to PsyZ. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* getaddrinfo, select under -std=c11 */
#endif
#include <psyz.h>
#include "ztest.h"

/* POSIX hosts: on Windows the PS1 headers' u_long and OpenEvent clash with
 * winsock's, and link_net.c's Winsock side is compiled but not run here. */
#if !defined(__psx__) && !defined(__PSP__) && !defined(__EMSCRIPTEN__) &&       \
    !defined(__ANDROID__) && !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int PeerSocket;
#define PEER_BAD (-1)
#define PeerClose close
#include <kernel.h>
#include <libapi.h>
#include <libcomb.h>
#include <psyz/link.h>
#include <psyz/system.h>
#include <string.h>

enum { PORT = 47391, HELLO = 1, LINES = 2, DATA = 3, PING = 4, PONG = 5 };

static PeerSocket peer = PEER_BAD;
static unsigned char inbox[8192];
static int inbox_len;
/* what PsyZ reported last, and how many data bytes it sent */
static int seen_dtr, seen_rts, seen_window, seen_lines;
static unsigned seen_start, seen_end;
static unsigned char got[64];
static int got_len;

static void Spend(unsigned us) { Psyz_SpendTime(us * 1000); }

static int PeerSend(int type, const void* payload, int len) {
    unsigned char frame[2 + 255];
    frame[0] = (unsigned char)type;
    frame[1] = (unsigned char)len;
    memcpy(frame + 2, payload, (size_t)len);
    return send(peer, (const char*)frame, 2 + len, 0) == 2 + len;
}

static void PeerLines(int dtr, int rts, int window, unsigned start, unsigned end, int chunk) {
    unsigned char l[16] = {0};
    l[0] = (unsigned char)((dtr ? 1 : 0) | (rts ? 2 : 0) | (window ? 4 : 0));
    l[1] = (unsigned char)chunk;
    for (int i = 0; i < 4; i++) {
        l[4 + i] = (unsigned char)(start >> (8 * i)); /* next = start */
        l[8 + i] = (unsigned char)(start >> (8 * i));
        l[12 + i] = (unsigned char)(end >> (8 * i));
    }
    PeerSend(LINES, l, 16);
}

/* Reads what PsyZ sent within `us`. */
static void PeerPump(unsigned us) {
    for (;;) {
        fd_set set;
        struct timeval tv = {0, (long)us};
        int n;
        FD_ZERO(&set);
        FD_SET(peer, &set);
        if (select((int)peer + 1, &set, NULL, NULL, &tv) <= 0)
            return;
        n = (int)recv(peer, (char*)inbox + inbox_len, (int)sizeof(inbox) - inbox_len, 0);
        if (n <= 0)
            return;
        inbox_len += n;
        while (inbox_len >= 2 && inbox_len >= 2 + inbox[1]) {
            int type = inbox[0], len = inbox[1];
            const unsigned char* p = inbox + 2;
            if (type == LINES && len >= 16) {
                seen_dtr = p[0] & 1;
                seen_rts = (p[0] >> 1) & 1;
                seen_window = (p[0] >> 2) & 1;
                seen_start = p[8] | p[9] << 8 | p[10] << 16 | (unsigned)p[11] << 24;
                seen_end = p[12] | p[13] << 8 | p[14] << 16 | (unsigned)p[15] << 24;
                seen_lines++;
            } else if (type == DATA) {
                for (int i = 0; i < len && got_len < (int)sizeof(got); i++)
                    got[got_len++] = p[i];
            } else if (type == PING) {
                PeerSend(PONG, p, len);
            }
            memmove(inbox, inbox + 2 + len, (size_t)(inbox_len - 2 - len));
            inbox_len -= 2 + len;
        }
        us = 0;
    }
}

static int LinkLevels(void) { return (int)((unsigned long)_comb_control(0, 0, 0) >> 7) & 3; }

static int gave_up_after;
static int GiveUpAt2000(int kind, unsigned polls) {
    (void)kind;
    if (polls >= 2000) {
        gave_up_after = (int)polls;
        return 0;
    }
    return 1;
}

ZTEST(link, cable_over_tcp) {
    static const unsigned char packet[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    unsigned char buffer[12] = {0};
    unsigned char hello[64] = {'R', 'R', 'L', 'K', 1};
    struct sockaddr_in addr;
    char status[256];
    long event;
    int i;

    AddCOMB();
    _comb_control(1, 3, 0x7E900);
    _comb_control(1, 4, 4);
    /* no cable: DSR and CTS off */
    zassert_s32_eq(LinkLevels(), 0);
    zassert_s32_eq(Psyz_LinkConfigure(PSYZ_LINK_HOST, "", PORT, "link test"), 0);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(0x7F000001);
    for (i = 0; i < 100; i++) {
        peer = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(peer, (struct sockaddr*)&addr, sizeof(addr)) == 0)
            break;
        PeerClose(peer);
        peer = PEER_BAD;
        Spend(20000);
    }
    zassert_s32_eq(1, !!(peer != PEER_BAD));
    {
        int one = 1;
        setsockopt(peer, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    }
    memcpy(hello + 5, "link test", 9);
    PeerSend(HELLO, hello, 14);
    for (i = 0; i < 200 && Psyz_LinkStatus(status, sizeof(status)) != PSYZ_LINK_CONNECTED; i++) {
        PeerPump(5000);
    }
    zassert_s32_eq(Psyz_LinkStatus(status, sizeof(status)), PSYZ_LINK_CONNECTED);

    /* the other end's DTR is this end's DSR */
    PeerLines(1, 0, 0, 0, 0, 0);
    for (i = 0; i < 200 && LinkLevels() != 1; i++)
        PeerPump(1000);
    zassert_s32_eq(LinkLevels(), 1);

    /* asynchronous read of a 12-byte packet, 4 bytes per interrupt */
    event = OpenEvent(HwSIO, EvSpIOER, EvMdNOINTR, NULL);
    EnableEvent(event);
    zassert_s32_eq(Psyz_CombRead(buffer, 12, 1), 0);
    for (i = 0; i < 200 && !(seen_rts && seen_window); i++)
        PeerPump(1000);
    zassert_s32_eq(1, !!(seen_rts && seen_window));
    zassert_s32_eq((int)(seen_end - seen_start), 12);
    zassert_s32_eq(TestEvent(event), 0);
    seen_lines = 0;
    PeerSend(DATA, packet, 12);
    for (i = 0; i < 400 && !TestEvent(event); i++) {
        PeerPump(0);
        Spend(500);
    }
    zassert_s32_eq(1, !!(i < 400));
    zassert_s32_eq(memcmp(buffer, packet, 12), 0);
    /* RTS drops with the last byte; DTR toggled once per 4 bytes */
    for (i = 0; i < 200 && seen_rts; i++)
        PeerPump(1000);
    zassert_s32_eq(seen_rts, 0);
    zassert_s32_eq(seen_window, 0);
    zassert_s32_eq(seen_dtr, 1); /* three toggles from off */
    CloseEvent(event);

    /* a synchronous write into a read the other end announced: the
     * acknowledging DTR toggles follow from the bytes sent, at once */
    PeerLines(1, 1, 1, 0, 12, 4);
    for (i = 0; i < 200 && LinkLevels() != 3; i++)
        PeerPump(1000);
    zassert_s32_eq(LinkLevels(), 3);
    _comb_control(4, 0, (u_long)(uintptr_t)GiveUpAt2000);
    gave_up_after = 0;
    zassert_s32_eq(Psyz_CombWrite(packet, 12, 0), 12);
    zassert_s32_eq(gave_up_after, 0);
    /* its read is complete: CTS is off until it reads again */
    zassert_s32_eq(LinkLevels() >> 1, 0);
    for (i = 0; i < 200 && got_len < 12; i++)
        PeerPump(1000);
    zassert_s32_eq(got_len, 12);
    zassert_s32_eq(memcmp(got, packet, 12), 0);

    /* nobody reading: the first byte waits in the transmitter for CTS, the
     * wait callback gives up the transfer (libsio counts that byte) */
    zassert_s32_eq(Psyz_CombWrite(packet, 12, 0), 1);
    zassert_s32_eq(gave_up_after, 2000);
    _comb_control(2, 0, 0); /* drops the waiting byte */
    _comb_control(4, 0, 0);

    /* unplugged */
    Psyz_LinkConfigure(PSYZ_LINK_OFF, "", PORT, "link test");
    for (i = 0; i < 200 && LinkLevels() != 0; i++)
        Spend(1000);
    zassert_s32_eq(LinkLevels(), 0);
    PeerClose(peer);
    peer = PEER_BAD;
    DelCOMB();
}
#endif
