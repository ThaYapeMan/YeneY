// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cstdint>
#include <sys/ioctl.h>
namespace upnp {
// Linux TIOCOUTQ is SIOCOUTQ: queued TCP bytes not yet acknowledged,
// including unacknowledged transmitted data, not just TCP_INFO's notsent bytes.
inline int64_t tcpBacklog(int fd) {
#ifdef TIOCOUTQ
    int bytes = 0;
    if (!ioctl(fd, TIOCOUTQ, &bytes) && bytes >= 0) return bytes;
#else
    (void)fd;
#endif
    return -1;
}
}
