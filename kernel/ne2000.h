#ifndef EMACSOS_NE2000_H
#define EMACSOS_NE2000_H
#include "network.h"
#define NE2000_FRAME_MAX 1514
bool ne2000_init(uint8_t mac[6]);
enum net_error ne2000_transmit(const void *, size_t);
/* Zero bytes denotes an empty ring; malformed/overflow data is discarded. */
enum net_error ne2000_receive(void *, size_t, size_t *);
#endif
