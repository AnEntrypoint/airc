#include "common/text.hpp"
#include "harness/event.hpp"
#include "harness/transcript.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using ccb::Kind;
using ccb::KindSet;
using json = nlohmann::json;

namespace {

int failures = 0;

json text_block(const std::string& text) {
    json block = json::object();
    block["type"] = "text";
    block["text"] = text;
    return block;
}

json assistant_record(const std::string& message_id, long long index, const std::string& text) {
    json record = json::object();
    record["type"] = "assistant";
    record["message"] = {{"id", message_id}, {"content", json::array({text_block(text)})}};
    record["apiBlockIndex"] = index;
    return record;
}

void append_record(const fs::path& file, const json& record) {
    std::ofstream out(file, std::ios::app | std::ios::binary);
    out << record.dump() << '\n';
}

void check_poll(const char* label, ccb::Channel& channel, const std::vector<std::string>& expected) {
    std::vector<std::string> got;
    for (const auto& event : channel.poll(KindSet{Kind::Text, Kind::Tool})) {
        if (event.kind == Kind::Text) got.push_back(event.text);
    }
    const bool ok = got == expected;
    std::printf("%-28s text %s expected %s %s\n", label, json(got).dump().c_str(),
                json(expected).dump().c_str(), ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

}

int main() {
    const auto base = fs::temp_directory_path() /
                      ("claude-channel-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto root = base / "projects";
    const std::string cwd = (base / "proj").string();
    const auto file = root / ccb::slug_path(cwd) / "session.jsonl";
    fs::create_directories(file.parent_path());

    ccb::TranscriptSpec spec;
    spec.kind = "claude-jsonl";
    spec.root = root.string();
    auto transcript = ccb::make_transcript(spec);
    auto channel = transcript->channel(cwd, 0.0, false, std::nullopt);

    append_record(file, assistant_record("msg_1", 0, "fake "));
    check_poll("poll 1 block 0", *channel, {"fake "});

    append_record(file, assistant_record("msg_1", 1, "reply here"));
    check_poll("poll 2 block 1", *channel, {"reply here"});

    check_poll("poll 3 no new records", *channel, {});

    append_record(file, assistant_record("msg_1", 1, "reply here, sir"));
    check_poll("poll 4 block 1 grows", *channel, {", sir"});

    append_record(file, assistant_record("msg_1", 1, "reply here, sir"));
    check_poll("poll 5 duplicate record", *channel, {});

    append_record(file, assistant_record("msg_2", 0, "second"));
    check_poll("poll 6 other message", *channel, {"second"});

    append_record(file, assistant_record("msg_3", 0, "alpha"));
    append_record(file, assistant_record("msg_3", 1, "beta"));
    check_poll("poll 7 both blocks one poll", *channel, {"alpha", "beta"});

    std::error_code ignored;
    fs::remove_all(base, ignored);

    if (failures > 0) {
        std::printf("claude_channel_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("claude_channel_test: PASS\n");
    return 0;
}
