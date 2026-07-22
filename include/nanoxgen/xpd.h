#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace nanoxgen {

enum class XpdPrimitiveType : std::uint32_t {
    Point = 0u,
    Spline = 1u,
    Card = 2u,
    Sphere = 3u,
    Archive = 4u,
};

enum class XpdCoordinateSpace : std::uint32_t {
    World = 0u,
    Object = 1u,
    Local = 2u,
    Micro = 3u,
};

struct XpdParseLimits {
    std::uint64_t max_file_bytes{1024ull * 1024ull * 1024ull};
    std::uint32_t max_faces{10000000u};
    std::uint32_t max_blocks{4096u};
    std::uint32_t max_keys{1000000u};
    std::uint32_t max_string_bytes{64u * 1024u * 1024u};
    std::uint32_t max_floats_per_primitive{1000000u};
    std::uint64_t max_primitives{100000000u};
};

struct XpdBlockInfo {
    std::string name;
    std::uint32_t floats_per_primitive{};
};

struct XpdFaceInfo {
    std::int32_t face_id{};
    std::uint32_t primitive_count{};
    std::vector<std::uint64_t> block_offsets;
};

struct XpdDocument {
    std::uint8_t file_version{};
    XpdPrimitiveType primitive_type{};
    std::uint8_t primitive_version{};
    float time{};
    std::uint32_t cv_count{};
    XpdCoordinateSpace coordinate_space{};
    std::vector<XpdBlockInfo> blocks;
    std::vector<std::string> keys;
    std::vector<XpdFaceInfo> faces;
    std::vector<std::byte> bytes;
};

[[nodiscard]] XpdDocument parse_xpd_document(
    std::span<const std::byte> bytes, const XpdParseLimits &limits = {});

[[nodiscard]] XpdDocument load_xpd_document(
    const std::filesystem::path &path, const XpdParseLimits &limits = {});

// Decode one little-endian primitive record into caller-owned storage.
// The output size must equal blocks[block_index].floats_per_primitive.
void copy_xpd_primitive(
    const XpdDocument &document, std::size_t face_index,
    std::size_t block_index, std::uint32_t primitive_index,
    std::span<float> output);

} // namespace nanoxgen
