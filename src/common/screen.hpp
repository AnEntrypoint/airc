#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ccb {

class Screen {
public:
    using Reply = std::function<void(std::string_view)>;

    Screen(int cols, int rows, Reply reply);
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;
    ~Screen();

    void feed(std::string_view bytes);
    void resize(int cols, int rows);
    std::string text() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
