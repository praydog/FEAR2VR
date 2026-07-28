#include "CommandServer.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace cmdsrv {
namespace {

std::atomic<bool> g_running{false};
std::atomic<bool> g_unload{false};
SOCKET g_listen = INVALID_SOCKET;
std::thread g_server;
Handlers g_handlers;

void send_response(SOCKET c, int32_t status, const std::string& body) {
    const char* reason = status == 200   ? "OK"
                         : status == 404 ? "Not Found"
                         : status == 503 ? "Service Unavailable"
                                         : "Error";
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                       "\r\nContent-Type: application/json\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    send(c, head.data(), static_cast<int32_t>(head.size()), 0);
    if (!body.empty()) send(c, body.data(), static_cast<int32_t>(body.size()), 0);
}

void handle_client(SOCKET c) {
    // Bound recv() so a slow/silent client can't wedge the socket thread and
    // block a shutdown join indefinitely.
    uint32_t rcv_timeout = 3000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv_timeout), sizeof(rcv_timeout));

    std::string req;
    char buf[4096];

    size_t header_end = std::string::npos;
    while ((header_end = req.find("\r\n\r\n")) == std::string::npos) {
        int32_t n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) return;
        req.append(buf, n);
        if (req.size() > (1u << 22)) return; // 4 MiB guard
    }

    size_t sp1 = req.find(' ');
    if (sp1 == std::string::npos) return;
    size_t sp2 = req.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return;
    std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);

    // Consume the (optional) body; our endpoints only need method+path today.
    size_t body_start = header_end + 4;
    size_t content_length = 0;
    {
        std::string headers = req.substr(0, header_end);
        for (auto& ch : headers) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        size_t p = headers.find("content-length:");
        if (p != std::string::npos)
            content_length = static_cast<size_t>(strtoul(req.c_str() + p + 15, nullptr, 10));
    }
    while (req.size() - body_start < content_length) {
        int32_t n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, n);
    }

    if (path.compare(0, 7, "/health") == 0) {
        std::string fragment;
        if (g_handlers.health) {
            fragment = g_handlers.health();
        }
        if (fragment.empty()) {
            fragment = "\"state\":\"no-health-handler\"";
        }
        std::string b = "{\"ok\":true,\"unload_requested\":";
        b += g_unload.load() ? "true" : "false";
        b += ",";
        b += fragment;
        b += "}";
        send_response(c, 200, b);
        return;
    }

    if (path.compare(0, 5, "/test") == 0) {
        if (!g_handlers.test) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no test handler registered\"}");
            return;
        }
        // The handler returns the inner result object; wrap in the envelope.
        std::string b = "{\"ok\":true,\"result\":";
        b += g_handlers.test();
        b += "}";
        send_response(c, 200, b);
        return;
    }

    if (path.compare(0, 7, "/unload") == 0) {
        // Respond FIRST, then latch the flag: the supervisor sees the flag only
        // after the client got its response (graceful unload handshake).
        send_response(c, 200, "{\"ok\":true,\"unload\":\"requested\"}");
        g_unload.store(true);
        return;
    }

    send_response(c, 404, "{\"ok\":false,\"error\":\"unknown endpoint; use GET /health, GET /test, GET|POST /unload\"}");
}

void server_loop(int32_t port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        g_running.store(false);
        return;
    }

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) {
        WSACleanup();
        g_running.store(false);
        return;
    }

    int32_t yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(ls, 8) != 0) {
        closesocket(ls);
        WSACleanup();
        g_running.store(false);
        return;
    }

    g_listen = ls;

    while (g_running.load()) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (!g_running.load()) break;
            continue;
        }
        handle_client(c);
        closesocket(c);
    }

    closesocket(ls);
    g_listen = INVALID_SOCKET;
    WSACleanup();
}

} // namespace

bool start(int32_t port, Handlers handlers) {
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) {
        return true; // already running
    }

    g_handlers = std::move(handlers);

    try {
        g_server = std::thread(server_loop, port);
    } catch (...) {
        g_running.store(false);
        return false;
    }
    return true;
}

void stop() {
    if (!g_running.exchange(false)) {
        return;
    }

    // Break the blocking accept(): a self-connect wakes accept so the loop
    // observes g_running == false and exits on its own (bind may still be
    // racing start() in pathological cases -- just refuse to wait forever).
    if (g_listen != INVALID_SOCKET) {
        int32_t port = 0;
        sockaddr_in bound{};
        int32_t len = sizeof(bound);
        if (getsockname(g_listen, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
            port = static_cast<int32_t>(ntohs(bound.sin_port));
        }
        if (port != 0) {
            SOCKET poke = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (poke != INVALID_SOCKET) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(static_cast<u_short>(port));
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                connect(poke, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
                closesocket(poke);
            }
        }
        // Also close the listen socket under accept(): if the self-connect
        // missed (port==0 race), closing guarantees accept() unblocks with
        // WSAENOTSOCK/WSAECONNRESET instead of hanging the join.
        SOCKET doomed = g_listen;
        g_listen = INVALID_SOCKET;
        closesocket(doomed);
    }

    if (g_server.joinable()) {
        g_server.join();
    }
}

bool running() { return g_running.load(); }

bool unload_requested() { return g_unload.load(); }

} // namespace cmdsrv
