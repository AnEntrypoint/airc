#include "common/pty.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#else
#include <libutil.h>
#endif
#endif

namespace ccb {

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

std::wstring quote_argument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(ch);
            backslashes = 0;
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring search_program(const std::wstring& name) {
    for (const wchar_t* extension : {L".exe", L".cmd", L".bat", static_cast<const wchar_t*>(nullptr)}) {
        DWORD needed = SearchPathW(nullptr, name.c_str(), extension, 0, nullptr, nullptr);
        if (!needed) continue;
        std::wstring found(needed, L'\0');
        DWORD written = SearchPathW(nullptr, name.c_str(), extension, needed, found.data(), nullptr);
        if (written && written < needed) {
            found.resize(written);
            return found;
        }
    }
    return name;
}

bool ends_with_insensitive(const std::wstring& text, const std::wstring& suffix) {
    if (text.size() < suffix.size()) return false;
    return CompareStringOrdinal(text.c_str() + text.size() - suffix.size(), static_cast<int>(suffix.size()),
                                suffix.c_str(), static_cast<int>(suffix.size()), TRUE) == CSTR_EQUAL;
}

std::wstring build_command_line(const std::vector<std::string>& argv, std::wstring& application) {
    std::wstring program = search_program(widen(argv[0]));
    std::wstring line;
    if (ends_with_insensitive(program, L".cmd") || ends_with_insensitive(program, L".bat")) {
        application = search_program(L"cmd.exe");
        line = L"cmd.exe /d /c " + quote_argument(program);
    } else {
        application = program;
        line = quote_argument(program);
    }
    for (size_t index = 1; index < argv.size(); ++index) line += L" " + quote_argument(widen(argv[index]));
    return line;
}

std::wstring build_environment(const std::vector<std::string>& env) {
    std::wstring block;
    for (const auto& entry : env) {
        block += widen(entry);
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what + ": error " + std::to_string(GetLastError()));
}

}

class Pty::Impl {
public:
    explicit Impl(PtyOptions options) : on_output_(std::move(options.on_output)) {
        if (options.argv.empty()) throw std::runtime_error("cannot start: empty argv");
        HANDLE input_read = nullptr;
        HANDLE output_write = nullptr;
        if (!CreatePipe(&input_read, &input_write_, nullptr, 0)) fail("cannot start " + options.argv[0]);
        if (!CreatePipe(&output_read_, &output_write, nullptr, 0)) {
            CloseHandle(input_read);
            close_handles();
            fail("cannot start " + options.argv[0]);
        }
        COORD size{static_cast<SHORT>(options.cols), static_cast<SHORT>(options.rows)};
        HRESULT created = CreatePseudoConsole(size, input_read, output_write, 0, &console_);
        CloseHandle(input_read);
        CloseHandle(output_write);
        if (FAILED(created)) {
            close_handles();
            throw std::runtime_error("cannot start " + options.argv[0] + ": CreatePseudoConsole " + std::to_string(created));
        }

        SIZE_T attribute_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
        std::vector<unsigned char> attribute_storage(attribute_size);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size) ||
            !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console_,
                                       sizeof(console_), nullptr, nullptr)) {
            DWORD error = GetLastError();
            release_console();
            close_handles();
            throw std::runtime_error("cannot start " + options.argv[0] + ": error " + std::to_string(error));
        }

        std::wstring application;
        std::wstring command_line = build_command_line(options.argv, application);
        std::wstring environment = options.env.empty() ? std::wstring() : build_environment(options.env);
        std::wstring directory = widen(options.cwd);
        PROCESS_INFORMATION process{};
        BOOL started = CreateProcessW(application.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                                      EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                                      environment.empty() ? nullptr : environment.data(),
                                      directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &process);
        DWORD error = GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (!started) {
            release_console();
            close_handles();
            throw std::runtime_error("cannot start " + options.argv[0] + ": error " + std::to_string(error));
        }
        CloseHandle(process.hThread);
        process_ = process.hProcess;
        reader_ = std::thread([this] { read_loop(); });
    }

    ~Impl() { terminate(); }

    void write(std::string_view bytes) {
        std::lock_guard lock(write_mutex_);
        if (!input_write_) return;
        while (!bytes.empty()) {
            DWORD written = 0;
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size(), 1u << 20));
            if (!WriteFile(input_write_, bytes.data(), chunk, &written, nullptr) || written == 0) return;
            bytes.remove_prefix(written);
        }
    }

    void resize(int cols, int rows) {
        if (console_) ResizePseudoConsole(console_, COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)});
    }

    bool running() { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }

    void terminate() {
        std::lock_guard lock(terminate_mutex_);
        if (terminated_) return;
        terminated_ = true;
        if (running()) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 1000);
        }
        stop_ = true;
        release_console();
        if (reader_.joinable()) reader_.join();
        std::lock_guard write_lock(write_mutex_);
        close_handles();
    }

private:
    void read_loop() {
        char buffer[65536];
        for (;;) {
            DWORD received = 0;
            if (!ReadFile(output_read_, buffer, sizeof(buffer), &received, nullptr)) break;
            if (received == 0) continue;
            if (stop_) break;
            if (on_output_) on_output_(std::string_view(buffer, received));
        }
    }

    void release_console() {
        if (console_) {
            ClosePseudoConsole(console_);
            console_ = nullptr;
        }
    }

    void close_handles() {
        for (HANDLE* handle : {&input_write_, &output_read_, &process_}) {
            if (*handle) {
                CloseHandle(*handle);
                *handle = nullptr;
            }
        }
    }

    std::function<void(std::string_view)> on_output_;
    HANDLE input_write_ = nullptr;
    HANDLE output_read_ = nullptr;
    HANDLE process_ = nullptr;
    HPCON console_ = nullptr;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::mutex write_mutex_;
    std::mutex terminate_mutex_;
    bool terminated_ = false;
};

#else

namespace {

void write_errno_and_exit(int report_fd) {
    int error = errno;
    ssize_t ignored = ::write(report_fd, &error, sizeof(error));
    (void)ignored;
    _exit(127);
}

}

class Pty::Impl {
public:
    explicit Impl(PtyOptions options) : on_output_(std::move(options.on_output)) {
        if (options.argv.empty()) throw std::runtime_error("cannot start: empty argv");
        std::vector<char*> argv_pointers;
        for (auto& argument : options.argv) argv_pointers.push_back(argument.data());
        argv_pointers.push_back(nullptr);
        std::vector<char*> env_pointers;
        for (auto& entry : options.env) env_pointers.push_back(entry.data());
        env_pointers.push_back(nullptr);

        int report[2];
        if (pipe(report) != 0) throw std::runtime_error("cannot start " + options.argv[0] + ": " + std::strerror(errno));
        fcntl(report[0], F_SETFD, FD_CLOEXEC);
        fcntl(report[1], F_SETFD, FD_CLOEXEC);

        winsize size{};
        size.ws_col = static_cast<unsigned short>(options.cols);
        size.ws_row = static_cast<unsigned short>(options.rows);
        pid_ = forkpty(&master_, nullptr, nullptr, &size);
        if (pid_ < 0) {
            int error = errno;
            close(report[0]);
            close(report[1]);
            throw std::runtime_error("cannot start " + options.argv[0] + ": " + std::strerror(error));
        }
        if (pid_ == 0) {
            close(report[0]);
            signal(SIGPIPE, SIG_DFL);
            if (!options.cwd.empty() && chdir(options.cwd.c_str()) != 0) write_errno_and_exit(report[1]);
            if (options.env.empty()) {
                execvp(argv_pointers[0], argv_pointers.data());
            } else {
                execve_with_path(argv_pointers, env_pointers);
            }
            write_errno_and_exit(report[1]);
        }
        close(report[1]);
        int child_error = 0;
        ssize_t received;
        do {
            received = read(report[0], &child_error, sizeof(child_error));
        } while (received < 0 && errno == EINTR);
        close(report[0]);
        if (received == static_cast<ssize_t>(sizeof(child_error))) {
            int status = 0;
            while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
            close(master_);
            master_ = -1;
            exited_ = true;
            throw std::runtime_error("cannot start " + options.argv[0] + ": " + std::strerror(child_error));
        }
        fcntl(master_, F_SETFD, FD_CLOEXEC);
        reader_ = std::thread([this] { read_loop(); });
    }

    ~Impl() { terminate(); }

    void write(std::string_view bytes) {
        std::lock_guard lock(write_mutex_);
        if (master_ < 0) return;
        while (!bytes.empty()) {
            ssize_t written = ::write(master_, bytes.data(), bytes.size());
            if (written < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN) {
                    pollfd writable{master_, POLLOUT, 0};
                    if (poll(&writable, 1, 100) >= 0) continue;
                }
                return;
            }
            bytes.remove_prefix(static_cast<size_t>(written));
        }
    }

    void resize(int cols, int rows) {
        std::lock_guard lock(write_mutex_);
        if (master_ < 0) return;
        winsize size{};
        size.ws_col = static_cast<unsigned short>(cols);
        size.ws_row = static_cast<unsigned short>(rows);
        ioctl(master_, TIOCSWINSZ, &size);
    }

    bool running() {
        std::lock_guard lock(wait_mutex_);
        return poll_exit();
    }

    void terminate() {
        std::lock_guard lock(terminate_mutex_);
        if (terminated_) return;
        terminated_ = true;
        {
            std::lock_guard wait_lock(wait_mutex_);
            if (poll_exit()) {
                if (kill(-pid_, SIGHUP) != 0) kill(pid_, SIGHUP);
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                while (poll_exit() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (poll_exit()) {
                    if (kill(-pid_, SIGKILL) != 0) kill(pid_, SIGKILL);
                    int status = 0;
                    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
                    }
                    exited_ = true;
                }
            }
        }
        stop_ = true;
        if (reader_.joinable()) reader_.join();
        std::lock_guard write_lock(write_mutex_);
        if (master_ >= 0) {
            close(master_);
            master_ = -1;
        }
    }

private:
    static void execve_with_path(std::vector<char*>& argv, std::vector<char*>& env) {
        const char* program = argv[0];
        if (std::strchr(program, '/')) {
            execve(program, argv.data(), env.data());
            return;
        }
        const char* search = nullptr;
        for (char** entry = env.data(); *entry; ++entry)
            if (std::strncmp(*entry, "PATH=", 5) == 0) search = *entry + 5;
        if (!search) search = getenv("PATH");
        if (!search) search = "/usr/local/bin:/usr/bin:/bin";
        int last_error = ENOENT;
        std::string candidate;
        for (const char* start = search;; ) {
            const char* end = std::strchr(start, ':');
            size_t length = end ? static_cast<size_t>(end - start) : std::strlen(start);
            candidate.assign(start, length);
            if (candidate.empty()) candidate = ".";
            candidate.push_back('/');
            candidate += program;
            execve(candidate.c_str(), argv.data(), env.data());
            if (errno != ENOENT && errno != ENOTDIR) last_error = errno;
            if (!end) break;
            start = end + 1;
        }
        errno = last_error;
    }

    bool poll_exit() {
        if (exited_) return false;
        int status = 0;
        pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_ || (result < 0 && errno == ECHILD)) {
            exited_ = true;
            return false;
        }
        return true;
    }

    void read_loop() {
        char buffer[65536];
        while (!stop_) {
            pollfd readable{master_, POLLIN, 0};
            int ready = poll(&readable, 1, 100);
            if (ready < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ready == 0) continue;
            ssize_t received = read(master_, buffer, sizeof(buffer));
            if (received < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                break;
            }
            if (received == 0) break;
            if (stop_) break;
            if (on_output_) on_output_(std::string_view(buffer, static_cast<size_t>(received)));
        }
    }

    std::function<void(std::string_view)> on_output_;
    pid_t pid_ = -1;
    int master_ = -1;
    bool exited_ = false;
    bool terminated_ = false;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::mutex write_mutex_;
    std::mutex wait_mutex_;
    std::mutex terminate_mutex_;
};

#endif

std::unique_ptr<Pty> Pty::spawn(PtyOptions options) {
    std::unique_ptr<Pty> pty(new Pty());
    pty->impl_ = std::make_unique<Impl>(std::move(options));
    return pty;
}

Pty::~Pty() {
    if (impl_) impl_->terminate();
}

void Pty::write(std::string_view bytes) { impl_->write(bytes); }

void Pty::resize(int cols, int rows) { impl_->resize(cols, rows); }

bool Pty::running() { return impl_->running(); }

void Pty::terminate() { impl_->terminate(); }

}
