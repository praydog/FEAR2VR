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
        return std::string{"\"pid\":1234,\"state\":\"running\",\"hooks\":2"};
    };
    handlers.test = [] {
        return std::string{"{\"pass\":3,\"fail\":1,\"failures\":[{\"name\":\"mock\",\"error\":\"boom\"}]}"};
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
            check(body_has(resp, "\"pid\":1234"), "health: mock pid fragment verbatim");
            check(body_has(resp, "\"unload_requested\":false"), "health: unload not yet latched");
        }

        // /test wraps the mock result in the envelope.
        {
            std::string resp;
            check(http::get(kPort, "/test", resp), "test: transport");
            check(body_has(resp, "\"ok\":true"), "test: ok enveloped");
            check(body_has(resp, "\"pass\":3"), "test: result fragment verbatim");
            check(body_has(resp, "\"error\":\"boom\""), "test: failure list verbatim");
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
