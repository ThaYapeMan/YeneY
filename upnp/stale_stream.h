#pragma once
#include "stream_server.h"

namespace upnp {
// Empty, closed response also used for a URI retained across a product upgrade.
inline void rejectStaleStream(StreamRequest& request) {
    static const char response[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    request.send(response, sizeof(response) - 1);
    request.disconnect();
}
}
