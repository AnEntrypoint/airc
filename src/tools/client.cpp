#include "tools/tools.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>

namespace ccb {

namespace {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

constexpr const char* default_prompt = "Reply with exactly: BRIDGE-OK";
constexpr auto connect_timeout = std::chrono::seconds(30);

struct Endpoint {
    std::string host;
    std::string port;
    std::string target;
};

std::optional<Endpoint> parse_ws_url(const std::string& url) {
    static const std::string scheme = "ws://";
    if (url.compare(0, scheme.size(), scheme) != 0) return std::nullopt;
    const std::string rest = url.substr(scheme.size());
    const std::size_t slash = rest.find_first_of("/?");
    const std::string authority = rest.substr(0, slash);
    std::string target = slash == std::string::npos ? "/" : rest.substr(slash);
    if (target.front() == '?') target.insert(0, "/");
    Endpoint endpoint;
    endpoint.target = target;
    endpoint.port = "80";
    std::size_t colon = std::string::npos;
    if (!authority.empty() && authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) return std::nullopt;
        endpoint.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') colon = close + 1;
    } else {
        colon = authority.rfind(':');
        endpoint.host = authority.substr(0, colon);
    }
    if (colon != std::string::npos) endpoint.port = authority.substr(colon + 1);
    if (endpoint.host.empty() || endpoint.port.empty()) return std::nullopt;
    return endpoint;
}

std::string percent_encode(const std::string& value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out.push_back(raw);
        } else {
            out.push_back('%');
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0x0F]);
        }
    }
    return out;
}

std::string build_target(const Endpoint& endpoint, const ClientOptions& options) {
    std::string query = "session=" + percent_encode(options.session);
    if (!options.harness.empty()) query += "&harness=" + percent_encode(options.harness);
    query += "&kinds=" + percent_encode(options.kinds);
    if (!options.cwd.empty()) query += "&cwd=" + percent_encode(options.cwd);
    const char joiner = endpoint.target.find('?') == std::string::npos ? '?' : '&';
    return endpoint.target + joiner + query;
}

void emit(const std::string& text) {
    std::fputs(text.c_str(), stdout);
    std::fflush(stdout);
}

enum class TurnOutcome { Ended, Closed };

TurnOutcome read_turn(websocket::stream<beast::tcp_stream>& socket, double turn_timeout) {
    beast::flat_buffer buffer;
    while (true) {
        beast::get_lowest_layer(socket).expires_after(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(turn_timeout)));
        buffer.clear();
        beast::error_code error;
        socket.read(buffer, error);
        if (error == websocket::error::closed) {
            emit("\n[closed]\n");
            return TurnOutcome::Closed;
        }
        if (error) throw beast::system_error(error);
        if (!socket.got_text()) {
            emit("\n[closed]\n");
            return TurnOutcome::Closed;
        }
        const std::string payload = beast::buffers_to_string(buffer.data());
        if (payload.empty()) {
            emit("\n<< turn end >>\n");
            return TurnOutcome::Ended;
        }
        emit(payload);
    }
}

}

int run_client(const ClientOptions& options) {
    const auto endpoint = parse_ws_url(options.url);
    if (!endpoint) {
        std::fprintf(stderr, "unsupported url %s (expected ws://host:port/path)\n", options.url.c_str());
        return 1;
    }
    try {
        net::io_context io;
        tcp::resolver resolver(io);
        websocket::stream<beast::tcp_stream> socket(io);
        beast::get_lowest_layer(socket).expires_after(connect_timeout);
        const auto addresses = resolver.resolve(endpoint->host, endpoint->port);
        const auto connected = beast::get_lowest_layer(socket).connect(addresses);
        beast::get_lowest_layer(socket).expires_after(connect_timeout);
        const std::string host_header =
            endpoint->host.find(':') != std::string::npos
                ? "[" + endpoint->host + "]:" + std::to_string(connected.port())
                : endpoint->host + ":" + std::to_string(connected.port());
        socket.handshake(host_header, build_target(*endpoint, options));
        socket.text(true);

        std::vector<std::string> prompts = options.prompts;
        if (prompts.empty()) prompts.emplace_back(default_prompt);

        for (const std::string& prompt : prompts) {
            emit("\n>> " + prompt + "\n");
            beast::get_lowest_layer(socket).expires_never();
            socket.write(net::buffer(prompt));
            if (read_turn(socket, options.turn_timeout) == TurnOutcome::Closed) return 0;
        }
        beast::error_code ignored;
        beast::get_lowest_layer(socket).expires_never();
        socket.close(websocket::close_code::normal, ignored);
        return 0;
    } catch (const beast::system_error& error) {
        if (error.code() == beast::error::timeout) {
            std::fprintf(stderr, "timed out after %.0f seconds waiting for the bridge\n", options.turn_timeout);
        } else {
            std::fprintf(stderr, "%s\n", error.what());
        }
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

}
