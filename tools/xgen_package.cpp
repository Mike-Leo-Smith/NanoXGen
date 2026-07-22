#include "nanoxgen/xgen_package.h"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace nanoxgen;

namespace {

std::string escape_json(const std::string &value) {
    std::string result;
    for (const char c : value) {
        switch (c) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20u) {
                    std::ostringstream escaped;
                    escaped << "\\u" << std::hex << std::setw(4)
                            << std::setfill('0')
                            << static_cast<unsigned int>(static_cast<unsigned char>(c));
                    result += escaped.str();
                } else {
                    result.push_back(c);
                }
                break;
        }
    }
    return result;
}

} // namespace

int main(int argc, char **argv) try {
    XGenPackageOptions options{};
    bool require_complete = false;
    std::filesystem::path root;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--require-complete") {
            require_complete = true;
        } else if (argument == "--var" && index + 1 < argc) {
            const std::string assignment = argv[++index];
            const std::size_t equals = assignment.find('=');
            if (equals == std::string::npos || equals == 0u) {
                throw std::invalid_argument("--var requires NAME=PATH");
            }
            options.variables[assignment.substr(0u, equals)] =
                assignment.substr(equals + 1u);
        } else if (root.empty()) {
            root = argument;
        } else {
            throw std::invalid_argument("unexpected argument: " + argument);
        }
    }
    if (root.empty()) {
        std::cerr << "usage: nanoxgen_xgen_package [--require-complete] "
                     "[--var NAME=PATH]... <asset-file-or-directory>\n";
        return 2;
    }
    const XGenPackageManifest manifest = scan_xgen_package(root, options);
    std::cout << "{\"root\":\"" << escape_json(manifest.root.string())
              << "\",\"backend\":\"" << to_string(manifest.backend)
              << "\",\"dependency_closure_complete\":"
              << (manifest.dependency_closure_complete ? "true" : "false")
              << ",\"files\":[";
    for (std::size_t index = 0u; index < manifest.files.size(); ++index) {
        const XGenPackageFile &file = manifest.files[index];
        if (index != 0u) { std::cout << ','; }
        std::cout << "{\"path\":\"" << escape_json(file.relative_path.string())
                  << "\",\"kind\":\"" << to_string(file.kind)
                  << "\",\"bytes\":" << file.byte_size << '}';
    }
    std::cout << "],\"references\":[";
    for (std::size_t index = 0u; index < manifest.references.size(); ++index) {
        const XGenPackageReference &reference = manifest.references[index];
        if (index != 0u) { std::cout << ','; }
        std::cout << "{\"source\":\"" << escape_json(reference.source.string())
                  << "\",\"literal\":\"" << escape_json(reference.literal)
                  << "\",\"status\":\"" << to_string(reference.status) << '"';
        if (reference.resolved_path) {
            std::cout << ",\"resolved_path\":\""
                      << escape_json(reference.resolved_path->string()) << '"';
        }
        std::cout << '}';
    }
    std::cout << "],\"diagnostics\":[";
    for (std::size_t index = 0u; index < manifest.diagnostics.size(); ++index) {
        if (index != 0u) { std::cout << ','; }
        std::cout << '"' << escape_json(manifest.diagnostics[index]) << '"';
    }
    std::cout << "]}\n";
    return require_complete && !manifest.dependency_closure_complete ? 3 : 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
