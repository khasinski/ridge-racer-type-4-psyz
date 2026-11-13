#ifndef PSYZ_LINK_H
#define PSYZ_LINK_H

/**
 * @file link.h
 * @brief Link cable over the network
 *
 * The serial port (SIO1) that libcomb drives is connected to the serial
 * port of another PsyZ instance through a TCP connection: its data bytes
 * and the DTR/RTS lines of each end (the DSR/CTS inputs of the other) are
 * carried over it, so a game's own link cable protocol runs unmodified.
 * Each end models the PS1 port: bytes take their time on the wire at the
 * programmed baud rate, the transmitter waits for CTS, the receive FIFO
 * holds 8 bytes and overruns past that.
 *
 * Network latency is hidden where the cable's own timing allows it: the
 * DTR toggles with which the receiving driver acknowledges each received
 * packet, and the RTS it drops when its read is complete, follow from bytes
 * the sender itself sent into a read the receiver announced. The sender
 * sees them as a real cable would show them; everything else (line levels
 * set by the game, a read started or cancelled) reaches the other end after
 * the network's latency.
 */

#include <stddef.h>

typedef enum {
    PSYZ_LINK_OFF = 0,  /**< no cable: DSR and CTS stay off */
    PSYZ_LINK_HOST = 1, /**< wait for the other console on a TCP port */
    PSYZ_LINK_JOIN = 2, /**< connect to a host's address and port */
} PsyzLinkMode;

typedef enum {
    PSYZ_LINK_IDLE = 0,      /**< off */
    PSYZ_LINK_WAITING = 1,   /**< listening or connecting */
    PSYZ_LINK_CONNECTED = 2, /**< the cable is plugged in */
    PSYZ_LINK_ERROR = 3,     /**< cannot listen / resolve; retried */
} PsyzLinkState;

enum { PSYZ_LINK_DEFAULT_PORT = 47400 };

/**
 * @brief Plugs the cable in (or out)
 *
 * Starts the network thread on first use. HOST listens on port (all
 * interfaces, IPv6 and IPv4); JOIN connects to address:port and retries
 * until the host answers. Both ends must give the same identity (the game
 * build and its settings that change the game's timing), otherwise the
 * connection is refused with a status message. A dropped connection unplugs
 * the cable (the game sees its link error) and is re-established.
 *
 * @return 0, or -1 for an invalid mode or port
 */
int Psyz_LinkConfigure(PsyzLinkMode mode, const char* address, unsigned port,
                       const char* identity);

/**
 * @brief Test hook: adds latency
 *
 * Holds every message this end sends for ms milliseconds (one-way latency
 * added to the network's own; give it on both ends for a symmetric link).
 */
void Psyz_LinkSetDelay(unsigned ms);
/** The same delay as a variable, for tests that change it mid-run. */
extern volatile unsigned Psyz_LinkDelayMs;

/**
 * @brief Connection state and a status line for the user
 *
 * @param text receives a one-line description (may be NULL)
 * @param size size of text
 */
PsyzLinkState Psyz_LinkStatus(char* text, size_t size);

/**
 * @brief Link statistics for tests and the status line
 *
 * Round trip time of the last ping in microseconds (0 before the first),
 * data bytes sent and received on the current connection.
 */
void Psyz_LinkStats(unsigned* rtt_us, unsigned long* sent,
                    unsigned long* received);

/**
 * @brief BIOS read / write on the "sio:" device
 *
 * The device's I/O entry: a synchronous transfer busy-waits as the driver
 * does (calling the wait callback, CombWaitCallback, on every poll; each
 * poll takes the PS1 time of the driver's loop) and returns the bytes
 * transferred. An asynchronous one (FASYNC) starts the transfer and
 * returns 0, or -1 while one is running; its end is the HwSIO event
 * EvSpIOER for a read, EvSpIOEW for a write (EvSpERROR on a receive error,
 * EvSpTIMOUT when the wait callback gives up a synchronous transfer).
 */
long Psyz_CombRead(void* buffer, long length, int async);
long Psyz_CombWrite(const void* buffer, long length, int async);

#endif
