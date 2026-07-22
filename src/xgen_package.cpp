#include "nanoxgen/xgen_package.h"

#include "nanoxgen/curve_cache.h"
#include "nanoxgen/types.h"
#include "nanoxgen/xgen.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace nanoxgen {
namespace {

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::uint64_t read_prefix_u64(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 8u> bytes{};
    input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) { return 0u; }
    std::uint64_t value = 0u;
    for (std::size_t index = 0u; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8u);
    }
    return value;
}

XGenPackageFileKind classify_file(const std::filesystem::path &path) {
    const std::string extension = lowercase(path.extension().string());
    if (extension == ".xgen") {
        return read_prefix_u64(path) == kXGenFileMagic
            ? XGenPackageFileKind::EvaluatedSplineBlob
            : XGenPackageFileKind::ClassicCollection;
    }
    if (extension == ".xdsc") { return XGenPackageFileKind::ClassicDescription; }
    if (extension == ".xgd") { return XGenPackageFileKind::ClassicDelta; }
    if (extension == ".xgfx") { return XGenPackageFileKind::ClassicFxModule; }
    if (extension == ".xgip") { return XGenPackageFileKind::InteractiveGroomPreset; }
    if (extension == ".ma") { return XGenPackageFileKind::MayaAsciiScene; }
    if (extension == ".mb") { return XGenPackageFileKind::MayaBinaryScene; }
    if (extension == ".nxc") { return XGenPackageFileKind::CurveCache; }
    if (extension == ".nxg") { return XGenPackageFileKind::NanoXGenAsset; }
    if (extension == ".caf") { return XGenPackageFileKind::Caf; }
    if (extension == ".abc") { return XGenPackageFileKind::Alembic; }
    if (extension == ".ptx" || extension == ".ptex") {
        return XGenPackageFileKind::Ptex;
    }
    if (extension == ".xuv") { return XGenPackageFileKind::Xuv; }
    if (extension == ".xpd") { return XGenPackageFileKind::Xpd; }
    if (extension == ".xgc") { return XGenPackageFileKind::Xgc; }
    if (extension == ".ass" || extension == ".usd" || extension == ".usda" ||
        extension == ".usdc" || extension == ".obj" || extension == ".fbx" ||
        extension == ".rib") {
        return XGenPackageFileKind::Archive;
    }
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
        extension == ".exr" || extension == ".tif" || extension == ".tiff" ||
        extension == ".tx" || extension == ".bmp" || extension == ".iff" ||
        extension == ".cltx") {
        return XGenPackageFileKind::Texture;
    }
    if (extension == ".py" || extension == ".mel" || extension == ".so" ||
        extension == ".dll" || extension == ".dylib") {
        return XGenPackageFileKind::Script;
    }
    return XGenPackageFileKind::Unknown;
}

bool is_text_container(XGenPackageFileKind kind) noexcept {
    return kind == XGenPackageFileKind::ClassicCollection ||
        kind == XGenPackageFileKind::ClassicDescription ||
        kind == XGenPackageFileKind::ClassicDelta ||
        kind == XGenPackageFileKind::ClassicFxModule ||
        kind == XGenPackageFileKind::InteractiveGroomPreset ||
        kind == XGenPackageFileKind::MayaAsciiScene;
}

bool has_known_dependency_extension(std::string_view value) {
    const std::filesystem::path path{value};
    const std::string extension = lowercase(path.extension().string());
    static constexpr auto known = std::to_array<std::string_view>({
        ".xgen", ".xdsc", ".xgd", ".xgfx", ".xgip", ".ma", ".mb",
        ".nxc", ".nxg", ".caf", ".abc", ".ptx", ".ptex", ".xuv",
        ".xpd", ".xgc", ".ass", ".usd", ".usda", ".usdc", ".obj",
        ".fbx", ".rib", ".png", ".jpg", ".jpeg", ".exr", ".tif",
        ".tiff", ".tx", ".bmp", ".iff", ".cltx", ".py", ".mel",
        ".so", ".dll", ".dylib"});
    return std::find(known.begin(), known.end(), extension) != known.end();
}

std::string trim_candidate(std::string value) {
    const auto punctuation = [](unsigned char c) {
        return std::isspace(c) || c == '\'' || c == '"' || c == ';' || c == ',' ||
            c == '(' || c == ')' || c == '[' || c == ']';
    };
    while (!value.empty() && punctuation(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && punctuation(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

bool looks_like_reference(const std::string &candidate) {
    if (candidate.empty()) { return false; }
    const bool has_expression_operator =
        candidate.find_first_of("+*()[]<>=?") != std::string::npos;
    if (candidate.starts_with("${")) { return !has_expression_operator; }
    if (has_known_dependency_extension(candidate)) { return !has_expression_operator; }
    if (candidate.starts_with("./") || candidate.starts_with("../") ||
        candidate.starts_with('/')) {
        return !has_expression_operator;
    }
    // A bare expression such as a/b is not a path. Requiring a known suffix
    // avoids the false positives common in XGen density expressions.
    return false;
}

class ReferenceExtractor {
public:
    void feed(std::string_view text) {
        for (const char c : text) { feed(c); }
    }

    [[nodiscard]] std::vector<std::string> finish(bool complete) {
        if (complete) { flush(); }
        std::sort(_candidates.begin(), _candidates.end());
        _candidates.erase(
            std::unique(_candidates.begin(), _candidates.end()), _candidates.end());
        return std::move(_candidates);
    }

private:
    static constexpr std::size_t kMaxCandidateBytes = 16u * 1024u;

    void append(char c) {
        if (_discard_token) { return; }
        if (_token.size() == kMaxCandidateBytes) {
            _token.clear();
            _discard_token = true;
            return;
        }
        _token.push_back(c);
    }

    void flush() {
        if (!_discard_token) {
            std::string candidate = trim_candidate(_token);
            if (looks_like_reference(candidate)) {
                _candidates.push_back(std::move(candidate));
            }
        }
        _token.clear();
        _discard_token = false;
    }

    void feed(char c) {
        if (_skip_line) {
            if (c == '\n') {
                _skip_line = false;
                _line_start = true;
                _pending_line_slash = false;
            }
            return;
        }
        if (_quote != '\0') {
            if (c == _quote) {
                flush();
                _quote = '\0';
            } else if (c == '\n') {
                _token.clear();
                _discard_token = false;
                _quote = '\0';
                _line_start = true;
            } else {
                append(c);
            }
            return;
        }
        if (c == '\n') {
            flush();
            _line_start = true;
            return;
        }
        if (_line_start) {
            if (std::isspace(static_cast<unsigned char>(c))) { return; }
            _line_start = false;
            if (c == '#') {
                _skip_line = true;
                return;
            }
            if (c == '/') {
                _pending_line_slash = true;
                return;
            }
        }
        if (_pending_line_slash) {
            _pending_line_slash = false;
            if (c == '/') {
                _skip_line = true;
                return;
            }
            append('/');
        }
        if (c == '#') {
            flush();
            _skip_line = true;
        } else if (c == '\'' || c == '"') {
            flush();
            _quote = c;
        } else if (std::isspace(static_cast<unsigned char>(c)) || c == ';' || c == ',') {
            flush();
        } else {
            append(c);
        }
    }

    std::vector<std::string> _candidates;
    std::string _token;
    char _quote{'\0'};
    bool _discard_token{};
    bool _line_start{true};
    bool _skip_line{};
    bool _pending_line_slash{};
};

struct TextScanResult {
    std::vector<std::string> references;
    bool truncated{};
};

TextScanResult scan_text_file(
    const std::filesystem::path &path,
    std::uint64_t byte_size,
    std::uint64_t byte_limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw std::runtime_error("cannot open candidate text container"); }
    const std::uint64_t scan_size = std::min(byte_size, byte_limit);
    std::uint64_t remaining = scan_size;
    std::uint64_t control_bytes = 0u;
    std::array<char, 64u * 1024u> buffer{};
    ReferenceExtractor extractor;
    while (remaining != 0u) {
        const std::size_t request = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), static_cast<std::streamsize>(request));
        const std::streamsize count = input.gcount();
        if (count != static_cast<std::streamsize>(request)) {
            throw std::runtime_error("cannot read candidate text container");
        }
        for (std::streamsize index = 0; index < count; ++index) {
            const unsigned char c = static_cast<unsigned char>(buffer[index]);
            if (c == 0u) {
                throw std::runtime_error(
                    "candidate text container contains NUL bytes");
            }
            if (c < 0x20u && c != '\n' && c != '\r' && c != '\t' && c != '\f') {
                ++control_bytes;
            }
        }
        extractor.feed({buffer.data(), static_cast<std::size_t>(count)});
        remaining -= static_cast<std::uint64_t>(count);
    }
    if (control_bytes > std::max<std::uint64_t>(8u, scan_size / 100u)) {
        throw std::runtime_error(
            "candidate text container has binary control-byte content");
    }
    const bool truncated = scan_size != byte_size;
    return {extractor.finish(!truncated), truncated};
}

bool path_is_within(const std::filesystem::path &root, const std::filesystem::path &path) {
    const std::filesystem::path normalized_root = root.lexically_normal();
    const std::filesystem::path normalized_path = path.lexically_normal();
    auto root_it = normalized_root.begin();
    auto path_it = normalized_path.begin();
    for (; root_it != normalized_root.end(); ++root_it, ++path_it) {
        if (path_it == normalized_path.end() || *root_it != *path_it) { return false; }
    }
    return true;
}

struct ExpandedReference {
    std::optional<std::filesystem::path> path;
    bool unresolved{};
};

ExpandedReference expand_reference(
    std::string value, const std::filesystem::path &source_parent,
    const std::map<std::string, std::filesystem::path> &variables) {
    std::string expanded;
    for (std::size_t offset = 0u; offset < value.size();) {
        if (value.compare(offset, 2u, "${") != 0) {
            expanded.push_back(value[offset++]);
            continue;
        }
        const std::size_t close = value.find('}', offset + 2u);
        if (close == std::string::npos) { return {std::nullopt, true}; }
        const std::string name = value.substr(offset + 2u, close - offset - 2u);
        const auto variable = variables.find(name);
        if (variable == variables.end()) { return {std::nullopt, true}; }
        expanded += variable->second.string();
        offset = close + 1u;
        // XGen packages commonly concatenate path variables without writing an
        // explicit slash, for example ${PROJECT}xgen/collections. Treat path
        // variables as directory prefixes in that spelling as well.
        if (offset < value.size() && value[offset] != '/' && value[offset] != '\\' &&
            !expanded.empty() && expanded.back() != '/' && expanded.back() != '\\') {
            expanded.push_back(std::filesystem::path::preferred_separator);
        }
    }
    std::replace(expanded.begin(), expanded.end(), '\\', '/');
    const bool windows_absolute = expanded.size() >= 3u &&
        std::isalpha(static_cast<unsigned char>(expanded[0])) &&
        expanded[1] == ':' && expanded[2] == '/';
    std::filesystem::path path{expanded};
    if (path.empty()) { return {std::nullopt, true}; }
    if (path.is_relative() && !windows_absolute) { path = source_parent / path; }
    return {path.lexically_normal(), false};
}

bool absolute_path_has_symlink_component(const std::filesystem::path &path) {
    const std::filesystem::path absolute =
        std::filesystem::absolute(path).lexically_normal();
    std::filesystem::path current = absolute.root_path();
    for (const std::filesystem::path &component : absolute.relative_path()) {
        current /= component;
        std::error_code error;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory) { return false; }
        if (error) {
            throw std::runtime_error(
                "cannot inspect package root component: " + current.string());
        }
        if (std::filesystem::is_symlink(status)) { return true; }
    }
    return false;
}

bool contains_symlink_component(
    const std::filesystem::path &root, const std::filesystem::path &path) {
    const std::filesystem::path relative = path.lexically_relative(root);
    if (relative.empty() && path != root) { return true; }
    std::filesystem::path current = root;
    for (const std::filesystem::path &component : relative) {
        current /= component;
        std::error_code error;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory) { return false; }
        if (error || std::filesystem::is_symlink(status)) {
            return true;
        }
    }
    return false;
}

} // namespace

const char *to_string(XGenPackageFileKind kind) noexcept {
    switch (kind) {
        case XGenPackageFileKind::ClassicCollection: return "classic-collection";
        case XGenPackageFileKind::ClassicDescription: return "classic-description";
        case XGenPackageFileKind::ClassicDelta: return "classic-delta";
        case XGenPackageFileKind::ClassicFxModule: return "classic-fx-module";
        case XGenPackageFileKind::InteractiveGroomPreset: return "interactive-groom-preset";
        case XGenPackageFileKind::EvaluatedSplineBlob: return "evaluated-spline-blob";
        case XGenPackageFileKind::MayaAsciiScene: return "maya-ascii";
        case XGenPackageFileKind::MayaBinaryScene: return "maya-binary";
        case XGenPackageFileKind::CurveCache: return "curve-cache";
        case XGenPackageFileKind::NanoXGenAsset: return "nanoxgen-asset";
        case XGenPackageFileKind::Caf: return "caf";
        case XGenPackageFileKind::Alembic: return "alembic";
        case XGenPackageFileKind::Ptex: return "ptex";
        case XGenPackageFileKind::Xuv: return "xuv";
        case XGenPackageFileKind::Xpd: return "xpd";
        case XGenPackageFileKind::Xgc: return "xgc";
        case XGenPackageFileKind::Archive: return "archive";
        case XGenPackageFileKind::Texture: return "texture";
        case XGenPackageFileKind::Script: return "script";
        case XGenPackageFileKind::Directory: return "directory";
        case XGenPackageFileKind::Symlink: return "symlink";
        case XGenPackageFileKind::Unknown: return "unknown";
    }
    return "unknown";
}

const char *to_string(XGenReferenceStatus status) noexcept {
    switch (status) {
        case XGenReferenceStatus::Resolved: return "resolved";
        case XGenReferenceStatus::Missing: return "missing";
        case XGenReferenceStatus::UnresolvedVariable: return "unresolved-variable";
        case XGenReferenceStatus::External: return "external";
        case XGenReferenceStatus::Unsafe: return "unsafe";
    }
    return "unsafe";
}

const char *to_string(XGenEvaluationBackend backend) noexcept {
    switch (backend) {
        case XGenEvaluationBackend::NativeSnapshot: return "native-snapshot";
        case XGenEvaluationBackend::AutodeskClassicTyped: return "autodesk-classic-typed";
        case XGenEvaluationBackend::AutodeskInteractiveMaya: return "autodesk-interactive-maya";
        case XGenEvaluationBackend::InventoryOnly: return "inventory-only";
    }
    return "inventory-only";
}

XGenPackageManifest scan_xgen_package(
    const std::filesystem::path &root, const XGenPackageOptions &options) {
    if (options.max_files == 0u || options.max_text_file_bytes == 0u) {
        throw std::invalid_argument("XGen package scan limits must be non-zero");
    }
    if (absolute_path_has_symlink_component(root)) {
        throw std::runtime_error(
            "XGen package root must not traverse a symbolic link");
    }
    const std::filesystem::file_status root_status = std::filesystem::symlink_status(root);
    if (!std::filesystem::exists(root_status)) {
        throw std::runtime_error("XGen package root does not exist: " + root.string());
    }
    if (std::filesystem::is_symlink(root_status)) {
        throw std::runtime_error("XGen package root must not be a symbolic link");
    }
    const std::filesystem::path absolute_root = std::filesystem::absolute(
        std::filesystem::is_directory(root_status) ? root : root.parent_path()).lexically_normal();
    XGenPackageManifest manifest{};
    manifest.root = absolute_root;
    std::map<std::string, std::filesystem::path> variables = options.variables;
    variables.try_emplace("PROJECT", absolute_root);
    std::vector<std::filesystem::path> paths;
    if (std::filesystem::is_regular_file(root_status)) {
        paths.push_back(std::filesystem::absolute(root).lexically_normal());
    } else if (std::filesystem::is_directory(root_status)) {
        std::filesystem::recursive_directory_iterator iterator(
            root, std::filesystem::directory_options::skip_permission_denied);
        const std::filesystem::recursive_directory_iterator end;
        for (; iterator != end; ++iterator) {
            const std::filesystem::file_status status = iterator->symlink_status();
            if (std::filesystem::is_symlink(status)) {
                iterator.disable_recursion_pending();
                paths.push_back(std::filesystem::absolute(iterator->path()).lexically_normal());
            } else if (std::filesystem::is_regular_file(status)) {
                paths.push_back(std::filesystem::absolute(iterator->path()).lexically_normal());
            }
            if (paths.size() > options.max_files) {
                throw std::runtime_error("XGen package exceeds max_files");
            }
        }
    } else {
        throw std::runtime_error("XGen package root is not a file or directory");
    }
    std::sort(paths.begin(), paths.end());

    bool has_classic = false;
    bool has_interactive_authoring = false;
    bool has_native_snapshot = false;
    bool has_ptex = false;
    bool has_script_or_plugin = false;
    for (const std::filesystem::path &path : paths) {
        const std::filesystem::file_status status = std::filesystem::symlink_status(path);
        const bool symlink = std::filesystem::is_symlink(status);
        const XGenPackageFileKind kind = symlink
            ? XGenPackageFileKind::Symlink : classify_file(path);
        std::uint64_t byte_size = 0u;
        if (!symlink) { byte_size = std::filesystem::file_size(path); }
        manifest.files.push_back({path.lexically_relative(absolute_root), kind, byte_size});
        if (symlink) {
            manifest.dependency_closure_complete = false;
            manifest.diagnostics.push_back(
                "symbolic link was inventoried but not followed: " + path.string());
            continue;
        }
        has_classic = has_classic || kind == XGenPackageFileKind::ClassicCollection ||
            kind == XGenPackageFileKind::ClassicDescription ||
            kind == XGenPackageFileKind::ClassicDelta ||
            kind == XGenPackageFileKind::ClassicFxModule;
        has_interactive_authoring = has_interactive_authoring ||
            kind == XGenPackageFileKind::InteractiveGroomPreset ||
            kind == XGenPackageFileKind::MayaAsciiScene ||
            kind == XGenPackageFileKind::MayaBinaryScene;
        has_native_snapshot = has_native_snapshot ||
            kind == XGenPackageFileKind::EvaluatedSplineBlob ||
            kind == XGenPackageFileKind::CurveCache ||
            kind == XGenPackageFileKind::NanoXGenAsset;
        has_ptex = has_ptex || kind == XGenPackageFileKind::Ptex;
        has_script_or_plugin = has_script_or_plugin ||
            kind == XGenPackageFileKind::Script;
        if (kind == XGenPackageFileKind::InteractiveGroomPreset ||
            kind == XGenPackageFileKind::MayaAsciiScene ||
            kind == XGenPackageFileKind::MayaBinaryScene) {
            manifest.dependency_closure_complete = false;
            manifest.diagnostics.push_back(
                "Maya DG dependencies require Maya-side enumeration: " + path.string());
        }
        if (!is_text_container(kind)) { continue; }
        TextScanResult text_scan;
        try {
            text_scan = scan_text_file(path, byte_size, options.max_text_file_bytes);
        } catch (const std::exception &error) {
            manifest.dependency_closure_complete = false;
            manifest.diagnostics.push_back(path.string() + ": " + error.what());
            continue;
        }
        if (text_scan.truncated) {
            manifest.dependency_closure_complete = false;
            manifest.diagnostics.push_back(
                "text scan reached max_text_file_bytes after extracting the bounded prefix: " +
                path.string());
        }
        for (const std::string &literal : text_scan.references) {
            XGenPackageReference reference{};
            reference.source = path.lexically_relative(absolute_root);
            reference.literal = literal;
            const ExpandedReference expanded = expand_reference(
                literal, path.parent_path(), variables);
            if (expanded.unresolved || !expanded.path) {
                reference.status = XGenReferenceStatus::UnresolvedVariable;
                manifest.dependency_closure_complete = false;
            } else {
                reference.resolved_path = expanded.path;
                if (!path_is_within(absolute_root, *expanded.path)) {
                    reference.status = XGenReferenceStatus::External;
                    manifest.dependency_closure_complete = false;
                } else if (contains_symlink_component(absolute_root, *expanded.path)) {
                    reference.status = XGenReferenceStatus::Unsafe;
                    manifest.dependency_closure_complete = false;
                } else if (!std::filesystem::exists(
                               std::filesystem::symlink_status(*expanded.path))) {
                    reference.status = XGenReferenceStatus::Missing;
                    manifest.dependency_closure_complete = false;
                } else {
                    reference.status = XGenReferenceStatus::Resolved;
                }
            }
            manifest.references.emplace_back(std::move(reference));
        }
    }

    if (has_interactive_authoring ||
        (!has_classic && (has_ptex || has_script_or_plugin))) {
        manifest.backend = XGenEvaluationBackend::AutodeskInteractiveMaya;
    } else if (has_classic) {
        manifest.backend = XGenEvaluationBackend::AutodeskClassicTyped;
    } else if (has_native_snapshot) {
        manifest.backend = XGenEvaluationBackend::NativeSnapshot;
    } else {
        manifest.backend = XGenEvaluationBackend::InventoryOnly;
    }
    XGenBackendExecutionPlan &plan = manifest.execution_plan;
    plan.backend = manifest.backend;
    plan.requires_autodesk = manifest.backend == XGenEvaluationBackend::AutodeskClassicTyped ||
        manifest.backend == XGenEvaluationBackend::AutodeskInteractiveMaya;
    plan.native_compatible = manifest.backend == XGenEvaluationBackend::NativeSnapshot &&
        manifest.dependency_closure_complete && !has_ptex && !has_script_or_plugin;
    switch (manifest.backend) {
        case XGenEvaluationBackend::NativeSnapshot:
            plan.stages = {
                "load and validate evaluated renderer data with NanoXGen",
                "pack or reuse pointCounts and float4(position, radius)",
                "retain the validated renderer data on the target device"};
            break;
        case XGenEvaluationBackend::AutodeskClassicTyped:
            plan.stages = {
                "resolve package variables and configure Classic XGen project paths",
                "evaluate each description through public XGenRenderAPI typed callbacks",
                "validate and cache renderer curves as NanoXGen .nxc outside the package"};
            break;
        case XGenEvaluationBackend::AutodeskInteractiveMaya:
            plan.stages = {
                "open the authoring scene and enumerate Maya DG, reference, and plugin dependencies",
                "evaluate Interactive outRenderData in a licensed Maya runtime",
                "serialize MPxData once to memory and build a validated NanoXGen .nxc"};
            break;
        case XGenEvaluationBackend::InventoryOnly:
            plan.stages = {
                "inventory package files and resolve safe textual references",
                "supply an evaluated renderer snapshot before runtime ingestion"};
            break;
    }
    if (has_classic) {
        plan.reasons.emplace_back(
            "Classic procedural descriptions and expressions require Autodesk evaluation");
    }
    if (has_interactive_authoring) {
        plan.reasons.emplace_back(
            "Maya authoring state requires DG and file-reference enumeration");
    }
    if (has_ptex) {
        plan.reasons.emplace_back(
            "PTEX assets are inventoried but native PTEX expression evaluation is unsupported");
        plan.native_compatible = false;
    }
    if (has_script_or_plugin) {
        plan.reasons.emplace_back(
            "custom scripts or plugins require their owning Autodesk runtime");
        plan.native_compatible = false;
    }
    if (!manifest.dependency_closure_complete) {
        plan.reasons.emplace_back(
            "the static package dependency closure is incomplete");
        plan.native_compatible = false;
    }
    return manifest;
}

} // namespace nanoxgen
