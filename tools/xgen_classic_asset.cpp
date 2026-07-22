#include "nanoxgen/asset.h"
#include "nanoxgen/xgen_classic.h"
#include "nanoxgen/xgen_classic_alembic.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct Options {
    std::string description;
    std::filesystem::path output;
    std::filesystem::path collection;
    std::filesystem::path archive;
};

Options parse_options(int argc, char **argv) {
    Options options{};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        auto value = [&](const char *name) -> std::string_view {
            if (++index >= argc) {
                throw std::runtime_error(std::string{"missing value for "} + name);
            }
            return argv[index];
        };
        if (argument == "--description") {
            options.description = value("--description");
        } else if (argument == "--output") {
            options.output = value("--output");
        } else if (options.collection.empty()) {
            options.collection = argument;
        } else if (options.archive.empty()) {
            options.archive = argument;
        } else {
            throw std::runtime_error("unexpected argument: " +
                                     std::string{argument});
        }
    }
    if (options.description.empty() || options.output.empty() ||
        options.collection.empty() || options.archive.empty()) {
        throw std::runtime_error(
            "usage: nanoxgen_xgen_classic_asset --description NAME "
            "--output OUTPUT.nxg COLLECTION.xgen PATCHES.abc");
    }
    return options;
}

} // namespace

int main(int argc, char **argv) try {
    const Options options = parse_options(argc, argv);
    const nanoxgen::ClassicCollection collection =
        nanoxgen::load_xgen_classic_collection(options.collection);
    const nanoxgen::ClassicDescription *description =
        nanoxgen::find_classic_description(collection, options.description);
    if (!description) {
        throw std::runtime_error("Classic description not found: " +
                                 options.description);
    }
    nanoxgen::ClassicAlembicAssetInput imported =
        nanoxgen::build_xgen_classic_alembic_asset_input(
            *description, options.archive);
    const std::size_t vertex_count = imported.asset.positions.size();
    const std::size_t triangle_count = imported.asset.triangles.size();
    const std::size_t guide_count = imported.asset.guides.size();
    std::size_t guide_cv_count = 0u;
    for (const nanoxgen::GuideInput &guide : imported.asset.guides) {
        guide_cv_count += guide.cvs.size();
    }
    const nanoxgen::Asset asset = nanoxgen::build_asset(imported.asset);
    nanoxgen::save_asset(asset, options.output);
    std::cout << "{\"description\":\"" << options.description
              << "\",\"source_vertices\":" << imported.source_vertex_count
              << ",\"source_faces\":" << imported.source_face_count
              << ",\"selected_faces\":" << imported.selected_face_count
              << ",\"vertices\":" << vertex_count
              << ",\"triangles\":" << triangle_count
              << ",\"guides\":" << guide_count
              << ",\"guide_cvs\":" << guide_cv_count
              << ",\"asset_bytes\":" << asset.bytes().size() << "}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
