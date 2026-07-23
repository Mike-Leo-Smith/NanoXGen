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

namespace {

void require(bool condition, const char *message) {
    if (!condition) { throw std::runtime_error(message); }
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 1.0e-6f;
}

struct TemporaryArchive {
    std::filesystem::path path;
    ~TemporaryArchive() {
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
    require(near(guide.cvs[0].x, 3.5f) && near(guide.cvs[0].y, 4.0f) &&
                near(guide.cvs[0].z, 6.0f),
            "transformed bilinear guide root mismatch");
    require(near(guide.cvs[1].x, 4.5f) && near(guide.cvs[1].y, 6.0f) &&
                near(guide.cvs[1].z, 3.0f),
            "patch-frame guide CV transform mismatch");
    require(guide.triangle_index == 1u && near(guide.barycentric.x, 0.25f) &&
                near(guide.barycentric.y, 0.25f),
            "guide triangle binding mismatch");
    require(guide.support_radii.size() == 2u &&
                near(guide.support_radii[0], 3.0f) &&
                near(guide.support_radii[1], 1.5f) &&
                guide.support_angles.size() == 1u &&
                near(guide.support_angles[0], 0.5f),
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
    const nanoxgen::GuideInput &guide = imported.asset.guides.front();
    require(guide.triangle_index < imported.asset.triangles.size(),
            "subdivision guide triangle is invalid");
    require(std::isfinite(guide.cvs[0].x) && std::isfinite(guide.cvs[0].y) &&
                std::isfinite(guide.cvs[0].z),
            "subdivision guide root is non-finite");
    require(near(guide.cvs[1].y - guide.cvs[0].y, 2.0f),
            "subdivision guide relative CV mismatch");
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
        near(interpolated.asset.positions[0].x, 2.79289322f) &&
            near(interpolated.asset.positions[0].y, 4.70710678f) &&
            near(interpolated.asset.positions[0].z, 5.0f),
        "linear frame lookup did not interpolate transform op channels");

    sample.interpolation = nanoxgen::ClassicAlembicInterpolation::None;
    const nanoxgen::ClassicAlembicAssetInput previous =
        nanoxgen::build_xgen_classic_alembic_asset_input(
            description(), archive.path, sample);
    require(
        near(previous.asset.positions[0].x, 3.0f) &&
            near(previous.asset.positions[0].y, 4.0f) &&
            near(previous.asset.positions[0].z, 5.0f),
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

} // namespace

int main() try {
    test_import();
    test_subd_import();
    test_missing_patch();
    test_motion_lookup_and_interpolation();
    test_motion_topology_change_rejected();
    std::cout << "Classic Alembic import tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
}
