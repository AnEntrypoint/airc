#include "tools/tools.hpp"

#include "common/pty.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ccb {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int marker_tail_lines = 60;
constexpr std::size_t note_key_preview = 60;
constexpr std::size_t marker_sample_size = 4;
constexpr double tick_seconds = 0.05;
constexpr double paste_settle_seconds = 0.15;
constexpr double interrupt_settle_seconds = 0.3;
constexpr double exit_settle_seconds = 0.6;
constexpr double drain_settle_seconds = 0.2;

struct SequenceItem {
    std::string keys;
    bool submit = true;
};

void sleep_seconds(double seconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

std::string python_repr(const std::string& text) {
    const bool has_single = text.find('\'') != std::string::npos;
    const bool has_double = text.find('"') != std::string::npos;
    const char quote = has_single && !has_double ? '"' : '\'';
    std::string out(1, quote);
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (raw == quote || raw == '\\') {
            out.push_back('\\');
            out.push_back(raw);
        } else if (raw == '\n') {
            out += "\\n";
        } else if (raw == '\r') {
            out += "\\r";
        } else if (raw == '\t') {
            out += "\\t";
        } else if (c < 0x20 || c == 0x7F) {
            static constexpr char digits[] = "0123456789abcdef";
            out += "\\x";
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0x0F]);
        } else {
            out.push_back(raw);
        }
    }
    out.push_back(quote);
    return out;
}

std::string python_repr_list(const std::vector<std::string>& items, std::size_t limit) {
    std::string out = "[";
    const std::size_t count = std::min(items.size(), limit);
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) out += ", ";
        out += python_repr(items[i]);
    }
    return out + "]";
}

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

bool is_banned_arg(const std::string& arg) {
    static const std::set<std::string> banned = {"--bare", "--api-key", "--settings", "-p",
                                                 "--print", "--safe-mode", "--restricted"};
    return banned.count(arg) != 0;
}

std::vector<std::string> split_marker_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

std::string marker_report(const std::string& raw, const Harness& harness) {
    std::vector<std::string> lines = split_marker_lines(strip_ansi(raw));
    if (lines.size() > static_cast<std::size_t>(marker_tail_lines)) {
        lines.erase(lines.begin(), lines.end() - marker_tail_lines);
    }
    std::vector<std::string> ready;
    std::vector<std::string> busy;
    for (const std::string& line : lines) {
        if (harness.pane_ready(line)) ready.push_back(line);
        if (harness.pane_busy(line)) busy.push_back(line);
    }
    return "harness=" + harness.name + " ready=" + (ready.empty() ? "False" : "True") +
           " busy=" + (busy.empty() ? "False" : "True") +
           "\nready_lines=" + python_repr_list(ready, marker_sample_size) +
           "\nbusy_lines=" + python_repr_list(busy, marker_sample_size);
}

bool parse_sequence(const nlohmann::json& document, std::vector<SequenceItem>& out) {
    if (!document.is_array()) return false;
    for (const nlohmann::json& item : document) {
        SequenceItem entry;
        if (item.is_string()) {
            entry.keys = item.get<std::string>();
        } else if (item.is_object()) {
            const auto keys = item.find("keys");
            if (keys != item.end()) {
                if (!keys->is_string()) return false;
                entry.keys = keys->get<std::string>();
            }
            const auto submit = item.find("submit");
            if (submit != item.end()) {
                if (!submit->is_boolean()) return false;
                entry.submit = submit->get<bool>();
            }
        } else {
            return false;
        }
        out.push_back(std::move(entry));
    }
    return true;
}

bool load_sequence(const CaptureOptions& options, std::vector<SequenceItem>& out, std::string& error) {
    try {
        nlohmann::json document = nlohmann::json::array();
        if (!options.seq_json.empty()) document = nlohmann::json::parse(options.seq_json);
        if (!options.seq_file.empty()) {
            std::ifstream in(options.seq_file, std::ios::binary);
            if (!in) {
                error = "cannot read " + options.seq_file;
                return false;
            }
            document = nlohmann::json::parse(in);
        }
        if (!parse_sequence(document, out)) {
            error = "sequence must be an array of strings or {\"keys\", \"submit\"} objects";
            return false;
        }
        return true;
    } catch (const nlohmann::json::exception& failure) {
        error = std::string("invalid sequence: ") + failure.what();
        return false;
    }
}

class Recorder {
public:
    Recorder(const std::string& harness_name, Clock::time_point started)
        : harness_name_(harness_name), started_(started) {}

    double elapsed() const { return std::chrono::duration<double>(Clock::now() - started_).count(); }

    void note(const std::string& message) const {
        char stamp[32];
        std::snprintf(stamp, sizeof stamp, "%6.1f", elapsed());
        const std::string line = "[tty " + harness_name_ + " t=" + stamp + "] " + message + "\n";
        std::fputs(line.c_str(), stderr);
        std::fflush(stderr);
    }

private:
    std::string harness_name_;
    Clock::time_point started_;
};

class OutputSink {
public:
    OutputSink(std::ofstream& raw, const Recorder& recorder) : raw_(raw), recorder_(recorder) {
        last_output_ = recorder.elapsed();
    }

    void append(std::string_view bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        raw_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        raw_.flush();
        captured_.append(bytes);
        last_output_ = recorder_.elapsed();
    }

    void mark_output_now() {
        std::lock_guard<std::mutex> lock(mutex_);
        last_output_ = recorder_.elapsed();
    }

    double last_output() {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_output_;
    }

    std::string captured() {
        std::lock_guard<std::mutex> lock(mutex_);
        return captured_;
    }

private:
    std::ofstream& raw_;
    const Recorder& recorder_;
    std::mutex mutex_;
    std::string captured_;
    double last_output_ = 0.0;
};

struct SendRecord {
    std::size_t index;
    double at;
    std::string keys;
};

std::string timing_document(const std::vector<SendRecord>& records) {
    nlohmann::ordered_json document = nlohmann::ordered_json::array();
    for (const SendRecord& record : records) {
        nlohmann::ordered_json entry;
        entry["i"] = record.index;
        entry["t"] = std::round(record.at * 100.0) / 100.0;
        entry["keys"] = record.keys;
        document.push_back(entry);
    }
    return document.dump(2, ' ', true);
}

struct CaptureTimings {
    double startup_delay;
    double gap;
    double min_turn;
    double final_idle;
    double max_seconds;
};

struct Teardown {
    Pty& pty;

    ~Teardown() {
        try {
            pty.write(paste_close);
            pty.write("\x03");
            sleep_seconds(interrupt_settle_seconds);
            pty.write("/exit\r");
            sleep_seconds(exit_settle_seconds);
        } catch (...) {
        }
        try {
            pty.terminate();
        } catch (...) {
        }
    }
};

void send_item(Pty& pty, const SequenceItem& item) {
    pty.write(paste_open);
    pty.write(item.keys);
    pty.write(paste_close);
    sleep_seconds(paste_settle_seconds);
    if (item.submit) pty.write("\r");
}

std::vector<SendRecord> drive(Pty& pty, const std::vector<SequenceItem>& sequence, const CaptureTimings& timings,
                              const Recorder& recorder, OutputSink& sink) {
    std::vector<SendRecord> records;
    std::size_t index = 0;
    bool has_sent = false;
    double sent_at = 0.0;

    while (true) {
        const double now = recorder.elapsed();
        if (now > timings.max_seconds) {
            recorder.note("max-seconds hit");
            break;
        }
        const double last_output = sink.last_output();
        if (index < sequence.size() && now >= timings.startup_delay) {
            const bool turn_ready =
                !has_sent || ((now - last_output >= timings.gap) && (now - sent_at >= timings.min_turn));
            if (turn_ready) {
                const SequenceItem& item = sequence[index];
                send_item(pty, item);
                sent_at = recorder.elapsed();
                sink.mark_output_now();
                has_sent = true;
                recorder.note("send[" + std::to_string(index) + "] " +
                              python_repr(first_codepoints(item.keys, note_key_preview)));
                records.push_back({index, sent_at, item.keys});
                ++index;
                continue;
            }
        }
        if (index >= sequence.size() && !sequence.empty() && has_sent) {
            if (now - last_output >= timings.final_idle && now - sent_at >= timings.min_turn) {
                char idle[32];
                std::snprintf(idle, sizeof idle, "%.1f", now - last_output);
                recorder.note(std::string("final idle ") + idle + "s -> quit");
                break;
            }
        }
        if (!pty.running()) {
            sleep_seconds(drain_settle_seconds);
            break;
        }
        sleep_seconds(tick_seconds);
    }
    return records;
}

}

int run_capture(const CaptureOptions& options, const Registry& registry) {
    std::vector<std::string> extra;
    for (const std::string& arg : options.extra_args) {
        if (arg != "--") extra.push_back(arg);
    }
    std::vector<std::string> banned;
    for (const std::string& arg : extra) {
        if (is_banned_arg(arg)) banned.push_back(arg);
    }
    std::sort(banned.begin(), banned.end());
    banned.erase(std::unique(banned.begin(), banned.end()), banned.end());
    if (!banned.empty()) {
        std::fprintf(stderr, "refusing %s: would not be an unmodified interactive harness\n",
                     python_repr_list(banned, banned.size()).c_str());
        return 2;
    }

    const std::string harness_name = options.harness.empty() ? registry.default_name() : options.harness;
    const std::shared_ptr<const Harness> harness = registry.get(harness_name);
    if (!harness) {
        std::fprintf(stderr, "unknown harness %s\n", harness_name.c_str());
        return 2;
    }

    std::vector<SequenceItem> sequence;
    std::string error;
    if (!load_sequence(options, sequence, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }

    const std::filesystem::path out_path = std::filesystem::absolute(options.out);
    std::error_code directory_error;
    std::filesystem::create_directories(out_path.parent_path(), directory_error);
    std::ofstream raw(out_path, std::ios::binary);
    if (!raw) {
        std::fprintf(stderr, "cannot write %s\n", out_path.string().c_str());
        return 2;
    }

    const Clock::time_point started = Clock::now();
    const Recorder recorder(harness->name, started);
    OutputSink sink(raw, recorder);

    PtyOptions pty_options;
    pty_options.argv = harness->argv;
    pty_options.argv.insert(pty_options.argv.end(), extra.begin(), extra.end());
    if (!options.verbatim_env) pty_options.env = harness->child_environment();
    pty_options.cwd = std::filesystem::current_path().string();
    pty_options.cols = options.cols;
    pty_options.rows = options.rows;
    pty_options.on_output = [&sink](std::string_view bytes) { sink.append(bytes); };

    const CaptureTimings timings{options.startup_delay, options.gap, options.min_turn, options.final_idle,
                                 options.max_seconds};
    std::vector<SendRecord> records;
    try {
        std::unique_ptr<Pty> pty = Pty::spawn(std::move(pty_options));
        Teardown teardown{*pty};
        records = drive(*pty, sequence, timings, recorder, sink);
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "%s\n", failure.what());
        return 2;
    }
    const std::string captured = sink.captured();
    raw.close();

    recorder.note("captured " + std::to_string(captured.size()) + " bytes -> " + out_path.string());
    std::ofstream timing(out_path.string() + ".timing.json", std::ios::binary);
    timing << timing_document(records);
    timing.close();
    if (options.dump_markers) {
        const std::string report = marker_report(captured, *harness) + "\n";
        std::fwrite(report.data(), 1, report.size(), stdout);
    }
    return 0;
}

}
