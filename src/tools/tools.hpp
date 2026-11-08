#pragma once

#include "harness/registry.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ccb {

struct SendOptions {
    std::string harness;
    std::string cwd;
    std::string kinds = "text";
    double step_timeout = 180.0;
    std::vector<std::string> prompts;
};

int run_send(const SendOptions& options, const Registry& registry);

struct ClientOptions {
    std::string harness;
    std::string session = "clitest";
    std::string cwd;
    std::string url = "ws://127.0.0.1:8787/ws";
    std::string kinds = "text,tool";
    double turn_timeout = 180.0;
    std::vector<std::string> prompts;
};

int run_client(const ClientOptions& options);

struct CaptureOptions {
    std::string harness;
    std::string out;
    int cols = 120;
    int rows = 45;
    std::string seq_json;
    std::string seq_file;
    double startup_delay = 5.0;
    double gap = 5.0;
    double min_turn = 6.0;
    double final_idle = 15.0;
    double max_seconds = 300.0;
    bool dump_markers = false;
    bool verbatim_env = false;
    std::vector<std::string> extra_args;
};

int run_capture(const CaptureOptions& options, const Registry& registry);

struct AnalyzeOptions {
    std::string path;
    std::string view = "both";
    int tail = 0;
    std::string out;
};

int run_analyze(const AnalyzeOptions& options);

std::string strip_ansi(std::string_view raw);
std::string to_text(std::string_view stripped);

}
