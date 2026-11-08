#include "common/time.hpp"

#include <chrono>
#include <ctime>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace ccb {

namespace {

long long days_from_civil(long long year, unsigned month, unsigned day) {
    year -= month <= 2 ? 1 : 0;
    long long era = (year >= 0 ? year : year - 399) / 400;
    unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<long long>(day_of_era) - 719468;
}

bool is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month) {
    static constexpr int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && is_leap(year) ? 29 : lengths[month - 1];
}

class Cursor {
public:
    explicit Cursor(std::string_view text) : text_(text) {}

    bool digits(std::size_t count, int& value) {
        if (at_ + count > text_.size()) {
            return false;
        }
        value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            char c = text_[at_ + i];
            if (c < '0' || c > '9') {
                return false;
            }
            value = value * 10 + (c - '0');
        }
        at_ += count;
        return true;
    }

    bool take(char expected) {
        if (at_ < text_.size() && text_[at_] == expected) {
            ++at_;
            return true;
        }
        return false;
    }

    bool fraction(double& value) {
        double scale = 0.1;
        std::size_t start = at_;
        value = 0.0;
        while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
            value += (text_[at_] - '0') * scale;
            scale /= 10.0;
            ++at_;
        }
        return at_ > start;
    }

    bool done() const { return at_ == text_.size(); }
    char peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }

private:
    std::string_view text_;
    std::size_t at_ = 0;
};

std::optional<double> local_epoch(int year, int month, int day, int hour, int minute, int second) {
    std::tm parts{};
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    parts.tm_min = minute;
    parts.tm_sec = second;
    parts.tm_isdst = -1;
    std::time_t value = std::mktime(&parts);
    if (value == static_cast<std::time_t>(-1) && !(year == 1969 && month == 12 && day == 31)) {
        return std::nullopt;
    }
    return static_cast<double>(value);
}

}

double now_seconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(system_clock::now().time_since_epoch()).count();
}

std::optional<double> parse_iso8601(std::string_view text) {
    Cursor cursor(text);
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    double fraction = 0.0;
    if (!cursor.digits(4, year) || !cursor.take('-') || !cursor.digits(2, month) || !cursor.take('-') ||
        !cursor.digits(2, day)) {
        return std::nullopt;
    }
    if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, month)) {
        return std::nullopt;
    }
    if (!(cursor.take('T') || cursor.take(' '))) {
        return std::nullopt;
    }
    if (!cursor.digits(2, hour) || !cursor.take(':') || !cursor.digits(2, minute) || !cursor.take(':') ||
        !cursor.digits(2, second)) {
        return std::nullopt;
    }
    if (hour > 23 || minute > 59 || second > 59) {
        return std::nullopt;
    }
    if ((cursor.take('.') || cursor.take(',')) && !cursor.fraction(fraction)) {
        return std::nullopt;
    }
    if (cursor.done()) {
        auto local = local_epoch(year, month, day, hour, minute, second);
        if (!local) {
            return std::nullopt;
        }
        return *local + fraction;
    }
    int offset_seconds = 0;
    if (cursor.take('Z') || cursor.take('z')) {
        offset_seconds = 0;
    } else {
        char sign = cursor.peek();
        if (!(cursor.take('+') || cursor.take('-'))) {
            return std::nullopt;
        }
        int offset_hours = 0;
        int offset_minutes = 0;
        if (!cursor.digits(2, offset_hours)) {
            return std::nullopt;
        }
        if (cursor.take(':')) {
            if (!cursor.digits(2, offset_minutes)) {
                return std::nullopt;
            }
        } else if (!cursor.done() && !cursor.digits(2, offset_minutes)) {
            return std::nullopt;
        }
        if (offset_hours > 23 || offset_minutes > 59) {
            return std::nullopt;
        }
        offset_seconds = (offset_hours * 3600 + offset_minutes * 60) * (sign == '-' ? -1 : 1);
    }
    if (!cursor.done()) {
        return std::nullopt;
    }
    long long days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    long long seconds = days * 86400 + hour * 3600 + minute * 60 + second - offset_seconds;
    return static_cast<double>(seconds) + fraction;
}

std::optional<FileInfo> stat_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    std::error_code ec;
    auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(status)) {
        return std::nullopt;
    }
    auto written = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return std::nullopt;
    }
    FileInfo info;
    if (std::filesystem::is_regular_file(status)) {
        info.size = std::filesystem::file_size(path, ec);
        if (ec) {
            return std::nullopt;
        }
    }
    using namespace std::chrono;
    auto system_time = system_clock::now() + duration_cast<system_clock::duration>(
                                                 written - std::filesystem::file_time_type::clock::now());
    info.mtime = duration_cast<duration<double>>(system_time.time_since_epoch()).count();
    return info;
#else
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return std::nullopt;
    }
    FileInfo info;
    info.size = static_cast<std::uintmax_t>(st.st_size);
#if defined(__APPLE__)
    info.mtime = static_cast<double>(st.st_mtimespec.tv_sec) + static_cast<double>(st.st_mtimespec.tv_nsec) / 1e9;
#else
    info.mtime = static_cast<double>(st.st_mtim.tv_sec) + static_cast<double>(st.st_mtim.tv_nsec) / 1e9;
#endif
    return info;
#endif
}

}
