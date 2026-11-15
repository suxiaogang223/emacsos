#ifndef EMACSOS_LWIP_CC_H
#define EMACSOS_LWIP_CC_H
#include "kernel.h"
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_UNISTD_H 1
#define LWIP_NO_CTYPE_H 1
#define LWIP_PLATFORM_DIAG(message) ((void)0)
#define LWIP_PLATFORM_ASSERT(message) panic(message)
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "zu"
extern unsigned char *network_heap;
#endif
