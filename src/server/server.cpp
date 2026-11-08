#include "server/server.hpp"

#include "common/paths.hpp"
#include "common/time.hpp"
#include "harness/event.hpp"
#include "session/agent_session.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ccb {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using OrderedJson = nlohmann::ordered_json;
using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;
using Responder = std::function<void(Response)>;
using QueryParams = std::vector<std::pair<std::string, std::string>>;

constexpr std::size_t max_inbound_message = std::size_t{16} << 20;
constexpr auto pump_interval = std::chrono::milliseconds(150);
constexpr int liveness_check_ticks = 20;
constexpr auto reap_interval = std::chrono::seconds(60);
constexpr auto ready_timeout = std::chrono::seconds(60);
constexpr auto ready_probe = std::chrono::seconds(1);
constexpr auto websocket_idle_timeout = std::chrono::seconds(60);
constexpr auto http_idle_timeout = std::chrono::seconds(75);
constexpr std::size_t pane_tail_lines = 12;
constexpr std::size_t blocking_pool_threads = 4;
constexpr std::string_view html_content_type = "text/html; charset=utf-8";
constexpr std::string_view json_content_type = "application/json; charset=utf-8";
constexpr std::string_view text_content_type = "text/plain; charset=utf-8";
constexpr std::string_view server_name = "ccbridge";
constexpr std::string_view sessions_route_prefix = "/sessions/";

std::string_view as_std(beast::string_view view) {
    return {view.data(), view.size()};
}

bool is_python_space(unsigned char c) {
    return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f);
}

bool is_blank(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return is_python_space(static_cast<unsigned char>(c)); });
}

std::string rstrip_spaces(std::string_view text) {
    std::size_t end = text.size();
    while (end > 0 && is_python_space(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(0, end));
}

bool is_line_break(unsigned char c) {
    return c == '\n' || c == '\r' || c == '\v' || c == '\f' || (c >= 0x1c && c <= 0x1e);
}

std::vector<std::string> python_splitlines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        const auto c = static_cast<unsigned char>(text[index]);
        if (!is_line_break(c)) {
            ++index;
            continue;
        }
        lines.emplace_back(text.substr(start, index - start));
        const bool crlf = c == '\r' && index + 1 < text.size() && text[index + 1] == '\n';
        index += crlf ? 2 : 1;
        start = index;
    }
    if (start < text.size()) {
        lines.emplace_back(text.substr(start));
    }
    return lines;
}

int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void append_hex(std::string& out, unsigned value, int digits) {
    static constexpr char lower_hex[] = "0123456789abcdef";
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) {
        out += lower_hex[(value >> shift) & 0xFu];
    }
}

std::string url_decode(std::string_view text, bool plus_is_space) {
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '+' && plus_is_space) {
            decoded += ' ';
            continue;
        }
        if (c == '%' && index + 2 < text.size()) {
            const int high = hex_digit_value(text[index + 1]);
            const int low = hex_digit_value(text[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded += static_cast<char>(high * 16 + low);
                index += 2;
                continue;
            }
        }
        decoded += c;
    }
    return decoded;
}

std::pair<std::string_view, std::string_view> split_target(std::string_view target) {
    if (const auto fragment = target.find('#'); fragment != std::string_view::npos) {
        target = target.substr(0, fragment);
    }
    const auto mark = target.find('?');
    if (mark == std::string_view::npos) {
        return {target, {}};
    }
    return {target.substr(0, mark), target.substr(mark + 1)};
}

QueryParams parse_query(std::string_view query) {
    QueryParams params;
    std::size_t start = 0;
    while (start <= query.size()) {
        auto end = query.find('&', start);
        if (end == std::string_view::npos) {
            end = query.size();
        }
        const auto part = query.substr(start, end - start);
        if (!part.empty()) {
            const auto equals = part.find('=');
            std::string key = url_decode(part.substr(0, equals), true);
            std::string value =
                equals == std::string_view::npos ? std::string() : url_decode(part.substr(equals + 1), true);
            params.emplace_back(std::move(key), std::move(value));
        }
        start = end + 1;
    }
    return params;
}

std::optional<std::string> query_value(const QueryParams& params, std::string_view key) {
    for (const auto& [name, value] : params) {
        if (name == key) {
            return value;
        }
    }
    return std::nullopt;
}

std::string non_empty_or(const std::optional<std::string>& value, const std::string& fallback) {
    return value && !value->empty() ? *value : fallback;
}

bool is_truthy(const std::optional<std::string>& value) {
    if (!value) {
        return false;
    }
    std::string lowered = *value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on";
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string joined;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index > 0) {
            joined += separator;
        }
        joined += parts[index];
    }
    return joined;
}

std::string python_repr(std::string_view text) {
    const bool has_single = text.find('\'') != std::string_view::npos;
    const bool has_double = text.find('"') != std::string_view::npos;
    const char quote = has_single && !has_double ? '"' : '\'';
    std::string out(1, quote);
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (raw == '\\') {
            out += "\\\\";
        } else if (raw == quote) {
            out += '\\';
            out += quote;
        } else if (raw == '\n') {
            out += "\\n";
        } else if (raw == '\r') {
            out += "\\r";
        } else if (raw == '\t') {
            out += "\\t";
        } else if (c < 0x20 || c == 0x7f) {
            out += "\\x";
            append_hex(out, c, 2);
        } else {
            out += raw;
        }
    }
    out += quote;
    return out;
}

char32_t next_code_point(std::string_view text, std::size_t& index) {
    constexpr char32_t replacement = 0xFFFD;
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t length = 0;
    char32_t code_point = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        code_point = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        code_point = lead & 0x0Fu;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        code_point = lead & 0x07u;
    } else {
        ++index;
        return replacement;
    }
    if (index + length > text.size()) {
        ++index;
        return replacement;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto continuation = static_cast<unsigned char>(text[index + offset]);
        if ((continuation & 0xC0u) != 0x80u) {
            ++index;
            return replacement;
        }
        code_point = (code_point << 6) | (continuation & 0x3Fu);
    }
    const bool overlong = (length == 3 && code_point < 0x800) || (length == 4 && code_point < 0x10000);
    const bool surrogate = code_point >= 0xD800 && code_point <= 0xDFFF;
    if (overlong || surrogate || code_point > 0x10FFFF) {
        ++index;
        return replacement;
    }
    index += length;
    return code_point;
}

void append_unicode_escape(std::string& out, unsigned value) {
    out += "\\u";
    append_hex(out, value, 4);
}

void append_python_json_string(std::string& out, std::string_view text) {
    out += '"';
    std::size_t index = 0;
    while (index < text.size()) {
        const auto c = static_cast<unsigned char>(text[index]);
        if (c >= 0x80) {
            char32_t code_point = next_code_point(text, index);
            if (code_point > 0xFFFF) {
                code_point -= 0x10000;
                append_unicode_escape(out, 0xD800u + static_cast<unsigned>(code_point >> 10));
                append_unicode_escape(out, 0xDC00u + static_cast<unsigned>(code_point & 0x3FFu));
            } else {
                append_unicode_escape(out, static_cast<unsigned>(code_point));
            }
            continue;
        }
        ++index;
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (c < 0x20) {
                append_unicode_escape(out, c);
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void append_python_json(std::string& out, const OrderedJson& value) {
    switch (value.type()) {
    case OrderedJson::value_t::null:
        out += "null";
        return;
    case OrderedJson::value_t::boolean:
        out += value.get<bool>() ? "true" : "false";
        return;
    case OrderedJson::value_t::number_integer:
        out += std::to_string(value.get<std::int64_t>());
        return;
    case OrderedJson::value_t::number_unsigned:
        out += std::to_string(value.get<std::uint64_t>());
        return;
    case OrderedJson::value_t::string:
        append_python_json_string(out, value.get_ref<const std::string&>());
        return;
    case OrderedJson::value_t::array: {
        out += '[';
        bool first = true;
        for (const auto& item : value) {
            if (!first) {
                out += ", ";
            }
            first = false;
            append_python_json(out, item);
        }
        out += ']';
        return;
    }
    case OrderedJson::value_t::object: {
        out += '{';
        bool first = true;
        for (const auto& [key, item] : value.items()) {
            if (!first) {
                out += ", ";
            }
            first = false;
            append_python_json_string(out, key);
            out += ": ";
            append_python_json(out, item);
        }
        out += '}';
        return;
    }
    default:
        out += value.dump();
        return;
    }
}

std::string python_json(const OrderedJson& value) {
    std::string out;
    append_python_json(out, value);
    return out;
}

std::string_view tag_prefix(Kind kind) {
    switch (kind) {
    case Kind::Text:
        return "t:";
    case Kind::Thinking:
        return "k:";
    case Kind::Tool:
        return "u:";
    case Kind::Result:
        return "r:";
    case Kind::You:
        return "y:";
    case Kind::TurnEnd:
        return "e:";
    }
    return "";
}

struct RequestInfo {
    unsigned version = 11;
    bool keep_alive = true;
    bool head = false;
};

Response make_response(const RequestInfo& info, http::status status, std::string_view content_type,
                       std::string body) {
    Response response{status, info.version};
    response.set(http::field::server, server_name);
    response.set(http::field::content_type, content_type);
    response.keep_alive(info.keep_alive);
    if (info.head) {
        response.content_length(body.size());
    } else {
        response.body() = std::move(body);
        response.prepare_payload();
    }
    return response;
}

Response status_response(const RequestInfo& info, http::status status) {
    std::string body = std::to_string(static_cast<unsigned>(status)) + ": " +
                       std::string(as_std(http::obsolete_reason(status)));
    return make_response(info, status, text_content_type, std::move(body));
}

Response method_not_allowed(const RequestInfo& info, std::string_view allowed) {
    Response response = status_response(info, http::status::method_not_allowed);
    response.set(http::field::allow, allowed);
    return response;
}

Response json_response(const RequestInfo& info, http::status status, const OrderedJson& body) {
    return make_response(info, status, json_content_type, python_json(body));
}

struct LaunchRequest {
    std::string name;
    std::string mode;
    std::string cwd;
    std::shared_ptr<const Harness> harness;
    KindSet kinds;
    std::vector<std::string> launch_args;
    std::optional<std::string> session_id;
};

SessionOptions hub_session_options(const LaunchRequest& request) {
    SessionOptions options;
    options.cwd = request.cwd;
    options.name = "ccb-" + request.name;
    options.launch_args = request.launch_args;
    options.at_end = !request.launch_args.empty();
    options.session_id = request.session_id;
    return options;
}

template <typename T>
void remember(std::vector<std::weak_ptr<T>>& registry, const std::shared_ptr<T>& item) {
    std::erase_if(registry, [](const std::weak_ptr<T>& entry) { return entry.expired(); });
    registry.push_back(item);
}

class Server;
class WsConnection;

class Hub : public std::enable_shared_from_this<Hub> {
public:
    using StartedHandler = std::function<void(bool)>;
    using StoppedHandler = std::function<void()>;

    Hub(asio::io_context& io, const LaunchRequest& request, std::shared_ptr<Hub> predecessor);
    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;
    ~Hub();

    void launch(StartedHandler on_started);
    void submit(std::string text);
    void attach(std::shared_ptr<WsConnection> connection, std::string mode);
    void detach(const WsConnection* connection);
    void stop(StoppedHandler on_stopped);
    void join();
    void wait_finished();
    bool finished() const;
    bool alive_blocking() const;
    OrderedJson describe_blocking(std::size_t client_count) const;

    const std::string& name() const { return name_; }
    const std::vector<std::string>& launch_args() const { return launch_args_; }
    bool has_clients() const { return !clients_.empty(); }
    std::size_t client_count() const { return clients_.size(); }
    double last_touch() const { return last_touch_; }

private:
    struct Client {
        std::shared_ptr<WsConnection> connection;
        std::string mode;
    };

    void work(StartedHandler on_started);
    bool start_session();
    bool wait_until_ready();
    void flush_queue(bool ready);
    void pump();
    void wait_for_stop_request();
    void finish();
    bool stop_requested() const;
    void send_text(const std::string& text);
    void emit(const std::vector<Event>& events);
    void broadcast(const std::string& text);

    asio::io_context& io_;
    const std::string name_;
    const std::string cwd_;
    const std::string harness_name_;
    const KindSet kinds_;
    const std::vector<std::string> launch_args_;
    AgentSession session_;
    std::shared_ptr<Hub> predecessor_;
    std::vector<Client> clients_;
    double last_touch_ = now_seconds();
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> queued_;
    bool ready_ = false;
    bool stop_requested_ = false;
    bool finished_ = false;
    std::vector<StoppedHandler> stopped_handlers_;
    std::atomic<bool> starting_{true};
    std::thread worker_;
};

class WsConnection : public std::enable_shared_from_this<WsConnection> {
public:
    WsConnection(beast::tcp_stream&& stream, Server& server) : ws_(std::move(stream)), server_(server) {}

    void accept(Request request);
    void send(std::string text);
    void refuse(std::string message);
    void abort();
    void bind(std::shared_ptr<Hub> hub);
    void shutdown();
    bool open() const { return !closed_ && !closing_; }

private:
    void write_next();
    void read_next();
    void close_with(websocket::close_code code);
    void release();

    websocket::stream<beast::tcp_stream> ws_;
    Server& server_;
    Request request_;
    std::string target_;
    beast::flat_buffer buffer_;
    std::deque<std::string> outbox_;
    std::shared_ptr<Hub> hub_;
    std::optional<websocket::close_code> close_after_drain_;
    bool writing_ = false;
    bool closing_ = false;
    bool closed_ = false;
};

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket&& socket, Server& server) : stream_(std::move(socket)), server_(server) {}

    void run() { read_next(); }
    void shutdown();

private:
    void read_next();
    void on_request(Request request);
    void write(Response response);

    beast::tcp_stream stream_;
    Server& server_;
    beast::flat_buffer buffer_;
    std::optional<http::request_parser<http::string_body>> parser_;
    bool upgraded_ = false;
};

class Server {
public:
    Server(asio::io_context& io, const ServerOptions& options, const Registry& registry);
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool listen();
    void start();
    void join_workers();

    void open_websocket(const std::shared_ptr<WsConnection>& connection, std::string_view target);
    void handle_http(Request request, Responder respond);
    void remember_websocket(const std::shared_ptr<WsConnection>& connection);
    static bool is_websocket_route(const Request& request);

private:
    struct HubSnapshot {
        std::string name;
        std::shared_ptr<Hub> hub;
        std::size_t clients = 0;
    };

    void accept_next(tcp::acceptor& acceptor);
    void schedule_reap();
    void reap_idle();
    void prune_finished();
    void shutdown();
    void attach_or_create(const std::shared_ptr<WsConnection>& connection, LaunchRequest request);
    void on_liveness(const std::shared_ptr<WsConnection>& connection, const std::shared_ptr<Hub>& hub,
                     LaunchRequest request, bool alive);
    void create_hub(const std::shared_ptr<WsConnection>& connection, const LaunchRequest& request);
    std::shared_ptr<Hub> find_hub(const std::string& name) const;
    std::shared_ptr<Hub> take_hub(const std::string& name);
    std::shared_ptr<Hub> latest_unfinished(const std::string& name) const;
    void serve_index(const RequestInfo& info, const Responder& respond) const;
    void list_sessions(const RequestInfo& info, Responder respond);
    void delete_session(const RequestInfo& info, const std::string& name, Responder respond);

    asio::io_context& io_;
    ServerOptions options_;
    const Registry& registry_;
    asio::thread_pool blocking_pool_{blocking_pool_threads};
    asio::signal_set signals_;
    asio::steady_timer reap_timer_;
    std::vector<std::unique_ptr<tcp::acceptor>> acceptors_;
    std::vector<std::pair<std::string, std::shared_ptr<Hub>>> hubs_;
    std::vector<std::shared_ptr<Hub>> all_hubs_;
    std::vector<std::weak_ptr<WsConnection>> websockets_;
    std::vector<std::weak_ptr<HttpSession>> http_sessions_;
    bool shutting_down_ = false;
};

Hub::Hub(asio::io_context& io, const LaunchRequest& request, std::shared_ptr<Hub> predecessor)
    : io_(io),
      name_(request.name),
      cwd_(request.cwd),
      harness_name_(request.harness->name),
      kinds_(request.kinds),
      launch_args_(request.launch_args),
      session_(request.harness, hub_session_options(request)),
      predecessor_(std::move(predecessor)) {}

Hub::~Hub() {
    if (!worker_.joinable()) {
        return;
    }
    if (worker_.get_id() == std::this_thread::get_id()) {
        worker_.detach();
    } else {
        worker_.join();
    }
}

void Hub::launch(StartedHandler on_started) {
    worker_ = std::thread([this, handler = std::move(on_started)]() mutable { work(std::move(handler)); });
}

void Hub::submit(std::string text) {
    last_touch_ = now_seconds();
    {
        std::lock_guard lock(mutex_);
        queued_.push_back(std::move(text));
    }
    wake_.notify_all();
}

void Hub::attach(std::shared_ptr<WsConnection> connection, std::string mode) {
    clients_.push_back(Client{std::move(connection), std::move(mode)});
    last_touch_ = now_seconds();
}

void Hub::detach(const WsConnection* connection) {
    std::erase_if(clients_, [connection](const Client& client) { return client.connection.get() == connection; });
}

void Hub::stop(StoppedHandler on_stopped) {
    bool already_finished = false;
    {
        std::lock_guard lock(mutex_);
        stop_requested_ = true;
        already_finished = finished_;
        if (!already_finished && on_stopped) {
            stopped_handlers_.push_back(on_stopped);
        }
    }
    wake_.notify_all();
    if (already_finished && on_stopped) {
        asio::post(io_, std::move(on_stopped));
    }
}

void Hub::join() {
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

void Hub::wait_finished() {
    std::unique_lock lock(mutex_);
    wake_.wait(lock, [this] { return finished_; });
}

bool Hub::finished() const {
    std::lock_guard lock(mutex_);
    return finished_;
}

bool Hub::stop_requested() const {
    std::lock_guard lock(mutex_);
    return stop_requested_;
}

bool Hub::alive_blocking() const {
    return starting_.load() || session_.alive();
}

OrderedJson Hub::describe_blocking(std::size_t client_count) const {
    const std::string pane = session_.pane();
    std::vector<std::string> lines;
    for (const auto& line : python_splitlines(pane)) {
        if (!is_blank(line)) {
            lines.push_back(rstrip_spaces(line));
        }
    }
    const std::size_t first = lines.size() > pane_tail_lines ? lines.size() - pane_tail_lines : 0;
    OrderedJson tail = OrderedJson::array();
    for (std::size_t index = first; index < lines.size(); ++index) {
        tail.push_back(lines[index]);
    }
    OrderedJson described = OrderedJson::object();
    described["clients"] = client_count;
    described["alive"] = session_.alive();
    described["harness"] = harness_name_;
    described["cwd"] = cwd_;
    described["busy"] = session_.harness().pane_busy(pane);
    described["launch_args"] = launch_args_;
    described["pane_tail"] = std::move(tail);
    return described;
}

void Hub::work(StartedHandler on_started) {
    try {
        if (predecessor_) {
            predecessor_->wait_finished();
            predecessor_.reset();
        }
        const bool started = start_session();
        const bool ready = started && wait_until_ready();
        if (started) {
            flush_queue(ready);
        }
        starting_ = false;
        const bool usable = started && !stop_requested();
        asio::post(io_, [self = shared_from_this(), handler = std::move(on_started), usable] { handler(usable); });
        if (usable) {
            pump();
        }
    } catch (const std::exception&) {
        starting_ = false;
    }
    wait_for_stop_request();
    try {
        session_.stop();
    } catch (const std::exception&) {
    }
    finish();
}

bool Hub::start_session() {
    if (stop_requested()) {
        return false;
    }
    try {
        session_.start();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool Hub::wait_until_ready() {
    const auto deadline = std::chrono::steady_clock::now() + ready_timeout;
    while (!stop_requested() && std::chrono::steady_clock::now() < deadline) {
        if (session_.wait_ready(ready_probe)) {
            return true;
        }
    }
    return false;
}

void Hub::flush_queue(bool ready) {
    std::deque<std::string> pending;
    {
        std::lock_guard lock(mutex_);
        pending.swap(queued_);
        ready_ = ready;
    }
    for (const auto& text : pending) {
        send_text(text);
    }
}

void Hub::send_text(const std::string& text) {
    try {
        session_.send(text);
    } catch (const std::exception&) {
    }
}

void Hub::pump() {
    int ticks = 0;
    while (true) {
        std::deque<std::string> outgoing;
        {
            std::lock_guard lock(mutex_);
            if (stop_requested_) {
                return;
            }
            if (ready_) {
                outgoing.swap(queued_);
            }
        }
        for (const auto& text : outgoing) {
            send_text(text);
        }
        std::vector<Event> events;
        try {
            events = session_.events(kinds_);
        } catch (const std::exception&) {
            return;
        }
        if (!events.empty()) {
            asio::post(io_, [self = shared_from_this(), events = std::move(events)] { self->emit(events); });
        }
        if (++ticks % liveness_check_ticks == 0 && !session_.alive()) {
            asio::post(io_, [self = shared_from_this()] { self->broadcast(""); });
            return;
        }
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, pump_interval, [this] { return stop_requested_ || (ready_ && !queued_.empty()); });
    }
}

void Hub::wait_for_stop_request() {
    std::unique_lock lock(mutex_);
    wake_.wait(lock, [this] { return stop_requested_; });
}

void Hub::finish() {
    std::vector<StoppedHandler> handlers;
    {
        std::lock_guard lock(mutex_);
        finished_ = true;
        handlers.swap(stopped_handlers_);
    }
    wake_.notify_all();
    for (auto& handler : handlers) {
        asio::post(io_, std::move(handler));
    }
}

void Hub::emit(const std::vector<Event>& events) {
    for (const Event& event : events) {
        const std::vector<Client> recipients = clients_;
        if (event.kind == Kind::TurnEnd) {
            for (const auto& client : recipients) {
                client.connection->send(client.mode == "tagged" ? "e:" : "");
            }
            continue;
        }
        if (event.text.empty() && event.kind != Kind::Tool) {
            continue;
        }
        const std::string tagged = std::string(tag_prefix(event.kind)) + event.text;
        for (const auto& client : recipients) {
            if (client.mode == "tagged") {
                client.connection->send(tagged);
            }
        }
        if (event.kind == Kind::Text) {
            for (const auto& client : recipients) {
                if (client.mode == "plain") {
                    client.connection->send(event.text);
                }
            }
        }
    }
}

void Hub::broadcast(const std::string& text) {
    const std::vector<Client> recipients = clients_;
    for (const auto& client : recipients) {
        client.connection->send(text);
    }
}

void WsConnection::accept(Request request) {
    request_ = std::move(request);
    target_ = std::string(as_std(request_.target()));
    beast::get_lowest_layer(ws_).expires_never();
    auto timeouts = websocket::stream_base::timeout::suggested(beast::role_type::server);
    timeouts.idle_timeout = websocket_idle_timeout;
    timeouts.keep_alive_pings = true;
    ws_.set_option(timeouts);
    ws_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& response) { response.set(http::field::server, server_name); }));
    ws_.read_message_max(max_inbound_message);
    ws_.async_accept(request_, [self = shared_from_this()](beast::error_code error) {
        if (error) {
            self->release();
            return;
        }
        self->server_.open_websocket(self, self->target_);
    });
}

void WsConnection::send(std::string text) {
    if (!open()) {
        return;
    }
    outbox_.push_back(std::move(text));
    if (!writing_) {
        write_next();
    }
}

void WsConnection::refuse(std::string message) {
    send(std::move(message));
    close_after_drain_ = websocket::close_code::normal;
    if (!writing_) {
        close_with(websocket::close_code::normal);
    }
}

void WsConnection::abort() {
    close_after_drain_ = websocket::close_code::internal_error;
    if (!writing_) {
        close_with(websocket::close_code::internal_error);
    }
}

void WsConnection::bind(std::shared_ptr<Hub> hub) {
    hub_ = std::move(hub);
    read_next();
}

void WsConnection::shutdown() {
    closed_ = true;
    beast::error_code ignored;
    beast::get_lowest_layer(ws_).socket().close(ignored);
}

void WsConnection::write_next() {
    if (outbox_.empty() || closed_) {
        writing_ = false;
        if (close_after_drain_ && !closed_) {
            close_with(*close_after_drain_);
        }
        return;
    }
    writing_ = true;
    ws_.text(true);
    ws_.async_write(asio::buffer(outbox_.front()), [self = shared_from_this()](beast::error_code error, std::size_t) {
        if (error) {
            self->writing_ = false;
            self->release();
            return;
        }
        self->outbox_.pop_front();
        self->write_next();
    });
}

void WsConnection::read_next() {
    ws_.async_read(buffer_, [self = shared_from_this()](beast::error_code error, std::size_t) {
        if (error) {
            self->release();
            return;
        }
        if (self->ws_.got_text()) {
            std::string text = beast::buffers_to_string(self->buffer_.data());
            if (self->hub_ && !is_blank(text)) {
                self->hub_->submit(std::move(text));
            }
        }
        self->buffer_.consume(self->buffer_.size());
        self->read_next();
    });
}

void WsConnection::close_with(websocket::close_code code) {
    if (closing_ || closed_) {
        return;
    }
    closing_ = true;
    ws_.async_close(code, [self = shared_from_this()](beast::error_code) { self->release(); });
}

void WsConnection::release() {
    closed_ = true;
    if (hub_) {
        hub_->detach(this);
        hub_.reset();
    }
}

void HttpSession::shutdown() {
    if (upgraded_) {
        return;
    }
    beast::error_code ignored;
    stream_.socket().close(ignored);
}

void HttpSession::read_next() {
    parser_.emplace();
    parser_->body_limit(max_inbound_message);
    stream_.expires_after(http_idle_timeout);
    http::async_read(stream_, buffer_, *parser_, [self = shared_from_this()](beast::error_code error, std::size_t) {
        if (error) {
            self->shutdown();
            return;
        }
        self->on_request(self->parser_->release());
    });
}

void HttpSession::on_request(Request request) {
    if (Server::is_websocket_route(request)) {
        upgraded_ = true;
        stream_.expires_never();
        auto connection = std::make_shared<WsConnection>(std::move(stream_), server_);
        server_.remember_websocket(connection);
        connection->accept(std::move(request));
        return;
    }
    server_.handle_http(std::move(request),
                        [self = shared_from_this()](Response response) { self->write(std::move(response)); });
}

void HttpSession::write(Response response) {
    auto message = std::make_shared<Response>(std::move(response));
    stream_.expires_after(http_idle_timeout);
    http::async_write(stream_, *message, [self = shared_from_this(), message](beast::error_code error, std::size_t) {
        if (error) {
            self->shutdown();
            return;
        }
        if (message->need_eof()) {
            beast::error_code ignored;
            self->stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
            return;
        }
        self->read_next();
    });
}

Server::Server(asio::io_context& io, const ServerOptions& options, const Registry& registry)
    : io_(io), options_(options), registry_(registry), signals_(io, SIGINT, SIGTERM), reap_timer_(io) {}

bool Server::listen() {
    beast::error_code error;
    tcp::resolver resolver(io_);
    const auto endpoints =
        resolver.resolve(options_.host, std::to_string(options_.port), tcp::resolver::passive, error);
    if (error) {
        std::cerr << "ccbridge: cannot resolve " << options_.host << ": " << error.message() << std::endl;
        return false;
    }
    beast::error_code last_error;
    for (const auto& entry : endpoints) {
        const tcp::endpoint endpoint = entry.endpoint();
        auto acceptor = std::make_unique<tcp::acceptor>(io_);
        beast::error_code step;
        acceptor->open(endpoint.protocol(), step);
        if (step) {
            last_error = step;
            continue;
        }
#if !defined(_WIN32)
        acceptor->set_option(asio::socket_base::reuse_address(true), step);
#endif
        if (endpoint.address().is_v6()) {
            acceptor->set_option(asio::ip::v6_only(true), step);
        }
        acceptor->bind(endpoint, step);
        if (step) {
            last_error = step;
            continue;
        }
        acceptor->listen(asio::socket_base::max_listen_connections, step);
        if (step) {
            last_error = step;
            continue;
        }
        acceptors_.push_back(std::move(acceptor));
    }
    if (acceptors_.empty()) {
        std::cerr << "ccbridge: cannot bind " << options_.host << ":" << options_.port << ": "
                  << (last_error ? last_error.message() : std::string("no address")) << std::endl;
        return false;
    }
    return true;
}

void Server::start() {
    signals_.async_wait([this](const beast::error_code& error, int) {
        if (!error) {
            shutdown();
        }
    });
    schedule_reap();
    for (auto& acceptor : acceptors_) {
        accept_next(*acceptor);
    }
}

void Server::join_workers() {
    for (const auto& hub : all_hubs_) {
        hub->stop({});
    }
    for (const auto& hub : all_hubs_) {
        hub->join();
    }
    blocking_pool_.join();
}

void Server::accept_next(tcp::acceptor& acceptor) {
    acceptor.async_accept([this, &acceptor](beast::error_code error, tcp::socket socket) {
        if (shutting_down_ || !acceptor.is_open()) {
            return;
        }
        if (!error) {
            auto session = std::make_shared<HttpSession>(std::move(socket), *this);
            remember(http_sessions_, session);
            session->run();
        }
        accept_next(acceptor);
    });
}

void Server::schedule_reap() {
    reap_timer_.expires_after(reap_interval);
    reap_timer_.async_wait([this](const beast::error_code& error) {
        if (error || shutting_down_) {
            return;
        }
        reap_idle();
        schedule_reap();
    });
}

void Server::reap_idle() {
    const double now = now_seconds();
    const auto snapshot = hubs_;
    for (const auto& [name, hub] : snapshot) {
        if (!hub->has_clients() && now - hub->last_touch() > options_.idle_reap_seconds) {
            take_hub(name);
            hub->stop({});
        }
    }
    prune_finished();
}

void Server::prune_finished() {
    std::erase_if(all_hubs_, [](const std::shared_ptr<Hub>& hub) {
        if (!hub->finished()) {
            return false;
        }
        hub->join();
        return true;
    });
}

void Server::shutdown() {
    if (shutting_down_) {
        return;
    }
    shutting_down_ = true;
    beast::error_code ignored;
    for (auto& acceptor : acceptors_) {
        acceptor->close(ignored);
    }
    reap_timer_.cancel();
    for (const auto& [name, hub] : hubs_) {
        hub->stop({});
    }
    hubs_.clear();
    for (const auto& entry : websockets_) {
        if (auto connection = entry.lock()) {
            connection->shutdown();
        }
    }
    for (const auto& entry : http_sessions_) {
        if (auto session = entry.lock()) {
            session->shutdown();
        }
    }
    io_.stop();
}

void Server::remember_websocket(const std::shared_ptr<WsConnection>& connection) {
    remember(websockets_, connection);
}

bool Server::is_websocket_route(const Request& request) {
    if (!websocket::is_upgrade(request)) {
        return false;
    }
    return url_decode(split_target(as_std(request.target())).first, false) == "/ws";
}

std::shared_ptr<Hub> Server::find_hub(const std::string& name) const {
    for (const auto& [hub_name, hub] : hubs_) {
        if (hub_name == name) {
            return hub;
        }
    }
    return nullptr;
}

std::shared_ptr<Hub> Server::take_hub(const std::string& name) {
    for (auto entry = hubs_.begin(); entry != hubs_.end(); ++entry) {
        if (entry->first == name) {
            auto hub = entry->second;
            hubs_.erase(entry);
            return hub;
        }
    }
    return nullptr;
}

std::shared_ptr<Hub> Server::latest_unfinished(const std::string& name) const {
    for (auto entry = all_hubs_.rbegin(); entry != all_hubs_.rend(); ++entry) {
        if ((*entry)->name() == name && !(*entry)->finished()) {
            return *entry;
        }
    }
    return nullptr;
}

void Server::open_websocket(const std::shared_ptr<WsConnection>& connection, std::string_view target) {
    if (shutting_down_) {
        connection->shutdown();
        return;
    }
    const QueryParams query = parse_query(split_target(target).second);
    LaunchRequest request;
    request.name = query_value(query, "session").value_or("default");
    request.mode = query_value(query, "mode").value_or("plain");
    request.cwd = non_empty_or(query_value(query, "cwd"), options_.default_cwd);
    const std::string harness_name = non_empty_or(query_value(query, "harness"), registry_.default_name());
    const std::vector<std::string> known = registry_.names();
    if (std::find(known.begin(), known.end(), harness_name) != known.end()) {
        request.harness = registry_.get(harness_name);
    }
    if (!request.harness) {
        connection->refuse("unknown harness " + python_repr(harness_name) + "; known: " + join(known, ", "));
        return;
    }
    request.kinds = kinds_from_csv(query_value(query, "kinds").value_or("text,tool"));
    const std::string resume_id = query_value(query, "resume").value_or("");
    const bool continue_last = is_truthy(query_value(query, "continue"));
    const bool fork = is_truthy(query_value(query, "fork"));
    if (!resume_id.empty() && continue_last) {
        connection->refuse("resume and continue are mutually exclusive");
        return;
    }
    if (fork && resume_id.empty() && !continue_last) {
        connection->refuse("fork needs resume or continue");
        return;
    }
    if (!resume_id.empty() || continue_last) {
        try {
            if (fork) {
                request.launch_args = continue_last ? request.harness->fork_continue_args()
                                                    : request.harness->fork_args(resume_id);
            } else {
                request.launch_args = continue_last ? request.harness->continue_args()
                                                    : request.harness->resume_args(resume_id);
            }
        } catch (const std::exception& error) {
            connection->refuse("harness " + python_repr(harness_name) + " cannot " + (fork ? "fork" : "resume") +
                               ": " + error.what());
            return;
        }
    }
    if (!request.launch_args.empty() && !resume_id.empty() && !fork) {
        request.session_id = resume_id;
    }
    attach_or_create(connection, std::move(request));
}

void Server::attach_or_create(const std::shared_ptr<WsConnection>& connection, LaunchRequest request) {
    if (shutting_down_ || !connection->open()) {
        return;
    }
    auto hub = find_hub(request.name);
    if (!hub) {
        create_hub(connection, request);
        return;
    }
    asio::post(blocking_pool_, [this, connection, hub, request = std::move(request)]() mutable {
        bool alive = false;
        try {
            alive = hub->alive_blocking();
        } catch (const std::exception&) {
        }
        asio::post(io_, [this, connection, hub, request = std::move(request), alive]() mutable {
            on_liveness(connection, hub, std::move(request), alive);
        });
    });
}

void Server::on_liveness(const std::shared_ptr<WsConnection>& connection, const std::shared_ptr<Hub>& hub,
                         LaunchRequest request, bool alive) {
    if (shutting_down_ || !connection->open()) {
        return;
    }
    if (find_hub(request.name) != hub) {
        attach_or_create(connection, std::move(request));
        return;
    }
    if (!alive) {
        take_hub(request.name);
        hub->stop({});
        create_hub(connection, request);
        return;
    }
    if (!request.launch_args.empty() && hub->launch_args() != request.launch_args) {
        connection->refuse("session " + python_repr(request.name) + " is already running with different launch args");
        return;
    }
    hub->attach(connection, request.mode);
    connection->bind(hub);
}

void Server::create_hub(const std::shared_ptr<WsConnection>& connection, const LaunchRequest& request) {
    auto hub = std::make_shared<Hub>(io_, request, latest_unfinished(request.name));
    hubs_.emplace_back(request.name, hub);
    all_hubs_.push_back(hub);
    std::weak_ptr<Hub> weak_hub = hub;
    hub->launch([this, connection, weak_hub, mode = request.mode](bool usable) {
        auto started = weak_hub.lock();
        if (!started || shutting_down_) {
            return;
        }
        if (!usable) {
            connection->abort();
            return;
        }
        if (!connection->open()) {
            return;
        }
        started->attach(connection, mode);
        connection->bind(started);
    });
}

void Server::handle_http(Request request, Responder respond) {
    const RequestInfo info{request.version(), request.keep_alive(), request.method() == http::verb::head};
    const std::string_view raw_path = split_target(as_std(request.target())).first;
    const std::string path = url_decode(raw_path, false);
    const auto method = request.method();
    const bool readable = method == http::verb::get || method == http::verb::head;
    if (path == "/") {
        if (!readable) {
            respond(method_not_allowed(info, "GET,HEAD"));
            return;
        }
        serve_index(info, respond);
        return;
    }
    if (path == "/sessions") {
        if (!readable) {
            respond(method_not_allowed(info, "GET,HEAD"));
            return;
        }
        list_sessions(info, std::move(respond));
        return;
    }
    if (path == "/ws") {
        if (!readable) {
            respond(method_not_allowed(info, "GET,HEAD"));
            return;
        }
        const auto upgrade = request.find(http::field::upgrade);
        const std::string shown =
            upgrade == request.end() ? std::string("None") : std::string(as_std(upgrade->value()));
        respond(make_response(info, http::status::bad_request, text_content_type,
                              "No WebSocket UPGRADE hdr: " + shown + "\n Can \"Upgrade\" only to \"WebSocket\"."));
        return;
    }
    const bool names_one_session = raw_path.size() > sessions_route_prefix.size() &&
                                   raw_path.substr(0, sessions_route_prefix.size()) == sessions_route_prefix &&
                                   raw_path.find('/', sessions_route_prefix.size()) == std::string_view::npos;
    if (names_one_session) {
        if (method != http::verb::delete_) {
            respond(method_not_allowed(info, "DELETE"));
            return;
        }
        delete_session(info, url_decode(raw_path.substr(sessions_route_prefix.size()), false), std::move(respond));
        return;
    }
    respond(status_response(info, http::status::not_found));
}

void Server::serve_index(const RequestInfo& info, const Responder& respond) const {
    std::ifstream file;
    try {
        file.open(web_root() / "web" / "index.html", std::ios::binary);
    } catch (const std::exception&) {
    }
    if (!file) {
        respond(status_response(info, http::status::not_found));
        return;
    }
    std::ostringstream content;
    content << file.rdbuf();
    respond(make_response(info, http::status::ok, html_content_type, content.str()));
}

void Server::list_sessions(const RequestInfo& info, Responder respond) {
    std::vector<HubSnapshot> snapshot;
    snapshot.reserve(hubs_.size());
    for (const auto& [name, hub] : hubs_) {
        snapshot.push_back(HubSnapshot{name, hub, hub->client_count()});
    }
    asio::post(blocking_pool_, [this, info, snapshot = std::move(snapshot), respond = std::move(respond)]() mutable {
        OrderedJson described = OrderedJson::object();
        for (const auto& entry : snapshot) {
            try {
                described[entry.name] = entry.hub->describe_blocking(entry.clients);
            } catch (const std::exception&) {
            }
        }
        snapshot.clear();
        asio::post(io_, [info, body = python_json(described), respond = std::move(respond)]() mutable {
            respond(make_response(info, http::status::ok, json_content_type, std::move(body)));
        });
    });
}

void Server::delete_session(const RequestInfo& info, const std::string& name, Responder respond) {
    auto hub = take_hub(name);
    if (!hub) {
        OrderedJson body = OrderedJson::object();
        body["ok"] = false;
        body["error"] = "unknown session " + name;
        respond(json_response(info, http::status::not_found, body));
        return;
    }
    hub->stop([info, name, respond = std::move(respond)] {
        OrderedJson body = OrderedJson::object();
        body["ok"] = true;
        body["session"] = name;
        respond(json_response(info, http::status::ok, body));
    });
}

}

int run_server(const ServerOptions& options, const Registry& registry) {
    asio::io_context io;
    Server server(io, options, registry);
    std::cout << "ccbridge on http://" << options.host << ":" << options.port
              << "/  (ws: /ws?session=NAME&harness=" << join(registry.names(), "|") << ")" << std::endl;
    if (!server.listen()) {
        return 1;
    }
    server.start();
    while (true) {
        try {
            io.run();
            break;
        } catch (const std::exception& error) {
            std::cerr << "ccbridge: " << error.what() << std::endl;
        }
    }
    server.join_workers();
    return 0;
}

}
