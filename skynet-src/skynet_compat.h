#ifndef SKYNET_COMPAT_H
#define SKYNET_COMPAT_H

#include <unistd.h>


#ifdef _MSC_VER
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#endif


#endif

