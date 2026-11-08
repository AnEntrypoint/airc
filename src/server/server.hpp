#pragma once

#include "harness/registry.hpp"

#include <cstdint>
#include <string>

namespace ccb {

struct ServerOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8787;
    std::string default_cwd;
    double idle_reap_seconds = 1800.0;
};

int run_server(const ServerOptions& options, const Registry& registry);

}
