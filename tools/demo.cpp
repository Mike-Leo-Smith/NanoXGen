#include "nanoxgen/asset.h"
#include "nanoxgen/xgen.h"

#include <filesystem>
#include <iostream>

using namespace nanoxgen;

namespace {

AssetBuildInput make_demo_input() {
    AssetBuildInput input{};
    input.positions = {{-1.0f, 0.0f, -1.0f}, {1.0f, 0.0f, -1.0f},
                       {1.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 1.0f}};
    input.normals.assign(4u, {0.0f, 1.0f, 0.0f});
    input.texcoords = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    input.triangles = {{0u, 1u, 2u}, {0u, 2u, 3u}};

    const auto guide = [](float x, float z, float bend_x, float bend_z) {
        GuideInput g{};
        g.root_normal = {0.0f, 1.0f, 0.0f};
        g.support_radius = 1.8f;
        for (int i = 0; i < 6; ++i) {
            const float t = static_cast<float>(i) / 5.0f;
            g.cvs.push_back({x + bend_x * t * t, 1.2f * t, z + bend_z * t * t});
        }
        return g;
    };
    input.guides.push_back(guide(-0.75f, -0.75f, -0.20f, 0.05f));
    input.guides.push_back(guide( 0.75f, -0.75f,  0.15f, 0.20f));
    input.guides.push_back(guide( 0.75f,  0.75f,  0.25f, 0.05f));
    input.guides.push_back(guide(-0.75f,  0.75f, -0.10f, 0.25f));
    return input;
}

} // namespace

int main(int argc, char **argv) try {
    const std::filesystem::path output_dir = argc > 1 ? argv[1] : "build/demo";
    std::filesystem::create_directories(output_dir);
    const Asset asset = build_asset(make_demo_input());
    save_asset(asset, output_dir / "demo.nxg");

    GenerationParams params{};
    params.strand_count = 4096u;
    params.cvs_per_strand = 12u;
    params.seed = 0x12345678u;
    params.root_width = 0.012f;
    params.tip_width = 0.001f;
    params.noise_amplitude = 0.025f;
    params.noise_frequency = 2.5f;
    const GeneratedCurves curves = generate_cpu(asset, params);
    write_curves_obj(curves, output_dir / "curves.obj");
    save_xgen_document(build_xgen_document(make_xgen_curves(curves)),
                       output_dir / "curves.xgen");

    const AssetHeader h = asset.view().header();
    std::cout << "NanoXGen demo generated\n"
              << "  asset bytes: " << h.byte_size << '\n'
              << "  triangles:   " << h.triangle_count << '\n'
              << "  guides:      " << h.guide_count << '\n'
              << "  strands:     " << curves.strand_count << '\n'
              << "  OBJ output:  " << (output_dir / "curves.obj") << '\n'
              << "  XGen output: " << (output_dir / "curves.xgen") << '\n';
    return 0;
} catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
}
