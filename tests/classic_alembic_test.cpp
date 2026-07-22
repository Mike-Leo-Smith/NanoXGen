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

nanoxgen::ClassicDescription description() {
    nanoxgen::ClassicGuide guide{};
    guide.id = 7u;
    guide.patch_u = 0.25;
    guide.patch_v = 0.5;
    guide.face_id = 0u;
    guide.cv_offset = 0u;
    guide.cv_count = 2u;
    nanoxgen::ClassicPatch patch{};
    patch.type = "Subd";
    patch.name = "testPatch";
    patch.face_ids = {0u};
    patch.guides = {guide};
    patch.guide_cvs = {{0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}};
    nanoxgen::ClassicDescription result{};
    result.name = "test";
    result.patches.emplace_back(std::move(patch));
    return result;
}

void test_import() {
    const TemporaryArchive archive = write_archive();
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
    require(near(guide.cvs[1].x, 3.5f) && near(guide.cvs[1].y, 5.0f) &&
                near(guide.cvs[1].z, 6.0f),
            "transformed relative guide CV mismatch");
    require(guide.triangle_index == 1u && near(guide.barycentric.x, 0.25f) &&
                near(guide.barycentric.y, 0.25f),
            "guide triangle binding mismatch");
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

} // namespace

int main() try {
    test_import();
    test_missing_patch();
    std::cout << "Classic Alembic import tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
}
