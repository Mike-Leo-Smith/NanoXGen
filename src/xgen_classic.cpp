#include "nanoxgen/xgen_classic.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace nanoxgen {
namespace {

[[noreturn]] void fail(std::size_t line, const std::string &message) {
    throw std::runtime_error(
        "Classic XGen line " + std::to_string(line) + ": " + message);
}

void check_limit(std::size_t value, std::size_t limit, std::size_t line,
                 const char *label) {
    if (value > limit) {
        fail(line, std::string(label) + " limit exceeded");
    }
}

std::string_view trim_left(std::string_view value) {
    const std::size_t first = value.find_first_not_of(" \t\v\f");
    return first == std::string_view::npos ? std::string_view{} : value.substr(first);
}

std::string_view trim_right(std::string_view value) {
    const std::size_t last = value.find_last_not_of(" \t\v\f\r");
    return last == std::string_view::npos ? std::string_view{} : value.substr(0u, last + 1u);
}

std::string_view trim(std::string_view value) {
    return trim_right(trim_left(value));
}

bool starts_with_space(std::string_view value) {
    return !value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\v' || value.front() == '\f');
}

std::pair<std::string_view, std::string_view> split_key_value(
    std::string_view line) {
    line = trim(line);
    const std::size_t separator = line.find_first_of(" \t\v\f");
    if (separator == std::string_view::npos) {
        return {line, {}};
    }
    const std::string_view key = line.substr(0u, separator);
    return {key, trim(line.substr(separator + 1u))};
}

template <typename Integer>
Integer parse_integer(std::string_view value, std::size_t line,
                      const char *label) {
    value = trim(value);
    Integer result{};
    const auto converted = std::from_chars(
        value.data(), value.data() + value.size(), result);
    if (value.empty() || converted.ec != std::errc{} ||
        converted.ptr != value.data() + value.size()) {
        fail(line, std::string("invalid ") + label);
    }
    return result;
}

double parse_float(std::string_view value, std::size_t line,
                   const char *label) {
    value = trim(value);
    double result{};
    const auto converted = std::from_chars(
        value.data(), value.data() + value.size(), result,
        std::chars_format::general);
    if (value.empty() || converted.ec != std::errc{} ||
        converted.ptr != value.data() + value.size() || !std::isfinite(result)) {
        fail(line, std::string("invalid or non-finite ") + label);
    }
    return result;
}

std::vector<std::string_view> tokens(std::string_view value) {
    std::vector<std::string_view> result;
    while (true) {
        value = trim_left(value);
        if (value.empty()) { return result; }
        const std::size_t end = value.find_first_of(" \t\v\f\r");
        if (end == std::string_view::npos) {
            result.push_back(value);
            return result;
        }
        result.push_back(value.substr(0u, end));
        value.remove_prefix(end + 1u);
    }
}

class BoundedLineReader {
public:
    BoundedLineReader(std::istream &input, const ClassicParseLimits &limits)
        : buffer_(input.rdbuf()), limits_(limits) {}

    bool next(std::string &line) {
        line.clear();
        using traits = std::char_traits<char>;
        while (true) {
            const auto next = buffer_->sbumpc();
            if (traits::eq_int_type(next, traits::eof())) {
                if (line.empty()) { return false; }
                ++line_number_;
                if (!line.empty() && line.back() == '\r') { line.pop_back(); }
                return true;
            }
            ++bytes_read_;
            if (bytes_read_ > limits_.max_source_bytes) {
                fail(line_number_ + 1u, "source byte limit exceeded");
            }
            const char character = traits::to_char_type(next);
            if (character == '\0') {
                fail(line_number_ + 1u, "NUL byte in text collection");
            }
            if (character == '\n') {
                ++line_number_;
                if (!line.empty() && line.back() == '\r') { line.pop_back(); }
                return true;
            }
            if (line.size() == limits_.max_line_bytes) {
                fail(line_number_ + 1u, "line byte limit exceeded");
            }
            line.push_back(character);
        }
    }

    std::size_t line_number() const noexcept { return line_number_; }

private:
    std::streambuf *buffer_{};
    const ClassicParseLimits &limits_;
    std::uint64_t bytes_read_{};
    std::size_t line_number_{};
};

bool next_content_line(BoundedLineReader &reader, std::string &line) {
    while (reader.next(line)) {
        const std::string_view value = trim(line);
        if (!value.empty() && value.front() != '#') { return true; }
    }
    return false;
}

ClassicAttribute make_attribute(std::string_view line, std::size_t line_number) {
    const auto [name, value] = split_key_value(line);
    if (name.empty()) { fail(line_number, "empty attribute name"); }
    return {std::string(name), std::string(value), line_number};
}

const ClassicAttribute &required_unique_attribute(
    const std::vector<ClassicAttribute> &attributes, std::string_view name,
    std::size_t line) {
    const ClassicAttribute *result = nullptr;
    for (const ClassicAttribute &attribute : attributes) {
        if (attribute.name != name) { continue; }
        if (result != nullptr) {
            fail(attribute.source_line, "duplicate '" + std::string(name) + "' attribute");
        }
        result = &attribute;
    }
    if (result == nullptr || result->value.empty()) {
        fail(line, "missing '" + std::string(name) + "' attribute");
    }
    return *result;
}

std::vector<std::uint32_t> parse_face_ids(
    const ClassicAttribute &attribute, const ClassicParseLimits &limits,
    std::size_t &global_face_ids) {
    const std::vector<std::string_view> values = tokens(attribute.value);
    if (values.empty()) { fail(attribute.source_line, "empty faceIds attribute"); }
    const std::size_t count = parse_integer<std::size_t>(
        values.front(), attribute.source_line, "faceIds count");
    if (values.size() != count + 1u) {
        fail(attribute.source_line, "faceIds count does not match payload");
    }
    check_limit(count, limits.max_face_ids, attribute.source_line, "face ID");
    if (count > limits.max_face_ids - global_face_ids) {
        fail(attribute.source_line, "face ID limit exceeded");
    }
    global_face_ids += count;
    std::vector<std::uint32_t> result;
    result.reserve(count);
    std::unordered_set<std::uint32_t> unique;
    unique.reserve(count);
    for (std::size_t index = 0u; index < count; ++index) {
        const std::uint32_t face_id = parse_integer<std::uint32_t>(
            values[index + 1u], attribute.source_line, "face ID");
        if (!unique.insert(face_id).second) {
            fail(attribute.source_line, "duplicate face ID");
        }
        result.push_back(face_id);
    }
    return result;
}

std::vector<ClassicCulledPrimitiveFace> parse_culled_primitives(
    const ClassicAttribute &attribute,
    const std::unordered_set<std::uint32_t> &patch_faces,
    const ClassicParseLimits &limits,
    std::size_t &global_culled_primitives) {
    const std::vector<std::string_view> values = tokens(attribute.value);
    if (values.empty()) {
        fail(attribute.source_line, "empty culledPrims attribute");
    }
    const std::size_t face_count = parse_integer<std::size_t>(
        values.front(), attribute.source_line, "culledPrims face count");
    if (face_count > patch_faces.size()) {
        fail(attribute.source_line,
             "culledPrims face count exceeds declared patch faces");
    }
    std::size_t cursor = 1u;
    std::vector<ClassicCulledPrimitiveFace> result;
    result.reserve(face_count);
    std::unordered_set<std::uint32_t> unique_faces;
    unique_faces.reserve(face_count);
    for (std::size_t face_index = 0u; face_index < face_count; ++face_index) {
        if (cursor + 2u > values.size()) {
            fail(attribute.source_line, "truncated culledPrims face header");
        }
        ClassicCulledPrimitiveFace face{};
        face.face_id = parse_integer<std::uint32_t>(
            values[cursor++], attribute.source_line, "culledPrims face ID");
        if (!patch_faces.contains(face.face_id)) {
            fail(attribute.source_line,
                 "culledPrims face ID is not declared by its patch");
        }
        if (!unique_faces.insert(face.face_id).second) {
            fail(attribute.source_line, "duplicate culledPrims face ID");
        }
        const std::size_t primitive_count = parse_integer<std::size_t>(
            values[cursor++], attribute.source_line,
            "culledPrims primitive count");
        if (primitive_count > values.size() - cursor) {
            fail(attribute.source_line, "truncated culledPrims primitive IDs");
        }
        if (primitive_count >
            limits.max_culled_primitives - global_culled_primitives) {
            fail(attribute.source_line, "culled primitive limit exceeded");
        }
        global_culled_primitives += primitive_count;
        face.primitive_ids.reserve(primitive_count);
        for (std::size_t primitive = 0u; primitive < primitive_count;
             ++primitive) {
            const std::uint32_t primitive_id = parse_integer<std::uint32_t>(
                values[cursor++], attribute.source_line,
                "culled primitive ID");
            if (primitive_id == 0u) {
                fail(attribute.source_line,
                     "culled primitive ID must be one-based");
            }
            face.primitive_ids.push_back(primitive_id);
        }
        std::sort(face.primitive_ids.begin(), face.primitive_ids.end());
        if (std::adjacent_find(face.primitive_ids.begin(),
                               face.primitive_ids.end()) !=
            face.primitive_ids.end()) {
            fail(attribute.source_line, "duplicate culled primitive ID");
        }
        result.push_back(std::move(face));
    }
    if (cursor != values.size()) {
        fail(attribute.source_line, "culledPrims count does not match payload");
    }
    std::sort(result.begin(), result.end(),
              [](const ClassicCulledPrimitiveFace &a,
                 const ClassicCulledPrimitiveFace &b) {
                  return a.face_id < b.face_id;
              });
    return result;
}

std::vector<double> parse_interpolation(std::string_view value,
                                        std::size_t line) {
    std::vector<double> result;
    while (true) {
        const std::size_t separator = value.find(':');
        const std::string_view element = separator == std::string_view::npos
            ? value : value.substr(0u, separator);
        if (element.empty()) { fail(line, "empty guide interpolation value"); }
        result.push_back(parse_float(element, line, "guide interpolation value"));
        if (separator == std::string_view::npos) { break; }
        value.remove_prefix(separator + 1u);
    }
    return result;
}

ClassicFloat3 parse_float3(std::string_view value, std::size_t line,
                           const char *label) {
    const std::vector<std::string_view> values = tokens(value);
    if (values.size() != 3u) {
        fail(line, std::string(label) + " requires three values");
    }
    return {
        parse_float(values[0], line, label),
        parse_float(values[1], line, label),
        parse_float(values[2], line, label),
    };
}

ClassicAttribute next_named_attribute(BoundedLineReader &reader,
                                      std::string &line,
                                      std::string_view expected) {
    if (!next_content_line(reader, line)) {
        fail(reader.line_number() + 1u, "truncated guide");
    }
    if (!starts_with_space(line)) {
        fail(reader.line_number(), "expected guide '" + std::string(expected) + "'");
    }
    ClassicAttribute attribute = make_attribute(line, reader.line_number());
    if (attribute.name != expected) {
        fail(reader.line_number(), "expected guide '" + std::string(expected) + "'");
    }
    return attribute;
}

ClassicPatch parse_patch(BoundedLineReader &reader, std::string_view header,
                         const ClassicParseLimits &limits,
                         std::size_t &attribute_count,
                         std::size_t &global_face_ids,
                         std::size_t &global_culled_primitives,
                         std::size_t &global_guides,
                         std::size_t &global_cvs,
                         std::size_t &global_interpolation) {
    const std::vector<std::string_view> patch_header = tokens(header);
    if (patch_header.size() != 2u || patch_header[0] != "Patch") {
        fail(reader.line_number(), "invalid Patch header");
    }
    ClassicPatch patch{};
    patch.type = patch_header[1];
    patch.source_line = reader.line_number();

    std::string line;
    std::size_t declared_guides = std::numeric_limits<std::size_t>::max();
    while (next_content_line(reader, line)) {
        const std::string_view value = trim(line);
        if (!starts_with_space(line)) {
            const std::vector<std::string_view> guide_header = tokens(value);
            if (guide_header.size() != 3u || guide_header[0] != "Guides" ||
                guide_header[1] != "Spline") {
                fail(reader.line_number(), "expected Guides Spline header");
            }
            declared_guides = parse_integer<std::size_t>(
                guide_header[2], reader.line_number(), "guide count");
            break;
        }
        if (value == "endAttrs") {
            fail(reader.line_number(), "unexpected endAttrs in Patch block");
        }
        ++attribute_count;
        check_limit(attribute_count, limits.max_attributes,
                    reader.line_number(), "attribute");
        patch.attributes.push_back(make_attribute(line, reader.line_number()));
    }
    if (declared_guides == std::numeric_limits<std::size_t>::max()) {
        fail(reader.line_number() + 1u, "truncated Patch block");
    }
    patch.name = required_unique_attribute(
        patch.attributes, "name", patch.source_line).value;
    const ClassicAttribute &face_ids = required_unique_attribute(
        patch.attributes, "faceIds", patch.source_line);
    patch.face_ids = parse_face_ids(face_ids, limits, global_face_ids);
    std::unordered_set<std::uint32_t> patch_faces(
        patch.face_ids.begin(), patch.face_ids.end());
    const ClassicAttribute *culled_primitives = nullptr;
    for (const ClassicAttribute &attribute : patch.attributes) {
        if (attribute.name != "culledPrims") { continue; }
        if (culled_primitives != nullptr) {
            fail(attribute.source_line, "duplicate 'culledPrims' attribute");
        }
        culled_primitives = &attribute;
    }
    if (culled_primitives != nullptr) {
        patch.culled_primitives = parse_culled_primitives(
            *culled_primitives, patch_faces, limits,
            global_culled_primitives);
    }
    if (declared_guides > limits.max_guides - global_guides) {
        fail(reader.line_number(), "guide limit exceeded");
    }
    global_guides += declared_guides;
    patch.guides.reserve(declared_guides);
    std::unordered_set<std::uint64_t> guide_ids;
    guide_ids.reserve(declared_guides);
    for (std::size_t guide_index = 0u; guide_index < declared_guides;
         ++guide_index) {
        const ClassicAttribute id = next_named_attribute(reader, line, "id");
        const ClassicAttribute loc = next_named_attribute(reader, line, "loc");
        const ClassicAttribute blend = next_named_attribute(reader, line, "blend");
        const ClassicAttribute interp = next_named_attribute(reader, line, "interp");
        const ClassicAttribute cvs = next_named_attribute(reader, line, "CVs");
        attribute_count += 5u;
        check_limit(attribute_count, limits.max_attributes,
                    reader.line_number(), "attribute");

        ClassicGuide guide{};
        guide.id = parse_integer<std::uint64_t>(id.value, id.source_line, "guide ID");
        guide.source_line = id.source_line;
        if (!guide_ids.insert(guide.id).second) {
            fail(id.source_line, "duplicate guide ID in patch");
        }
        const std::vector<std::string_view> location = tokens(loc.value);
        if (location.size() != 3u) {
            fail(loc.source_line, "guide loc requires u, v, and face ID");
        }
        guide.patch_u = parse_float(location[0], loc.source_line, "guide u");
        guide.patch_v = parse_float(location[1], loc.source_line, "guide v");
        guide.face_id = parse_integer<std::uint32_t>(
            location[2], loc.source_line, "guide face ID");
        if (!patch_faces.contains(guide.face_id)) {
            fail(loc.source_line, "guide face ID is not declared by its patch");
        }
        guide.blend = parse_float(blend.value, blend.source_line, "guide blend");

        std::vector<double> interpolation = parse_interpolation(
            interp.value, interp.source_line);
        if (interpolation.size() >
            limits.max_interpolation_values - global_interpolation) {
            fail(interp.source_line, "interpolation value limit exceeded");
        }
        global_interpolation += interpolation.size();
        guide.interpolation_offset = patch.guide_interpolation.size();
        guide.interpolation_count = interpolation.size();
        patch.guide_interpolation.insert(
            patch.guide_interpolation.end(), interpolation.begin(), interpolation.end());

        guide.cv_count = parse_integer<std::size_t>(
            cvs.value, cvs.source_line, "guide CV count");
        if (guide.cv_count < 2u) {
            fail(cvs.source_line, "guide must contain at least two CVs");
        }
        check_limit(guide.cv_count, limits.max_cvs_per_guide,
                    cvs.source_line, "per-guide CV");
        if (guide.cv_count > limits.max_guide_cvs - global_cvs) {
            fail(cvs.source_line, "guide CV limit exceeded");
        }
        global_cvs += guide.cv_count;
        guide.cv_offset = patch.guide_cvs.size();
        patch.guide_cvs.push_back({0.0, 0.0, 0.0});
        for (std::size_t cv_index = 1u; cv_index < guide.cv_count; ++cv_index) {
            if (!next_content_line(reader, line)) {
                fail(reader.line_number() + 1u, "truncated guide CV payload");
            }
            patch.guide_cvs.push_back(parse_float3(
                trim(line), reader.line_number(), "guide CV"));
        }
        patch.guides.push_back(guide);
    }

    if (!next_content_line(reader, line) || trim(line) != "endObject") {
        fail(reader.line_number(), "expected endObject after guide payload");
    }
    return patch;
}

void parse_patches(BoundedLineReader &reader, std::string_view header,
                   ClassicCollection &collection,
                   const ClassicParseLimits &limits,
                   std::size_t &attribute_count,
                   std::size_t &patch_count,
                   std::size_t &global_face_ids,
                   std::size_t &global_culled_primitives,
                   std::size_t &global_guides,
                   std::size_t &global_cvs,
                   std::size_t &global_interpolation) {
    const std::vector<std::string_view> values = tokens(header);
    if (values.size() != 3u || values[0] != "Patches") {
        fail(reader.line_number(), "invalid Patches header");
    }
    ClassicDescription *description = nullptr;
    for (ClassicDescription &candidate : collection.descriptions) {
        if (candidate.name == values[1]) {
            description = &candidate;
            break;
        }
    }
    if (description == nullptr) {
        fail(reader.line_number(), "Patches references an unknown description");
    }
    if (!description->patches.empty()) {
        fail(reader.line_number(), "duplicate Patches block for description");
    }
    const std::size_t declared_guides = parse_integer<std::size_t>(
        values[2], reader.line_number(), "Patches guide count");
    std::size_t parsed_guides = 0u;
    std::unordered_set<std::string> patch_names;
    std::string line;
    while (next_content_line(reader, line)) {
        const std::string_view value = trim(line);
        if (value == "endPatches") { break; }
        if (!value.starts_with("Patch")) {
            fail(reader.line_number(), "expected Patch or endPatches");
        }
        ++patch_count;
        check_limit(patch_count, limits.max_patches,
                    reader.line_number(), "patch");
        ClassicPatch patch = parse_patch(
            reader, value, limits, attribute_count, global_face_ids,
            global_culled_primitives, global_guides, global_cvs,
            global_interpolation);
        if (!patch_names.insert(patch.name).second) {
            fail(patch.source_line, "duplicate patch name in description");
        }
        if (parsed_guides > declared_guides ||
            patch.guides.size() > declared_guides - parsed_guides) {
            fail(patch.source_line, "Patches guide count is smaller than payload");
        }
        parsed_guides += patch.guides.size();
        description->patches.push_back(std::move(patch));
    }
    if (trim(line) != "endPatches") {
        fail(reader.line_number() + 1u, "truncated Patches block");
    }
    if (description->patches.empty()) {
        fail(reader.line_number(), "Patches block contains no Patch objects");
    }
    if (parsed_guides != declared_guides) {
        fail(reader.line_number(), "Patches guide count does not match payload");
    }
}

} // namespace

ClassicCollection parse_xgen_classic_collection(
    std::istream &input, const ClassicParseLimits &limits) {
    if (limits.max_source_bytes == 0u || limits.max_line_bytes == 0u) {
        throw std::invalid_argument("Classic XGen byte limits must be positive");
    }
    BoundedLineReader reader(input, limits);
    ClassicCollection collection{};
    enum class AttributeTarget { None, Palette, Description, Object };
    AttributeTarget target = AttributeTarget::None;
    std::size_t description_index = 0u;
    std::size_t object_index = 0u;
    bool palette_seen = false;
    bool patch_phase = false;
    std::size_t attribute_count = 0u;
    std::size_t object_count = 0u;
    std::size_t patch_count = 0u;
    std::size_t global_face_ids = 0u;
    std::size_t global_culled_primitives = 0u;
    std::size_t global_guides = 0u;
    std::size_t global_cvs = 0u;
    std::size_t global_interpolation = 0u;
    std::unordered_set<std::string> description_names;

    auto attributes = [&]() -> std::vector<ClassicAttribute> & {
        switch (target) {
            case AttributeTarget::Palette:
                return collection.palette_attributes;
            case AttributeTarget::Description:
                return collection.descriptions[description_index].attributes;
            case AttributeTarget::Object:
                return collection.descriptions[description_index]
                    .objects[object_index].attributes;
            case AttributeTarget::None:
                break;
        }
        throw std::logic_error("Classic XGen parser has no attribute target");
    };

    std::string line;
    while (reader.next(line)) {
        const std::string_view value = trim(line);
        if (value.empty() || (!starts_with_space(line) && value.front() == '#')) {
            continue;
        }
        if (starts_with_space(line)) {
            if (value == "endAttrs") {
                if (target == AttributeTarget::None) {
                    fail(reader.line_number(), "endAttrs without an open object");
                }
                if (target == AttributeTarget::Description) {
                    ClassicDescription &description =
                        collection.descriptions[description_index];
                    description.name = required_unique_attribute(
                        description.attributes, "name",
                        description.source_line).value;
                    if (!description_names.insert(description.name).second) {
                        fail(reader.line_number(), "duplicate description name");
                    }
                }
                target = AttributeTarget::None;
                continue;
            }
            if (target != AttributeTarget::None) {
                ++attribute_count;
                check_limit(attribute_count, limits.max_attributes,
                            reader.line_number(), "attribute");
                attributes().push_back(make_attribute(line, reader.line_number()));
                continue;
            }
            const auto [role, object] = split_key_value(value);
            if ((role == "Active" || role == "Preview" || role == "Renderer") &&
                !object.empty() && !collection.descriptions.empty() && !patch_phase) {
                collection.descriptions.back().bindings.push_back(
                    {std::string(role), std::string(object), reader.line_number()});
                continue;
            }
            fail(reader.line_number(), "unexpected indented line");
        }

        const std::vector<std::string_view> header = tokens(value);
        if (header.empty()) { continue; }
        if (target != AttributeTarget::None) {
            fail(reader.line_number(), "object is missing endAttrs");
        }
        if (header[0] == "FileVersion") {
            if (header.size() != 2u || collection.file_version != 0u) {
                fail(reader.line_number(), "invalid or duplicate FileVersion");
            }
            collection.file_version = parse_integer<std::uint32_t>(
                header[1], reader.line_number(), "FileVersion");
            if (collection.file_version == 0u) {
                fail(reader.line_number(), "FileVersion must be positive");
            }
        } else if (header[0] == "Palette") {
            if (header.size() != 1u || palette_seen || patch_phase ||
                !collection.descriptions.empty()) {
                fail(reader.line_number(), "invalid or duplicate Palette");
            }
            palette_seen = true;
            target = AttributeTarget::Palette;
        } else if (header[0] == "Description") {
            if (header.size() != 1u || !palette_seen || patch_phase) {
                fail(reader.line_number(), "invalid Description header");
            }
            collection.descriptions.push_back({});
            check_limit(collection.descriptions.size(), limits.max_descriptions,
                        reader.line_number(), "description");
            description_index = collection.descriptions.size() - 1u;
            collection.descriptions.back().source_line = reader.line_number();
            target = AttributeTarget::Description;
        } else if (header[0] == "Patches") {
            if (!palette_seen || collection.descriptions.empty()) {
                fail(reader.line_number(), "Patches appears before descriptions");
            }
            patch_phase = true;
            parse_patches(
                reader, value, collection, limits, attribute_count, patch_count,
                global_face_ids, global_culled_primitives, global_guides,
                global_cvs, global_interpolation);
        } else {
            if (header.size() != 1u || collection.descriptions.empty() || patch_phase) {
                fail(reader.line_number(), "unexpected top-level object");
            }
            ++object_count;
            check_limit(object_count, limits.max_objects,
                        reader.line_number(), "object");
            description_index = collection.descriptions.size() - 1u;
            ClassicDescription &description =
                collection.descriptions[description_index];
            if (description.name.empty()) {
                fail(reader.line_number(), "object appears before Description endAttrs");
            }
            description.objects.push_back({});
            object_index = description.objects.size() - 1u;
            ClassicObject &object = description.objects.back();
            object.type = std::string(header[0]);
            object.source_line = reader.line_number();
            target = AttributeTarget::Object;
        }
    }

    if (target != AttributeTarget::None) {
        fail(reader.line_number() + 1u, "truncated object attributes");
    }
    if (collection.file_version == 0u) {
        fail(1u, "missing FileVersion");
    }
    if (!palette_seen) { fail(1u, "missing Palette"); }
    if (collection.descriptions.empty()) { fail(1u, "missing Description"); }
    return collection;
}

ClassicCollection load_xgen_classic_collection(
    const std::filesystem::path &path, const ClassicParseLimits &limits) {
    const std::filesystem::file_status status = std::filesystem::symlink_status(path);
    if (std::filesystem::is_symlink(status)) {
        throw std::runtime_error("refusing to read Classic XGen collection through a symlink");
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw std::runtime_error("Classic XGen collection is not a regular file: " +
                                 path.string());
    }
    const std::uint64_t size = std::filesystem::file_size(path);
    if (size > limits.max_source_bytes) {
        throw std::runtime_error("Classic XGen collection exceeds source byte limit");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open Classic XGen collection: " +
                                 path.string());
    }
    return parse_xgen_classic_collection(input, limits);
}

const ClassicAttribute *find_classic_attribute(
    const std::vector<ClassicAttribute> &attributes,
    std::string_view name) noexcept {
    const auto found = std::find_if(
        attributes.begin(), attributes.end(),
        [&](const ClassicAttribute &attribute) { return attribute.name == name; });
    return found == attributes.end() ? nullptr : &*found;
}

const ClassicDescription *find_classic_description(
    const ClassicCollection &collection, std::string_view name) noexcept {
    const auto found = std::find_if(
        collection.descriptions.begin(), collection.descriptions.end(),
        [&](const ClassicDescription &description) { return description.name == name; });
    return found == collection.descriptions.end() ? nullptr : &*found;
}

} // namespace nanoxgen
