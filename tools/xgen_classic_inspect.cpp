#include "nanoxgen/xgen_classic.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace nanoxgen;

namespace {

std::string escape_json(std::string_view value) {
    std::string result;
    for (const char character : value) {
        switch (character) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(character) < 0x20u) {
                    std::ostringstream escaped;
                    escaped << "\\u" << std::hex << std::setw(4)
                            << std::setfill('0')
                            << static_cast<unsigned int>(
                                   static_cast<unsigned char>(character));
                    result += escaped.str();
                } else {
                    result.push_back(character);
                }
                break;
        }
    }
    return result;
}

bool contains_attribute_text(const ClassicDescription &description,
                             std::string_view needle) {
    const auto contains = [&](const std::vector<ClassicAttribute> &attributes) {
        return std::any_of(
            attributes.begin(), attributes.end(),
            [&](const ClassicAttribute &attribute) {
                return attribute.value.find(needle) != std::string::npos;
            });
    };
    if (contains(description.attributes)) { return true; }
    return std::any_of(
        description.objects.begin(), description.objects.end(),
        [&](const ClassicObject &object) { return contains(object.attributes); });
}

bool has_object(const ClassicDescription &description, std::string_view type) {
    return std::any_of(
        description.objects.begin(), description.objects.end(),
        [&](const ClassicObject &object) { return object.type == type; });
}

bool object_is_active(const ClassicObject &object) {
    const ClassicAttribute *active = find_classic_attribute(
        object.attributes, "active");
    return active == nullptr || (active->value != "false" && active->value != "False" &&
                                 active->value != "0");
}

std::vector<std::string> requirements(const ClassicDescription &description) {
    std::vector<std::string> result{"subd-patch", "embedded-guides"};
    if (has_object(description, "RandomGenerator")) {
        result.push_back("random-root-sampling");
    }
    for (const ClassicObject &object : description.objects) {
        if (object.type.ends_with("FXModule") && object_is_active(object)) {
            result.push_back(object.type);
        }
    }
    const std::pair<std::string_view, std::string_view> expressions[] = {
        {"map(", "ptex-map"}, {"rampUI(", "ramp"}, {"hash(", "hash"},
        {"rand(", "rand"}, {"?", "ternary-expression"},
    };
    for (const auto &[needle, label] : expressions) {
        if (contains_attribute_text(description, needle)) {
            result.emplace_back(label);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void write_string_array(const std::vector<std::string> &values) {
    std::cout << '[';
    for (std::size_t index = 0u; index < values.size(); ++index) {
        if (index != 0u) { std::cout << ','; }
        std::cout << '"' << escape_json(values[index]) << '"';
    }
    std::cout << ']';
}

} // namespace

int main(int argc, char **argv) try {
    std::filesystem::path path;
    std::string selected_description;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--description" && index + 1 < argc) {
            selected_description = argv[++index];
        } else if (path.empty()) {
            path = argument;
        } else {
            throw std::invalid_argument("unexpected argument: " + argument);
        }
    }
    if (path.empty()) {
        std::cerr << "usage: nanoxgen_xgen_classic_inspect "
                     "[--description NAME] <collection.xgen>\n";
        return 2;
    }

    const ClassicCollection collection = load_xgen_classic_collection(path);
    if (!selected_description.empty() &&
        find_classic_description(collection, selected_description) == nullptr) {
        throw std::invalid_argument(
            "description not found: " + selected_description);
    }
    const ClassicAttribute *palette_name = find_classic_attribute(
        collection.palette_attributes, "name");
    std::cout << "{\"file_version\":" << collection.file_version
              << ",\"palette\":\""
              << escape_json(palette_name == nullptr ? "" : palette_name->value)
              << "\",\"description_count\":" << collection.descriptions.size()
              << ",\"descriptions\":[";
    std::size_t output_index = 0u;
    for (const ClassicDescription &description : collection.descriptions) {
        if (!selected_description.empty() &&
            description.name != selected_description) {
            continue;
        }
        if (output_index++ != 0u) { std::cout << ','; }
        std::map<std::string, std::size_t> object_counts;
        std::size_t face_ids = 0u;
        std::size_t guides = 0u;
        std::size_t guide_cvs = 0u;
        for (const ClassicObject &object : description.objects) {
            ++object_counts[object.type];
        }
        for (const ClassicPatch &patch : description.patches) {
            face_ids += patch.face_ids.size();
            guides += patch.guides.size();
            guide_cvs += patch.guide_cvs.size();
        }
        std::cout << "{\"name\":\"" << escape_json(description.name)
                  << "\",\"object_count\":" << description.objects.size()
                  << ",\"objects\":{";
        std::size_t object_index = 0u;
        for (const auto &[type, count] : object_counts) {
            if (object_index++ != 0u) { std::cout << ','; }
            std::cout << '"' << escape_json(type) << "\":" << count;
        }
        std::cout << "},\"binding_count\":" << description.bindings.size()
                  << ",\"patch_count\":" << description.patches.size()
                  << ",\"face_id_count\":" << face_ids
                  << ",\"guide_count\":" << guides
                  << ",\"guide_cv_count\":" << guide_cvs
                  << ",\"native_generation_ready\":false,\"requirements\":";
        write_string_array(requirements(description));
        std::cout << '}';
    }
    std::cout << "]}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
