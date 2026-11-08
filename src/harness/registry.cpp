#include "harness/registry.hpp"

#include "common/paths.hpp"
#include "common/text.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace ccb {

namespace {

using nlohmann::json;

class TemplateError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void reject(const std::string& reason) {
    throw TemplateError(reason);
}

bool is_template_name(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    auto lower_or_digit = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!lower_or_digit(name.front())) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [&](char c) { return lower_or_digit(c) || c == '-'; });
}

std::optional<std::string> env_lookup(const std::string& name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name.c_str()) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

std::vector<std::string> string_array(const json& value, const std::string& field) {
    if (!value.is_array()) {
        reject("'" + field + "' must be an array of strings");
    }
    std::vector<std::string> words;
    words.reserve(value.size());
    for (const auto& item : value) {
        if (!item.is_string()) {
            reject("'" + field + "' must be an array of strings");
        }
        words.push_back(item.get<std::string>());
    }
    return words;
}

std::vector<std::string> optional_string_array(const json& document, const std::string& field) {
    auto found = document.find(field);
    if (found == document.end()) {
        return {};
    }
    return string_array(*found, field);
}

std::optional<std::vector<std::string>> optional_form(const json& document, const std::string& field) {
    auto found = document.find(field);
    if (found == document.end() || found->is_null()) {
        return std::nullopt;
    }
    return string_array(*found, field);
}

std::string required_string(const json& object, const std::string& field, const std::string& where) {
    auto found = object.find(field);
    if (found == object.end() || !found->is_string()) {
        reject("'" + where + field + "' must be a string");
    }
    return found->get<std::string>();
}

std::vector<ArgRule> parse_arg_rules(const json& document) {
    std::vector<ArgRule> rules;
    auto found = document.find("argv_env");
    if (found == document.end()) {
        return rules;
    }
    if (!found->is_array()) {
        reject("'argv_env' must be an array of rule objects");
    }
    for (const auto& item : *found) {
        if (!item.is_object()) {
            reject("'argv_env' must be an array of rule objects");
        }
        ArgRule rule;
        rule.env = required_string(item, "env", "argv_env.");
        if (rule.env.empty()) {
            reject("'argv_env.env' must not be empty");
        }
        auto equals = item.find("equals");
        if (equals != item.end()) {
            if (!equals->is_string()) {
                reject("'argv_env.equals' must be a string");
            }
            rule.equals = equals->get<std::string>();
        }
        auto append = item.find("append");
        if (append == item.end()) {
            reject("'argv_env.append' is required");
        }
        rule.append = string_array(*append, "argv_env.append");
        rules.push_back(std::move(rule));
    }
    return rules;
}

void apply_arg_rules(Harness& harness) {
    for (const auto& rule : harness.argv_env) {
        auto value = env_lookup(rule.env);
        if (!value) {
            continue;
        }
        bool matches = rule.equals ? *value == *rule.equals : !value->empty();
        if (!matches) {
            continue;
        }
        for (const auto& word : rule.append) {
            harness.argv.push_back(replace_all(word, "$VALUE", *value));
        }
    }
}

TranscriptSpec parse_transcript_spec(const json& document) {
    auto found = document.find("transcript");
    if (found == document.end() || !found->is_object()) {
        reject("'transcript' must be an object");
    }
    TranscriptSpec spec;
    spec.kind = required_string(*found, "kind", "transcript.");
    spec.root = required_string(*found, "root", "transcript.");
    auto root_env = found->find("root_env");
    if (root_env != found->end()) {
        if (!root_env->is_string()) {
            reject("'transcript.root_env' must be a string");
        }
        spec.root_env = root_env->get<std::string>();
    }
    auto notices = found->find("notices");
    if (notices != found->end()) {
        spec.notices = string_array(*notices, "transcript.notices");
    }
    return spec;
}

std::shared_ptr<const Harness> build_harness(const std::string& name, const json& document) {
    if (!document.is_object()) {
        reject("template must be a JSON object");
    }
    auto harness = std::make_shared<Harness>();
    harness->name = name;
    auto argv = document.find("argv");
    if (argv == document.end()) {
        reject("'argv' is required");
    }
    harness->argv = string_array(*argv, "argv");
    if (harness->argv.empty() || harness->argv.front().empty()) {
        reject("'argv' must be a non-empty array of strings");
    }
    harness->argv_env = parse_arg_rules(document);
    harness->env_strip = optional_string_array(document, "env_strip");
    harness->ready_markers = optional_string_array(document, "ready");
    harness->busy_markers = optional_string_array(document, "busy");
    if (auto term = document.find("term"); term != document.end()) {
        if (!term->is_string() || term->get_ref<const std::string&>().empty()) {
            reject("'term' must be a non-empty string");
        }
        harness->term = term->get<std::string>();
    }
    if (auto delay = document.find("submit_delay"); delay != document.end()) {
        if (!delay->is_number() || delay->get<double>() < 0.0) {
            reject("'submit_delay' must be a non-negative number");
        }
        harness->submit_delay = delay->get<double>();
    }
    harness->resume_form = optional_form(document, "resume");
    harness->continue_form = optional_form(document, "continue");
    harness->fork_form = optional_form(document, "fork");
    harness->fork_continue_form = optional_form(document, "fork_continue");
    if (auto is_default = document.find("default"); is_default != document.end()) {
        if (!is_default->is_boolean()) {
            reject("'default' must be a boolean");
        }
        harness->is_default = is_default->get<bool>();
    }
    auto spec = parse_transcript_spec(document);
    auto transcript = make_transcript(spec);
    if (!transcript) {
        reject("transcript kind '" + spec.kind + "' is not supported");
    }
    harness->transcript = std::shared_ptr<const Transcript>(std::move(transcript));
    apply_arg_rules(*harness);
    return harness;
}

}

std::shared_ptr<const Harness> parse_template(const std::filesystem::path& file) {
    auto label = file.string();
    try {
        auto name = file.stem().string();
        if (!is_template_name(name)) {
            reject("template name '" + name + "' must match ^[a-z0-9][a-z0-9-]*$");
        }
        std::ifstream stream(file, std::ios::binary);
        if (!stream) {
            reject("cannot be read");
        }
        auto document = json::parse(stream, nullptr, false);
        if (document.is_discarded()) {
            reject("is not valid JSON");
        }
        return build_harness(name, document);
    } catch (const TemplateError& error) {
        throw std::runtime_error("harness template " + label + ": " + error.what());
    } catch (const std::exception& error) {
        throw std::runtime_error("harness template " + label + ": " + error.what());
    }
}

Registry Registry::load(const std::vector<std::filesystem::path>& dirs) {
    Registry registry;
    std::string first_default;
    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            continue;
        }
        std::vector<std::filesystem::path> files;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code type_ec;
            if (it->path().extension() == ".json" && it->is_regular_file(type_ec)) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end(),
                  [](const auto& left, const auto& right) { return left.filename() < right.filename(); });
        for (const auto& file : files) {
            auto name = file.stem().string();
            if (registry.harnesses_.count(name) != 0) {
                continue;
            }
            auto harness = parse_template(file);
            if (harness->is_default && first_default.empty()) {
                first_default = name;
            }
            registry.harnesses_.emplace(name, std::move(harness));
        }
    }
    if (!first_default.empty()) {
        registry.default_ = first_default;
    } else if (registry.harnesses_.count("claude") != 0) {
        registry.default_ = "claude";
    } else if (!registry.harnesses_.empty()) {
        registry.default_ = registry.harnesses_.begin()->first;
    }
    return registry;
}

std::shared_ptr<const Harness> Registry::get(const std::string& name) const {
    auto found = harnesses_.find(name);
    if (found != harnesses_.end()) {
        return found->second;
    }
    std::string known;
    for (const auto& entry : harnesses_) {
        if (!known.empty()) {
            known += ", ";
        }
        known += entry.first;
    }
    throw std::runtime_error("unknown harness '" + name + "'; known: " + known);
}

std::vector<std::string> Registry::names() const {
    std::vector<std::string> result;
    result.reserve(harnesses_.size());
    for (const auto& entry : harnesses_) {
        result.push_back(entry.first);
    }
    return result;
}

const std::string& Registry::default_name() const {
    if (harnesses_.empty()) {
        throw std::runtime_error("no harness templates loaded");
    }
    return default_;
}

}
