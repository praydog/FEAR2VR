#pragma once

// Minimal localhost HTTP client (GET/POST) over raw winsock, shared by the
// injector and the ctest fixture runner. Header-only; each call does its own
// WSAStartup/WSACleanup so callers need no init. Best-effort: returns false on
// any transport failure.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <cstdio>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace http {

// Connect to 127.0.0.1:port, send `request`, append the full raw response to
// `out`. Returns false on any socket failure.
inline bool request_raw(int32_t port, const std::string& request, std::string& out) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    bool ok = false;
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        if (send(s, request.data(), static_cast<int32_t>(request.size()), 0) == static_cast<int32_t>(request.size())) {
            char buf[1024];
            int32_t r;
            while ((r = recv(s, buf, sizeof(buf), 0)) > 0) {
                out.append(buf, r);
            }
            ok = true;
        }
    }
    closesocket(s);
    WSACleanup();
    return ok;
}

// GET path -> full raw response in `out`, REPLACING whatever was there.
//
// The clear is load-bearing and was missing. request_raw APPENDS, deliberately, but these two wrappers promise
// "the full raw response in out" -- and a caller that reused one buffer across several requests silently got a
// concatenation, with body_of() then returning the FIRST response's body. That cost a long debugging detour in
// the fixture: a report read as empty while assertions using find() passed against data further down the blob.
// Either contract is defensible; having both in one header was not.
inline bool get(int32_t port, const char* path, std::string& out) {
    out.clear();
    char req[256];
    int32_t n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path);
    return request_raw(port, std::string(req, n), out);
}

// POST path with `body` -> full raw response in `out`.
inline bool post(int32_t port, const char* path, const std::string& body, std::string& out) {
    out.clear();
    char hdr[256];
    int32_t n = snprintf(hdr, sizeof(hdr),
                         "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %zu\r\n"
                         "Connection: close\r\n\r\n", path, body.size());
    std::string req(hdr, n);
    req += body;
    return request_raw(port, req, out);
}

// True if a server answers /health on the port.
inline bool port_open(int32_t port) {
    std::string resp;
    return get(port, "/health", resp);
}

// Just the body (after the CRLFCRLF header/body separator).
inline std::string body_of(const std::string& resp) {
    size_t p = resp.find("\r\n\r\n");
    return (p == std::string::npos) ? resp : resp.substr(p + 4);
}

} // namespace http
