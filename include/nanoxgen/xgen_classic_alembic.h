#pragma once

#include "nanoxgen/asset.h"
#include "nanoxgen/xgen_classic.h"

#include <cstddef>
#include <filesystem>

namespace nanoxgen {

struct ClassicAlembicLimits {
    std::size_t max_objects{100000u};
    std::size_t max_vertices{100000000u};
    std::size_t max_faces{100000000u};
    std::size_t max_face_vertices{1000000000u};
    std::size_t max_triangles{200000000u};
    std::uint32_t subd_face_resolution{2u};
};

struct ClassicAlembicAssetInput {
    AssetBuildInput asset;
    std::size_t source_vertex_count{};
    std::size_t source_face_count{};
    std::size_t selected_face_count{};
    std::size_t subdivision_face_count{};
    float guide_cage_root_rms_distance{};
    float guide_cage_root_max_distance{};
};

// Read the first Alembic sample for each Classic patch, select only its
// declared face IDs, triangulate those faces, and turn embedded relative guide
// CVs into absolute positions. The system Alembic dependency is confined to
// the optional nanoxgen_classic_alembic target.
[[nodiscard]] ClassicAlembicAssetInput build_xgen_classic_alembic_asset_input(
    const ClassicDescription &description,
    const std::filesystem::path &archive_path,
    const ClassicAlembicLimits &limits = {});

} // namespace nanoxgen
