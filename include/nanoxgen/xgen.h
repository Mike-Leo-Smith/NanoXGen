#pragma once

#include "nanoxgen/asset.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace nanoxgen {

inline constexpr std::uint64_t kXGenFileMagic = 0x8099ceadull;
inline constexpr std::uint64_t kXGenGroupMagic = 0x041e065full;
inline constexpr std::uint64_t kXGenUInt32ArrayTag = 0x43f60844ull;
inline constexpr std::uint64_t kXGenUInt32ScalarArrayTag = 0x000197efull;
inline constexpr std::uint64_t kXGenFloatArrayTag = 0x05d0225cull;
inline constexpr std::uint64_t kXGenVec2ArrayTag = 0xdb53a6f4ull;
inline constexpr std::uint64_t kXGenVec3ArrayTag = 0xdb53a713ull;

// Generic, lossless representation of an Interactive Grooming outRenderData
// BLOB. Unknown array tags are preserved so a document can round-trip even
// when NanoXGen does not yet interpret a custom channel.
struct XGenArray {
    std::uint64_t type_tag{};
    std::vector<std::byte> bytes;
};

struct XGenGroup {
    std::uint32_t index{};
    std::uint64_t flags{};
    std::vector<XGenArray> arrays;
};

struct XGenDocument {
    std::string metadata_json;
    std::uint32_t version{};
    std::uint32_t group_version{};
    bool group_base64{};
    bool group_deflate{};
    std::uint32_t group_deflate_level{};
    std::vector<XGenGroup> groups;
};

// Owning, canonicalized renderer-relevant view of all Items in an evaluated
// XgSplineData document. Widths remain XGen full widths (diameters).
struct XGenEvaluatedCurves {
    std::vector<std::uint32_t> point_counts;
    std::vector<Vec3> positions;
    std::vector<float> widths;
    std::vector<Vec2> texcoords;
    std::vector<Vec2> patch_uvs;
    std::vector<Vec2> face_uvs;
    std::vector<std::uint32_t> face_ids;
};

// Source order avoids the surface-identity sort and is the preferred path for
// one-shot static rendering. Canonical order is stable across XGen batch/group
// emission order and is required for reproducible caches and motion matching.
enum class XGenCurveOrder {
    Source,
    Canonical,
};

// Minimal renderer payload. Widths are converted from XGen diameters to
// radii while the data is packed. The resident-byte parse API uses a fused
// selective reader and does not construct a lossless XGenDocument.
struct XGenPackedCurves {
    std::vector<std::uint32_t> point_counts;
    std::vector<PackedCurvePoint> points;
};

struct XGenBuildOptions {
    std::string item_name{"NanoXGenGroom"};
    std::uint64_t target_group_bytes{64u * 1024u * 1024u};
    std::uint32_t group_deflate_level{9u};
};

struct XGenProcessParams {
    Vec3 translation{};
    float length_scale{1.0f};
    float width_scale{1.0f};
};

[[nodiscard]] XGenDocument parse_xgen_document(std::span<const std::byte> bytes);
[[nodiscard]] XGenDocument load_xgen_document(const std::filesystem::path &path);
[[nodiscard]] std::vector<std::byte> serialize_xgen_document(
    const XGenDocument &document);
void save_xgen_document(
    const XGenDocument &document, const std::filesystem::path &path);

[[nodiscard]] XGenEvaluatedCurves materialize_xgen_curves(
    const XGenDocument &document,
    XGenCurveOrder order = XGenCurveOrder::Canonical);
[[nodiscard]] XGenPackedCurves materialize_xgen_packed_curves(
    const XGenDocument &document,
    XGenCurveOrder order = XGenCurveOrder::Source);
[[nodiscard]] XGenPackedCurves parse_xgen_packed_curves(
    std::span<const std::byte> bytes,
    XGenCurveOrder order = XGenCurveOrder::Source);
[[nodiscard]] XGenPackedCurves load_xgen_packed_curves(
    const std::filesystem::path &path,
    XGenCurveOrder order = XGenCurveOrder::Source);
[[nodiscard]] XGenEvaluatedCurves make_xgen_curves(
    const GeneratedCurves &curves);
void process_xgen_curves(
    XGenEvaluatedCurves &curves, const XGenProcessParams &params);
void process_xgen_document(
    XGenDocument &document, const XGenProcessParams &params);
[[nodiscard]] XGenDocument build_xgen_document(
    const XGenEvaluatedCurves &curves, const XGenBuildOptions &options = {});

} // namespace nanoxgen
