#pragma once

#include "common/pty.hpp"
#include "common/screen.hpp"
#include "harness/harness.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ccb {

struct SessionOptions {
    std::string cwd;
    std::optional<std::string> name;
    int cols = 200;
    int rows = 50;
    std::vector<std::string> launch_args;
    bool at_end = false;
    std::optional<std::string> session_id;
};

class AgentSession {
public:
    AgentSession(std::shared_ptr<const Harness> harness, SessionOptions options);
    AgentSession(const AgentSession&) = delete;
    AgentSession& operator=(const AgentSession&) = delete;
    ~AgentSession();

    AgentSession& start();
    void stop();
    bool alive() const;
    std::string pane() const;
    bool busy() const;
    bool wait_ready(std::chrono::seconds timeout = std::chrono::seconds(60));
    void send(std::string_view text);
    std::vector<Event> events(const KindSet& kinds);

    const std::string& name() const;
    const std::string& cwd() const;
    const Harness& harness() const;
    std::string command() const;
    double start_time() const;

private:
    std::shared_ptr<const Harness> harness_;
    SessionOptions options_;
    std::string name_;
    double start_time_;
    std::unique_ptr<Channel> channel_;
    std::unique_ptr<Screen> screen_;
    std::unique_ptr<Pty> pty_;
    mutable std::mutex screen_mutex_;
};

}
