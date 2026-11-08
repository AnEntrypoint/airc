#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ccb {

std::string env_or_empty(const char* name);
std::filesystem::path home_dir();
void add_template_dir_override(std::filesystem::path dir);
std::vector<std::filesystem::path> template_search_path();
std::filesystem::path web_root();

}
