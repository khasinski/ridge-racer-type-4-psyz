// The link cable between libcomb's serial port model (libcomb.c) and the
// network transport (link_net.c). Private to PsyZ.
#ifndef PSYZ_LINK_WIRE_H
#define PSYZ_LINK_WIRE_H

#include <stdint.h>

// Messages on the connection: a type byte, a length byte, the payload.
enum {
    LINK_MSG_HELLO = 1, // magic, protocol version, identity string
    LINK_MSG_LINES = 2, // LinkLines, as LinkLinesEncode writes it
    LINK_MSG_DATA = 3,  // bytes that came off the transmitter, in order
    LINK_MSG_PING = 4,  // 8-byte host time, echoed by PONG
    LINK_MSG_PONG = 5,
    LINK_MSG_BYE = 6,   // refused: reason text
};

// What one end tells the other about its control lines and its receiver.
// Stream offsets count the bytes that arrived from the other end on this
// connection (consumed, lost or still in the FIFO), modulo 2^32.
typedef struct {
    uint8_t dtr, rts; // control line outputs: the other end's DSR and CTS
    uint8_t window;   // a read is running (async or sync) ...
    uint8_t chunk;    // ... toggling DTR after each `chunk` bytes it takes
    uint32_t next;    // offset of the next byte the receiver will take
    uint32_t start;   // offset where the running read started
    uint32_t end;     // offset at which it completes (and drops RTS)
} LinkLines;

enum { LINK_LINES_BYTES = 16 };
void LinkLinesEncode(const LinkLines* lines, uint8_t* out);
int LinkLinesDecode(LinkLines* lines, const uint8_t* in, int len);

// Transport -> serial port, from the network thread. Queued in order and
// taken by the port model under the kernel lock. Init first (main thread).
void Psyz_SioWireInit(void);
void Psyz_SioWireUp(void);   // a peer is connected: a fresh cable
void Psyz_SioWireDown(void); // the peer is gone: the cable is unplugged
void Psyz_SioWireLines(const LinkLines* lines);
void Psyz_SioWireData(const uint8_t* bytes, int len);

// Serial port -> transport. Returns 0 when no peer is connected.
int Psyz_LinkSend(int type, const void* payload, int len);

#endif
