/* Custom control commands for the companion dynamic USB/IP server. */
#pragma once

#include <libusbip/remote.h>
#include <usbip/proto_op.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include <winsock2.h>

namespace usbip::custom
{
inline bool send_exact(SOCKET s, const void *data, size_t length)
{
    const char *p = static_cast<const char *>(data);
    while (length) {
        const int n = ::send(s, p, static_cast<int>(length), 0);
        if (n <= 0) return false;
        p += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

inline bool recv_exact(SOCKET s, void *data, size_t length)
{
    char *p = static_cast<char *>(data);
    while (length) {
        const int n = ::recv(s, p, static_cast<int>(length), 0);
        if (n <= 0) return false;
        p += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

// LIST has a standard USB/IP DEVLIST reply body. BIND/DETACH return two
// network-order uint32 values: command echo and status (0 means success).
inline bool request_status(const std::string &host, const std::string &service,
                           UINT32 command, const std::string &busid = {})
{
    auto sock = usbip::connect(host.c_str(), service.c_str());
    if (!sock) return false;

    const UINT32 wire_command = htonl(command);
    if (!send_exact(sock.get(), &wire_command, sizeof(wire_command))) {
        SetLastError(WSAECONNRESET);
        return false;
    }

    if (command == USBIP_CMD_CUSTOM_BIND || command == USBIP_CMD_CUSTOM_DETACH) {
        if (busid.empty() || busid.size() >= BUS_ID_SIZE) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        std::array<char, BUS_ID_SIZE> wire_busid{};
        std::memcpy(wire_busid.data(), busid.data(), busid.size());
        if (!send_exact(sock.get(), wire_busid.data(), wire_busid.size())) {
            SetLastError(WSAECONNRESET);
            return false;
        }

        UINT32 reply[2]{};
        if (!recv_exact(sock.get(), reply, sizeof(reply))) {
            SetLastError(WSAECONNRESET);
            return false;
        }
        const UINT32 reply_command = ntohl(reply[0]);
        const UINT32 status = ntohl(reply[1]);
        if (reply_command != command) {
            SetLastError(ERROR_INVALID_DATA);
            return false;
        }
        if (status != 0) {
            SetLastError(status == 2 ? ERROR_BUSY :
                         status == 4 ? ERROR_NOT_FOUND : ERROR_GEN_FAILURE);
            return false;
        }
    } else {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    return true;
}
} // namespace usbip::custom
