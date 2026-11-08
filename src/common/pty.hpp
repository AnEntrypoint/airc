#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ccb {

struct PtyOptions {
    std::vector<std::string> argv;
    std::vector<std::string> env;
    std::string cwd;
    int cols = 200;
    int rows = 50;
    std::function<void(std::string_view)> on_output;
};

class Pty {
public:
    static std::unique_ptr<Pty> spawn(PtyOptions options);

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;
    ~Pty();

    void write(std::string_view bytes);
    void resize(int cols, int rows);
    bool running();
    void terminate();

private:
    Pty() = default;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
