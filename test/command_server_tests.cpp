// Tier-1 headless test for the IPC CommandServer: no game, no injection, no
// GPU. Spins up the real server in-process on port 8788 (distinct from the
// in-game 8798) with MOCK handlers and black-box-tests it over localhost HTTP.
// Unlike fixture-test this has no environment excuse to skip: it runs on any
// CI and must pass.
//
// ORDER MATTERS: /unload latches a one-way global flag inside CommandServer;
// the unload assertion therefore runs LAST.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include "CommandServer.hpp"
#include "HttpClient.hpp"

namespace {

constexpr int32_t kPort = 8788;
int64_t g_checks = 0;
int64_t g_failures = 0;

void check(bool ok, const char* name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        printf("[FAIL] %s\n", name);
    }
}

bool body_has(const std::string& resp, const std::string& needle) {
    return http::body_of(resp).find(needle) != std::string::npos;
}

std::string status_line_of(const std::string& resp) {
    const size_t eol = resp.find("\r\n");
    return eol == std::string::npos ? resp : resp.substr(0, eol);
}

} // namespace

int main() {
    cmdsrv::Handlers handlers;
    handlers.health = [] {
        return std::string{"\"pid\":1234,\"state\":\"running\",\"hooks\":1,\"frame_ticks\":42"};
    };
    handlers.targets = [] {
        return std::string{"{\"ok\":true,\"client_mgr_update\":\"0x0040B665\"}"};
    };
    handlers.database = [] {
        return std::string{"{\"ok\":true,\"instance\":\"0x00A022BC\",\"array_begin\":\"0x175CAD00\",\"entry_count\":1}"};
    };
    handlers.engine_hook = [](const std::string& name) {
        if (name == "hwnd") {
            return std::string{"{\"ok\":true,\"rc\":0,\"value\":\"0x000700C4\"}"};
        }
        return std::string{"{\"ok\":true,\"rc\":1,\"value\":\"0x00000000\"}"};
    };

    check(cmdsrv::start(kPort, handlers), "server starts");
    // Wait for the bind to complete (start() returns before server_loop binds).
    bool up = false;
    for (int32_t i = 0; i < 100 && !up; ++i) {
        up = cmdsrv::running() && http::port_open(kPort);
        if (!up) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(up, "server up and answering");

    if (up) {
        // /health carries our mock fragment.
        {
            std::string resp;
            check(http::get(kPort, "/health", resp), "health: transport");
            check(body_has(resp, "\"ok\":true"), "health: ok");
            check(body_has(resp, "\"frame_ticks\":42"), "health: mock fragment verbatim");
            check(body_has(resp, "\"unload_requested\":false"), "health: unload not yet latched");
        }

        // /sdk/targets passes the handler's object straight through.
        {
            std::string resp;
            check(http::get(kPort, "/sdk/targets", resp), "targets: transport");
            check(body_has(resp, "\"client_mgr_update\":\"0x0040B665\""), "targets: body verbatim");
        }

        // /sdk/database passes the handler's object straight through, same
        // shape as /sdk/targets (dedicated route, dedicated handler slot).
        {
            std::string resp;
            check(http::get(kPort, "/sdk/database", resp), "database: transport");
            check(body_has(resp, "\"instance\":\"0x00A022BC\""), "database: body verbatim");
            check(body_has(resp, "\"entry_count\":1"), "database: entry_count verbatim");
        }

        // /engine-hook positive + negative + missing-name 400.
        {
            std::string resp;
            check(http::get(kPort, "/engine-hook?name=hwnd", resp), "engine-hook: transport");
            check(body_has(resp, "\"rc\":0"), "engine-hook: positive path");
            check(body_has(resp, "\"value\":\"0x000700C4\""), "engine-hook: value verbatim");
        }
        {
            std::string resp;
            check(http::get(kPort, "/engine-hook?name=fear2vr_no_such", resp), "engine-hook neg: transport");
            check(body_has(resp, "\"rc\":1"), "engine-hook: negative path (LT_ERROR)");
        }
        {
            std::string resp;
            check(http::get(kPort, "/engine-hook", resp), "engine-hook bare: transport");
            check(status_line_of(resp).find("400") != std::string::npos, "engine-hook bare: 400");
        }

        // Unknown endpoint -> 404 with usage error.
        {
            std::string resp;
            check(http::get(kPort, "/nope", resp), "404: transport");
            check(status_line_of(resp).find("404") != std::string::npos, "404: status line");
            check(body_has(resp, "unknown endpoint"), "404: body explains");
        }

        // POST body is tolerated (ignored) on GET-capable endpoints.
        {
            std::string resp;
            check(http::post(kPort, "/health", "garbage body", resp), "post health: transport+ok");
            check(body_has(resp, "\"ok\":true"), "post health: ok");
        }

        // LAST: /unload responds ok and latches the flag for the supervisor.
        {
            std::string resp;
            check(http::get(kPort, "/unload", resp), "unload: transport");
            check(body_has(resp, "\"unload\":\"requested\""), "unload: response");
            check(cmdsrv::unload_requested(), "unload: flag latched");

            std::string resp2;
            http::get(kPort, "/health", resp2);
            check(body_has(resp2, "\"unload_requested\":true"), "unload: health reports latched flag");
        }
    }

    cmdsrv::stop();
    check(!cmdsrv::running(), "server stops");
    {
        // After stop(): connections are refused (nothing listening).
        std::string resp;
        check(!http::get(kPort, "/health", resp), "no listener after stop");
    }

    printf("%s (%lld checks)\n", g_failures == 0 ? "All tests passed" : "TESTS FAILED", g_checks);
    return g_failures == 0 ? 0 : 1;
}
