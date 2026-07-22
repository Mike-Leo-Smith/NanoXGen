#include "nanoxgen/xpd.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace nanoxgen {
namespace {

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error("invalid XPD3 document: " + std::string{message});
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : _bytes(bytes) {}

    std::uint8_t u8() {
        require(1u);
        return std::to_integer<std::uint8_t>(_bytes[_offset++]);
    }

    std::uint32_t u32() {
        require(4u);
        const auto *p = _bytes.data() + _offset;
        _offset += 4u;
        return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 8u) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 16u) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3])) << 24u);
    }

    std::uint64_t u64() {
        const std::uint64_t low = u32();
        return low | (static_cast<std::uint64_t>(u32()) << 32u);
    }

    float f32() { return std::bit_cast<float>(u32()); }

    std::span<const std::byte> take(std::size_t size) {
        require(size);
        const auto result = _bytes.subspan(_offset, size);
        _offset += size;
        return result;
    }

    [[nodiscard]] std::size_t offset() const noexcept { return _offset; }

private:
    void require(std::size_t size) const {
        if (size > _bytes.size() - _offset) { fail("truncated header"); }
    }

    std::span<const std::byte> _bytes;
    std::size_t _offset{};
};

std::vector<std::string> decode_strings(
    std::span<const std::byte> data, std::uint32_t count) {
    std::vector<std::string> result;
    result.reserve(count);
    std::size_t offset = 0u;
    for (std::uint32_t index = 0u; index < count; ++index) {
        std::size_t end = offset;
        while (end < data.size() && data[end] != std::byte{}) { ++end; }
        if (end == data.size()) { fail("unterminated header string"); }
        result.emplace_back(
            reinterpret_cast<const char *>(data.data() + offset), end - offset);
        offset = end + 1u;
    }
    if (offset != data.size()) { fail("header string table has trailing bytes"); }
    return result;
}

std::uint64_t checked_product(
    std::uint64_t a, std::uint64_t b, std::string_view label) {
    if (a != 0u && b > std::numeric_limits<std::uint64_t>::max() / a) {
        fail(std::string{label} + " size overflow");
    }
    return a * b;
}

std::uint32_t load_u32(const std::byte *p) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 8u) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 16u) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3])) << 24u);
}

} // namespace

XpdDocument parse_xpd_document(
    std::span<const std::byte> bytes, const XpdParseLimits &limits) {
    if (bytes.size() > limits.max_file_bytes) { fail("file limit exceeded"); }
    if (bytes.size() < 4u ||
        std::memcmp(bytes.data(), "XPD3", 4u) != 0) {
        fail("bad magic");
    }
    Reader reader{bytes.subspan(4u)};
    XpdDocument result{};
    result.file_version = reader.u8();
    result.primitive_type = static_cast<XpdPrimitiveType>(reader.u32());
    result.primitive_version = reader.u8();
    result.time = reader.f32();
    result.cv_count = reader.u32();
    result.coordinate_space = static_cast<XpdCoordinateSpace>(reader.u32());
    const std::uint32_t block_count = reader.u32();
    if (!std::isfinite(result.time)) { fail("non-finite time"); }
    if (block_count == 0u || block_count > limits.max_blocks) {
        fail("block count limit exceeded");
    }
    const std::uint32_t block_string_bytes = reader.u32();
    if (block_string_bytes > limits.max_string_bytes) {
        fail("block string table limit exceeded");
    }
    const std::vector<std::string> block_names = decode_strings(
        reader.take(block_string_bytes), block_count);
    result.blocks.reserve(block_count);
    for (const std::string &name : block_names) {
        const std::uint32_t primitive_size = reader.u32();
        if (primitive_size == 0u ||
            primitive_size > limits.max_floats_per_primitive) {
            fail("primitive float count limit exceeded");
        }
        result.blocks.push_back({name, primitive_size});
    }

    const std::uint32_t key_count = reader.u32();
    const std::uint32_t key_string_bytes = reader.u32();
    if (key_count > limits.max_keys ||
        key_string_bytes > limits.max_string_bytes) {
        fail("key table limit exceeded");
    }
    result.keys = decode_strings(reader.take(key_string_bytes), key_count);

    const std::uint32_t face_count = reader.u32();
    if (face_count > limits.max_faces) { fail("face count limit exceeded"); }
    result.faces.resize(face_count);
    for (XpdFaceInfo &face : result.faces) {
        face.face_id = std::bit_cast<std::int32_t>(reader.u32());
    }
    std::uint64_t primitive_total = 0u;
    for (XpdFaceInfo &face : result.faces) {
        face.primitive_count = reader.u32();
        primitive_total += face.primitive_count;
        if (primitive_total > limits.max_primitives) {
            fail("primitive count limit exceeded");
        }
    }
    for (XpdFaceInfo &face : result.faces) {
        face.block_offsets.resize(block_count);
        for (std::uint64_t &offset : face.block_offsets) {
            offset = reader.u64();
        }
    }
    const std::uint64_t header_end = 4u + reader.offset();
    for (const XpdFaceInfo &face : result.faces) {
        // Autodesk writes UINT64_MAX for block offsets on empty faces. The
        // offset is deliberately non-dereferenceable and carries no payload.
        if (face.primitive_count == 0u) { continue; }
        for (std::size_t block = 0u; block < result.blocks.size(); ++block) {
            const std::uint64_t bytes_per_primitive = checked_product(
                result.blocks[block].floats_per_primitive, 4u,
                "primitive payload");
            const std::uint64_t payload_size = checked_product(
                face.primitive_count, bytes_per_primitive,
                "face payload");
            const std::uint64_t offset = face.block_offsets[block];
            if (offset < header_end || offset > bytes.size() ||
                payload_size > bytes.size() - offset) {
                fail("face block points outside the file");
            }
        }
    }
    result.bytes.assign(bytes.begin(), bytes.end());
    return result;
}

XpdDocument load_xpd_document(
    const std::filesystem::path &path, const XpdParseLimits &limits) {
    std::ifstream input{path, std::ios::binary | std::ios::ate};
    if (!input) { throw std::runtime_error("cannot open XPD file"); }
    const std::streamoff end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > limits.max_file_bytes ||
        static_cast<std::uint64_t>(end) >
            std::numeric_limits<std::size_t>::max()) {
        fail("file limit exceeded");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty() && !input.read(
            reinterpret_cast<char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("cannot read XPD file");
    }
    return parse_xpd_document(bytes, limits);
}

void copy_xpd_primitive(
    const XpdDocument &document, std::size_t face_index,
    std::size_t block_index, std::uint32_t primitive_index,
    std::span<float> output) {
    if (face_index >= document.faces.size() ||
        block_index >= document.blocks.size()) {
        throw std::out_of_range("XPD face/block index is out of range");
    }
    const XpdFaceInfo &face = document.faces[face_index];
    const XpdBlockInfo &block = document.blocks[block_index];
    if (primitive_index >= face.primitive_count ||
        output.size() != block.floats_per_primitive) {
        throw std::out_of_range("XPD primitive/output size is out of range");
    }
    const std::uint64_t stride =
        static_cast<std::uint64_t>(block.floats_per_primitive) * 4u;
    const std::uint64_t offset = face.block_offsets[block_index] +
        static_cast<std::uint64_t>(primitive_index) * stride;
    const std::byte *source = document.bytes.data() + offset;
    for (std::size_t index = 0u; index < output.size(); ++index) {
        output[index] = std::bit_cast<float>(load_u32(source + index * 4u));
    }
}

} // namespace nanoxgen
