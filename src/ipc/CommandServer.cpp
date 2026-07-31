#include "CommandServer.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace cmdsrv {
namespace {

std::atomic<bool> g_running{false};
std::atomic<bool> g_unload{false};
SOCKET g_listen = INVALID_SOCKET;
std::thread g_server;
Handlers g_handlers;

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < s.size()) {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

void send_response(SOCKET c, int32_t status, const std::string& body) {
    const char* reason = status == 200   ? "OK"
                         : status == 400 ? "Bad Request"
                         : status == 404 ? "Not Found"
                         : status == 503 ? "Service Unavailable"
                                         : "Error";
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                       "\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *"
                       "\r\nContent-Length: " +
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

    if (path.compare(0, 12, "/sdk/targets") == 0) {
        if (!g_handlers.targets) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no targets handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.targets());
        return;
    }

    if (path.compare(0, 15, "/sdk/interfaces") == 0) {
        if (!g_handlers.interfaces) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no interfaces handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.interfaces());
        return;
    }

    if (path.compare(0, 10, "/sdk/piece") == 0) {
        if (!g_handlers.piece) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no piece handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.piece(path));
        return;
    }

    if (path.compare(0, 13, "/sdk/skeleton") == 0) {
        if (!g_handlers.skeleton) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no skeleton handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.skeleton(path));
        return;
    }

    if (path.rfind("/vr/comfort", 0) == 0) {
        if (!g_handlers.comfort) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no comfort handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.comfort(path));
        return;
    }

    if (path.rfind("/vr/turn", 0) == 0) {
        if (!g_handlers.turn) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no turn handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.turn(path));
        return;
    }

    if (path.rfind("/vr/viewmodel", 0) == 0) {
        if (!g_handlers.viewmodel) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no viewmodel handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.viewmodel(path));
        return;
    }

    if (path.rfind("/vr/bone", 0) == 0) {
        if (!g_handlers.bone) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no bone handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.bone(path));
        return;
    }

    if (path.rfind("/vr/hud", 0) == 0) {
        if (!g_handlers.hud) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no hud handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.hud(path));
        return;
    }

    if (path.rfind("/vr/head", 0) == 0) {
        if (!g_handlers.head) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no head handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.head(path));
        return;
    }

    if (path.rfind("/stereo/", 0) == 0) {
        if (!g_handlers.stereo) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no stereo handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.stereo(path));
        return;
    }

    if (path.rfind("/console/", 0) == 0) {
        if (!g_handlers.console) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no console handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.console(path));
        return;
    }

    if (path.rfind("/input/", 0) == 0) {
        if (!g_handlers.input) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no input handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.input(path));
        return;
    }

    if (path.rfind("/watch", 0) == 0) {
        if (!g_handlers.watch) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no watch handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.watch(path));
        return;
    }

    if (path.rfind("/focus-keep", 0) == 0) {
        if (!g_handlers.focus_keep) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no focus-keep handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.focus_keep(path));
        return;
    }

    if (path.rfind("/view-override", 0) == 0) {
        if (!g_handlers.view_override) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no view-override handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.view_override(path));
        return;
    }

    // BEFORE the shader-params prefix test, since "/sdk/write-probe" must not be swallowed by a broader match
    // and because this is the only route in this server that changes the game rather than reporting on it.
    if (path.rfind("/sdk/write-probe", 0) == 0) {
        if (!g_handlers.write_probe) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no write-probe handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.write_probe());
        return;
    }

    if (path.compare(0, 18, "/sdk/shader-params") == 0) {
        if (!g_handlers.shader_params) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no shader-params handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.shader_params());
        return;
    }

    if (path.compare(0, 12, "/sdk/objects") == 0) {
        if (!g_handlers.objects) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no objects handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.objects());
        return;
    }
    if (path.compare(0, 3, "/xr") == 0) {
        if (!g_handlers.xr) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no xr handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.xr(path));
        return;
    }

    if (path.compare(0, 12, "/sdk/weapons") == 0) {
        if (!g_handlers.weapons) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no weapons handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.weapons(path));
        return;
    }

    if (path.compare(0, 11, "/sdk/spawns") == 0) {
        if (!g_handlers.spawns) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no spawns handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.spawns(path));
        return;
    }


    if (path.compare(0, 11, "/sdk/models") == 0) {
        if (!g_handlers.models) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no models handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.models());
        return;
    }

    if (path.compare(0, 13, "/sdk/database") == 0) {
        if (!g_handlers.database) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no database handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.database());
        return;
    }

    if (path.compare(0, 12, "/engine-hook") == 0) {
        if (!g_handlers.engine_hook) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no engine-hook handler registered\"}");
            return;
        }
        // /engine-hook[?name=<n>] -- missing/empty name is a client error.
        std::string name;
        const size_t q = path.find("?name=");
        if (q != std::string::npos) {
            name = url_decode(path.substr(q + 6));
        }
        if (name.empty()) {
            send_response(c, 400, "{\"ok\":false,\"error\":\"missing ?name= parameter\"}");
            return;
        }
        send_response(c, 200, g_handlers.engine_hook(name));
        return;
    }

    if (path.compare(0, 5, "/api/") == 0) {
        if (!g_handlers.api) {
            send_response(c, 404, "{\"ok\":false,\"error\":\"no api handler registered\"}");
            return;
        }
        send_response(c, 200, g_handlers.api(path));
        return;
    }

    if (path.compare(0, 7, "/unload") == 0) {
        // Respond FIRST, then latch the flag: the supervisor sees the flag only
        // after the client got its response (graceful unload handshake).
        send_response(c, 200, "{\"ok\":true,\"unload\":\"requested\"}");
        g_unload.store(true);
        return;
    }

    send_response(c, 404,
                  "{\"ok\":false,\"error\":\"unknown endpoint; use GET /health, GET /sdk/targets, "
                  "GET /sdk/database, GET /sdk/objects, GET /sdk/models, GET /sdk/interfaces, "
                  "GET /engine-hook?name=, GET|POST /unload\"}");
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
