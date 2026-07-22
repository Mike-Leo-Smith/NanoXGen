#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <string>
#include <string_view>
#include <vector>

namespace nanoxgen {

struct ClassicAttribute {
    std::string name;
    std::string value;
    std::size_t source_line{};
};

struct ClassicObject {
    std::string type;
    std::vector<ClassicAttribute> attributes;
    std::size_t source_line{};
};

struct ClassicBinding {
    std::string role;
    std::string object;
    std::size_t source_line{};
};

struct ClassicFloat3 {
    double x{};
    double y{};
    double z{};
};

// Guide roots are located on the patch by (face_id, patch_u, patch_v). Guide
// CVs are relative to that root. cv_offset/cv_count index the owning patch's
// packed guide_cvs array, whose first element is always the implicit zero root.
struct ClassicGuide {
    std::uint64_t id{};
    double patch_u{};
    double patch_v{};
    std::uint32_t face_id{};
    double blend{};
    std::size_t interpolation_offset{};
    std::size_t interpolation_count{};
    std::size_t cv_offset{};
    std::size_t cv_count{};
    std::size_t source_line{};
};

struct ClassicPatch {
    std::string type;
    std::string name;
    std::vector<ClassicAttribute> attributes;
    std::vector<std::uint32_t> face_ids;
    std::vector<ClassicGuide> guides;
    std::vector<double> guide_interpolation;
    std::vector<ClassicFloat3> guide_cvs;
    std::size_t source_line{};
};

struct ClassicDescription {
    std::string name;
    std::vector<ClassicAttribute> attributes;
    std::vector<ClassicObject> objects;
    std::vector<ClassicBinding> bindings;
    std::vector<ClassicPatch> patches;
    std::size_t source_line{};
};

struct ClassicCollection {
    std::uint32_t file_version{};
    std::vector<ClassicAttribute> palette_attributes;
    std::vector<ClassicDescription> descriptions;
};

struct ClassicParseLimits {
    std::uint64_t max_source_bytes{1024ull * 1024ull * 1024ull};
    std::size_t max_line_bytes{4u * 1024u * 1024u};
    std::size_t max_descriptions{4096u};
    std::size_t max_objects{65536u};
    std::size_t max_attributes{1000000u};
    std::size_t max_patches{65536u};
    std::size_t max_face_ids{10000000u};
    std::size_t max_guides{10000000u};
    std::size_t max_guide_cvs{100000000u};
    std::size_t max_cvs_per_guide{1000000u};
    std::size_t max_interpolation_values{100000000u};
};

[[nodiscard]] ClassicCollection parse_xgen_classic_collection(
    std::istream &input, const ClassicParseLimits &limits = {});

[[nodiscard]] ClassicCollection load_xgen_classic_collection(
    const std::filesystem::path &path, const ClassicParseLimits &limits = {});

[[nodiscard]] const ClassicAttribute *find_classic_attribute(
    const std::vector<ClassicAttribute> &attributes,
    std::string_view name) noexcept;

[[nodiscard]] const ClassicDescription *find_classic_description(
    const ClassicCollection &collection, std::string_view name) noexcept;

} // namespace nanoxgen
