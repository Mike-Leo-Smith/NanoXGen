#include "nanoxgen/xgen_package.h"

#include "nanoxgen/xgen.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

using namespace nanoxgen;

namespace {

void require(bool condition, const char *message) {
    if (!condition) { throw std::runtime_error(message); }
}

struct TempDirectory {
    std::filesystem::path path;
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto nonce = std::random_device{}();
        path = std::filesystem::temp_directory_path() /
            ("nanoxgen-package-test-" + std::to_string(stamp) + "-" +
             std::to_string(nonce));
        std::filesystem::create_directories(path);
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void write_text(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
    if (!output) { throw std::runtime_error("failed to write test fixture"); }
}

const XGenPackageReference *find_reference(
    const XGenPackageManifest &manifest, const std::string &literal) {
    const auto found = std::find_if(
        manifest.references.begin(), manifest.references.end(),
        [&](const XGenPackageReference &reference) {
            return reference.literal == literal;
        });
    return found == manifest.references.end() ? nullptr : &*found;
}

void test_classic_inventory_and_boundaries() {
    TempDirectory temporary;
    const std::filesystem::path root = temporary.path / "asset";
    std::filesystem::create_directories(root / "desc/density");
    write_text(root / "desc/density/map.ptx", "ptex-placeholder");
    write_text(root / "xgen/maps/project-map.ptx", "ptex-placeholder");
    write_text(root / "archives/fur.ass", "archive");
    write_text(
        root / "collection.xgen",
        "Palette\n"
        "density \"${DESC}/density/\"\n"
        "archive \"archives/fur.ass\"\n"
        "projectMap \"${PROJECT}xgen/maps/project-map.ptx\"\n"
        "missing \"${DESC}/missing.ptx\"\n"
        "external \"${EXTERNAL}/outside.abc\"\n"
        "unresolved \"${STUDIO}/map.ptx\"\n"
        "symlinked \"density-link/map.ptx\"\n"
        "expression \"a/b\"\n");
    std::error_code symlink_error;
    std::filesystem::create_directory_symlink(
        root / "desc/density", root / "density-link", symlink_error);

    XGenPackageOptions options{};
    options.variables["DESC"] = root / "desc";
    options.variables["EXTERNAL"] = temporary.path / "outside";
    const XGenPackageManifest manifest = scan_xgen_package(root, options);
    require(manifest.backend == XGenEvaluationBackend::AutodeskClassicTyped,
            "Classic package must select the typed Autodesk backend");
    require(!manifest.dependency_closure_complete,
            "missing, external, and unresolved dependencies must make closure incomplete");
    const XGenPackageReference *density = find_reference(
        manifest, "${DESC}/density/");
    const XGenPackageReference *archive = find_reference(
        manifest, "archives/fur.ass");
    const XGenPackageReference *missing = find_reference(
        manifest, "${DESC}/missing.ptx");
    const XGenPackageReference *project_map = find_reference(
        manifest, "${PROJECT}xgen/maps/project-map.ptx");
    const XGenPackageReference *external = find_reference(
        manifest, "${EXTERNAL}/outside.abc");
    const XGenPackageReference *unresolved = find_reference(
        manifest, "${STUDIO}/map.ptx");
    require(density && density->status == XGenReferenceStatus::Resolved,
            "DESC directory dependency must resolve");
    require(archive && archive->status == XGenReferenceStatus::Resolved,
            "relative archive dependency must resolve");
    require(missing && missing->status == XGenReferenceStatus::Missing,
            "missing dependency must be reported");
    require(project_map && project_map->status == XGenReferenceStatus::Resolved,
            "concatenated PROJECT variable must resolve as a directory prefix");
    require(external && external->status == XGenReferenceStatus::External,
            "package-external dependency must be reported");
    require(unresolved && unresolved->status == XGenReferenceStatus::UnresolvedVariable,
            "unknown variable must be reported");
    require(!find_reference(manifest, "a/b"),
            "division expression must not be misclassified as a path");
    if (!symlink_error) {
        const XGenPackageReference *symlinked = find_reference(
            manifest, "density-link/map.ptx");
        require(std::any_of(
                    manifest.files.begin(), manifest.files.end(),
                    [](const XGenPackageFile &file) {
                        return file.kind == XGenPackageFileKind::Symlink;
                    }),
                "symbolic link must be inventoried without traversal");
        require(symlinked && symlinked->status == XGenReferenceStatus::Unsafe,
                "reference through a symlink component must be unsafe");
    }

    write_text(root / "interactive.ma", "requires maya DG");
    const XGenPackageManifest interactive = scan_xgen_package(root, options);
    require(interactive.backend == XGenEvaluationBackend::AutodeskInteractiveMaya &&
                !interactive.dependency_closure_complete,
            "Maya scene must select Maya backend and prevent a complete core-only closure");
}

void test_content_based_xgen_detection() {
    TempDirectory temporary;
    const std::filesystem::path blob = temporary.path / "snapshot.xgen";
    XGenDocument document{};
    document.metadata_json = R"json({"Header":{"Version":1,"Type":"XgSplineData","GroupVersion":1,"GroupCount":1,"GroupBase64":false,"GroupDeflate":true,"GroupDeflateLevel":9},"Items":[],"RefMeshArray":[],"CustomData":{}})json";
    document.version = 1u;
    document.group_version = 1u;
    document.group_deflate = true;
    document.group_deflate_level = 9u;
    document.groups = {{0u, 0u, {}}};
    save_xgen_document(document, blob);
    const XGenPackageManifest manifest = scan_xgen_package(temporary.path);
    require(manifest.files.size() == 1u &&
                manifest.files[0].kind == XGenPackageFileKind::EvaluatedSplineBlob,
            ".xgen must be classified by content rather than extension alone");
    require(manifest.backend == XGenEvaluationBackend::NativeSnapshot,
            "evaluated snapshot must select the native backend");
}

void test_explicit_symlink_root_rejected() {
    TempDirectory temporary;
    const std::filesystem::path target = temporary.path / "target";
    std::filesystem::create_directories(target);
    const std::filesystem::path link = temporary.path / "link";
    std::error_code error;
    std::filesystem::create_directory_symlink(target, link, error);
    if (error) { return; }
    bool rejected = false;
    try {
        (void)scan_xgen_package(link);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "explicit symbolic-link root must be rejected");
}

} // namespace

int main() try {
    test_classic_inventory_and_boundaries();
    test_content_based_xgen_detection();
    test_explicit_symlink_root_rejected();
    std::cout << "all NanoXGen package tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
}
