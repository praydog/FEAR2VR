#include "GameTests.hpp"

#include <cstdio>
#include <exception>

#include "Log.hpp"

namespace gametests {
namespace {
std::vector<std::pair<std::string, TestFn>>& tests() {
    static std::vector<std::pair<std::string, TestFn>> s_tests;
    return s_tests;
}

void json_escape(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}
} // namespace

void TestSink::check(bool ok, std::string message) {
    if (!ok) {
        failures.push_back(std::move(message));
    }
}

void add(std::string name, TestFn fn) {
    tests().emplace_back(std::move(name), std::move(fn));
}

size_t count() {
    return tests().size();
}

std::string run_all() {
    int64_t pass = 0;
    int64_t fail = 0;
    std::string failures_json;
    bool first = true;

    for (auto& [name, fn] : tests()) {
        TestSink sink;
        try {
            fn(sink);
        } catch (const std::exception& e) {
            // An escaping exception is a fail, never a crash of the server thread
            // (TESTING.MD: a red test must be inspectable, half-run state isn't).
            sink.check(false, std::string{"uncaught exception: "} + e.what());
        } catch (...) {
            sink.check(false, "uncaught exception (unknown)");
        }

        if (sink.any_failure()) {
            ++fail;
            for (const auto& msg : sink.failures) {
                if (!first) failures_json += ',';
                first = false;
                failures_json += "{\"name\":";
                json_escape(name, failures_json);
                failures_json += ",\"error\":";
                json_escape(msg, failures_json);
                failures_json += "}";
            }
            LOGX("[test] FAIL %s (%zu failing checks)", name.c_str(), sink.failures.size());
        } else {
            ++pass;
            LOGX("[test] pass %s", name.c_str());
        }
    }

    std::string out = "{\"pass\":";
    out += std::to_string(pass);
    out += ",\"fail\":";
    out += std::to_string(fail);
    out += ",\"failures\":[";
    out += failures_json;
    out += "]}";
    return out;
}

} // namespace gametests
