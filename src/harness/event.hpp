#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace ccb {

enum class Kind { Text, Thinking, Tool, Result, You, TurnEnd };

using KindSet = std::set<Kind>;

struct Event {
    Kind kind = Kind::Text;
    std::string text;
    std::optional<nlohmann::json> input;
};

std::string_view kind_name(Kind kind);
std::optional<Kind> kind_from_name(std::string_view name);
KindSet kinds_from_csv(std::string_view csv);

}
