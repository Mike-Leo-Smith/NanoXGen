#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace nanoxgen {

enum class XGenPackageFileKind {
    ClassicCollection,
    ClassicDescription,
    ClassicDelta,
    ClassicFxModule,
    InteractiveGroomPreset,
    EvaluatedSplineBlob,
    MayaAsciiScene,
    MayaBinaryScene,
    CurveCache,
    NanoXGenAsset,
    Caf,
    Alembic,
    Ptex,
    Xuv,
    Xpd,
    Xgc,
    Archive,
    Texture,
    Script,
    Directory,
    Symlink,
    Unknown,
};

enum class XGenReferenceStatus {
    Resolved,
    Missing,
    UnresolvedVariable,
    External,
    Unsafe,
};

enum class XGenEvaluationBackend {
    NativeSnapshot,
    AutodeskClassicTyped,
    AutodeskInteractiveMaya,
    InventoryOnly,
};

struct XGenPackageFile {
    std::filesystem::path relative_path;
    XGenPackageFileKind kind{XGenPackageFileKind::Unknown};
    std::uint64_t byte_size{};
};

struct XGenPackageReference {
    std::filesystem::path source;
    std::string literal;
    std::optional<std::filesystem::path> resolved_path;
    XGenReferenceStatus status{XGenReferenceStatus::UnresolvedVariable};
};

struct XGenPackageOptions {
    std::size_t max_files{100000u};
    std::uint64_t max_text_file_bytes{16u * 1024u * 1024u};
    // PROJECT defaults to the package root. DESC/PAL and studio variables are
    // intentionally explicit because guessing them can silently bind a wrong
    // production dependency.
    std::map<std::string, std::filesystem::path> variables;
};

struct XGenPackageManifest {
    std::filesystem::path root;
    std::vector<XGenPackageFile> files;
    std::vector<XGenPackageReference> references;
    std::vector<std::string> diagnostics;
    XGenEvaluationBackend backend{XGenEvaluationBackend::InventoryOnly};
    // False means a Maya DG, unresolved variable, missing dependency, unsafe
    // reference, unreadable text container, or an enforced scan limit prevents
    // NanoXGen from proving the dependency closure.
    bool dependency_closure_complete{true};
};

[[nodiscard]] XGenPackageManifest scan_xgen_package(
    const std::filesystem::path &root,
    const XGenPackageOptions &options = {});

[[nodiscard]] const char *to_string(XGenPackageFileKind kind) noexcept;
[[nodiscard]] const char *to_string(XGenReferenceStatus status) noexcept;
[[nodiscard]] const char *to_string(XGenEvaluationBackend backend) noexcept;

} // namespace nanoxgen
