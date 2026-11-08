#pragma once

#include "harness/harness.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ccb {

class Registry {
public:
    static Registry load(const std::vector<std::filesystem::path>& dirs);
    std::shared_ptr<const Harness> get(const std::string& name) const;
    std::vector<std::string> names() const;
    const std::string& default_name() const;

private:
    std::map<std::string, std::shared_ptr<const Harness>> harnesses_;
    std::string default_;
};

std::shared_ptr<const Harness> parse_template(const std::filesystem::path& file);

}
