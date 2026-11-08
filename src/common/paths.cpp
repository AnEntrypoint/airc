#include "common/paths.hpp"

#include <cstdlib>
#include <mutex>
#include <optional>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <cstdint>
#endif

namespace ccb {

namespace {

std::mutex override_mutex;
std::optional<std::filesystem::path> template_dir_override;

std::filesystem::path executable_path() {
    std::error_code ec;
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    auto resolved = std::filesystem::weakly_canonical(buffer, ec);
    return ec ? std::filesystem::path(buffer) : resolved;
#else
    auto resolved = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : resolved;
#endif
}

std::filesystem::path normalized(const std::filesystem::path& path) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    auto canonical = std::filesystem::weakly_canonical(absolute, ec);
    return ec ? absolute.lexically_normal() : canonical;
}

std::filesystem::path resource_root() {
    auto home = env_or_empty("CCBRIDGE_HOME");
    if (!home.empty()) {
        return normalized(home);
    }
    auto exe = executable_path();
    if (!exe.empty()) {
        auto exe_dir = exe.parent_path();
        for (const auto& candidate : {exe_dir / ".." / "share" / "ccbridge", exe_dir / "..", exe_dir / ".." / ".."}) {
            std::error_code ec;
            if (std::filesystem::is_directory(candidate / "templates", ec)) {
                return normalized(candidate);
            }
        }
    }
    std::error_code ec;
    auto current = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path(".") : current;
}

}

std::string env_or_empty(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

std::filesystem::path home_dir() {
#if defined(_WIN32)
    return std::filesystem::path(env_or_empty("USERPROFILE"));
#else
    return std::filesystem::path(env_or_empty("HOME"));
#endif
}

void add_template_dir_override(std::filesystem::path dir) {
    std::lock_guard lock(override_mutex);
    template_dir_override = std::move(dir);
}

std::vector<std::filesystem::path> template_search_path() {
    std::vector<std::filesystem::path> candidates;
    {
        std::lock_guard lock(override_mutex);
        if (template_dir_override && !template_dir_override->empty()) {
            candidates.push_back(*template_dir_override);
        }
    }
    auto env_dir = env_or_empty("CCBRIDGE_TEMPLATES");
    if (!env_dir.empty()) {
        candidates.emplace_back(env_dir);
    }
    candidates.push_back(resource_root() / "templates");
    candidates.emplace_back(std::filesystem::path(".") / "templates");
    std::vector<std::filesystem::path> unique;
    for (const auto& candidate : candidates) {
        auto key = normalized(candidate);
        bool seen = false;
        for (const auto& existing : unique) {
            if (existing == key) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            unique.push_back(key);
        }
    }
    return unique;
}

std::filesystem::path web_root() {
    return resource_root() / "web";
}

}
