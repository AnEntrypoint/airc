#include "tools/tools.hpp"

#include "session/agent_session.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>

namespace ccb {

namespace {

constexpr std::size_t thinking_preview_codepoints = 200;
constexpr auto poll_interval = std::chrono::milliseconds(200);

struct StopOnExit {
    AgentSession& session;
    ~StopOnExit() {
        try {
            session.stop();
        } catch (...) {
        }
    }
};

std::string first_codepoints(const std::string& utf8, std::size_t limit) {
    std::size_t counted = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        if ((static_cast<unsigned char>(utf8[i]) & 0xC0) != 0x80) {
            if (counted == limit) break;
            ++counted;
        }
        ++i;
    }
    return utf8.substr(0, i);
}

void emit(const std::string& text) {
    std::fputs(text.c_str(), stdout);
    std::fflush(stdout);
}

bool print_events(const std::vector<Event>& events) {
    for (const Event& event : events) {
        switch (event.kind) {
            case Kind::TurnEnd:
                return true;
            case Kind::Tool:
                emit("  [tool] " + event.text + "\n");
                break;
            case Kind::Thinking:
                emit("  [think] " + first_codepoints(event.text, thinking_preview_codepoints) + "\n");
                break;
            default:
                emit(event.text);
                break;
        }
    }
    return false;
}

bool run_prompt(AgentSession& session, const std::string& prompt, const KindSet& kinds, double step_timeout) {
    emit("\n>>> " + prompt + "\n");
    session.send(prompt);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(step_timeout);
    while (std::chrono::steady_clock::now() < deadline) {
        if (print_events(session.events(kinds))) return true;
        std::this_thread::sleep_for(poll_interval);
    }
    return false;
}

}

int run_send(const SendOptions& options, const Registry& registry) {
    try {
        const std::string harness_name = options.harness.empty() ? registry.default_name() : options.harness;
        const auto harness = registry.get(harness_name);
        if (!harness) {
            std::fprintf(stderr, "unknown harness %s\n", harness_name.c_str());
            return 2;
        }
        SessionOptions session_options;
        session_options.cwd = options.cwd;
        AgentSession session(harness, session_options);
        session.start();
        StopOnExit guard{session};

        if (!session.wait_ready()) {
            std::fprintf(stderr, "%s did not become ready\n", harness_name.c_str());
            return 1;
        }
        emit("[session " + session.name() + " harness=" + harness_name + " cwd=" + session.cwd() + "]\n");

        const KindSet kinds = kinds_from_csv(options.kinds);
        for (const std::string& prompt : options.prompts) {
            run_prompt(session, prompt, kinds, options.step_timeout);
            emit("\n");
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

}
