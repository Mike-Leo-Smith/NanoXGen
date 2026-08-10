#include "nanoxgen/asset.h"
#include "nanoxgen/xgen_classic_alembic.h"

#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(bool condition, const char *message) {
    if (!condition) { throw std::runtime_error(message); }
}

bool near_value(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 1.0e-6f;
}

struct TemporaryArchive {
    std::filesystem::path path;

    explicit TemporaryArchive(std::filesystem::path value)
        : path{std::move(value)} {}
    TemporaryArchive(const TemporaryArchive &) = delete;
    TemporaryArchive &operator=(const TemporaryArchive &) = delete;
    TemporaryArchive(TemporaryArchive &&other) noexcept
        : path{std::move(other.path)} {
        other.path.clear();
    }
    TemporaryArchive &operator=(TemporaryArchive &&) = delete;

    ~TemporaryArchive() {
        if (path.empty()) { return; }
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

TemporaryArchive write_archive() {
    const auto stamp = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    TemporaryArchive result{
        std::filesystem::temp_directory_path() /
        ("nanoxgen-classic-alembic-" + std::to_string(stamp) + ".abc")};
    {
        Alembic::Abc::OArchive archive{
            Alembic::AbcCoreOgawa::WriteArchive(), result.path.string()};
        Alembic::AbcGeom::OXform xform{archive.getTop(), "testPatch"};
        Alembic::AbcGeom::XformSample xform_sample;
        xform_sample.setTranslation({3.0, 4.0, 5.0});
        xform.getSchema().set(xform_sample);
        Alembic::AbcGeom::OPolyMesh mesh{xform, "testPatchShape"};
        const Imath::V3f positions[]{
            {0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f},
            {2.0f, 0.0f, 2.0f}, {0.0f, 0.0f, 2.0f}};
        const std::int32_t indices[]{0, 1, 2, 3};
        const std::int32_t counts[]{4};
        mesh.getSchema().set(Alembic::AbcGeom::OPolyMeshSchema::Sample{
            Alembic::Abc::P3fArraySample{positions, 4u},
            Alembic::Abc::Int32ArraySample{indices, 4u},
            Alembic::Abc::Int32ArraySample{counts, 1u}});
    }
    return result;
}

TemporaryArchive write_nonquad_archive() {
    const auto stamp = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    TemporaryArchive result{
        std::filesystem::temp_directory_path() /
        ("nanoxgen-classic-alembic-nonquad-" +
         std::to_string(stamp) + ".abc")};
    {
        Alembic::Abc::OArchive archive{
            Alembic::AbcCoreOgawa::WriteArchive(), result.path.string()};
        Alembic::AbcGeom::OXform xform{archive.getTop(), "testPatch"};
        Alembic::AbcGeom::OPolyMesh mesh{xform, "testPatchShape"};
        const Imath::V3f positions[]{
            {0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f},
            {1.0f, 0.0f, 2.0f}, {4.0f, 0.0f, 0.0f},
            {6.0f, 0.0f, 0.0f}, {6.0f, 0.0f, 2.0f},
            {4.0f, 0.0f, 2.0f}};
        const std::int32_t indices[]{0, 1, 2, 3, 4, 5, 6};
        const std::int32_t counts[]{3, 4};
        mesh.getSchema().set(Alembic::AbcGeom::OPolyMeshSchema::Sample{
            Alembic::Abc::P3fArraySample{positions, 7u},
            Alembic::Abc::Int32ArraySample{indices, 7u},
            Alembic::Abc::Int32ArraySample{counts, 2u}});
    }
    return result;
}

TemporaryArchive write_animated_archive(bool changing_topology = false) {
    const auto stamp = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    TemporaryArchive result{
        std::filesystem::temp_directory_path() /
        ("nanoxgen-classic-alembic-motion-" +
         std::to_string(stamp) + ".abc")};
    {
        Alembic::Abc::OArchive archive{
            Alembic::AbcCoreOgawa::WriteArchive(), result.path.string()};
        const auto sampling =
            std::make_shared<Alembic::AbcCoreAbstract::TimeSampling>(
                1.0, 1.0);
        Alembic::AbcGeom::OXform xform{archive.getTop(), "testPatch"};
        xform.getSchema().setTimeSampling(sampling);
        Alembic::AbcGeom::OPolyMesh mesh{xform, "testPatchShape"};
        mesh.getSchema().setTimeSampling(sampling);
        const std::int32_t quad_indices[]{0, 1, 2, 3};
        const std::int32_t triangle_indices[]{0, 1, 2};
        const std::int32_t quad_counts[]{4};
        const std::int32_t triangle_counts[]{3};
        for (std::uint32_t sample_index = 0u;
             sample_index < 3u; ++sample_index) {
            Alembic::AbcGeom::XformSample xform_sample;
            xform_sample.setTranslation(
                {3.0 + static_cast<double>(sample_index), 4.0, 5.0});
            xform_sample.setZRotation(
                90.0 * static_cast<double>(sample_index));
            xform.getSchema().set(xform_sample);
            const float height = 2.0f * static_cast<float>(sample_index);
            const Imath::V3f positions[]{
                {0.0f, height, 0.0f}, {2.0f, height, 0.0f},
                {2.0f, height, 2.0f}, {0.0f, height, 2.0f}};
            const bool triangle =
                changing_topology && sample_index == 2u;
            mesh.getSchema().set(
                Alembic::AbcGeom::OPolyMeshSchema::Sample{
                    Alembic::Abc::P3fArraySample{positions, 4u},
                    Alembic::Abc::Int32ArraySample{
                        triangle ? triangle_indices : quad_indices,
                        triangle ? 3u : 4u},
                    Alembic::Abc::Int32ArraySample{
                        triangle ? triangle_counts : quad_counts, 1u}});
        }
    }
    return result;
}

TemporaryArchive write_guide_cache(bool animated, std::size_t cv_count = 2u,
                                   std::size_t curve_count = 1u) {
    const auto stamp = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    TemporaryArchive result{
        std::filesystem::temp_directory_path() /
        ("nanoxgen-classic-guide-cache-" +
         std::to_string(stamp) + ".abc")};
    {
        Alembic::Abc::OArchive archive{
            Alembic::AbcCoreOgawa::WriteArchive(), result.path.string()};
        const auto sampling =
            std::make_shared<Alembic::AbcCoreAbstract::TimeSampling>(
                1.0, 1.0);
        Alembic::AbcGeom::OXform xform{archive.getTop(), "guideGroup"};
        Alembic::AbcGeom::OCurves curves{xform, "guideShape"};
        if (animated) {
            xform.getSchema().setTimeSampling(sampling);
            curves.getSchema().setTimeSampling(sampling);
        }
        const std::vector<std::int32_t> counts(
            curve_count, static_cast<std::int32_t>(cv_count));
        for (std::size_t sample_index = 0u;
             sample_index < (animated ? 2u : 1u); ++sample_index) {
            Alembic::AbcGeom::XformSample xform_sample;
            xform_sample.setTranslation(
                {animated ? 10.0 + 2.0 * sample_index : 3.0,
                 animated ? 0.0 : 4.0,
                 animated ? 0.0 : 5.0});
            xform.getSchema().set(xform_sample);
            std::vector<Imath::V3f> positions(cv_count * curve_count);
            for (std::size_t curve = 0u; curve < curve_count; ++curve) {
                for (std::size_t cv = 0u; cv < cv_count; ++cv) {
                    const float curve_offset = static_cast<float>(curve * 20u);
                    positions[curve * cv_count + cv] = animated
                        ? Imath::V3f{
                              curve_offset + static_cast<float>(cv),
                              static_cast<float>(sample_index * 2u + cv), 0.0f}
                        : Imath::V3f{
                              curve_offset + 1.0f +
                                  static_cast<float>(cv * 3u),
                              2.0f + static_cast<float>(cv * 3u),
                              3.0f + static_cast<float>(cv * 3u)};
                }
            }
            curves.getSchema().set(
                Alembic::AbcGeom::OCurvesSchema::Sample{
                    Alembic::Abc::P3fArraySample{
                        positions.data(), positions.size()},
                    Alembic::Abc::Int32ArraySample{
                        counts.data(), counts.size()},
                    Alembic::AbcGeom::kCubic,
                    Alembic::AbcGeom::kNonPeriodic});
        }
    }
    return result;
}

TemporaryArchive write_named_guide_cache() {
    const auto stamp = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    TemporaryArchive result{
        std::filesystem::temp_directory_path() /
        ("nanoxgen-classic-named-guide-cache-" +
         std::to_string(stamp) + ".abc")};
    {
        Alembic::Abc::OArchive archive{
            Alembic::AbcCoreOgawa::WriteArchive(), result.path.string()};
        Alembic::AbcGeom::OXform xform{archive.getTop(), "guideGroup"};
        const std::int32_t count[]{2};
        const auto write_curve = [&](const char *name, float root_x) {
            Alembic::AbcGeom::OCurves curves{xform, name};
            const Imath::V3f positions[]{
                {root_x, 0.0f, 0.0f}, {root_x, 1.0f, 0.0f}};
            curves.getSchema().set(
                Alembic::AbcGeom::OCurvesSchema::Sample{
                    Alembic::Abc::P3fArraySample{positions, 2u},
                    Alembic::Abc::Int32ArraySample{count, 1u},
                    Alembic::AbcGeom::kCubic,
                    Alembic::AbcGeom::kNonPeriodic});
        };
        // Archive traversal order intentionally differs from _wireNames.
        write_curve("curveShape20", 200.0f);
        write_curve("curveShape10", 100.0f);
    }
    return result;
}

nanoxgen::ClassicDescription description() {
    nanoxgen::ClassicGuide guide{};
    guide.id = 7u;
    guide.patch_u = 0.25;
    guide.patch_v = 0.5;
    guide.face_id = 0u;
    guide.blend = 0.5;
    guide.interpolation_offset = 0u;
    guide.interpolation_count = 3u;
    guide.cv_offset = 0u;
    guide.cv_count = 2u;
    nanoxgen::ClassicPatch patch{};
    patch.type = "Poly";
    patch.name = "testPatch";
    patch.face_ids = {0u};
    patch.guides = {guide};
    patch.guide_interpolation = {2.0, 1.0, 0.5};
    patch.guide_cvs = {{0.0, 0.0, 0.0}, {1.0, 2.0, 3.0}};
    nanoxgen::ClassicDescription result{};
    result.name = "test";
    result.patches.emplace_back(std::move(patch));
    return result;
}

void test_import() {
    const TemporaryArchive archive = write_archive();
    require(
        nanoxgen::xgen_classic_alembic_deformation_is_static(
            description(), archive.path),
        "single-sample archive was not classified as static");
    const nanoxgen::ClassicAlembicAssetInput imported =
        nanoxgen::build_xgen_classic_alembic_asset_input(
            description(), archive.path);
    require(imported.source_vertex_count == 4u, "source vertex count mismatch");
    require(imported.source_face_count == 1u, "source face count mismatch");
    require(imported.selected_face_count == 1u, "selected face count mismatch");
    require(imported.asset.positions.size() == 4u, "position count mismatch");
    require(imported.asset.triangles.size() == 2u, "quad triangulation mismatch");
    require(imported.asset.guides.size() == 1u, "guide count mismatch");
    const nanoxgen::GuideInput &guide = imported.asset.guides.front();
    require(guide.cvs.size() == 2u, "guide CV count mismatch");
    require(near_value(guide.cvs[0].x, 3.5f) &&
                near_value(guide.cvs[0].y, 4.0f) &&
                near_value(guide.cvs[0].z, 6.0f),
            "transformed bilinear guide root mismatch");
    require(near_value(guide.cvs[1].x, 4.5f) &&
                near_value(guide.cvs[1].y, 6.0f) &&
                near_value(guide.cvs[1].z, 3.0f),
            "patch-frame guide CV transform mismatch");
    require(guide.triangle_index == 1u &&
                near_value(guide.barycentric.x, 0.25f) &&
                near_value(guide.barycentric.y, 0.25f),
            "guide triangle binding mismatch");
    require(guide.support_radii.size() == 2u &&
                near_value(guide.support_radii[0], 3.0f) &&
                near_value(guide.support_radii[1], 1.5f) &&
                guide.support_angles.size() == 1u &&
                near_value(guide.support_angles[0], 0.5f),
            "guide blend radius scale mismatch");
    const nanoxgen::Asset asset = nanoxgen::build_asset(imported.asset);
    require(nanoxgen::validate_asset(asset.bytes()).empty(),
            "imported asset failed validation");
}

void test_missing_patch() {
    const TemporaryArchive archive = write_archive();
    nanoxgen::ClassicDescription input = description();
    input.patches.front().name = "missing";
    try {
        (void)nanoxgen::build_xgen_classic_alembic_asset_input(input, archive.path);
    } catch (const std::runtime_error &error) {
        require(std::string{error.what()}.find("not found") != std::string::npos,
                "wrong missing patch diagnostic");
        return;
    }
    throw std::runtime_error("missing patch was accepted");
}

void test_subd_import() {
    const TemporaryArchive archive = write_archive();
    nanoxgen::ClassicDescription input = description();
    input.patches.front().type = "Subd";
    nanoxgen::ClassicAlembicLimits limits{};
    limits.subd_face_resolution = 2u;
    const nanoxgen::ClassicAlembicAssetInput imported =
        nanoxgen::build_xgen_classic_alembic_asset_input(
            input, archive.path, limits);
    require(imported.asset.positions.size() == 9u,
            "subdivision tessellation vertex count mismatch");
    require(imported.asset.triangles.size() == 8u,
            "subdivision tessellation triangle count mismatch");
    require(imported.surface_faces.size() == 1u &&
                std::abs(imported.surface_faces[0].surface_area - 2.25) <= 1.0e-12 &&
                std::abs(imported.surface_faces[0].center_u_length - 1.5) <= 1.0e-12 &&
                std::abs(imported.surface_faces[0].center_v_length - 1.5) <= 1.0e-12,
            "subdivision boundary corners changed the SESubd metric cage");
    const nanoxgen::GuideInput &guide = imported.asset.guides.front();
    require(guide.triangle_index < imported.asset.triangles.size(),
            "subdivision guide triangle is invalid");
    require(std::isfinite(guide.cvs[0].x) && std::isfinite(guide.cvs[0].y) &&
                std::isfinite(guide.cvs[0].z),
            "subdivision guide root is non-finite");
    require(near_value(guide.cvs[1].y - guide.cvs[0].y, 2.0f),
            "subdivision guide relative CV mismatch");
}

void test_nonquad_subd_ptex_faces() {
    const TemporaryArchive archive = write_nonquad_archive();
    nanoxgen::ClassicDescription input = description();
    input.patches.front().type = "Subd";
    // The triangle expands to PTEX faces 0..2; the following coarse quad is
    // PTEX face 3 rather than coarse face 1.
    input.patches.front().face_ids = {0u, 1u, 2u, 3u};
    input.patches.front().guides.front().face_id = 0u;
    nanoxgen::ClassicAlembicLimits limits{};
    limits.subd_face_resolution = 2u;
    const nanoxgen::ClassicAlembicAssetInput imported =
        nanoxgen::build_xgen_classic_alembic_asset_input(
            input, archive.path, limits);
    require(imported.source_face_count == 2u,
            "non-quad source face count mismatch");
    require(imported.selected_face_count == 4u &&
                imported.subdivision_face_count == 4u,
            "non-quad PTEX face count mismatch");
    require(imported.asset.positions.size() == 36u,
            "non-quad subdivision vertex count mismatch");
    require(imported.asset.triangles.size() == 32u,
            "non-quad subdivision triangle count mismatch");
    require(imported.surface_faces.size() == 4u,
            "non-quad surface metadata count mismatch");
    for (std::uint32_t face = 0u; face < 4u; ++face) {
        require(imported.surface_faces[face].face_id == face,
                "non-quad PTEX face ordering mismatch");
    }
    require(imported.asset.guides.front().triangle_index <
                imported.asset.triangles.size(),
            "non-quad guide triangle is invalid");
    const nanoxgen::ClassicReferenceSurfaceSample quad =
        imported.reference_surface->evaluate(
            "testPatch", 3u, 0.5f, 0.5f);
    require(std::isfinite(quad.position.x) &&
                std::isfinite(quad.position.y) &&
                std::isfinite(quad.position.z),
            "quad after non-quad PTEX face evaluated non-finite");
}

void test_motion_lookup_and_interpolation() {
    const TemporaryArchive archive = write_animated_archive();
    require(
        !nanoxgen::xgen_classic_alembic_deformation_is_static(
            description(), archive.path),
        "animated archive was classified as static");
    nanoxgen::ClassicAlembicFrameSample sample{};
    sample.frame = 24.0;
    sample.lookup_offset = 12.0;
    sample.frames_per_second = 24.0;
    sample.interpolation =
        nanoxgen::ClassicAlembicInterpolation::Linear;
    const nanoxgen::ClassicAlembicAssetInput interpolated =
        nanoxgen::build_xgen_classic_alembic_asset_input(
            description(), archive.path, sample);
    require(
        near_value(interpolated.asset.positions[0].x, 2.79289322f) &&
            near_value(interpolated.asset.positions[0].y, 4.70710678f) &&
            near_value(interpolated.asset.positions[0].z, 5.0f),
        "linear frame lookup did not interpolate transform op channels");

    sample.interpolation = nanoxgen::ClassicAlembicInterpolation::None;
    const nanoxgen::ClassicAlembicAssetInput previous =
        nanoxgen::build_xgen_classic_alembic_asset_input(
            description(), archive.path, sample);
    require(
        near_value(previous.asset.positions[0].x, 3.0f) &&
            near_value(previous.asset.positions[0].y, 4.0f) &&
            near_value(previous.asset.positions[0].z, 5.0f),
        "none interpolation did not select the previous archive sample");
}

void test_motion_topology_change_rejected() {
    const TemporaryArchive archive = write_animated_archive(true);
    nanoxgen::ClassicAlembicFrameSample sample{};
    sample.frame = 48.0;
    sample.lookup_offset = 12.0;
    sample.frames_per_second = 24.0;
    try {
        (void)nanoxgen::build_xgen_classic_alembic_asset_input(
            description(), archive.path, sample);
    } catch (const std::runtime_error &error) {
        require(
            std::string{error.what()}.find("topology changes") !=
                std::string::npos,
            "wrong animated topology diagnostic");
        return;
    }
    throw std::runtime_error("animated topology change was accepted");
}

void test_guide_cache_import_and_motion() {
    const TemporaryArchive surface_archive = write_archive();
    nanoxgen::ClassicAlembicAssetInput imported =
        nanoxgen::build_xgen_classic_alembic_asset_input(
            description(), surface_archive.path);
    const TemporaryArchive static_cache = write_guide_cache(false);
    require(
        nanoxgen::xgen_classic_alembic_guide_cache_is_static(
            static_cache.path),
        "single-sample guide cache was not classified as static");
    nanoxgen::apply_xgen_classic_alembic_guide_cache(
        imported, static_cache.path);
    require(
        near_value(imported.asset.guides[0].cvs[0].x, 3.5f) &&
            near_value(imported.asset.guides[0].cvs[0].y, 4.0f) &&
            near_value(imported.asset.guides[0].cvs[0].z, 6.0f) &&
            near_value(imported.asset.guides[0].cvs[1].x, 7.0f) &&
            near_value(imported.asset.guides[0].cvs[1].y, 9.0f) &&
            near_value(imported.asset.guides[0].cvs[1].z, 11.0f),
        "guide-cache transform or preserved surface root is incorrect");

    const TemporaryArchive animated_cache = write_guide_cache(true);
    require(
        !nanoxgen::xgen_classic_alembic_guide_cache_is_static(
            animated_cache.path),
        "animated guide cache was classified as static");
    nanoxgen::ClassicAlembicFrameSample sample{};
    sample.frame = 24.0;
    sample.lookup_offset = 12.0;
    sample.frames_per_second = 24.0;
    sample.interpolation = nanoxgen::ClassicAlembicInterpolation::Linear;
    nanoxgen::apply_xgen_classic_alembic_guide_cache(
        imported, animated_cache.path, sample);
    require(
        near_value(imported.asset.guides[0].cvs[0].x, 3.5f) &&
            near_value(imported.asset.guides[0].cvs[0].y, 4.0f) &&
            near_value(imported.asset.guides[0].cvs[1].x, 12.0f) &&
            near_value(imported.asset.guides[0].cvs[1].y, 2.0f),
        "guide-cache motion or preserved surface root is incorrect");
}

void test_guide_cache_mismatch_rejected() {
    const TemporaryArchive surface_archive = write_archive();
    nanoxgen::ClassicAlembicAssetInput imported =
        nanoxgen::build_xgen_classic_alembic_asset_input(
            description(), surface_archive.path);
    const TemporaryArchive cache = write_guide_cache(false, 3u);
    try {
        nanoxgen::apply_xgen_classic_alembic_guide_cache(
            imported, cache.path);
    } catch (const std::runtime_error &error) {
        require(
            std::string{error.what()}.find("CV count") != std::string::npos,
            "wrong guide-cache mismatch diagnostic");
        return;
    }
    throw std::runtime_error("mismatched guide cache was accepted");
}

void test_guide_cache_authored_mapping_allows_nonrigid_roots() {
    const TemporaryArchive surface_archive = write_archive();
    nanoxgen::ClassicAlembicAssetInput imported =
        nanoxgen::build_xgen_classic_alembic_asset_input(
            description(), surface_archive.path);
    nanoxgen::GuideInput second = imported.asset.guides.front();
    second.cvs = {{1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}};
    imported.asset.guides.push_back(std::move(second));

    const TemporaryArchive cache = write_named_guide_cache();
    nanoxgen::apply_xgen_classic_alembic_guide_cache(
        imported, cache.path, "|curve10|curveShape10,|curve20|curveShape20");
    require(
        near_value(imported.asset.guides[0].cvs[0].x, 3.5f) &&
            near_value(imported.asset.guides[1].cvs[0].x, 1.0f) &&
            near_value(imported.asset.guides[0].cvs[1].x, 100.0f) &&
            near_value(imported.asset.guides[1].cvs[1].x, 200.0f),
        "authored guide-cache mapping or preserved roots are incorrect");
}

void test_guide_cache_restores_directional_support_frame() {
    const TemporaryArchive surface_archive = write_archive();
    nanoxgen::ClassicAlembicAssetInput imported =
        nanoxgen::build_xgen_classic_alembic_asset_input(
            description(), surface_archive.path);
    nanoxgen::GuideInput &guide = imported.asset.guides.front();
    guide.cvs = {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    guide.reference_root_position = {0.0f, 0.0f, 0.0f};
    guide.reference_root_normal = {0.0f, 1.0f, 0.0f};
    guide.reference_root_tangent = {1.0f, 0.0f, 0.0f};
    guide.reference_root_binormal = {0.0f, 0.0f, 1.0f};
    guide.support_radii = {3.0f, 0.5f, 2.0f, 2.0f, 2.0f};
    guide.support_angles = {0.0f, 1.0f, 2.0f, 3.0f};

    nanoxgen::GuideInput neighbor = guide;
    neighbor.cvs = {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}};
    neighbor.reference_root_position = {0.0f, 0.0f, 1.0f};
    neighbor.support_radii.clear();
    neighbor.support_angles.clear();
    imported.asset.guides.push_back(std::move(neighbor));

    const TemporaryArchive cache = write_guide_cache(false, 2u, 2u);
    nanoxgen::apply_xgen_classic_alembic_guide_cache(imported, cache.path);
    const nanoxgen::GuideInput &oriented = imported.asset.guides.front();
    require(
        near_value(oriented.reference_root_tangent.x, 0.0f) &&
            near_value(oriented.reference_root_tangent.y, 0.0f) &&
            near_value(oriented.reference_root_tangent.z, 1.0f) &&
            near_value(oriented.reference_root_binormal.x, -1.0f) &&
            near_value(oriented.reference_root_binormal.y, 0.0f) &&
            near_value(oriented.reference_root_binormal.z, 0.0f),
        "guide-cache directional support frame was not restored");
}

} // namespace

int main() try {
    test_import();
    test_subd_import();
    test_nonquad_subd_ptex_faces();
    test_missing_patch();
    test_motion_lookup_and_interpolation();
    test_motion_topology_change_rejected();
    test_guide_cache_import_and_motion();
    test_guide_cache_mismatch_rejected();
    test_guide_cache_authored_mapping_allows_nonrigid_roots();
    test_guide_cache_restores_directional_support_frame();
    std::cout << "Classic Alembic import tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
}
