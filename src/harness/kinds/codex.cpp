#include "common/paths.hpp"
#include "common/text.hpp"
#include "common/time.hpp"
#include "harness/transcript.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <sqlite3.h>

namespace ccb {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::size_t result_limit = 4000;

std::string safe_dump(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string truncate_code_points(const std::string& text, std::size_t limit) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
        if (count == limit) return text.substr(0, i);
        ++count;
    }
    return text;
}

std::string string_member(const json& object, const char* key, const std::string& fallback = "") {
    if (!object.is_object()) return fallback;
    auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

json member_or_null(const json& object, const char* key) {
    if (!object.is_object()) return json(nullptr);
    auto it = object.find(key);
    return it == object.end() ? json(nullptr) : *it;
}

std::string python_str(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null()) return "None";
    return safe_dump(value);
}

Event make_event(Kind kind, std::string text) {
    Event event;
    event.kind = kind;
    event.text = std::move(text);
    return event;
}

std::string absolute_cwd(const std::string& cwd) {
    std::error_code ec;
    fs::path absolute = fs::absolute(fs::path(cwd), ec).lexically_normal();
    if (!absolute.has_filename()) absolute = absolute.parent_path();
    return absolute.string();
}

std::vector<fs::path> children(const fs::path& dir) {
    std::vector<fs::path> found;
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) return found;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    return found;
}

bool dir_exists(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

bool is_file(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

SessionEntry entry_for(const std::string& id, const fs::path& path, const FileInfo& info) {
    SessionEntry entry;
    entry.session_id = id;
    entry.path = path.string();
    entry.mtime = info.mtime;
    entry.size = info.size;
    return entry;
}

std::string without_extension(const fs::path& path, const std::string& extension) {
    std::string name = path.filename().string();
    return name.substr(0, name.size() - extension.size());
}

std::vector<fs::path> rollout_paths(const fs::path& root) {
    std::vector<fs::path> found;
    for (const auto& year : children(root / "sessions")) {
        if (!dir_exists(year)) continue;
        for (const auto& month : children(year)) {
            if (!dir_exists(month)) continue;
            for (const auto& day : children(month)) {
                if (!dir_exists(day)) continue;
                for (const auto& file : children(day)) {
                    std::string name = file.filename().string();
                    if (starts_with(name, "rollout-") && file.extension() == ".jsonl" && is_file(file)) {
                        found.push_back(file);
                    }
                }
            }
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

json session_meta(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line)) return json::object();
    json record = json::parse(line, nullptr, false);
    if (record.is_discarded() || !record.is_object()) return json::object();
    json payload = member_or_null(record, "payload");
    return payload.is_object() ? payload : json::object();
}

std::optional<std::string> uuid_in(const std::string& name) {
    for (std::size_t i = 0; i + 36 <= name.size(); ++i) {
        if (is_uuid(std::string_view(name).substr(i, 36))) return name.substr(i, 36);
    }
    return std::nullopt;
}

std::string session_id_of(const fs::path& path) {
    json meta = session_meta(path);
    json id = member_or_null(meta, "id");
    if (id.is_string() && !id.get<std::string>().empty()) return id.get<std::string>();
    if (!id.is_null() && !id.is_string() && id != 0 && id != false) return safe_dump(id);
    auto embedded = uuid_in(path.filename().string());
    if (embedded) return *embedded;
    return without_extension(path, ".jsonl");
}

long long database_version(const fs::path& path) {
    std::string name = path.filename().string();
    auto at = name.find("state_");
    if (at == std::string::npos) return -1;
    std::size_t begin = at + 6;
    std::size_t end = begin;
    while (end < name.size() && name[end] >= '0' && name[end] <= '9') ++end;
    if (end == begin) return -1;
    std::string digits = name.substr(begin, std::min<std::size_t>(end - begin, 18));
    return std::stoll(digits);
}

std::vector<fs::path> thread_databases(const fs::path& root) {
    std::vector<fs::path> found;
    for (const auto& path : children(root)) {
        std::string name = path.filename().string();
        if (starts_with(name, "state_") && path.extension() == ".sqlite") found.push_back(path);
    }
    std::stable_sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) {
        return database_version(a) > database_version(b);
    });
    return found;
}

struct DatabaseCloser {
    void operator()(sqlite3* db) const { sqlite3_close(db); }
};

struct StatementCloser {
    void operator()(sqlite3_stmt* statement) const { sqlite3_finalize(statement); }
};

using Rows = std::vector<std::vector<std::string>>;

std::optional<Rows> query_rows(const fs::path& database, const char* sql, const std::string& argument,
                               int columns) {
    sqlite3* raw = nullptr;
    int opened = sqlite3_open_v2(database.string().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
    std::unique_ptr<sqlite3, DatabaseCloser> db(raw);
    if (opened != SQLITE_OK) return std::nullopt;
    sqlite3_stmt* raw_statement = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql, -1, &raw_statement, nullptr) != SQLITE_OK) return std::nullopt;
    std::unique_ptr<sqlite3_stmt, StatementCloser> statement(raw_statement);
    if (sqlite3_bind_text(statement.get(), 1, argument.c_str(), static_cast<int>(argument.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        return std::nullopt;
    }
    Rows rows;
    int code = 0;
    while ((code = sqlite3_step(statement.get())) == SQLITE_ROW) {
        std::vector<std::string> row;
        for (int column = 0; column < columns; ++column) {
            const unsigned char* text = sqlite3_column_text(statement.get(), column);
            int length = sqlite3_column_bytes(statement.get(), column);
            row.emplace_back(text ? std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(length))
                                  : std::string());
        }
        rows.push_back(std::move(row));
    }
    if (code != SQLITE_DONE) return std::nullopt;
    return rows;
}

std::uintmax_t size_of(const std::string& rollout) {
    if (rollout.empty()) return 0;
    auto info = stat_file(fs::path(rollout));
    return info ? info->size : 0;
}

double milliseconds_to_seconds(const std::string& text) {
    if (text.empty()) return 0.0;
    char* end = nullptr;
    double value = std::strtod(text.c_str(), &end);
    return value / 1000.0;
}

std::vector<SessionEntry> database_sessions(const fs::path& root, const std::string& cwd) {
    static const char* sql =
        "select id, rollout_path, coalesce(updated_at_ms, created_at_ms, 0), "
        "title, name, first_user_message, preview from threads "
        "where cwd = ? and coalesce(archived, 0) = 0 "
        "and coalesce(has_user_event, 1) = 1";
    std::vector<SessionEntry> found;
    std::map<std::string, std::size_t> position;
    for (const auto& database : thread_databases(root)) {
        auto rows = query_rows(database, sql, absolute_cwd(cwd), 7);
        if (!rows) continue;
        for (const auto& row : *rows) {
            SessionEntry entry;
            entry.session_id = row[0];
            entry.path = row[1];
            entry.mtime = milliseconds_to_seconds(row[2]);
            entry.size = size_of(row[1]);
            for (std::size_t i = 3; i < 7; ++i) {
                if (!row[i].empty()) {
                    entry.title = row[i];
                    break;
                }
            }
            auto known = position.find(entry.session_id);
            if (known == position.end()) {
                position[entry.session_id] = found.size();
                found.push_back(std::move(entry));
            } else {
                found[known->second] = std::move(entry);
            }
        }
    }
    return found;
}

std::optional<SessionEntry> find_in_databases(const fs::path& root, const std::string& session_id) {
    static const char* sql =
        "select id, rollout_path, coalesce(updated_at_ms, created_at_ms, 0), cwd "
        "from threads where id = ?";
    for (const auto& database : thread_databases(root)) {
        auto rows = query_rows(database, sql, session_id, 4);
        if (!rows || rows->empty()) continue;
        const auto& row = rows->front();
        SessionEntry entry;
        entry.session_id = row[0];
        entry.path = row[1];
        entry.mtime = milliseconds_to_seconds(row[2]);
        entry.size = size_of(row[1]);
        entry.cwd = row[3];
        return entry;
    }
    return std::nullopt;
}

std::vector<SessionEntry> rollout_sessions(const fs::path& root, const std::string& cwd) {
    std::vector<SessionEntry> rows;
    std::string wanted_cwd = absolute_cwd(cwd);
    for (const auto& path : rollout_paths(root)) {
        if (string_member(session_meta(path), "cwd") != wanted_cwd) continue;
        auto info = stat_file(path);
        if (!info) continue;
        rows.push_back(entry_for(session_id_of(path), path, *info));
    }
    return rows;
}

std::string joined_text(const json& blocks) {
    std::string text;
    if (!blocks.is_array()) return text;
    for (const auto& block : blocks) text += string_member(block, "text");
    return text;
}

class CodexChannel : public Channel {
public:
    CodexChannel(fs::path root, std::string cwd, double since, bool at_end,
                 std::optional<std::string> session_id)
        : Channel(std::move(cwd), since, at_end, std::move(session_id)), root_(std::move(root)) {}

    std::vector<Event> poll(const KindSet& kinds) override {
        std::vector<Event> out;
        if (!attach()) return out;
        KindSet want = wanted(kinds);
        want.insert(Kind::TurnEnd);
        for (const auto& record : tail_records()) {
            if (before_start(record) || !record.is_object()) continue;
            std::string type = string_member(record, "type");
            json payload = member_or_null(record, "payload");
            if (!payload.is_object()) payload = json::object();
            std::string payload_type = string_member(payload, "type");
            if (type == "event_msg") {
                if (payload_type == "task_complete") out.push_back(make_event(Kind::TurnEnd, ""));
                continue;
            }
            if (type != "response_item") continue;
            emit_payload(record, payload, payload_type, want, out);
        }
        return out;
    }

private:
    void emit_payload(const json& record, const json& payload, const std::string& payload_type,
                      const KindSet& want, std::vector<Event>& out) {
        if (payload_type == "message") {
            std::string role = string_member(payload, "role");
            std::string text = joined_text(member_or_null(payload, "content"));
            if (role == "assistant") {
                if (!want.count(Kind::Text) || text.empty()) return;
                std::string key = string_member(payload, "id");
                if (key.empty()) key = python_str(member_or_null(record, "ordinal"));
                std::string piece = delta(key, text, text_len_);
                if (!piece.empty()) out.push_back(make_event(Kind::Text, std::move(piece)));
            } else if (role == "user" && !trim(text).empty() && want.count(Kind::You)) {
                out.push_back(make_event(Kind::You, std::move(text)));
            }
        } else if (payload_type == "reasoning" && want.count(Kind::Thinking)) {
            std::string text = joined_text(member_or_null(payload, "summary"));
            if (!text.empty()) out.push_back(make_event(Kind::Thinking, std::move(text)));
        } else if (payload_type == "function_call" && want.count(Kind::Tool)) {
            Event event = make_event(Kind::Tool, string_member(payload, "name"));
            event.input = member_or_null(payload, "arguments");
            out.push_back(std::move(event));
        } else if (payload_type == "function_call_output" && want.count(Kind::Result)) {
            std::string text = python_str(member_or_null(payload, "output"));
            out.push_back(make_event(Kind::Result, truncate_code_points(text, result_limit)));
        }
    }

    bool attach() {
        if (session_id_ && !session_id_->empty()) {
            for (const auto& path : rollout_paths(root_)) {
                if (session_id_of(path) == *session_id_) return bind(path);
            }
            if (is_uuid(*session_id_)) return false;
        }
        std::vector<fs::path> accepted;
        for (const auto& path : rollout_paths(root_)) {
            if (newest({path}, since_)) accepted.push_back(path);
        }
        for (auto it = accepted.rbegin(); it != accepted.rend(); ++it) {
            if (string_member(session_meta(*it), "cwd") == cwd_) return bind(*it);
        }
        return false;
    }

    fs::path root_;
};

class CodexTranscript : public Transcript {
public:
    explicit CodexTranscript(fs::path root) : root_(std::move(root)) {}

    std::unique_ptr<Channel> channel(const std::string& cwd, double since, bool at_end,
                                     std::optional<std::string> session_id) const override {
        return std::make_unique<CodexChannel>(root_, cwd, since, at_end, std::move(session_id));
    }

    std::vector<SessionEntry> sessions(const std::string& cwd) const override {
        std::vector<SessionEntry> rows = database_sessions(root_, cwd);
        std::map<std::string, bool> seen;
        for (const auto& row : rows) seen[row.session_id] = true;
        for (auto& row : rollout_sessions(root_, cwd)) {
            if (seen.count(row.session_id)) continue;
            seen[row.session_id] = true;
            rows.push_back(std::move(row));
        }
        std::stable_sort(rows.begin(), rows.end(),
                         [](const SessionEntry& a, const SessionEntry& b) { return a.mtime < b.mtime; });
        return rows;
    }

    std::optional<SessionEntry> find(const std::string& session_id) const override {
        auto from_database = find_in_databases(root_, session_id);
        if (from_database) return from_database;
        for (const auto& path : rollout_paths(root_)) {
            if (session_id_of(path) != session_id) continue;
            auto info = stat_file(path);
            if (!info) continue;
            return entry_for(session_id, path, *info);
        }
        return std::nullopt;
    }

    void check_selector(const std::string& selector) const override {
        if (is_uuid(selector) || is_session_name(selector)) return;
        throw std::invalid_argument("bad session selector '" + selector + "'");
    }

private:
    fs::path root_;
};

}

std::unique_ptr<Transcript> make_codex_transcript(const TranscriptSpec& spec) {
    return std::make_unique<CodexTranscript>(resolve_root(spec));
}

}
