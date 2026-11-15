/* Test-only unavailable network PAL for the existing file backend suites. */
#include "network.h"
#include <stdlib.h>

void network_service(void) {}
void wait_interrupt(void) { abort(); }
enum net_error stream_create(struct net_stream **out)
{ (void) out; return NET_NO_DEVICE; }
enum net_error stream_connect(struct net_stream *s, const struct net_address *a)
{ (void) s; (void) a; return NET_NO_DEVICE; }
enum net_error stream_local(struct net_stream *s, struct net_address *a)
{ (void) s; (void) a; return NET_NO_DEVICE; }
enum net_error stream_read(struct net_stream *s, void *p, size_t n, size_t *done)
{ (void) s; (void) p; (void) n; *done = 0; return NET_NO_DEVICE; }
enum net_error stream_write(struct net_stream *s, const void *p, size_t n, size_t *done)
{ (void) s; (void) p; (void) n; *done = 0; return NET_NO_DEVICE; }
enum net_error stream_shutdown(struct net_stream *s, bool read, bool write)
{ (void) s; (void) read; (void) write; return NET_NO_DEVICE; }
enum net_error stream_close(struct net_stream *s)
{ (void) s; return NET_NO_DEVICE; }
enum net_error stream_take_error(struct net_stream *s)
{ (void) s; return NET_NO_DEVICE; }
bool stream_read_ready(struct net_stream *s) { (void) s; return false; }
bool stream_write_ready(struct net_stream *s) { (void) s; return false; }
