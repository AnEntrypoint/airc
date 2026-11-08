#include "common/paths.hpp"
#include "harness/registry.hpp"
#include "server/server.hpp"
#include "tools/tools.hpp"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifndef CCB_VERSION
#define CCB_VERSION "0.0.0"
#endif

namespace {

constexpr double default_idle_reap_seconds = 1800.0;
constexpr int exit_usage = 2;

std::string absolute_path(const std::string& path) {
    return std::filesystem::absolute(path).lexically_normal().string();
}

std::string default_working_directory() {
    const std::string from_env = ccb::env_or_empty("CCBRIDGE_CWD");
    if (!from_env.empty()) return absolute_path(from_env);
    return absolute_path((std::filesystem::current_path() / ".ccbridge" / "scratch").string());
}

std::string resolve_working_directory(const std::string& requested) {
    return requested.empty() ? default_working_directory() : absolute_path(requested);
}

double default_idle_reap() {
    const std::string from_env = ccb::env_or_empty("CCBRIDGE_IDLE_REAP");
    if (from_env.empty()) return default_idle_reap_seconds;
    try {
        return std::stod(from_env);
    } catch (const std::exception&) {
        return default_idle_reap_seconds;
    }
}

std::string join_names(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out;
}

bool resolve_harness(std::string& name, const ccb::Registry& registry) {
    if (name.empty()) name = registry.default_name();
    if (registry.get(name)) return true;
    std::fprintf(stderr, "unknown harness %s (available: %s)\n", name.c_str(),
                 join_names(registry.names()).c_str());
    return false;
}

int list_templates(const ccb::Registry& registry, bool check) {
    int problems = 0;
    for (const std::string& name : registry.names()) {
        const auto harness = registry.get(name);
        if (check) {
            const bool launchable = harness && !harness->argv.empty() && harness->transcript;
            if (!launchable) {
                std::fprintf(stderr, "template %s is incomplete\n", name.c_str());
                ++problems;
                continue;
            }
        }
        std::printf("%s%s\n", name.c_str(), name == registry.default_name() ? " (default)" : "");
    }
    if (check && problems == 0) std::printf("ok: %zu templates\n", registry.names().size());
    return problems == 0 ? 0 : 1;
}

}

int main(int argc, char** argv) {
    CLI::App app{"ccbridge: drive an interactive coding agent in a PTY over a WebSocket"};
    app.set_version_flag("--version", CCB_VERSION);
    app.require_subcommand(1);

    std::string templates_dir;
    app.add_option("--templates", templates_dir, "directory searched first for harness templates");

    ccb::ServerOptions serve_options;
    std::string serve_cwd;
    int serve_port = serve_options.port;
    CLI::App* serve = app.add_subcommand("serve", "run the WebSocket bridge");
    serve->add_option("--host", serve_options.host, "listen address")->capture_default_str();
    serve->add_option("--port", serve_port, "listen port")->check(CLI::Range(0, 65535))->capture_default_str();
    serve->add_option("--cwd", serve_cwd, "default working directory for new sessions");
    serve->add_option("--idle-reap", serve_options.idle_reap_seconds,
                      "seconds before an idle hub is reaped (env CCBRIDGE_IDLE_REAP)");

    ccb::SendOptions send_options;
    CLI::App* send = app.add_subcommand("send", "send prompts to a harness and print the reply");
    send->add_option("prompts", send_options.prompts, "prompts to send in order")->required();
    send->add_option("--harness", send_options.harness, "harness template name");
    send->add_option("--cwd", send_options.cwd, "working directory");
    send->add_option("--kinds", send_options.kinds, "comma list: text,thinking,tool,result,you")
        ->capture_default_str();
    send->add_option("--step-timeout", send_options.step_timeout, "seconds to wait for each reply")
        ->capture_default_str();

    ccb::ClientOptions client_options;
    CLI::App* client = app.add_subcommand("client", "WebSocket client for a running bridge");
    client->add_option("prompts", client_options.prompts, "prompts to send in order");
    client->add_option("--harness", client_options.harness, "harness template name");
    client->add_option("--session", client_options.session, "named hub")->capture_default_str();
    client->add_option("--cwd", client_options.cwd, "working directory for a new hub");
    client->add_option("--url", client_options.url, "bridge URL")->capture_default_str();
    client->add_option("--kinds", client_options.kinds, "event kinds")->capture_default_str();
    client->add_option("--turn-timeout", client_options.turn_timeout, "seconds to wait for each frame")
        ->capture_default_str();

    ccb::CaptureOptions capture_options;
    std::string env_mode = "nesting-stripped";
    CLI::App* capture = app.add_subcommand("capture", "record raw PTY bytes from a harness");
    capture->prefix_command();
    capture->add_option("--harness", capture_options.harness, "harness template name");
    capture->add_option("--out", capture_options.out, "raw output file")->required();
    capture->add_option("--cols", capture_options.cols)->capture_default_str();
    capture->add_option("--rows", capture_options.rows)->capture_default_str();
    capture->add_option("--seq", capture_options.seq_json, "JSON array of prompts");
    capture->add_option("--seq-file", capture_options.seq_file, "file holding the JSON array");
    capture->add_option("--startup-delay", capture_options.startup_delay)->capture_default_str();
    capture->add_option("--gap", capture_options.gap)->capture_default_str();
    capture->add_option("--min-turn", capture_options.min_turn)->capture_default_str();
    capture->add_option("--final-idle", capture_options.final_idle)->capture_default_str();
    capture->add_option("--max-seconds", capture_options.max_seconds)->capture_default_str();
    capture->add_flag("--dump-markers", capture_options.dump_markers, "print ready and busy marker report");
    capture->add_option("--env-mode", env_mode, "environment given to the harness")
        ->check(CLI::IsMember({"nesting-stripped", "verbatim"}))
        ->capture_default_str();

    ccb::AnalyzeOptions analyze_options;
    CLI::App* analyze = app.add_subcommand("analyze", "strip ANSI and TUI chrome from a raw capture");
    analyze->add_option("path", analyze_options.path, "raw capture file")->required();
    analyze->add_option("--view", analyze_options.view, "strip, text or both")
        ->check(CLI::IsMember({"strip", "text", "both"}))
        ->capture_default_str();
    analyze->add_option("--tail", analyze_options.tail, "keep the last N lines");
    analyze->add_option("--out", analyze_options.out, "write PREFIX.strip and PREFIX.text");

    bool check_templates = false;
    CLI::App* templates = app.add_subcommand("templates", "list harness templates");
    templates->add_flag("--check", check_templates, "validate every template and exit non-zero on error");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return app.exit(error);
    }

    if (!templates_dir.empty()) ccb::add_template_dir_override(templates_dir);

    std::optional<ccb::Registry> registry;
    try {
        registry.emplace(ccb::Registry::load(ccb::template_search_path()));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return exit_usage;
    }

    try {
        if (templates->parsed()) return list_templates(*registry, check_templates);
        if (analyze->parsed()) return ccb::run_analyze(analyze_options);
        if (serve->parsed()) {
            serve_options.port = static_cast<std::uint16_t>(serve_port);
            serve_options.default_cwd = resolve_working_directory(serve_cwd);
            if (serve->count("--idle-reap") == 0) serve_options.idle_reap_seconds = default_idle_reap();
            return ccb::run_server(serve_options, *registry);
        }
        if (send->parsed()) {
            if (!resolve_harness(send_options.harness, *registry)) return exit_usage;
            send_options.cwd = resolve_working_directory(send_options.cwd);
            return ccb::run_send(send_options, *registry);
        }
        if (client->parsed()) {
            if (!resolve_harness(client_options.harness, *registry)) return exit_usage;
            if (!client_options.cwd.empty()) client_options.cwd = absolute_path(client_options.cwd);
            return ccb::run_client(client_options);
        }
        if (capture->parsed()) {
            if (!resolve_harness(capture_options.harness, *registry)) return exit_usage;
            capture_options.verbatim_env = env_mode == "verbatim";
            capture_options.extra_args = capture->remaining();
            return ccb::run_capture(capture_options, *registry);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return exit_usage;
}
