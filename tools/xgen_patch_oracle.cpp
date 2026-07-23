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
#else
#error "Autodesk XGen SDK headers were not found"
#endif

#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <charconv>
#include <cmath>
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
    std::optional<unsigned int> primitive_id;
    bool faces = false;
    bool weights = false;
    bool geometry = false;
    bool apply_fx = false;
    bool guides = false;
    bool subd_arrays = false;
    bool cv_attrs = false;
    std::vector<Sample> samples;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if ((argument == "--xgen-args" || argument == "--description" ||
             argument == "--patch" || argument == "--expression" ||
             argument == "--module" ||
             argument == "--clump-module" ||
             argument == "--clump-guide-index" ||
             argument == "--clump-noise-mask" ||
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
        using FloatArrayMethod = const float *(*)(const void *);
        using IntArrayMethod = const int *(*)(const void *);
        using IntMethod = int (*)(const void *);
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
        if (!verts || !counts || !indices || index_count < 0) {
            throw std::runtime_error("invalid XgSubdPatch calibration arrays");
        }
        // Autodesk does not publish an accessor for SESubd's computed limit
        // vertices. Keep this ABI probe confined to the calibration oracle:
        // SgSubdSurface is the pointer at XgSubdPatch+0xb0 in Maya 2027 and
        // owns SESubd at +8; virtual slot 0x108 is limitVerts(unsigned).
        const auto patch_bytes = reinterpret_cast<const unsigned char *>(
            matched_patch);
        void *surface = *reinterpret_cast<void *const *>(patch_bytes + 0xb0u);
        if (!surface) {
            throw std::runtime_error("missing XgSubdPatch SgSubdSurface");
        }
        const auto surface_bytes = reinterpret_cast<const unsigned char *>(surface);
        void *subd = *reinterpret_cast<void *const *>(surface_bytes + 8u);
        if (!subd) { throw std::runtime_error("missing SgSubdSurface SESubd"); }
        void **vtable = *reinterpret_cast<void ***>(subd);
        using LimitVertexMethod = const float *(*)(void *, unsigned int);
        const auto limit_vertex = reinterpret_cast<LimitVertexMethod>(
            vtable[0x108u / sizeof(void *)]);
        if (!limit_vertex) {
            throw std::runtime_error("missing SESubd limitVerts virtual slot");
        }
        std::size_t offset = 0u;
        std::cout << std::setprecision(17);
        for (unsigned int face = 0u; face < matched_patch->numGeomFaces(); ++face) {
            const int count = counts[face];
            if (count < 0 || offset > static_cast<std::size_t>(index_count) ||
                static_cast<std::size_t>(count) >
                    static_cast<std::size_t>(index_count) - offset) {
                throw std::runtime_error(
                    "invalid XgSubdPatch face topology arrays");
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
                }
                std::cout << '\n';
            }
            offset += static_cast<std::size_t>(count);
        }
        if (offset != static_cast<std::size_t>(index_count)) {
            throw std::runtime_error(
                "XgSubdPatch face topology arrays do not consume all indices");
        }
    }

    XgPrimitive *primitive = matched_description->activePrimitive();
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
        XgExpression expression{
            expression_name, object_type, matched_description,
            source, "float"};
        std::string diagnostic;
        if (!expression.syntaxOK(&diagnostic) ||
            !expression.isValid(&diagnostic)) {
            throw std::runtime_error(
                "XGen rejected expression '" + expression_name + "': " +
                diagnostic);
        }
        std::cout << std::setprecision(17);
        for (const Sample &sample : samples) {
            const SgVec3d value = expression.eval(
                sample.u, sample.v, sample.face, &patch_name);
            std::cout << "sample " << sample.face << ' ' << sample.u << ' '
                      << sample.v << ' ' << value[0] << '\n';
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
            SgCurve::frame(
                points, surface_n, surface_u, normals, binormals);
            std::cout << "frame " << sample.face << ' ' << sample.u << ' '
                      << sample.v << " surface_n " << surface_n[0] << ' '
                      << surface_n[1] << ' ' << surface_n[2]
                      << " surface_u " << surface_u[0] << ' '
                      << surface_u[1] << ' ' << surface_u[2]
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
