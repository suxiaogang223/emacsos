#ifndef EMACSOS_NETWORK_H
#define EMACSOS_NETWORK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_STREAM_LIMIT 16
#define NET_BUFFER_SIZE 16384
#define NET_CONNECT_TIMEOUT_MS 8000

enum net_error {
    NET_OK, NET_INVALID, NET_NO_MEMORY, NET_NO_DEVICE, NET_NOT_SUPPORTED,
    NET_IN_PROGRESS, NET_WOULD_BLOCK, NET_REFUSED, NET_TIMEOUT, NET_RESET,
    NET_NOT_CONNECTED, NET_BROKEN_PIPE, NET_ALREADY, NET_IS_CONNECTED,
    NET_ADDRESS_IN_USE, NET_UNREACHABLE, NET_IO
};
struct net_address { uint8_t ip[4]; uint16_t port; };
struct net_stream;

/* Service only at kernel call/wait boundaries, never in an interrupt handler. */
void network_init(void);
bool network_available(void);
void network_service(void);

/* An open description owns one stream; final close consumes it on every result.
 * Addresses use network-order IP bytes and a host-order port. A nonempty read
 * reports NET_OK with zero bytes only after queued data drains at peer EOF. */
enum net_error stream_create(struct net_stream **);
enum net_error stream_connect(struct net_stream *, const struct net_address *);
enum net_error stream_local(struct net_stream *, struct net_address *);
enum net_error stream_read(struct net_stream *, void *, size_t, size_t *);
enum net_error stream_write(struct net_stream *, const void *, size_t, size_t *);
enum net_error stream_shutdown(struct net_stream *, bool, bool);
enum net_error stream_close(struct net_stream *);
/* SO_ERROR consumes the pending error; read/write still retain terminal state. */
enum net_error stream_take_error(struct net_stream *);
bool stream_read_ready(struct net_stream *);
bool stream_write_ready(struct net_stream *);

#endif
