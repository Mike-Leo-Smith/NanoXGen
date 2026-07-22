#include <xgen/src/xgcore/XgExpression.h>
#include <xgen/src/sggeom/SgRampUIComp.h>

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Options {
    std::string name{"nanoxgenOracle"};
    std::string object_type{"NanoXGenOracle"};
    std::string expression;
    std::string ramp;
    std::string patch_name;
    std::vector<std::pair<std::string, double>> variables;
    double u{};
    double v{};
    double t{};
    int face_id{};
    int repeat{1};
};

double parse_double(std::string_view text, const char *what) {
    std::string storage{text};
    char *end = nullptr;
    const double value = std::strtod(storage.c_str(), &end);
    if (end != storage.c_str() + storage.size() || !std::isfinite(value)) {
        throw std::runtime_error(std::string{"invalid "} + what + ": " + storage);
    }
    return value;
}

int parse_int(std::string_view text, const char *what) {
    std::string storage{text};
    char *end = nullptr;
    const long value = std::strtol(storage.c_str(), &end, 10);
    if (end != storage.c_str() + storage.size()) {
        throw std::runtime_error(std::string{"invalid "} + what + ": " + storage);
    }
    return static_cast<int>(value);
}

Options parse_options(int argc, char **argv) {
    Options options{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        auto value = [&](const char *name) -> std::string_view {
            if (++i >= argc) {
                throw std::runtime_error(std::string{"missing value for "} + name);
            }
            return argv[i];
        };
        if (arg == "--expression") {
            options.expression = value("--expression");
        } else if (arg == "--ramp") {
            options.ramp = value("--ramp");
        } else if (arg == "--name") {
            options.name = value("--name");
        } else if (arg == "--object-type") {
            options.object_type = value("--object-type");
        } else if (arg == "--patch-name") {
            options.patch_name = value("--patch-name");
        } else if (arg == "--var") {
            const std::string assignment{value("--var")};
            const std::size_t equal = assignment.find('=');
            if (equal == 0u || equal == std::string::npos) {
                throw std::runtime_error("--var expects NAME=VALUE");
            }
            std::string name = assignment.substr(0u, equal);
            if (!name.empty() && name.front() == '$') { name.erase(name.begin()); }
            options.variables.emplace_back(
                std::move(name), parse_double(assignment.substr(equal + 1u), "variable"));
        } else if (arg == "--u") {
            options.u = parse_double(value("--u"), "u");
        } else if (arg == "--v") {
            options.v = parse_double(value("--v"), "v");
        } else if (arg == "--t") {
            options.t = parse_double(value("--t"), "t");
        } else if (arg == "--face-id") {
            options.face_id = parse_int(value("--face-id"), "face ID");
        } else if (arg == "--repeat") {
            options.repeat = parse_int(value("--repeat"), "repeat count");
            if (options.repeat <= 0 || options.repeat > 1000000) {
                throw std::runtime_error("repeat count must be in [1, 1000000]");
            }
        } else {
            throw std::runtime_error("unknown argument: " + std::string{arg});
        }
    }
    if (options.expression.empty() == options.ramp.empty()) {
        throw std::runtime_error(
            "exactly one of --expression or --ramp is required");
    }
    return options;
}

} // namespace

int main(int argc, char **argv) try {
    const Options options = parse_options(argc, argv);
    if (!options.ramp.empty()) {
        SgRampUIComp ramp;
        if (!ramp.init(options.ramp)) {
            throw std::runtime_error("XGen rejected the rampUI encoding");
        }
        const float value = ramp.getValue(static_cast<float>(options.t));
        if (!std::isfinite(value)) {
            throw std::runtime_error("XGen ramp produced a non-finite value");
        }
        std::cout << std::setprecision(9)
                  << "{\"t\":" << static_cast<float>(options.t)
                  << ",\"value\":" << value << "}\n";
        return 0;
    }
    XgExpression::initTLS();
    struct Cleanup {
        ~Cleanup() {
            XgExpression::removeAllCustomVariables();
            XgExpression::clearTLS();
        }
    } cleanup;
    for (const auto &[name, value] : options.variables) {
        XgExpression::setCustomVariable(name, value);
    }

    XgExpression expression{
        options.name, options.object_type, nullptr, options.expression, "float"};
    std::string diagnostic;
    if (!expression.syntaxOK(&diagnostic)) {
        throw std::runtime_error("XGen syntax error: " + diagnostic);
    }
    if (!expression.isValid(&diagnostic)) {
        throw std::runtime_error("XGen binding error: " + diagnostic);
    }

    std::cout << std::setprecision(17)
              << "{\"expression_seed\":" << expression.exprSeed()
              << ",\"values\":[";
    for (int i = 0; i < options.repeat; ++i) {
        const std::string *patch = options.patch_name.empty()
                                       ? nullptr
                                       : &options.patch_name;
        const SgVec3d result =
            expression.eval(options.u, options.v, options.face_id, patch);
        if (i != 0) { std::cout << ','; }
        std::cout << result[0];
    }
    std::cout << "],\"face_seed\":" << expression.faceSeed() << "}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
