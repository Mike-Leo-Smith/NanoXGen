#include <cstddef>

#include "nanoxgen/detail/decimal_parse.h"

#if __has_include(<xgen/src/xgrenderer/XgRenderAPI.h>)
#include <xgen/src/xgrenderer/XgRenderAPI.h>
#include <xgen/src/xgcore/XgDescription.h>
#include <xgen/src/xgcore/XgExpression.h>
#include <xgen/src/xgcore/XgExternalAPI.h>
#include <xgen/src/xgcore/XgFXModule.h>
#include <xgen/src/xgcore/XgGenerator.h>
#include <xgen/src/xgcore/XgPalette.h>
#include <xgen/src/xgcore/XgPatch.h>
#include <xgen/src/xgcore/XgPrimitive.h>
#include <xgen/src/xgfxmodule/XgWireSupport.h>
#include <xgen/src/xgprimitive/XgSplinePrimitive.h>
#include <xgen/src/sggeom/SgCurve.h>
#include <xgen/src/sggeom/SgRampUIComp.h>
#else
#error "Autodesk XGen SDK headers were not found"
#endif

#include <algorithm>
#include <array>
#include <cstring>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace api = XGenRenderAPI;

namespace {

struct Sample {
    int face{};
    double u{};
    double v{};
};

double parse_double(std::string_view text, const char *label) {
    double result{};
    const auto parsed = nanoxgen::detail::parse_decimal(
        text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        !std::isfinite(result)) {
        throw std::invalid_argument(std::string{"invalid "} + label);
    }
    return result;
}

int parse_int(std::string_view text, const char *label) {
    int result{};
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument(std::string{"invalid "} + label);
    }
    return result;
}

Sample parse_sample(std::string_view text) {
    const std::size_t first = text.find(',');
    const std::size_t second = first == std::string_view::npos
                                   ? first
                                   : text.find(',', first + 1u);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        text.find(',', second + 1u) != std::string_view::npos) {
        throw std::invalid_argument("--sample expects FACE,U,V");
    }
    const Sample result{
        parse_int(text.substr(0u, first), "sample face"),
        parse_double(text.substr(first + 1u, second - first - 1u), "sample u"),
        parse_double(text.substr(second + 1u), "sample v")};
    if (result.face < 0 || result.u < 0.0 || result.u > 1.0 ||
        result.v < 0.0 || result.v > 1.0) {
        throw std::invalid_argument("sample face/UV is out of range");
    }
    return result;
}

class Callbacks final : public api::ProceduralCallbacks {
public:
    void flush(const char *, api::PrimitiveCache *) override {}
    void log(const char *message) override {
        if (message && *message) { std::cerr << "XGen: " << message << '\n'; }
    }
    bool get(EBoolAttribute) const override { return false; }
    float get(EFloatAttribute) const override { return 0.0f; }
    const char *get(EStringAttribute attribute) const override {
        switch (attribute) {
        case CacheDir: return "xgenCache/";
        case Generator: return "undefined";
        case RenderCam: return "false,0.000000,0.000000,10.000000";
        case RenderCamFOV: return "45.000000";
        case RenderCamRatio: return "1.000000";
        case RenderCamXform:
            return "1.000000,0.000000,0.000000,0.000000,"
                   "0.000000,1.000000,0.000000,0.000000,"
                   "0.000000,0.000000,1.000000,0.000000,"
                   "0.000000,0.000000,0.000000,1.000000";
        case RenderMethod: return "0";
        case BypassFXModulesAfterBGM:
        case Off:
        case Phase: return "";
        }
        return "";
    }
    const float *get(EFloatArrayAttribute) const override { return nullptr; }
    unsigned int getSize(EFloatArrayAttribute) const override { return 0u; }
    const char *getOverride(const char *) const override { return ""; }
    void getTransform(float, api::mat44 &matrix) const override {
        std::memset(&matrix, 0, sizeof(matrix));
        matrix._00 = matrix._11 = matrix._22 = matrix._33 = 1.0f;
    }
    bool getArchiveBoundingBox(const char *, api::bbox &) const override {
        return false;
    }
};

class CleanupOnce {
public:
    ~CleanupOnce() { run(); }
    void run() noexcept {
        if (_done) { return; }
        _done = true;
        api::PatchRenderer::deleteTempRenderPalettes();
    }
private:
    bool _done{};
};

class ClumpGuideProbe final : public XgClumpGuides {
public:
    void exportClumpCurves() override {}
    static const safevector<clumpGuide> &inspect(
        const XgClumpGuides &source) noexcept {
        // The concrete Autodesk Clumping module inherits this public support
        // class. RTTI finds that base; this calibration-only cast exposes its
        // protected public-SDK guide records without naming the unshipped
        // concrete class.
        return reinterpret_cast<const ClumpGuideProbe &>(source)._cGuides;
    }
};

struct ModuleAttributeOverride {
    std::string module;
    std::string attribute;
    std::string value;
};

} // namespace

int main(int argc, char **argv) try {
    std::string xgen_args;
    std::string description_name;
    std::string patch_name;
    std::string expression_name;
    std::string module_name;
    std::string stop_at_name;
    std::string clump_module_name;
    std::string project_path;
    std::string data_path;
    std::optional<std::size_t> clump_guide_index;
    std::optional<double> clump_noise_mask;
    std::optional<double> expression_t;
    std::optional<unsigned int> primitive_id;
    bool faces = false;
    bool weights = false;
    bool geometry = false;
    bool apply_fx = false;
    bool guides = false;
    bool subd_arrays = false;
    bool cv_attrs = false;
    std::vector<Sample> samples;
    std::vector<ModuleAttributeOverride> module_attribute_overrides;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--module-attr") {
            if (index + 3 >= argc) {
                throw std::invalid_argument(
                    "--module-attr requires MODULE ATTRIBUTE VALUE");
            }
            module_attribute_overrides.push_back(
                {argv[++index], argv[++index], argv[++index]});
        } else if ((argument == "--xgen-args" || argument == "--description" ||
             argument == "--patch" || argument == "--expression" ||
             argument == "--module" ||
             argument == "--clump-module" ||
             argument == "--clump-guide-index" ||
             argument == "--clump-noise-mask" ||
             argument == "--t" ||
             argument == "--data-path" ||
             argument == "--project" ||
             argument == "--sample" || argument == "--sample-file" ||
             argument == "--stop-at" || argument == "--id") &&
            index + 1 < argc) {
            const std::string value = argv[++index];
            if (argument == "--xgen-args") { xgen_args = value; }
            else if (argument == "--description") { description_name = value; }
            else if (argument == "--patch") { patch_name = value; }
            else if (argument == "--expression") { expression_name = value; }
            else if (argument == "--module") { module_name = value; }
            else if (argument == "--stop-at") { stop_at_name = value; }
            else if (argument == "--clump-module") {
                clump_module_name = value;
            }
            else if (argument == "--data-path") { data_path = value; }
            else if (argument == "--project") { project_path = value; }
            else if (argument == "--clump-guide-index") {
                const int parsed = parse_int(value, "clump guide index");
                if (parsed < 0) {
                    throw std::invalid_argument("clump guide index is negative");
                }
                clump_guide_index = static_cast<std::size_t>(parsed);
            }
            else if (argument == "--clump-noise-mask") {
                const double parsed = parse_double(value, "clump noise mask");
                if (parsed < 0.0 || parsed > 1.0) {
                    throw std::invalid_argument(
                        "clump noise mask is out of range");
                }
                clump_noise_mask = parsed;
            }
            else if (argument == "--t") {
                expression_t = parse_double(value, "expression t");
            }
            else if (argument == "--id") {
                const int parsed = parse_int(value, "primitive id");
                if (parsed < 0) {
                    throw std::invalid_argument("primitive id is negative");
                }
                primitive_id = static_cast<unsigned int>(parsed);
            }
            else if (argument == "--sample") {
                samples.push_back(parse_sample(value));
            } else {
                std::ifstream input{value};
                if (!input) {
                    throw std::runtime_error(
                        "cannot open sample file '" + value + "'");
                }
                Sample sample{};
                while (input >> sample.face >> sample.u >> sample.v) {
                    if (sample.face < 0 || !std::isfinite(sample.u) ||
                        !std::isfinite(sample.v) || sample.u < 0.0 ||
                        sample.u > 1.0 || sample.v < 0.0 || sample.v > 1.0) {
                        throw std::runtime_error(
                            "sample file contains an invalid face/UV");
                    }
                    samples.emplace_back(sample);
                }
                if (!input.eof()) {
                    throw std::runtime_error("invalid sample-file record");
                }
            }
        } else if (argument == "--faces") {
            faces = true;
        } else if (argument == "--weights") {
            weights = true;
        } else if (argument == "--geometry") {
            geometry = true;
        } else if (argument == "--apply-fx") {
            apply_fx = true;
        } else if (argument == "--guides") {
            guides = true;
        } else if (argument == "--subd-arrays") {
            subd_arrays = true;
        } else if (argument == "--cv-attrs") {
            cv_attrs = true;
        } else {
            throw std::invalid_argument(
                "usage: nanoxgen_xgen_patch_oracle --xgen-args ARGS "
                "--description NAME --patch NAME [--faces] "
                "[--module FX --expression ATTR] [--weights] [--geometry] [--apply-fx] "
                "[--guides] "
                "[--subd-arrays] [--cv-attrs] [--stop-at FX] [--id ID] "
                "[--clump-module FX] "
                "[--clump-guide-index INDEX] "
                "[--clump-noise-mask MASK] "
                "[--module-attr MODULE ATTRIBUTE VALUE ...] "
                "[--t VALUE] "
                "[--data-path PATH] "
                "[--project PATH] "
                "[--sample FACE,U,V ...] [--sample-file PATH]");
        }
    }
    if (xgen_args.empty() || description_name.empty() || patch_name.empty()) {
        throw std::invalid_argument("missing required patch-oracle argument");
    }
    if (clump_guide_index && clump_module_name.empty()) {
        throw std::invalid_argument(
            "--clump-guide-index requires --clump-module");
    }
    if (clump_noise_mask && !clump_guide_index) {
        throw std::invalid_argument(
            "--clump-noise-mask requires --clump-guide-index");
    }

    Callbacks callbacks;
    CleanupOnce cleanup;
    if (!project_path.empty()) {
        xgapi::setProjectPath(project_path);
    }
    std::unique_ptr<api::PatchRenderer> renderer{
        api::PatchRenderer::init(&callbacks, xgen_args.c_str())};
    if (!renderer) { throw std::runtime_error("PatchRenderer::init returned null"); }

    XgPalette *matched_palette = nullptr;
    XgDescription *matched_description = nullptr;
    XgPatch *matched_patch = nullptr;
    for (const std::string &palette_name : XgPalette::palettes()) {
        XgPalette *palette = XgPalette::palette(palette_name);
        XgDescription *description = palette
            ? palette->description(description_name)
            : nullptr;
        XgPatch *patch = description
            ? description->patch(patch_name)
            : nullptr;
        if (!patch || !patch->isBound()) { continue; }
        if (matched_patch) {
            throw std::runtime_error("multiple bound XGen patches matched");
        }
        matched_palette = palette;
        matched_description = description;
        matched_patch = patch;
    }
    if (!matched_patch) { throw std::runtime_error("bound XGen patch was not found"); }
    if (!data_path.empty() &&
        !matched_palette->setAttr("xgDataPath", data_path, "string")) {
        throw std::runtime_error("failed to override palette xgDataPath");
    }

    if (subd_arrays) {
        if (samples.empty()) {
            throw std::invalid_argument(
                "--subd-arrays requires at least one --sample");
        }
        using FloatArrayMethod = const float *(*)(const void *);
        using IntArrayMethod = const int *(*)(const void *);
        using IntMethod = int (*)(const void *);
#if defined(_WIN32)
        const HMODULE image = GetModuleHandleW(L"AdskXGen.dll");
        if (!image) {
            throw std::runtime_error(
                "AdskXGen.dll is not loaded for XgSubdPatch calibration");
        }
        const auto load_symbol = [image](const char *name) {
            const FARPROC symbol = GetProcAddress(image, name);
            if (!symbol) {
                throw std::runtime_error(
                    std::string{"missing XgSubdPatch calibration symbol: "} +
                    name);
            }
            return symbol;
        };
        const auto verts = reinterpret_cast<FloatArrayMethod>(load_symbol(
            "?verts@XgSubdPatch@@QEBAPEBMXZ"))(matched_patch);
        const auto counts = reinterpret_cast<IntArrayMethod>(load_symbol(
            "?nVertsPerFace@XgSubdPatch@@QEBAPEBHXZ"))(matched_patch);
        const auto indices = reinterpret_cast<IntArrayMethod>(load_symbol(
            "?faceVerts@XgSubdPatch@@QEBAPEBHXZ"))(matched_patch);
        const int index_count = reinterpret_cast<IntMethod>(load_symbol(
            "?nFaceVerts@XgSubdPatch@@QEBAHXZ"))(matched_patch);
#else
        const auto load_symbol = [](const char *name) {
            void *symbol = dlsym(RTLD_DEFAULT, name);
            if (!symbol) {
                throw std::runtime_error(
                    std::string{"missing XgSubdPatch calibration symbol: "} +
                    name);
            }
            return symbol;
        };
        const auto verts = reinterpret_cast<FloatArrayMethod>(load_symbol(
            "_ZNK11XgSubdPatch5vertsEv"))(matched_patch);
        const auto counts = reinterpret_cast<IntArrayMethod>(load_symbol(
            "_ZNK11XgSubdPatch13nVertsPerFaceEv"))(matched_patch);
        const auto indices = reinterpret_cast<IntArrayMethod>(load_symbol(
            "_ZNK11XgSubdPatch9faceVertsEv"))(matched_patch);
        const int index_count = reinterpret_cast<IntMethod>(load_symbol(
            "_ZNK11XgSubdPatch10nFaceVertsEv"))(matched_patch);
#endif
        if (!verts || !counts || !indices || index_count < 0) {
            throw std::runtime_error(
                "invalid XgSubdPatch calibration arrays: verts=" +
                std::to_string(reinterpret_cast<std::uintptr_t>(verts)) +
                ", counts=" +
                std::to_string(reinterpret_cast<std::uintptr_t>(counts)) +
                ", indices=" +
                std::to_string(reinterpret_cast<std::uintptr_t>(indices)) +
                ", index_count=" + std::to_string(index_count));
        }
        // Autodesk does not publish an accessor for SESubd's computed limit
        // vertices. Keep this ABI probe confined to the calibration oracle.
        const auto patch_bytes = reinterpret_cast<const unsigned char *>(
            matched_patch);
#if defined(_WIN32)
        // Maya 2024: XgSubdPatch::_surface is +0x90 and the SESubd
        // limitVerts(unsigned) virtual is slot +0x100.
        constexpr std::size_t surface_offset = 0x90u;
        constexpr std::size_t limit_vertex_slot = 0x100u;
#else
        // Maya 2027 Linux calibration ABI.
        constexpr std::size_t surface_offset = 0xb0u;
        constexpr std::size_t limit_vertex_slot = 0x108u;
#endif
        void *surface = *reinterpret_cast<void *const *>(
            patch_bytes + surface_offset);
        if (!surface) {
            throw std::runtime_error("missing XgSubdPatch SgSubdSurface");
        }
        const auto surface_bytes = reinterpret_cast<const unsigned char *>(surface);
        void *subd = *reinterpret_cast<void *const *>(surface_bytes + 8u);
        if (!subd) { throw std::runtime_error("missing SgSubdSurface SESubd"); }
        void **vtable = *reinterpret_cast<void ***>(subd);
#if defined(_WIN32)
        const HMODULE subengine_image = GetModuleHandleW(L"AdskSubEngine.dll");
        if (!subengine_image) {
            throw std::runtime_error(
                "AdskSubEngine.dll is not loaded for SESubd calibration");
        }
        std::array<wchar_t, 32768u> subengine_path{};
        const DWORD subengine_path_size = GetModuleFileNameW(
            subengine_image, subengine_path.data(),
            static_cast<DWORD>(subengine_path.size()));
        if (subengine_path_size == 0u ||
            subengine_path_size == subengine_path.size()) {
            throw std::runtime_error(
                "cannot resolve AdskSubEngine calibration path");
        }
        std::wcout << L"subd_module " << subengine_path.data() << L'\n';
        std::cout << "subd_vtable";
        for (std::size_t slot = 0u; slot < 40u; ++slot) {
            std::cout << " v" << slot << " 0x" << std::hex
                      << (reinterpret_cast<std::uintptr_t>(vtable[slot]) -
                          reinterpret_cast<std::uintptr_t>(subengine_image))
                      << std::dec;
        }
        std::cout << '\n';
        using FaceMapMethod = void (*)(
            void *, int, double, double, int *, double *, double *);
        using EvalMethod = void (*)(
            void *, int, double, double, double *, double *, double *);
        using GetBoolMethod = bool (*)(void *);
        using SetBoolMethod = void (*)(void *, bool);
        const auto cage_face_map = reinterpret_cast<FaceMapMethod>(vtable[4u]);
        const auto logical_face_map = reinterpret_cast<FaceMapMethod>(
            reinterpret_cast<unsigned char *>(subengine_image) + 0xca10u);
        const auto eval = reinterpret_cast<EvalMethod>(vtable[14u]);
        const auto get_use_logical_ids = reinterpret_cast<GetBoolMethod>(
            GetProcAddress(
                subengine_image,
                "?getUseLogicalIds@SESubd@@QEAA_NXZ"));
        const auto set_use_logical_ids = reinterpret_cast<SetBoolMethod>(
            GetProcAddress(
                subengine_image,
                "?setUseLogicalIds@SESubd@@QEAAX_N@Z"));
        if (!eval || !get_use_logical_ids || !set_use_logical_ids) {
            throw std::runtime_error(
                "missing SESubd evaluation calibration entry point");
        }
        const bool original_use_logical_ids = get_use_logical_ids(subd);
        std::cout << "subd_use_logical_ids "
                  << static_cast<unsigned int>(original_use_logical_ids)
                  << " cage "
                  << static_cast<unsigned int>(
                         reinterpret_cast<const unsigned char *>(subd)[0x11u])
                  << " level "
                  << *reinterpret_cast<const int *>(
                         reinterpret_cast<const unsigned char *>(subd) + 0x0cu)
                  << '\n';
        for (const Sample &sample : samples) {
            int cage_face = -1;
            double cage_u = -1.0;
            double cage_v = -1.0;
            cage_face_map(
                subd, sample.face, sample.u, sample.v,
                &cage_face, &cage_u, &cage_v);
            std::cout << std::setprecision(17)
                      << "subd_cage_map " << sample.face << ' '
                      << sample.u << ' ' << sample.v << " -> "
                      << cage_face << ' ' << cage_u << ' '
                      << cage_v << '\n';
            double logical_p[3]{};
            double logical_du[3]{};
            double logical_dv[3]{};
            eval(subd, sample.face, sample.u, sample.v,
                 logical_p, logical_du, logical_dv);
            std::cout << "subd_logical_eval " << sample.face << ' '
                      << sample.u << ' ' << sample.v << " p "
                      << logical_p[0] << ' ' << logical_p[1] << ' '
                      << logical_p[2] << " du " << logical_du[0] << ' '
                      << logical_du[1] << ' ' << logical_du[2] << " dv "
                      << logical_dv[0] << ' ' << logical_dv[1] << ' '
                      << logical_dv[2] << '\n';
            double flipped_p[3]{};
            double flipped_du[3]{};
            double flipped_dv[3]{};
            eval(subd, sample.face, sample.u, 1.0 - sample.v,
                 flipped_p, flipped_du, flipped_dv);
            std::cout << "subd_flipped_eval " << sample.face << ' '
                      << sample.u << ' ' << (1.0 - sample.v) << " p "
                      << flipped_p[0] << ' ' << flipped_p[1] << ' '
                      << flipped_p[2] << " du " << flipped_du[0] << ' '
                      << flipped_du[1] << ' ' << flipped_du[2] << " dv "
                      << flipped_dv[0] << ' ' << flipped_dv[1] << ' '
                      << flipped_dv[2] << '\n';
            SgVec3d patch_p;
            SgVec3d patch_n;
            SgVec3d patch_u;
            SgVec3d patch_v;
            if (!matched_patch->evalFrame(
                    sample.u, sample.v, sample.face,
                    patch_p, patch_n, patch_u, patch_v, false)) {
                throw std::runtime_error("XgSubdPatch evalFrame failed");
            }
            std::cout << "patch_eval " << sample.face << ' '
                      << sample.u << ' ' << sample.v << " p "
                      << patch_p[0] << ' ' << patch_p[1] << ' '
                      << patch_p[2] << '\n';
        }
        set_use_logical_ids(subd, original_use_logical_ids);
#endif
        using LimitVertexMethod = const float *(*)(void *, unsigned int);
        const auto limit_vertex = reinterpret_cast<LimitVertexMethod>(
            vtable[limit_vertex_slot / sizeof(void *)]);
        if (!limit_vertex) {
            throw std::runtime_error("missing SESubd limitVerts virtual slot");
        }
#if defined(_WIN32)
        using SurfaceFaceVertsMethod = const int *(*)(const void *, int);
        using SurfaceCountsMethod = const int *(*)(const void *);
        using SurfaceNVertsMethod = int (*)(const void *, bool);
        using SurfaceVertsMethod = const float *(*)(const void *,
                                                    unsigned int);
        std::cout << "subd_limit_vertex_rva 0x" << std::hex
                  << (reinterpret_cast<std::uintptr_t>(limit_vertex) -
                      reinterpret_cast<std::uintptr_t>(subengine_image))
                  << std::dec << '\n';
        using LimitWeightMethod = const unsigned char *(*)(unsigned int);
        const auto limit_weights = reinterpret_cast<LimitWeightMethod>(
            reinterpret_cast<std::uintptr_t>(subengine_image) + 0xc0a0u);
        const auto surface_face_vertices =
            reinterpret_cast<SurfaceFaceVertsMethod>(load_symbol(
                "?faceVerts@SgSubdSurface@@QEBAPEBHH@Z"));
        const auto surface_face_offsets =
            reinterpret_cast<SurfaceCountsMethod>(load_symbol(
                "?nVertsPerFace@SgSubdSurface@@QEBAPEBHXZ"))(surface);
        const auto surface_vertices = reinterpret_cast<SurfaceVertsMethod>(
            load_symbol("?verts@SgSubdSurface@@QEBAPEBMI@Z"));
        const auto surface_vertex_count =
            reinterpret_cast<SurfaceNVertsMethod>(load_symbol(
                "?nVerts@SgSubdSurface@@QEBAH_N@Z"));
        if (!surface_face_vertices || !surface_face_offsets ||
            !surface_vertices || !surface_vertex_count) {
            throw std::runtime_error(
                "missing SgSubdSurface face topology calibration data");
        }
        std::cout << std::setprecision(17);
        const int all_vertex_count = surface_vertex_count(surface, false);
        const int cage_vertex_count = surface_vertex_count(surface, true);
        if (cage_vertex_count < 0 || all_vertex_count < cage_vertex_count ||
            all_vertex_count > 100000000) {
            throw std::runtime_error(
                "invalid SgSubdSurface vertex counts: cage=" +
                std::to_string(cage_vertex_count) + ", all=" +
                std::to_string(all_vertex_count));
        }
        std::cout << "subd_vertex_counts cage " << cage_vertex_count
                  << " all " << all_vertex_count << '\n';
        using BuildSubdMethod = void *(*)(
            int, const float *, int, const int *, const int *, bool, bool);
        const auto build_subd = reinterpret_cast<BuildSubdMethod>(
            GetProcAddress(
                subengine_image,
                "?build@SESubd@@SAPEAV1@HPEBMHPEBH1_N2@Z"));
        if (!build_subd) {
            throw std::runtime_error("missing SESubd build calibration entry point");
        }
        using PatchNVertsMethod = int (*)(const void *, bool);
        const auto patch_nverts = reinterpret_cast<PatchNVertsMethod>(
            GetProcAddress(image, "?nVerts@XgSubdPatch@@QEBAH_N@Z"));
        if (!patch_nverts) {
            throw std::runtime_error("missing XgSubdPatch vertex count accessor");
        }
        const int patch_vertices_false = patch_nverts(matched_patch, false);
        const int patch_vertices_true = patch_nverts(matched_patch, true);
        std::cout << "subd_patch_vertex_counts false "
                  << patch_vertices_false << " true "
                  << patch_vertices_true << " face_count "
                  << matched_patch->numGeomFaces() << " index_count "
                  << index_count << " index_min "
                  << *std::min_element(indices, indices + index_count)
                  << " index_max "
                  << *std::max_element(indices, indices + index_count)
                  << '\n' << std::flush;
        const int clone_face_count = static_cast<int>(
            matched_patch->numGeomFaces());
        std::vector<int> clone_indices;
        for (int face = 0; face < clone_face_count; ++face) {
            const int count = surface_face_offsets[face];
            const int *face_indices = surface_face_vertices(surface, face);
            if (count < 3 || !face_indices) {
                throw std::runtime_error(
                    "invalid SgSubdSurface clone topology");
            }
            clone_indices.insert(
                clone_indices.end(), face_indices, face_indices + count);
        }
        const float *clone_vertices = surface_vertices(surface, 0u);
        if (!clone_vertices ||
            (cage_vertex_count > 1 &&
             surface_vertices(surface, 1u) != clone_vertices + 3u)) {
            throw std::runtime_error(
                "SgSubdSurface clone vertices are not contiguous");
        }
        std::cout << "subd_clone_input faces " << clone_face_count
                  << " indices " << clone_indices.size() << " min "
                  << *std::min_element(
                         clone_indices.begin(), clone_indices.end())
                  << " max "
                  << *std::max_element(
                         clone_indices.begin(), clone_indices.end())
                  << '\n' << std::flush;
        if (std::getenv("NANOXGEN_ORACLE_BUILD_SUBD") != nullptr) {
          for (unsigned int first_flag = 0u; first_flag <= 1u; ++first_flag) {
            for (unsigned int second_flag = 0u; second_flag <= 1u; ++second_flag) {
                void *clone = build_subd(
                    all_vertex_count, clone_vertices, clone_face_count,
                    surface_face_offsets, clone_indices.data(),
                    first_flag != 0u, second_flag != 0u);
                if (!clone) {
                    std::cout << "subd_clone " << first_flag << ' '
                              << second_flag << " null\n";
                    continue;
                }
                auto **clone_vtable = *reinterpret_cast<void ***>(clone);
                const auto clone_eval = reinterpret_cast<EvalMethod>(
                    clone_vtable[14u]);
                const auto clone_set_logical = reinterpret_cast<SetBoolMethod>(
                    GetProcAddress(
                        subengine_image,
                        "?setUseLogicalIds@SESubd@@QEAAX_N@Z"));
                *reinterpret_cast<int *>(
                    reinterpret_cast<unsigned char *>(clone) + 0x0cu) = 1;
                clone_set_logical(clone, true);
                for (const Sample &sample : samples) {
                    double clone_p[3]{};
                    double clone_du[3]{};
                    double clone_dv[3]{};
                    clone_eval(
                        clone, sample.face, sample.u, sample.v,
                        clone_p, clone_du, clone_dv);
                    std::cout << "subd_clone_eval " << first_flag << ' '
                              << second_flag << ' ' << sample.face << " p "
                              << clone_p[0] << ' ' << clone_p[1] << ' '
                              << clone_p[2] << " du " << clone_du[0] << ' '
                              << clone_du[1] << ' ' << clone_du[2] << " dv "
                              << clone_dv[0] << ' ' << clone_dv[1] << ' '
                              << clone_dv[2] << '\n';
                }
            }
          }
        }
        for (int vertex = 0; vertex < all_vertex_count; ++vertex) {
            const float *source = surface_vertices(
                surface, static_cast<unsigned int>(vertex));
            if (!source || !std::isfinite(source[0]) ||
                !std::isfinite(source[1]) || !std::isfinite(source[2])) {
                throw std::runtime_error(
                    "invalid SgSubdSurface vertex calibration value at " +
                    std::to_string(vertex));
            }
            std::cout << "subd_vertex " << vertex << " source "
                      << source[0] << ' ' << source[1] << ' '
                      << source[2] << '\n';
        }
        if (all_vertex_count != 0) {
            (void)limit_vertex(subd, 0u);
        }
        const auto subd_bytes =
            reinterpret_cast<const unsigned char *>(subd);
        const int *refined_face_counts =
            *reinterpret_cast<const int *const *>(subd_bytes + 0x618u);
        const int *refined_face_counts_end =
            *reinterpret_cast<const int *const *>(subd_bytes + 0x620u);
        const int *refined_face_vertices =
            *reinterpret_cast<const int *const *>(subd_bytes + 0x630u);
        const int *refined_face_vertices_end =
            *reinterpret_cast<const int *const *>(subd_bytes + 0x638u);
        if (!refined_face_counts || !refined_face_counts_end ||
            !refined_face_vertices || !refined_face_vertices_end ||
            refined_face_counts_end < refined_face_counts ||
            refined_face_vertices_end < refined_face_vertices) {
            throw std::runtime_error(
                "invalid SESubd refined topology calibration arrays");
        }
        const std::size_t refined_face_count = static_cast<std::size_t>(
            refined_face_counts_end - refined_face_counts);
        const std::size_t refined_index_count = static_cast<std::size_t>(
            refined_face_vertices_end - refined_face_vertices);
        std::size_t refined_offset = 0u;
        for (std::size_t face = 0u; face < refined_face_count; ++face) {
            const int count = refined_face_counts[face];
            if (count <= 0 || count > 64 ||
                refined_offset > refined_index_count ||
                static_cast<std::size_t>(count) >
                    refined_index_count - refined_offset) {
                throw std::runtime_error(
                    "invalid SESubd refined face calibration data");
            }
            bool selected = face >= 1364u && face <= 1367u;
            for (int corner = 0; corner < count; ++corner) {
                const int vertex = refined_face_vertices[
                    refined_offset + static_cast<std::size_t>(corner)];
                selected = selected || vertex == 223 || vertex == 229 ||
                    vertex == 801 || vertex == 870;
            }
            if (selected) {
                std::cout << "subd_refined_face " << face << " count "
                          << count << " vertices";
                for (int corner = 0; corner < count; ++corner) {
                    std::cout << ' ' << refined_face_vertices[
                        refined_offset + static_cast<std::size_t>(corner)];
                }
                std::cout << '\n';
            }
            refined_offset += static_cast<std::size_t>(count);
        }
        if (refined_offset != refined_index_count) {
            throw std::runtime_error(
                "SESubd refined face topology does not consume all indices");
        }
        for (const int face : matched_patch->faceIds()) {
            if (face < 0) {
                throw std::runtime_error(
                    "invalid SgSubdSurface face offset calibration data");
            }
            const int count = surface_face_offsets[face];
            const int *face_vertices = surface_face_vertices(surface, face);
            if (!face_vertices || count <= 0 || count > 64) {
                throw std::runtime_error(
                    "invalid SgSubdSurface face calibration data: face=" +
                    std::to_string(face) + ", count=" +
                    std::to_string(count));
            }
            std::cout << "subd_face " << face << " count " << count;
            for (int corner = 0; corner < count; ++corner) {
                const int vertex = face_vertices[corner];
                if (vertex < 0) {
                    throw std::runtime_error(
                        "negative SgSubdSurface vertex index");
                }
                const float *limit = limit_vertex(
                    subd, static_cast<unsigned int>(vertex));
                const float *source = surface_vertices(
                    surface, static_cast<unsigned int>(vertex));
                if (!source || !limit || !std::isfinite(limit[0]) ||
                    !std::isfinite(limit[1]) || !std::isfinite(limit[2])) {
                    throw std::runtime_error(
                        "invalid SESubd limit vertex calibration value");
                }
                std::cout << " vertex " << vertex << " source "
                          << source[0] << ' ' << source[1] << ' '
                          << source[2] << " limit "
                          << limit[0] << ' ' << limit[1] << ' '
                          << limit[2];
                const unsigned char *vertex_array =
                    *reinterpret_cast<unsigned char *const *>(
                        subd_bytes + 0x680u);
                if (!vertex_array) {
                    throw std::runtime_error(
                        "missing SESubd calibration vertex metadata");
                }
                const unsigned char *record = vertex_array +
                    static_cast<std::size_t>(vertex) * 0x1cu;
                const float *metadata =
                    reinterpret_cast<const float *>(record);
                const unsigned int valence =
                    static_cast<unsigned int>(record[0x18u]);
                const unsigned char *weights = limit_weights(valence);
                if (!weights) {
                    throw std::runtime_error(
                        "missing SESubd limit weight calibration data");
                }
                std::cout << " edge_sum " << metadata[0] << ' '
                          << metadata[1] << ' ' << metadata[2]
                          << " face_sum " << metadata[3] << ' '
                          << metadata[4] << ' ' << metadata[5]
                          << " valence "
                          << valence
                          << " boundary "
                          << static_cast<unsigned int>(record[0x19u])
                          << " corner "
                          << static_cast<unsigned int>(record[0x1au]);
                std::cout << " weights "
                          << *reinterpret_cast<const double *>(weights + 0x10u)
                          << ' '
                          << *reinterpret_cast<const double *>(weights + 0x18u)
                          << ' '
                          << *reinterpret_cast<const double *>(weights + 0x20u);
            }
            std::cout << '\n';
        }
#else
        std::size_t offset = 0u;
        std::cout << std::setprecision(17);
        for (unsigned int face = 0u; face < matched_patch->numGeomFaces(); ++face) {
            const int count = counts[face];
            if (count < 0 || offset > static_cast<std::size_t>(index_count) ||
                static_cast<std::size_t>(count) >
                    static_cast<std::size_t>(index_count) - offset) {
                throw std::runtime_error(
                    "invalid XgSubdPatch face topology arrays at face=" +
                    std::to_string(face) + ", count=" +
                    std::to_string(count) + ", offset=" +
                    std::to_string(offset) + ", index_count=" +
                    std::to_string(index_count) + ", num_geom_faces=" +
                    std::to_string(matched_patch->numGeomFaces()));
            }
            if (matched_patch->hasFaceId(static_cast<int>(face))) {
                std::cout << "subd_face " << face << " count " << count;
                for (int corner = 0; corner < count; ++corner) {
                    const int vertex = indices[offset +
                                               static_cast<std::size_t>(corner)];
                    if (vertex < 0) {
                        throw std::runtime_error(
                            "negative XgSubdPatch vertex index");
                    }
                    const float *p = verts + static_cast<std::size_t>(vertex) * 3u;
                    std::cout << " vertex " << vertex << ' ' << p[0] << ' '
                              << p[1] << ' ' << p[2];
                    const float *limit = limit_vertex(
                        subd, static_cast<unsigned int>(vertex));
                    if (!limit || !std::isfinite(limit[0]) ||
                        !std::isfinite(limit[1]) || !std::isfinite(limit[2])) {
                        throw std::runtime_error(
                            "invalid SESubd limit vertex calibration value");
                    }
                    std::cout << " limit " << limit[0] << ' ' << limit[1]
                              << ' ' << limit[2];
#if !defined(_WIN32)
                    const auto subd_bytes =
                        reinterpret_cast<const unsigned char *>(subd);
                    const unsigned char *vertex_array =
                        *reinterpret_cast<unsigned char *const *>(
                            subd_bytes + 0x748u);
                    if (!vertex_array) {
                        throw std::runtime_error(
                            "missing SESubd calibration vertex metadata");
                    }
                    const float *metadata = reinterpret_cast<const float *>(
                        vertex_array + static_cast<std::size_t>(vertex) * 0x1cu);
                    std::cout << " edge_sum " << metadata[0] << ' '
                              << metadata[1] << ' ' << metadata[2]
                              << " face_sum " << metadata[3] << ' '
                              << metadata[4] << ' ' << metadata[5]
                              << " valence "
                              << static_cast<unsigned int>(
                                     vertex_array[static_cast<std::size_t>(vertex) *
                                                      0x1cu +
                                                  0x18u])
                              << " boundary "
                              << static_cast<unsigned int>(
                                     vertex_array[static_cast<std::size_t>(vertex) *
                                                      0x1cu +
                                                  0x19u])
                              << " corner "
                              << static_cast<unsigned int>(
                                     vertex_array[static_cast<std::size_t>(vertex) *
                                                      0x1cu +
                                                  0x1au]);
#endif
                }
                std::cout << '\n';
            }
            offset += static_cast<std::size_t>(count);
        }
        if (offset != static_cast<std::size_t>(index_count)) {
            throw std::runtime_error(
                "XgSubdPatch face topology arrays do not consume all indices");
        }
#endif
    }

    XgPrimitive *primitive = matched_description->activePrimitive();
    for (const ModuleAttributeOverride &override_value :
         module_attribute_overrides) {
        XgFXModule *module = primitive
            ? primitive->findFXModule(override_value.module)
            : nullptr;
        if (!module ||
            !module->setAttr(override_value.attribute, override_value.value)) {
            throw std::runtime_error(
                "failed to override " + override_value.module + '.' +
                override_value.attribute);
        }
    }
    if ((weights || geometry || guides) && !primitive) {
        throw std::runtime_error("description has no active primitive");
    }

    if (guides) {
        std::cout << std::setprecision(17);
        for (int index = 0; index < primitive->numGuides(); ++index) {
            const XgGuide *guide = primitive->guide(
                static_cast<unsigned int>(index));
            if (!guide || guide->patch() != matched_patch) { continue; }
            std::cout << "guide " << index << " id " << guide->id()
                      << " face " << guide->faceId() << " u "
                      << guide->u() << " v " << guide->v()
                      << " patch_u " << guide->patchU()
                      << " patch_v " << guide->patchV()
                      << " blend " << guide->blend()
                      << " region " << guide->region() << " radius";
            for (const double value : guide->radius()) {
                std::cout << ' ' << value;
            }
            std::cout << " angle";
            for (const double value : guide->angle()) {
                std::cout << ' ' << value;
            }
            std::cout << " delta";
            for (const double value : guide->delta()) {
                std::cout << ' ' << value;
            }
            std::cout << " igeom";
            for (const SgVec3d &point : guide->iGuideGeom()) {
                std::cout << ' ' << point[0] << ' ' << point[1] << ' '
                          << point[2];
            }
            std::cout << " cgeom";
            for (const SgVec3d &point : guide->cGuideGeom()) {
                std::cout << ' ' << point[0] << ' ' << point[1] << ' '
                          << point[2];
            }
            const SgVec3d &position = guide->cGuideP(true);
            const SgVec3d &normal = guide->cGuideN(true);
            const SgVec3d &tangent = guide->cGuideTangent(true);
            const SgVec3d &binormal = guide->cGuideBinormal(true);
            std::cout << " position " << position[0] << ' ' << position[1]
                      << ' ' << position[2] << " normal " << normal[0] << ' '
                      << normal[1] << ' ' << normal[2] << " tangent "
                      << tangent[0] << ' ' << tangent[1] << ' ' << tangent[2]
                      << " binormal " << binormal[0] << ' ' << binormal[1]
                      << ' ' << binormal[2];
            std::cout << '\n';
        }
    }

    if (!clump_module_name.empty()) {
        auto *spline = dynamic_cast<XgSplinePrimitive *>(primitive);
        XgFXModule *module = primitive
            ? primitive->findFXModule(clump_module_name) : nullptr;
        if (!spline || !module) {
            throw std::runtime_error(
                "requested clump spline/module was not found");
        }
#if defined(_WIN32)
        const HMODULE xgen_image = GetModuleHandleW(L"AdskXGen.dll");
        void **module_vtable = *reinterpret_cast<void ***>(module);
        std::cout << "clump_module_type " << typeid(*module).name();
        for (std::size_t slot = 0u; slot < 32u; ++slot) {
            std::cout << " v" << slot << " 0x" << std::hex
                      << (reinterpret_cast<std::uintptr_t>(
                              module_vtable[slot]) -
                          reinterpret_cast<std::uintptr_t>(xgen_image))
                      << std::dec;
        }
        std::cout << '\n';
#endif
        primitive->setupInterpolation(true);
        api::bbox bounds{};
        unsigned int face_id = std::numeric_limits<unsigned int>::max();
        while (renderer->nextFace(bounds, face_id)) {
            std::unique_ptr<api::FaceRenderer> face{
                api::FaceRenderer::init(renderer.get(), face_id, &callbacks)};
            if (!face || !face->render()) {
                throw std::runtime_error(
                    "clump calibration face render failed");
            }
        }
        const XgClumpGuides *loaded = dynamic_cast<XgClumpGuides *>(module);
        if (!loaded) {
            throw std::runtime_error(
                "requested module does not expose XgClumpGuides");
        }
        std::cout << std::setprecision(17);
        const auto &clump_guides = ClumpGuideProbe::inspect(*loaded);
        using ComputeNoiseAxis = void (*)(
            void *, double, unsigned int, safevector<SgVec3d> &);
        ComputeNoiseAxis compute_noise_axis = nullptr;
        if (clump_noise_mask) {
#if defined(_WIN32)
            throw std::runtime_error(
                "--clump-noise-mask private ABI calibration is unavailable on Windows");
#else
            // This is deliberately confined to the Maya-2027 calibration
            // executable. computeNoiseAxis is not exported, so locate it
            // relative to an exported symbol in the already loaded XGen DSO.
            // The native runtime never depends on this private ABI.
            void *anchor = dlsym(
                RTLD_DEFAULT,
                "_ZN7SgCurve5frameERKSt6vectorI7SgVec3TIdESaIS2_EERKS2_"
                "S8_RS4_S9_");
            Dl_info image{};
            if (!anchor || dladdr(anchor, &image) == 0 || !image.dli_fbase) {
                throw std::runtime_error(
                    "cannot locate loaded Maya-2027 XGen image");
            }
            constexpr std::uintptr_t maya_2027_compute_noise_axis =
                0x02f6f70u;
            compute_noise_axis = reinterpret_cast<ComputeNoiseAxis>(
                reinterpret_cast<std::uintptr_t>(image.dli_fbase) +
                maya_2027_compute_noise_axis);
#endif
        }
        std::cout << "clump_guides " << clump_module_name << " count "
                  << clump_guides.size() << '\n';
        for (std::size_t index = 0u; index < clump_guides.size(); ++index) {
            if (clump_guide_index && index != *clump_guide_index) { continue; }
            const auto &guide = clump_guides[index];
            std::cout << "clump_guide " << index << " valid " << guide.valid
                      << " face " << guide.faceId << " u " << guide.u
                      << " v " << guide.v << " patch " << guide.patch
                      << " best " << guide.best << " len " << guide.len
                      << " poly_len " << guide.polyLen << " P "
                      << guide.P[0] << ' ' << guide.P[1] << ' ' << guide.P[2]
                      << " n " << guide.nVec[0] << ' ' << guide.nVec[1]
                      << ' ' << guide.nVec[2] << " u " << guide.uVec[0]
                      << ' ' << guide.uVec[1] << ' ' << guide.uVec[2]
                      << " v " << guide.vVec[0] << ' ' << guide.vVec[1]
                      << ' ' << guide.vVec[2] << " seg_len "
                      << guide.segLen.size();
            for (const double length : guide.segLen) {
                std::cout << ' ' << length;
            }
            std::cout
                      << " axis " << guide.axis.size();
            for (const SgVec3d &point : guide.axis) {
                std::cout << ' ' << point[0] << ' ' << point[1] << ' '
                          << point[2];
            }
            safevector<SgVec3d> frame_normals;
            safevector<SgVec3d> frame_binormals;
            SgCurve::frame(
                guide.axis, guide.nVec, guide.uVec,
                frame_normals, frame_binormals);
            std::cout << " frame " << frame_normals.size();
            if (frame_normals.size() != frame_binormals.size()) {
                throw std::runtime_error(
                    "clump guide frame arrays are inconsistent");
            }
            for (std::size_t cv = 0u; cv < frame_normals.size(); ++cv) {
                std::cout << ' ' << frame_normals[cv][0] << ' '
                          << frame_normals[cv][1] << ' '
                          << frame_normals[cv][2] << ' '
                          << frame_binormals[cv][0] << ' '
                          << frame_binormals[cv][1] << ' '
                          << frame_binormals[cv][2];
            }
            std::cout << '\n';
            if (compute_noise_axis) {
                safevector<SgVec3d> noisy_axis = guide.axis;
                compute_noise_axis(
                    module, *clump_noise_mask,
                    static_cast<unsigned int>(index), noisy_axis);
                std::cout << "clump_noise_axis " << index << " mask "
                          << *clump_noise_mask << " count "
                          << noisy_axis.size();
                for (const SgVec3d &point : noisy_axis) {
                    std::cout << ' ' << point[0] << ' ' << point[1] << ' '
                              << point[2];
                }
                std::cout << '\n';
            }
        }
    }

    if (!expression_name.empty()) {
        if (samples.empty()) {
            throw std::invalid_argument(
                "--expression requires at least one --sample");
        }
        XgGenerator *generator = matched_description->activeGenerator();
        if (!generator) {
            throw std::runtime_error("description has no active generator");
        }
        std::string source;
        std::string object_type;
        if (!module_name.empty()) {
            XgFXModule *module = primitive
                ? primitive->findFXModule(module_name) : nullptr;
            if (!module || !module->getAttr(expression_name, source)) {
                throw std::runtime_error(
                    "FX module has no attribute named '" + expression_name + "'");
            }
            object_type = module->typeName();
        } else if (generator->getAttr(expression_name, source)) {
            object_type = generator->typeName();
        } else if (primitive && primitive->getAttr(expression_name, source)) {
            object_type = primitive->typeName();
        } else {
            throw std::runtime_error(
                "active generator/primitive has no attribute named '" +
                expression_name + "'");
        }
        if (primitive_id && primitive) { primitive->setId(*primitive_id); }
        std::cout << std::setprecision(17);
        if (source.starts_with("rampUI(")) {
            if (!expression_t) {
                throw std::invalid_argument(
                    "rampUI evaluation requires --t");
            }
            SgRampUIComp ramp;
            if (!ramp.init(source)) {
                throw std::runtime_error(
                    "XGen rejected rampUI '" + expression_name + "'");
            }
            for (const Sample &sample : samples) {
                std::cout << "sample " << sample.face << ' ' << sample.u
                          << ' ' << sample.v << ' '
                          << ramp.getValue(static_cast<float>(*expression_t))
                          << '\n';
            }
        } else {
            XgExpression expression{
                expression_name, object_type, matched_description,
                source, "float"};
            std::string diagnostic;
            if (!expression.syntaxOK(&diagnostic) ||
                !expression.isValid(&diagnostic)) {
                throw std::runtime_error(
                    "XGen rejected expression '" + expression_name +
                    "': " + diagnostic);
            }
            if (expression_t) {
                XgExpression::setCustomVariable("t", *expression_t);
            }
            for (const Sample &sample : samples) {
                const SgVec3d value = expression.eval(
                    sample.u, sample.v, sample.face, &patch_name);
                std::cout << "sample " << sample.face << ' ' << sample.u
                          << ' ' << sample.v << ' ' << value[0] << '\n';
            }
        }
    } else if (!samples.empty() && !weights && !geometry) {
        throw std::invalid_argument(
            "--sample requires --expression or --weights");
    }

    if (weights) {
        primitive->setupInterpolation(true);
        std::cout << std::setprecision(17);
        for (const Sample &sample : samples) {
            primitive->setActivePatchFace(*matched_patch, sample.face);
            const unsigned int region = primitive->guideRegion(
                sample.u, sample.v, sample.face, patch_name);
            const double region_mask = primitive->guideRegionMask(
                sample.u, sample.v, sample.face, patch_name);
            primitive->findGuidesAndWeights(sample.u, sample.v);
            const SgVec3d &position = primitive->cPg(
                sample.u, sample.v, true);
            const SgVec3d &normal = primitive->cNg(
                sample.u, sample.v, true);
            std::cout << "weights " << sample.face << ' ' << sample.u << ' '
                      << sample.v << " position " << position[0] << ' '
                      << position[1] << ' ' << position[2] << " normal "
                      << normal[0] << ' ' << normal[1] << ' ' << normal[2]
                      << " region " << region
                      << " region_mask " << region_mask
                      << " weight_n " << primitive->weightN();
            const auto &active = primitive->activeGuides();
            const auto &values = primitive->weight();
            if (active.size() != values.size()) {
                throw std::runtime_error(
                    "XGen guide and weight arrays have inconsistent sizes");
            }
            for (std::size_t index = 0; index < active.size(); ++index) {
                std::cout << ' ' << active[index] << ':' << values[index];
            }
            std::cout << '\n';
        }
    }

    if (geometry) {
        primitive->setupInterpolation(true);
        XgFXModule *stop_at = stop_at_name.empty()
            ? nullptr : primitive->findFXModule(stop_at_name);
        if (!stop_at_name.empty() && stop_at == nullptr) {
            throw std::runtime_error(
                "requested stop-at FX module was not found");
        }
        std::cout << std::setprecision(17);
        for (const Sample &sample : samples) {
            primitive->setActivePatchFace(*matched_patch, sample.face);
            if (primitive_id) { primitive->setId(*primitive_id); }
            primitive->findGuidesAndWeights(sample.u, sample.v);
            primitive->makeGeometry(sample.u, sample.v, stop_at);
            if (apply_fx) { primitive->applyFXModules(stop_at); }
            if (guides) {
                for (const int guide_index : primitive->activeGuides()) {
                    const XgGuide *guide = primitive->guide(
                        static_cast<unsigned int>(guide_index));
                    if (!guide) { continue; }
                    std::cout << "active_cgeom " << guide_index;
                    for (const SgVec3d &point : guide->cGuideGeom()) {
                        std::cout << ' ' << point[0] << ' ' << point[1]
                                  << ' ' << point[2];
                    }
                    std::cout << '\n';
                }
            }
            const auto &points = primitive->getGeom();
            std::cout << "geometry " << sample.face << ' ' << sample.u << ' '
                      << sample.v << " count " << points.size();
            for (const SgVec3d &point : points) {
                std::cout << ' ' << point[0] << ' ' << point[1] << ' '
                          << point[2];
            }
            std::cout << '\n';
            if (cv_attrs) {
                auto &attributes = primitive->cvAttrs();
                std::cout << "cv_attrs " << sample.face << ' ' << sample.u
                          << ' ' << sample.v << " count "
                          << attributes.size() << '\n';
                for (auto attribute = attributes.begin();
                     attribute != attributes.end(); ++attribute) {
                    std::cout << "cv_attr " << attribute->first << " count "
                              << attribute->second.size();
                    for (const SgVec3d &value : attribute->second) {
                        std::cout << ' ' << value[0] << ' ' << value[1]
                                  << ' ' << value[2];
                    }
                    std::cout << '\n';
                }
            }
            safevector<SgVec3d> normals;
            safevector<SgVec3d> binormals;
            const SgVec3d surface_n =
                primitive->cN(sample.u, sample.v, false);
            const SgVec3d surface_u =
                primitive->cU(sample.u, sample.v, false);
            SgVec3d patch_position;
            SgVec3d patch_normal;
            SgVec3d patch_u;
            SgVec3d patch_v;
            SgVec3d patch_dpdu;
            SgVec3d patch_dpdv;
            SgVec3d reference_position;
            SgVec3d reference_normal;
            SgVec3d reference_u;
            SgVec3d reference_v;
            if (!matched_patch->evalFrame(
                    sample.u, sample.v, sample.face, patch_position,
                    patch_normal, patch_u, patch_v, false) ||
                !matched_patch->evalFrame(
                    sample.u, sample.v, sample.face, reference_position,
                    reference_normal, reference_u, reference_v, true) ||
                !matched_patch->evalGeom(
                    sample.u, sample.v, sample.face, patch_position,
                    patch_normal, patch_dpdu, patch_dpdv, false)) {
                throw std::runtime_error("patch frame evaluation failed");
            }
            SgCurve::frame(
                points, surface_n, surface_u, normals, binormals);
            std::cout << "frame " << sample.face << ' ' << sample.u << ' '
                      << sample.v << " surface_n " << surface_n[0] << ' '
                      << surface_n[1] << ' ' << surface_n[2]
                      << " surface_u " << surface_u[0] << ' '
                      << surface_u[1] << ' ' << surface_u[2]
                      << " patch_u " << patch_u[0] << ' ' << patch_u[1]
                      << ' ' << patch_u[2]
                      << " patch_v " << patch_v[0] << ' ' << patch_v[1]
                      << ' ' << patch_v[2]
                      << " patch_p " << patch_position[0] << ' '
                      << patch_position[1] << ' ' << patch_position[2]
                      << " reference_p " << reference_position[0] << ' '
                      << reference_position[1] << ' ' << reference_position[2]
                      << " reference_n " << reference_normal[0] << ' '
                      << reference_normal[1] << ' ' << reference_normal[2]
                      << " reference_u " << reference_u[0] << ' '
                      << reference_u[1] << ' ' << reference_u[2]
                      << " reference_v " << reference_v[0] << ' '
                      << reference_v[1] << ' ' << reference_v[2]
                      << " dpdu " << patch_dpdu[0] << ' ' << patch_dpdu[1]
                      << ' ' << patch_dpdu[2]
                      << " dpdv " << patch_dpdv[0] << ' ' << patch_dpdv[1]
                      << ' ' << patch_dpdv[2]
                      << " count " << normals.size();
            for (std::size_t index = 0u; index < normals.size(); ++index) {
                std::cout << ' ' << normals[index][0] << ' '
                          << normals[index][1] << ' ' << normals[index][2]
                          << ' ' << binormals[index][0] << ' '
                          << binormals[index][1] << ' '
                          << binormals[index][2];
            }
            std::cout << '\n';
        }
    }

    double total_area = 0.0;
    std::cout << std::setprecision(17);
    for (const int face : matched_patch->faceIds()) {
        const double area = matched_patch->area(face);
        const double length_u = matched_patch->lengthU(0.5, face);
        const double length_v = matched_patch->lengthV(0.5, face);
        total_area += area;
        if (faces) {
            const SgBox3d reference_bounds =
                matched_patch->boundingBox(face, true);
            SgVec3d bounds_min;
            SgVec3d bounds_max;
            reference_bounds.getMin(bounds_min);
            reference_bounds.getMax(bounds_max);
            std::cout << "face " << face << " area " << area
                      << " length_u " << length_u
                      << " length_v " << length_v
                      << " reference_bounds " << bounds_min[0] << ' '
                      << bounds_min[1] << ' ' << bounds_min[2] << ' '
                      << bounds_max[0] << ' ' << bounds_max[1] << ' '
                      << bounds_max[2] << '\n';
        }
    }
    std::cout << "{\"palette\":\"" << matched_palette->name()
              << "\",\"description\":\"" << matched_description->name()
              << "\",\"patch\":\"" << matched_patch->name()
              << "\",\"faces\":" << matched_patch->faceIds().size()
              << ",\"area\":" << total_area << "}\n";
    renderer.reset();
    cleanup.run();
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
