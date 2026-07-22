#include "nanoxgen/xgen.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace nanoxgen;

namespace {

void hash_bytes(std::uint64_t &hash, const void *data, std::size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (std::size_t index = 0u; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
}

} // namespace

int main(int argc, char **argv) try {
    if (argc != 2 && argc != 4) {
        std::cerr << "usage: nanoxgen_xgen_inspect <input.xgen> "
                     "[--round-trip <output.xgen>]\n";
        return 2;
    }
    const std::filesystem::path input = argv[1];
    const XGenDocument document = load_xgen_document(input);
    if (argc == 4) {
        if (std::string_view{argv[2]} != "--round-trip") {
            throw std::invalid_argument("expected --round-trip before output path");
        }
        save_xgen_document(document, argv[3]);
    }
    const XGenEvaluatedCurves curves = materialize_xgen_curves(document);
    std::uint32_t min_count = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t max_count = 0u;
    float min_width = std::numeric_limits<float>::infinity();
    float max_width = -std::numeric_limits<float>::infinity();
    Vec3 min_position{min_width, min_width, min_width};
    Vec3 max_position{max_width, max_width, max_width};
    Vec2 min_patch_uv{min_width, min_width};
    Vec2 max_patch_uv{max_width, max_width};
    std::vector<std::uint64_t> curve_hashes;
    curve_hashes.reserve(curves.point_counts.size());
    std::size_t first = 0u;
    for (std::size_t curve = 0u; curve < curves.point_counts.size(); ++curve) {
        const std::uint32_t count = curves.point_counts[curve];
        min_count = std::min(min_count, count);
        max_count = std::max(max_count, count);
        min_patch_uv.x = std::min(min_patch_uv.x, curves.patch_uvs[curve].x);
        min_patch_uv.y = std::min(min_patch_uv.y, curves.patch_uvs[curve].y);
        max_patch_uv.x = std::max(max_patch_uv.x, curves.patch_uvs[curve].x);
        max_patch_uv.y = std::max(max_patch_uv.y, curves.patch_uvs[curve].y);
        std::uint64_t hash = 1469598103934665603ull;
        hash_bytes(hash, &count, sizeof(count));
        hash_bytes(hash, &curves.face_ids[curve], sizeof(curves.face_ids[curve]));
        hash_bytes(hash, &curves.face_uvs[curve].x, sizeof(float));
        hash_bytes(hash, &curves.face_uvs[curve].y, sizeof(float));
        hash_bytes(hash, &curves.patch_uvs[curve].x, sizeof(float));
        hash_bytes(hash, &curves.patch_uvs[curve].y, sizeof(float));
        for (std::size_t point = first; point < first + count; ++point) {
            const float width = curves.widths[point];
            const Vec2 texcoord = curves.texcoords[point];
            const Vec3 position = curves.positions[point];
            min_width = std::min(min_width, width);
            max_width = std::max(max_width, width);
            min_position.x = std::min(min_position.x, position.x);
            min_position.y = std::min(min_position.y, position.y);
            min_position.z = std::min(min_position.z, position.z);
            max_position.x = std::max(max_position.x, position.x);
            max_position.y = std::max(max_position.y, position.y);
            max_position.z = std::max(max_position.z, position.z);
            hash_bytes(hash, &width, sizeof(width));
            hash_bytes(hash, &texcoord.x, sizeof(float));
            hash_bytes(hash, &texcoord.y, sizeof(float));
            hash_bytes(hash, &position.x, sizeof(float));
            hash_bytes(hash, &position.y, sizeof(float));
            hash_bytes(hash, &position.z, sizeof(float));
        }
        first += count;
        curve_hashes.push_back(hash);
    }
    std::uint64_t canonical_hash = 1469598103934665603ull;
    for (const std::uint64_t hash : curve_hashes) {
        hash_bytes(canonical_hash, &hash, sizeof(hash));
    }
    std::cout << std::setprecision(9)
              << "{\n"
              << "  \"valid\": true,\n"
              << "  \"groups\": " << document.groups.size() << ",\n"
              << "  \"curves\": " << curves.point_counts.size() << ",\n"
              << "  \"vertices\": " << curves.positions.size() << ",\n"
              << "  \"canonical_hash\": \"0x" << std::hex << canonical_hash
              << std::dec << "\",\n"
              << "  \"vertices_per_curve\": {\"min\": " << min_count
              << ", \"max\": " << max_count << "},\n"
              << "  \"width\": {\"min\": " << min_width
              << ", \"max\": " << max_width << "},\n"
              << "  \"position_bounds\": {\"min\": [" << min_position.x << ", "
              << min_position.y << ", " << min_position.z << "], \"max\": ["
              << max_position.x << ", " << max_position.y << ", "
              << max_position.z << "]},\n"
              << "  \"patch_uv_bounds\": {\"min\": [" << min_patch_uv.x << ", "
              << min_patch_uv.y << "], \"max\": [" << max_patch_uv.x << ", "
              << max_patch_uv.y << "]}\n"
              << "}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
