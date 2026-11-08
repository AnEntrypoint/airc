#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ccb {

bool starts_with(std::string_view text, std::string_view prefix);
bool contains(std::string_view text, std::string_view needle);
std::string_view trim(std::string_view text);
std::vector<std::string> split_lines(std::string_view text);
std::string replace_all(std::string text, std::string_view from, std::string_view to);
std::string shell_quote(std::string_view text);
std::string slug_path(std::string_view path);
bool is_uuid(std::string_view text);
bool is_session_name(std::string_view text);

}
