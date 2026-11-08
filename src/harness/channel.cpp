#include "harness/channel.hpp"

#include "common/text.hpp"
#include "common/time.hpp"

#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace ccb {

namespace {

std::string absolute_path(const std::string& cwd) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(cwd.empty() ? std::filesystem::path(".") : std::filesystem::path(cwd), ec);
    if (ec) {
        return cwd;
    }
    auto text = absolute.lexically_normal().string();
    while (text.size() > 1 && (text.back() == '/' || text.back() == '\\') &&
           !(text.size() == 3 && text[1] == ':')) {
        text.pop_back();
    }
    return text;
}

}

Channel::Channel(std::string cwd, double since, bool at_end, std::optional<std::string> session_id)
    : cwd_(absolute_path(cwd)),
      since_(since),
      at_end_(at_end),
      session_id_(std::move(session_id)),
      pane_([] { return std::string(); }) {}

void Channel::set_pane_source(PaneSource source) {
    pane_ = source ? std::move(source) : PaneSource([] { return std::string(); });
}

bool Channel::bind(const std::filesystem::path& path) {
    if (!path_ || *path_ != path) {
        path_ = path;
        offset_ = 0;
        if (at_end_) {
            if (auto info = stat_file(path)) {
                offset_ = info->size;
            }
        }
        pending_.clear();
    }
    return true;
}

std::vector<nlohmann::json> Channel::tail_records() {
    std::vector<nlohmann::json> records;
    if (!path_) {
        return records;
    }
    std::ifstream file(*path_, std::ios::binary);
    if (!file) {
        return records;
    }
    file.seekg(static_cast<std::streamoff>(offset_));
    if (!file) {
        return records;
    }
    std::string chunk((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (chunk.empty()) {
        return records;
    }
    offset_ += chunk.size();
    std::string buffer = std::move(pending_);
    buffer += chunk;
    std::size_t start = 0;
    for (;;) {
        std::size_t end = buffer.find('\n', start);
        if (end == std::string::npos) {
            break;
        }
        auto line = trim(std::string_view(buffer).substr(start, end - start));
        start = end + 1;
        if (line.empty()) {
            continue;
        }
        auto record = nlohmann::json::parse(line, nullptr, false);
        if (!record.is_discarded()) {
            records.push_back(std::move(record));
        }
    }
    pending_ = buffer.substr(start);
    return records;
}

bool Channel::before_start(const nlohmann::json& record) const {
    if (!record.is_object()) {
        return false;
    }
    auto found = record.find("timestamp");
    if (found == record.end() || !found->is_string()) {
        return false;
    }
    const auto& stamp = found->get_ref<const std::string&>();
    if (stamp.empty()) {
        return false;
    }
    auto value = parse_iso8601(stamp);
    if (!value) {
        return false;
    }
    return *value + 3.0 < since_;
}

std::optional<std::filesystem::path> Channel::newest(const std::vector<std::filesystem::path>& paths, double since) {
    std::optional<std::filesystem::path> best;
    double best_mtime = 0.0;
    for (const auto& path : paths) {
        auto info = stat_file(path);
        if (!info || info->size == 0 || info->mtime + 3 < since) {
            continue;
        }
        if (!best || info->mtime > best_mtime) {
            best = path;
            best_mtime = info->mtime;
        }
    }
    return best;
}

std::string Channel::delta(const std::string& key, const std::string& text, std::map<std::string, std::size_t>& store) {
    auto found = store.find(key);
    std::size_t previous = found == store.end() ? 0 : found->second;
    if (text.size() > previous) {
        store[key] = text.size();
        return text.substr(previous);
    }
    if (text.size() < previous) {
        store[key] = text.size();
    }
    return {};
}

KindSet Channel::wanted(const KindSet& kinds) const {
    KindSet result = kinds;
    result.insert(Kind::TurnEnd);
    return result;
}

}
