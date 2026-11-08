#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace ccb {

struct FileInfo {
    double mtime = 0.0;
    std::uintmax_t size = 0;
};

double now_seconds();
std::optional<double> parse_iso8601(std::string_view text);
std::optional<FileInfo> stat_file(const std::filesystem::path& path);

}
