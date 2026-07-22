#pragma once

#include "nanoxgen/asset.h"
#include "nanoxgen/xgen_classic.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nanoxgen {

struct ClassicAlembicLimits {
    std::size_t max_objects{100000u};
    std::size_t max_vertices{100000000u};
    std::size_t max_faces{100000000u};
    std::size_t max_face_vertices{1000000000u};
    std::size_t max_triangles{200000000u};
    std::uint32_t subd_face_resolution{2u};
};

struct ClassicReferenceSurfaceSample {
    Vec3 position{};
    Vec3 normal{};
    Vec3 tangent{};
};

// CPU-only access to the imported OpenSubdiv limit surface. Keeping the
// evaluator alive avoids approximating guide association through the compact
// renderer tessellation; no OpenSubdiv type or double-precision payload enters
// the Asset/GPU ABI.
class ClassicReferenceSurfaceEvaluator {
public:
    virtual ~ClassicReferenceSurfaceEvaluator() = default;
    [[nodiscard]] virtual ClassicReferenceSurfaceSample evaluate_current(
        std::string_view patch_name, std::uint32_t face_id,
        float u, float v) const = 0;
    [[nodiscard]] virtual ClassicReferenceSurfaceSample evaluate(
        std::string_view patch_name, std::uint32_t face_id,
        float u, float v) const = 0;
};

struct ClassicAlembicAssetInput {
    struct SurfaceFace {
        std::string patch_name;
        std::uint32_t face_id{};
        std::uint32_t first_triangle{};
        std::uint32_t triangle_count{};
        std::uint32_t uv_resolution{};
        // XGen computes these values in double from the SESubd float limit cage.
        // They remain CPU-side authoring metadata and are never uploaded to a
        // generation backend.
        double surface_area{};
        double center_u_length{};
        double center_v_length{};
        // Reference control-cage bounds used by XGen before evaluating guide
        // weights. For Subd faces this includes every face incident to any
        // corner of the active face (the SESubd face umbrella).
        Vec3 reference_bounds_min{};
        Vec3 reference_bounds_max{};
    };

    AssetBuildInput asset;
    std::vector<SurfaceFace> surface_faces;
    std::shared_ptr<const ClassicReferenceSurfaceEvaluator> reference_surface;
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
