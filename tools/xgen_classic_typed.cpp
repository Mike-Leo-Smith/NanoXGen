#include "nanoxgen/curve_cache.h"
#include "xgen_classic_typed_validation.h"

#if __has_include(<xgen/src/xgrenderer/XgRenderAPI.h>)
#include <xgen/src/xgrenderer/XgRenderAPI.h>
#include <xgen/src/xgcore/XgDescription.h>
#include <xgen/src/xgcore/XgFXModule.h>
#include <xgen/src/xgcore/XgGenerator.h>
#include <xgen/src/xgcore/XgPalette.h>
#include <xgen/src/xgcore/XgPrimitive.h>
#include <xgen/src/xgcore/XgRenderer.h>
#else
#error "Autodesk XgRenderAPI.h was not found"
#endif

#include <algorithm>
#include <chrono>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nanoxgen;
namespace classic = nanoxgen::classic_typed;
namespace api = XGenRenderAPI;

namespace {

struct ModuleAttributeOverride {
    std::string module;
    std::string attribute;
    std::string type;
    std::string value;
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

class TypedCallbacks final : public api::ProceduralCallbacks {
public:
    TypedCallbacks(std::string cache_dir, bool dump_surface_frame,
                   std::optional<std::uint32_t> dump_primitive_face,
                   std::vector<float> shutter_samples,
                   float shutter_offset)
        : _cache_dir(std::move(cache_dir)),
          _dump_surface_frame(dump_surface_frame),
          _dump_primitive_face(dump_primitive_face),
          _shutter_samples(std::move(shutter_samples)),
          _shutter_offset(shutter_offset) {}

    void flush(const char *, api::PrimitiveCache *cache) override {
        if (!_error.empty()) { return; }
        try {
            if (!cache) { throw std::runtime_error("XGen supplied a null PrimitiveCache"); }
            if (!cache->get(api::PrimitiveCache::PrimIsSpline)) {
                throw std::runtime_error(
                    "Classic typed bridge only accepts spline primitives");
            }
            const unsigned int sample_count =
                cache->get(api::PrimitiveCache::NumMotionSamples);
            if (sample_count == 0u || sample_count > 20u) {
                throw std::runtime_error(
                    "Classic typed bridge requires 1-20 motion samples");
            }
            if (cache->getSize(api::PrimitiveCache::Points) != sample_count ||
                cache->getSize(api::PrimitiveCache::NumVertices) != sample_count) {
                throw std::runtime_error(
                    "Classic Points/NumVertices motion sample counts disagree");
            }
            if (_motion_sample_count == 0u) {
                _motion_sample_count = sample_count;
                _curves.motion_positions.resize(sample_count - 1u);
            } else if (_motion_sample_count != sample_count) {
                throw std::runtime_error(
                    "Classic motion sample count changes between batches");
            }

            const unsigned int primitive_count =
                cache->getSize2(api::PrimitiveCache::NumVertices, 0u);
            const unsigned int point_count =
                cache->getSize2(api::PrimitiveCache::Points, 0u);
            const unsigned int width_count = cache->getSize(api::PrimitiveCache::Widths);
            const unsigned int u_count = cache->getSize(api::PrimitiveCache::U_XS);
            const unsigned int v_count = cache->getSize(api::PrimitiveCache::V_XS);
            const unsigned int face_id_count =
                cache->getSize(api::PrimitiveCache::FaceID_XS);
            const int *num_vertices = cache->get(api::PrimitiveCache::NumVertices, 0u);
            const api::vec3 *points = cache->get(api::PrimitiveCache::Points, 0u);
            const float *widths = cache->get(api::PrimitiveCache::Widths);
            const float *u = cache->get(api::PrimitiveCache::U_XS);
            const float *v = cache->get(api::PrimitiveCache::V_XS);
            const int *face_ids = cache->get(api::PrimitiveCache::FaceID_XS);
            if ((primitive_count != 0u && !num_vertices) ||
                (point_count != 0u && !points) || (width_count != 0u && !widths) ||
                (u_count != 0u && !u) || (v_count != 0u && !v) ||
                (face_id_count != 0u && !face_ids)) {
                throw std::runtime_error("Classic PrimitiveCache returned a null channel");
            }
            if (_dump_primitive_face) {
                const unsigned int primitive_id_count =
                    cache->getSize(api::PrimitiveCache::PrimitiveID_XP);
                const int *primitive_ids =
                    cache->get(api::PrimitiveCache::PrimitiveID_XP);
                if (primitive_id_count != primitive_count || !primitive_ids ||
                    u_count != primitive_count || v_count != primitive_count ||
                    face_id_count != primitive_count) {
                    throw std::runtime_error(
                        "PrimitiveID calibration channel cardinality mismatch");
                }
                for (unsigned int primitive = 0u;
                     primitive < primitive_count; ++primitive) {
                    if (face_ids[primitive] !=
                        static_cast<int>(*_dump_primitive_face)) {
                        continue;
                    }
                    std::cerr << std::setprecision(9)
                              << "primitive " << face_ids[primitive] << ' '
                              << u[primitive] << ' ' << v[primitive]
                              << " bits "
                              << std::bit_cast<std::uint32_t>(u[primitive])
                              << ' '
                              << std::bit_cast<std::uint32_t>(v[primitive])
                              << " id " << primitive_ids[primitive] << '\n';
                }
            }
            if (_dump_surface_frame && _flush_count == 0u) {
                const unsigned int normal_count =
                    cache->getSize(api::PrimitiveCache::N_XS);
                const unsigned int tangent_count =
                    cache->getSize(api::PrimitiveCache::dPdu_XS);
                const api::vec3 *normals =
                    cache->get(api::PrimitiveCache::N_XS);
                const api::vec3 *tangents =
                    cache->get(api::PrimitiveCache::dPdu_XS);
                const api::vec3 *positions =
                    cache->get(api::PrimitiveCache::P_XS);
                const api::vec3 *reference_positions =
                    cache->get(api::PrimitiveCache::Pg_XS);
                const unsigned int primitive_id_count =
                    cache->getSize(api::PrimitiveCache::PrimitiveID_XP);
                const int *primitive_ids =
                    cache->get(api::PrimitiveCache::PrimitiveID_XP);
                if (normal_count != 0u && tangent_count != 0u &&
                    normals && tangents) {
                    std::cerr << std::setprecision(9)
                              << "surface_frame normal "
                              << normals[0].x << ' ' << normals[0].y << ' '
                              << normals[0].z << " tangent "
                              << tangents[0].x << ' ' << tangents[0].y << ' '
                              << tangents[0].z;
                    if (positions) {
                        std::cerr << " position " << positions[0].x << ' '
                                  << positions[0].y << ' ' << positions[0].z;
                    }
                    if (reference_positions) {
                        std::cerr << " reference_position "
                                  << reference_positions[0].x << ' '
                                  << reference_positions[0].y << ' '
                                  << reference_positions[0].z;
                    }
                    std::cerr << " primitive_id_count "
                              << primitive_id_count;
                    if (primitive_id_count != 0u && primitive_ids) {
                        std::cerr << " primitive_id " << primitive_ids[0];
                    }
                    std::cerr << '\n';
                }
            }
            std::optional<float> constant_width;
            if (width_count == 0u) {
                constant_width = cache->get(api::PrimitiveCache::ConstantWidth);
            }
            const std::size_t primitive_offset =
                _curves.point_counts.size();
            const std::size_t point_offset = _curves.points.size();
            classic::append_batch(
                std::span<const int>{num_vertices, primitive_count},
                std::span<const api::vec3>{points, point_count},
                std::span<const float>{widths, width_count}, constant_width,
                std::span<const float>{u, u_count},
                std::span<const float>{v, v_count},
                std::span<const int>{face_ids, face_id_count}, _curves);
            for (unsigned int sample = 1u;
                 sample < sample_count; ++sample) {
                const unsigned int sample_primitive_count =
                    cache->getSize2(
                        api::PrimitiveCache::NumVertices, sample);
                const unsigned int sample_point_count =
                    cache->getSize2(api::PrimitiveCache::Points, sample);
                const int *sample_num_vertices =
                    cache->get(
                        api::PrimitiveCache::NumVertices, sample);
                const api::vec3 *sample_points =
                    cache->get(api::PrimitiveCache::Points, sample);
                if ((sample_primitive_count != 0u &&
                     !sample_num_vertices) ||
                    (sample_point_count != 0u && !sample_points)) {
                    throw std::runtime_error(
                        "Classic motion PrimitiveCache returned a null channel");
                }
                classic::append_motion_batch(
                    std::span<const int>{
                        sample_num_vertices, sample_primitive_count},
                    std::span<const api::vec3>{
                        sample_points, sample_point_count},
                    primitive_offset, point_offset, sample, _curves);
            }
            ++_flush_count;
        } catch (const std::exception &error) {
            _error = error.what();
        }
    }

    void log(const char *message) override {
        if (message && *message != '\0') {
            std::cerr << "XGen: " << message << '\n';
        }
    }

    bool get(EBoolAttribute) const override { return false; }

    float get(EFloatAttribute attribute) const override {
        return attribute == ShutterOffset ? _shutter_offset : 0.0f;
    }

    const char *get(EStringAttribute attribute) const override {
        switch (attribute) {
            case CacheDir: return _cache_dir.c_str();
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

    const float *get(EFloatArrayAttribute attribute) const override {
        return attribute == Shutter && !_shutter_samples.empty()
            ? _shutter_samples.data() : nullptr;
    }
    unsigned int getSize(EFloatArrayAttribute attribute) const override {
        return attribute == Shutter
            ? static_cast<unsigned int>(_shutter_samples.size()) : 0u;
    }
    const char *getOverride(const char *) const override { return ""; }

    void getTransform(float, api::mat44 &matrix) const override {
        std::memset(&matrix, 0, sizeof(matrix));
        matrix._00 = 1.0f;
        matrix._11 = 1.0f;
        matrix._22 = 1.0f;
        matrix._33 = 1.0f;
    }

    bool getArchiveBoundingBox(const char *, api::bbox &) const override {
        return false;
    }

    [[nodiscard]] const classic::Curves &curves() const noexcept { return _curves; }
    [[nodiscard]] const std::string &error() const noexcept { return _error; }
    [[nodiscard]] std::size_t flush_count() const noexcept { return _flush_count; }
    [[nodiscard]] std::size_t motion_sample_count() const noexcept {
        return _motion_sample_count;
    }
    [[nodiscard]] std::size_t shutter_sample_count() const noexcept {
        return _shutter_samples.size();
    }
    [[nodiscard]] std::span<const float> shutter_samples() const noexcept {
        return _shutter_samples;
    }

private:
    std::string _cache_dir;
    bool _dump_surface_frame{};
    std::optional<std::uint32_t> _dump_primitive_face;
    std::vector<float> _shutter_samples;
    float _shutter_offset{};
    classic::Curves _curves;
    std::string _error;
    std::size_t _flush_count{};
    std::size_t _motion_sample_count{};
};

std::string escape_json(const std::string &value) {
    std::string result;
    for (const unsigned char byte : value) {
        switch (byte) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (byte < 0x20u) {
                    throw std::runtime_error(
                        "output path contains an unsupported control character");
                }
                result.push_back(static_cast<char>(byte));
        }
    }
    return result;
}

std::uint64_t hash_bytes(std::uint64_t hash, std::span<const std::byte> bytes) noexcept {
    constexpr std::uint64_t prime = 1099511628211ull;
    for (const std::byte byte : bytes) {
        hash ^= std::to_integer<std::uint8_t>(byte);
        hash *= prime;
    }
    return hash;
}

template<typename T>
std::uint64_t hash_values(std::uint64_t hash, std::span<const T> values) noexcept {
    return hash_bytes(hash, std::as_bytes(values));
}

} // namespace

int main(int argc, char **argv) try {
    std::string xgen_args;
    std::string cache_dir{"xgenCache/"};
    std::string description_name;
    std::optional<std::string> generator_mask;
    std::optional<std::size_t> fx_count;
    std::vector<ModuleAttributeOverride> module_attribute_overrides;
    std::optional<std::filesystem::path> output_path;
    bool dump_surface_frame = false;
    std::optional<std::uint32_t> dump_primitive_face;
    std::vector<float> shutter_samples;
    float shutter_offset{};
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--xgen-args" && index + 1 < argc) {
            xgen_args = argv[++index];
        } else if (argument == "--cache-dir" && index + 1 < argc) {
            cache_dir = argv[++index];
        } else if (argument == "--description" && index + 1 < argc) {
            description_name = argv[++index];
        } else if (argument == "--generator-mask" && index + 1 < argc) {
            generator_mask = argv[++index];
        } else if (argument == "--fx-count" && index + 1 < argc) {
            std::size_t consumed{};
            const std::string value = argv[++index];
            fx_count = std::stoull(value, &consumed);
            if (consumed != value.size()) {
                throw std::invalid_argument("--fx-count must be an integer");
            }
        } else if (argument == "--module-attr" && index + 4 < argc) {
            module_attribute_overrides.push_back({
                argv[++index], argv[++index], argv[++index], argv[++index]});
        } else if (argument == "--dump-surface-frame") {
            dump_surface_frame = true;
        } else if (argument == "--dump-primitive-ids" && index + 1 < argc) {
            dump_primitive_face = static_cast<std::uint32_t>(
                std::stoul(argv[++index]));
        } else if (argument == "--nxc" && index + 1 < argc) {
            output_path = argv[++index];
        } else if (argument == "--shutter-sample" && index + 1 < argc) {
            std::size_t consumed{};
            const std::string value = argv[++index];
            const float sample = std::stof(value, &consumed);
            if (consumed != value.size() || !std::isfinite(sample)) {
                throw std::invalid_argument(
                    "--shutter-sample must be finite");
            }
            shutter_samples.push_back(sample);
        } else if (argument == "--shutter-offset" && index + 1 < argc) {
            std::size_t consumed{};
            const std::string value = argv[++index];
            shutter_offset = std::stof(value, &consumed);
            if (consumed != value.size() ||
                !std::isfinite(shutter_offset)) {
                throw std::invalid_argument(
                    "--shutter-offset must be finite");
            }
        } else if (argument == "--help") {
            std::cout
                << "usage: nanoxgen_xgen_classic_typed --xgen-args <render-args> "
                   "[--cache-dir <dir>] [--nxc <output.nxc>] "
                   "[--description <name> [--generator-mask <expression>] "
                   "[--fx-count <count>] "
                   "[--module-attr <module> <attribute> <type> <value> ...]] "
                   "[--shutter-sample <placement> ...] "
                   "[--shutter-offset <value>] "
                   "[--dump-surface-frame] [--dump-primitive-ids <face>]\n";
            return 0;
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " + argument);
        }
    }
    if (xgen_args.empty()) {
        throw std::invalid_argument("--xgen-args must not be empty");
    }
    if ((generator_mask || fx_count || !module_attribute_overrides.empty() ||
         dump_surface_frame || dump_primitive_face) &&
        description_name.empty()) {
        throw std::invalid_argument(
            "runtime overrides and calibration dumps require --description");
    }

    TypedCallbacks callbacks{
        std::move(cache_dir), dump_surface_frame, dump_primitive_face,
        shutter_samples, shutter_offset};
    const auto evaluation_begin = std::chrono::steady_clock::now();
    {
        CleanupOnce cleanup;
        std::unique_ptr<api::PatchRenderer> patch{
            api::PatchRenderer::init(&callbacks, xgen_args.c_str())};
        if (!patch) { throw std::runtime_error("PatchRenderer::init returned null"); }
        if (generator_mask || fx_count || !module_attribute_overrides.empty() ||
            dump_surface_frame || dump_primitive_face) {
            XgDescription *matched_description = nullptr;
            for (const std::string &palette_name : XgPalette::palettes()) {
                XgPalette *palette = XgPalette::palette(palette_name);
                XgDescription *description = palette
                    ? palette->description(description_name)
                    : nullptr;
                if (!description) { continue; }
                if (matched_description) {
                    throw std::runtime_error(
                        "multiple XGen descriptions matched generator override");
                }
                matched_description = description;
            }
            if (!matched_description) {
                throw std::runtime_error(
                    "XGen description was not found for runtime override");
            }
            if (generator_mask) {
                XgGenerator *generator = matched_description->activeGenerator();
                if (!generator ||
                    !generator->setAttr("mask", *generator_mask, "float")) {
                    throw std::runtime_error(
                        "failed to override the active generator mask");
                }
            }
            if (dump_surface_frame || dump_primitive_face) {
                XgRenderer *renderer = matched_description->activeRenderer();
                if (!renderer ||
                    !renderer->setAttr("id_XP", "true", "bool")) {
                    throw std::runtime_error(
                        "failed to enable PrimitiveID_XP calibration channel");
                }
            }
            if (fx_count) {
                XgPrimitive *primitive = matched_description->activePrimitive();
                if (!primitive) {
                    throw std::runtime_error(
                        "XGen description has no active primitive");
                }
                auto &modules = primitive->modules();
                if (*fx_count > modules.size()) {
                    throw std::runtime_error(
                        "--fx-count exceeds the loaded FX module count");
                }
                for (std::size_t module = *fx_count;
                     module < modules.size(); ++module) {
                    if (!modules[module] ||
                        !modules[module]->setAttr("active", "false", "bool")) {
                        throw std::runtime_error(
                            "failed to disable an XGen FX module");
                    }
                }
            }
            for (const ModuleAttributeOverride &override_value :
                 module_attribute_overrides) {
                XgPrimitive *primitive = matched_description->activePrimitive();
                XgFXModule *module = primitive
                    ? primitive->findFXModule(override_value.module) : nullptr;
                if (!module || !module->setAttr(
                        override_value.attribute, override_value.value,
                        override_value.type)) {
                    throw std::runtime_error(
                        "failed to override XGen module attribute " +
                        override_value.module + "." +
                        override_value.attribute);
                }
            }
        }
        api::bbox bounds{};
        unsigned int face_id = std::numeric_limits<unsigned int>::max();
        while (callbacks.error().empty() && patch->nextFace(bounds, face_id)) {
            std::unique_ptr<api::FaceRenderer> face{
                api::FaceRenderer::init(patch.get(), face_id, &callbacks)};
            if (!face) { throw std::runtime_error("FaceRenderer::init returned null"); }
            if (!face->render()) {
                throw std::runtime_error("FaceRenderer::render failed for face " +
                                         std::to_string(face_id));
            }
        }
        patch.reset();
        cleanup.run();
    }
    const auto evaluation_end = std::chrono::steady_clock::now();
    if (!callbacks.error().empty()) {
        throw std::runtime_error(callbacks.error());
    }
    const classic::Curves &curves = callbacks.curves();
    if (curves.point_counts.empty() || curves.points.empty()) {
        throw std::runtime_error("Classic evaluation produced zero curves");
    }
    if (callbacks.shutter_sample_count() != 0u &&
        callbacks.shutter_sample_count() !=
            callbacks.motion_sample_count()) {
        throw std::runtime_error(
            "XGen motion sample count does not match renderer shutter table");
    }
    for (const auto &sample : curves.motion_positions) {
        if (sample.size() != curves.points.size()) {
            throw std::runtime_error(
                "Classic evaluation produced incomplete motion samples");
        }
    }
    std::uint64_t moving_points{};
    float maximum_motion_position_delta{};
    for (const auto &sample : curves.motion_positions) {
        for (std::size_t point = 0u;
             point < sample.size(); ++point) {
            const PackedCurvePoint &base = curves.points[point];
            const Vec3 motion = sample[point];
            maximum_motion_position_delta = std::max({
                maximum_motion_position_delta,
                std::abs(base.x - motion.x),
                std::abs(base.y - motion.y),
                std::abs(base.z - motion.z)});
            moving_points +=
                std::bit_cast<std::uint32_t>(base.x) !=
                    std::bit_cast<std::uint32_t>(motion.x) ||
                std::bit_cast<std::uint32_t>(base.y) !=
                    std::bit_cast<std::uint32_t>(motion.y) ||
                std::bit_cast<std::uint32_t>(base.z) !=
                    std::bit_cast<std::uint32_t>(motion.z);
        }
    }

    double cache_ms = 0.0;
    std::uint64_t cache_bytes = 0u;
    if (output_path) {
        std::vector<float> motion_times;
        std::vector<Vec3> motion_points;
        if (!curves.motion_positions.empty()) {
            if (callbacks.shutter_sample_count() !=
                curves.motion_positions.size() + 1u) {
                throw std::runtime_error(
                    "writing a motion .nxc needs one placement per sample");
            }
            const std::span<const float> placements =
                callbacks.shutter_samples();
            motion_times.assign(placements.begin() + 1u, placements.end());
            motion_points.reserve(
                curves.motion_positions.size() * curves.points.size());
            for (const auto &sample : curves.motion_positions) {
                motion_points.insert(
                    motion_points.end(), sample.begin(), sample.end());
            }
        }
        const auto cache_begin = std::chrono::steady_clock::now();
        const CurveCache cache = build_curve_cache(
            {curves.point_counts, curves.points, {}, {}, curves.face_uvs,
             curves.face_ids, motion_times, motion_points});
        save_curve_cache(cache, *output_path);
        cache_bytes = cache.bytes().size();
        const auto cache_end = std::chrono::steady_clock::now();
        cache_ms = std::chrono::duration<double, std::milli>(
                       cache_end - cache_begin).count();
    }

    std::uint64_t checksum = 1469598103934665603ull;
    checksum = hash_values(
        checksum, std::span<const std::uint32_t>{curves.point_counts});
    checksum = hash_values(
        checksum, std::span<const PackedCurvePoint>{curves.points});
    for (const auto &sample : curves.motion_positions) {
        checksum = hash_values(
            checksum, std::span<const Vec3>{sample});
    }
    checksum = hash_values(checksum, std::span<const Vec2>{curves.face_uvs});
    checksum = hash_values(
        checksum, std::span<const std::uint32_t>{curves.face_ids});
    const double evaluation_ms = std::chrono::duration<double, std::milli>(
        evaluation_end - evaluation_begin).count();
    std::cout << std::setprecision(9)
              << "{\"curves\":" << curves.point_counts.size()
              << ",\"points\":" << curves.points.size()
              << ",\"flushes\":" << callbacks.flush_count()
              << ",\"motion_samples\":"
              << callbacks.motion_sample_count()
              << ",\"moving_points\":" << moving_points
              << ",\"max_motion_position_delta\":"
              << maximum_motion_position_delta
              << ",\"evaluation_ms\":" << evaluation_ms
              << ",\"nxc_ms\":" << cache_ms
              << ",\"nxc_bytes\":" << cache_bytes
              << ",\"checksum\":\"0x" << std::hex << checksum << std::dec << '"'
              << ",\"typed_primitive_cache\":true"
              << ",\"intermediate_xgen_blob\":false";
    if (fx_count) {
        std::cout << ",\"fx_count\":" << *fx_count;
    }
    if (output_path) {
        std::cout << ",\"output\":\"" << escape_json(output_path->string()) << '"';
    }
    std::cout << "}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
