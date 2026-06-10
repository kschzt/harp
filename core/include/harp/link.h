/* HARP core — framed-link message I/O over a reliable byte pipe (fd).
 *
 * Blocking, single-threaded. Reassembles interleaved per-stream frames into
 * messages (FIN-delimited). Streams 0..3 are assembled; frames on other
 * streams are skipped (vendor-experimental streams MUST NOT be required,
 * spec §4.2).
 */
#ifndef HARP_LINK_H
#define HARP_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "harp/cbor.h"
#include "harp/frame.h"

typedef struct {
    harp_cbuf acc[4];
} harp_link;

void harp_link_init(harp_link *l);
void harp_link_free(harp_link *l);

/* Receive one complete message. 0 ok; -1 io/eof; -2 protocol violation
 * (malformed frame — fatal to the session per §4.2 / §12.4). On 0, *stream is
 * the stream id and *msg holds the message (reset+filled; caller-owned buf). */
int harp_link_recv(int fd, harp_link *l, uint8_t *stream, harp_cbuf *msg);

/* Send one message, chunked into frames of <= 65536 bytes. 0 ok, -1 io. */
int harp_link_send(int fd, uint8_t stream, const void *data, size_t len);

/* Raw helpers. */
bool harp_read_exact(int fd, void *buf, size_t n);
bool harp_write_all(int fd, const void *buf, size_t n);

#endif
