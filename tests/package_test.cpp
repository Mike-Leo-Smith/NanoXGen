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
        // macOS exposes its temporary directory through /var, which is itself
        // a system symlink. Resolve only the test harness base so package
        // fixtures do not accidentally begin behind a symlink; the tests below
        // still create and verify their own explicit symlink components.
        const std::filesystem::path temporary_root =
            std::filesystem::canonical(std::filesystem::temp_directory_path());
        path = temporary_root /
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

const XGenPackageFile *find_file(
    const XGenPackageManifest &manifest, const std::string &relative_path) {
    const auto found = std::find_if(
        manifest.files.begin(), manifest.files.end(),
        [&](const XGenPackageFile &file) {
            return file.relative_path.generic_string() == relative_path;
        });
    return found == manifest.files.end() ? nullptr : &*found;
}

void test_classic_inventory_and_boundaries() {
    TempDirectory temporary;
    const std::filesystem::path root = temporary.path / "asset";
    std::filesystem::create_directories(root / "desc/density");
    write_text(root / "desc/density/map.ptx", "ptex-placeholder");
    write_text(root / "xgen/maps/project-map.ptx", "ptex-placeholder");
    write_text(root / "archives/fur.ass", "archive");
    write_text(root / "archives/windows-separators.abc", "archive");
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
        "mixed \"archives\\windows-separators.abc\"\n"
        "driveRelative \"C:relative\\ambiguous.abc\"\n"
        "rootRelative \"\\root\\ambiguous.abc\"\n"
        "unc \"\\\\server\\share\\external.abc\"\n"
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
    require(manifest.execution_plan.requires_autodesk &&
                !manifest.execution_plan.native_compatible &&
                manifest.execution_plan.backend == manifest.backend,
            "Classic package must include an explicit Autodesk execution plan");
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
    const XGenPackageReference *mixed = find_reference(
        manifest, "archives\\windows-separators.abc");
    const XGenPackageReference *drive_relative = find_reference(
        manifest, "C:relative\\ambiguous.abc");
    const XGenPackageReference *root_relative = find_reference(
        manifest, "\\root\\ambiguous.abc");
    const XGenPackageReference *unc = find_reference(
        manifest, "\\\\server\\share\\external.abc");
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
    require(mixed && mixed->status == XGenReferenceStatus::Resolved,
            "foreign separators in a relative dependency must resolve");
    require(drive_relative &&
                drive_relative->status == XGenReferenceStatus::Unsafe,
            "drive-relative Windows dependency must not bind to the scan CWD");
    require(root_relative &&
                root_relative->status == XGenReferenceStatus::Unsafe,
            "root-relative Windows dependency must not bind to a process drive");
    require(unc && unc->status == XGenReferenceStatus::External,
            "UNC dependency must remain package-external");
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
    require(interactive.execution_plan.requires_autodesk &&
                interactive.execution_plan.stages.size() == 3u,
            "Interactive package must describe its Maya execution stages");
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
    require(manifest.execution_plan.native_compatible &&
                !manifest.execution_plan.requires_autodesk,
            "standalone evaluated snapshot must have a native execution plan");

    write_text(temporary.path / "mask.ptx", "ptex-placeholder");
    const XGenPackageManifest with_ptex = scan_xgen_package(temporary.path);
    require(with_ptex.backend == XGenEvaluationBackend::AutodeskInteractiveMaya &&
                with_ptex.execution_plan.requires_autodesk &&
                !with_ptex.execution_plan.native_compatible,
            "PTEX sidecars without a Classic owner must select Autodesk fallback");
}

void test_bounded_text_scan_and_content_probe() {
    TempDirectory temporary;
    const std::filesystem::path root = temporary.path / "asset";
    std::filesystem::create_directories(root);
    write_text(root / "early.abc", "archive");
    write_text(root / "late.abc", "archive");
    write_text(
        root / "collection.xgen",
        "early \"${PROJECT}/early.abc\"\n"
        "// ignored /not/a/dependency.abc\n"
        "value 1 # ignored /also/not/a/dependency.abc\n"
        "windows \"C:\\show\\asset\\external.abc\"\n" +
        std::string(1024u, 'x') +
        "\nlate \"${PROJECT}/late.abc\"\n");
    XGenPackageOptions options{};
    options.max_text_file_bytes = 220u;
    const XGenPackageManifest manifest = scan_xgen_package(root, options);
    const XGenPackageReference *early = find_reference(
        manifest, "${PROJECT}/early.abc");
    const XGenPackageReference *windows = find_reference(
        manifest, "C:\\show\\asset\\external.abc");
    require(early && early->status == XGenReferenceStatus::Resolved,
            "bounded prefix scan must retain early dependencies from large text files");
    require(windows && windows->status == XGenReferenceStatus::External &&
                windows->resolved_path &&
                windows->resolved_path->generic_string() == "C:/show/asset/external.abc",
            "Windows absolute paths must normalize and remain package-external");
    require(!find_reference(manifest, "/not/a/dependency.abc"),
            "full-line comments must not create dependencies");
    require(!find_reference(manifest, "/also/not/a/dependency.abc"),
            "inline XGen comments must not create dependencies");
    require(!find_reference(manifest, "${PROJECT}/late.abc") &&
                !manifest.dependency_closure_complete,
            "truncated text scan must not emit partial/late references or claim closure");
    require(std::any_of(
                manifest.diagnostics.begin(), manifest.diagnostics.end(),
                [](const std::string &diagnostic) {
                    return diagnostic.find("bounded prefix") != std::string::npos;
                }),
            "bounded text scan must report its enforced limit");

    TempDirectory binary_temporary;
    const std::filesystem::path binary_root = binary_temporary.path / "asset";
    std::filesystem::create_directories(binary_root);
    std::string binary = "fake \"/must/not/be/reported.abc\"";
    binary.push_back('\0');
    binary += "tail";
    write_text(binary_root / "binary.xgen", binary);
    const XGenPackageManifest binary_manifest = scan_xgen_package(binary_root);
    require(binary_manifest.references.empty() &&
                !binary_manifest.dependency_closure_complete,
            "an extension alone must not make binary content a text container");
    require(std::any_of(
                binary_manifest.diagnostics.begin(), binary_manifest.diagnostics.end(),
                [](const std::string &diagnostic) {
                    return diagnostic.find("NUL bytes") != std::string::npos;
                }),
            "binary content rejection must be diagnosed");
}

void test_sidecar_classification() {
    TempDirectory temporary;
    write_text(temporary.path / "description.xdsc", "description");
    write_text(temporary.path / "delta.xgd", "delta");
    write_text(temporary.path / "module.xgfx", "module");
    write_text(temporary.path / "preset.xgip", "preset");
    write_text(temporary.path / "data.caf", "caf");
    const XGenPackageManifest manifest = scan_xgen_package(temporary.path);
    require(find_file(manifest, "description.xdsc")->kind ==
                XGenPackageFileKind::ClassicDescription &&
                find_file(manifest, "delta.xgd")->kind ==
                XGenPackageFileKind::ClassicDelta &&
                find_file(manifest, "module.xgfx")->kind ==
                XGenPackageFileKind::ClassicFxModule &&
                find_file(manifest, "preset.xgip")->kind ==
                XGenPackageFileKind::InteractiveGroomPreset &&
                find_file(manifest, "data.caf")->kind == XGenPackageFileKind::Caf,
            "Classic/Interactive sidecar extensions must keep their typed inventory kinds");
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

    const std::filesystem::path real_parent = temporary.path / "real-parent";
    const std::filesystem::path nested_root = real_parent / "asset";
    std::filesystem::create_directories(nested_root);
    const std::filesystem::path parent_link = temporary.path / "parent-link";
    error.clear();
    std::filesystem::create_directory_symlink(real_parent, parent_link, error);
    if (error) { return; }
    rejected = false;
    try {
        (void)scan_xgen_package(parent_link / "asset");
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "package root must reject a symlink in any intermediate component");
}

} // namespace

int main() try {
    test_classic_inventory_and_boundaries();
    test_content_based_xgen_detection();
    test_bounded_text_scan_and_content_probe();
    test_sidecar_classification();
    test_explicit_symlink_root_rejected();
    std::cout << "all NanoXGen package tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
}
