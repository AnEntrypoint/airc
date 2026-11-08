#pragma once

#include "harness/event.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ccb {

class Channel {
public:
    using PaneSource = std::function<std::string()>;

    Channel(std::string cwd, double since, bool at_end, std::optional<std::string> session_id);
    virtual ~Channel() = default;

    virtual std::vector<Event> poll(const KindSet& kinds) = 0;
    void set_pane_source(PaneSource source);

protected:
    bool bind(const std::filesystem::path& path);
    std::vector<nlohmann::json> tail_records();
    bool before_start(const nlohmann::json& record) const;
    static std::optional<std::filesystem::path> newest(const std::vector<std::filesystem::path>& paths,
                                                       double since);
    static std::string delta(const std::string& key, const std::string& text,
                             std::map<std::string, std::size_t>& store);
    KindSet wanted(const KindSet& kinds) const;

    std::string cwd_;
    double since_;
    bool at_end_;
    std::optional<std::string> session_id_;
    PaneSource pane_;
    std::optional<std::filesystem::path> path_;
    std::uintmax_t offset_ = 0;
    std::string pending_;
    std::map<std::string, std::size_t> text_len_;
    std::set<std::string> done_ids_;
};

}
