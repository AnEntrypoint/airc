#include "harness/event.hpp"

#include "common/text.hpp"

#include <array>
#include <utility>

namespace ccb {

namespace {

constexpr std::array<std::pair<Kind, std::string_view>, 6> kind_names{{
    {Kind::Text, "text"},
    {Kind::Thinking, "thinking"},
    {Kind::Tool, "tool"},
    {Kind::Result, "result"},
    {Kind::You, "you"},
    {Kind::TurnEnd, "turn_end"},
}};

}

std::string_view kind_name(Kind kind) {
    for (const auto& [candidate, name] : kind_names) {
        if (candidate == kind) {
            return name;
        }
    }
    return "text";
}

std::optional<Kind> kind_from_name(std::string_view name) {
    for (const auto& [kind, candidate] : kind_names) {
        if (candidate == name) {
            return kind;
        }
    }
    return std::nullopt;
}

KindSet kinds_from_csv(std::string_view csv) {
    KindSet kinds;
    std::size_t start = 0;
    for (;;) {
        std::size_t end = csv.find(',', start);
        auto piece = trim(csv.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (!piece.empty()) {
            if (auto kind = kind_from_name(piece)) {
                kinds.insert(*kind);
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return kinds;
}

}
