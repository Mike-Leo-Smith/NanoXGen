#include "nanoxgen/curve_cache.h"
#include "xgen_classic_typed_validation.h"

#if __has_include(<xgen/src/xgrenderer/XgRenderAPI.h>)
#include <xgen/src/xgrenderer/XgRenderAPI.h>
#else
#error "Autodesk XgRenderAPI.h was not found"
#endif

#include <chrono>
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

using namespace nanoxgen;
namespace classic = nanoxgen::classic_typed;
namespace api = XGenRenderAPI;

namespace {

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
    explicit TypedCallbacks(std::string cache_dir) : _cache_dir(std::move(cache_dir)) {}

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
            if (sample_count != 1u) {
                throw std::runtime_error(
                    "Classic typed bridge currently requires one motion sample");
            }
            if (cache->getSize(api::PrimitiveCache::Points) != sample_count ||
                cache->getSize(api::PrimitiveCache::NumVertices) != sample_count) {
                throw std::runtime_error(
                    "Classic Points/NumVertices motion sample counts disagree");
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
            std::optional<float> constant_width;
            if (width_count == 0u) {
                constant_width = cache->get(api::PrimitiveCache::ConstantWidth);
            }
            classic::append_batch(
                std::span<const int>{num_vertices, primitive_count},
                std::span<const api::vec3>{points, point_count},
                std::span<const float>{widths, width_count}, constant_width,
                std::span<const float>{u, u_count},
                std::span<const float>{v, v_count},
                std::span<const int>{face_ids, face_id_count}, _curves);
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

    float get(EFloatAttribute) const override { return 0.0f; }

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

    const float *get(EFloatArrayAttribute) const override { return nullptr; }
    unsigned int getSize(EFloatArrayAttribute) const override { return 0u; }
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

private:
    std::string _cache_dir;
    classic::Curves _curves;
    std::string _error;
    std::size_t _flush_count{};
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
    std::optional<std::filesystem::path> output_path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--xgen-args" && index + 1 < argc) {
            xgen_args = argv[++index];
        } else if (argument == "--cache-dir" && index + 1 < argc) {
            cache_dir = argv[++index];
        } else if (argument == "--nxc" && index + 1 < argc) {
            output_path = argv[++index];
        } else if (argument == "--help") {
            std::cout
                << "usage: nanoxgen_xgen_classic_typed --xgen-args <render-args> "
                   "[--cache-dir <dir>] [--nxc <output.nxc>]\n";
            return 0;
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " + argument);
        }
    }
    if (xgen_args.empty()) {
        throw std::invalid_argument("--xgen-args must not be empty");
    }

    TypedCallbacks callbacks{std::move(cache_dir)};
    const auto evaluation_begin = std::chrono::steady_clock::now();
    {
        CleanupOnce cleanup;
        std::unique_ptr<api::PatchRenderer> patch{
            api::PatchRenderer::init(&callbacks, xgen_args.c_str())};
        if (!patch) { throw std::runtime_error("PatchRenderer::init returned null"); }
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

    double cache_ms = 0.0;
    std::uint64_t cache_bytes = 0u;
    if (output_path) {
        const auto cache_begin = std::chrono::steady_clock::now();
        const CurveCache cache = build_curve_cache(
            {curves.point_counts, curves.points, {}, {}, curves.face_uvs,
             curves.face_ids, {}, {}});
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
    checksum = hash_values(checksum, std::span<const Vec2>{curves.face_uvs});
    checksum = hash_values(
        checksum, std::span<const std::uint32_t>{curves.face_ids});
    const double evaluation_ms = std::chrono::duration<double, std::milli>(
        evaluation_end - evaluation_begin).count();
    std::cout << std::setprecision(9)
              << "{\"curves\":" << curves.point_counts.size()
              << ",\"points\":" << curves.points.size()
              << ",\"flushes\":" << callbacks.flush_count()
              << ",\"evaluation_ms\":" << evaluation_ms
              << ",\"nxc_ms\":" << cache_ms
              << ",\"nxc_bytes\":" << cache_bytes
              << ",\"checksum\":\"0x" << std::hex << checksum << std::dec << '"'
              << ",\"typed_primitive_cache\":true"
              << ",\"intermediate_xgen_blob\":false";
    if (output_path) {
        std::cout << ",\"output\":\"" << escape_json(output_path->string()) << '"';
    }
    std::cout << "}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
