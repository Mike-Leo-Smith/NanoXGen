#include "nanoxgen/asset.h"

#if __has_include(<xgen/src/xgsculptcore/api/XgSplineAPI.h>)
#include <xgen/src/xgsculptcore/api/XgSplineAPI.h>
#elif __has_include(<XGen/XgSplineAPI.h>)
#include <XGen/XgSplineAPI.h>
#else
#error "Autodesk XgSplineAPI.h was not found"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace nanoxgen;

namespace {

struct OfficialCurve {
    std::vector<Vec3> points;
    std::vector<float> widths;
    Vec2 patch_uv{};
    Vec2 face_uv{};
    std::uint32_t face_id{};
};

std::uint32_t float_bits(float value);

std::vector<OfficialCurve> load_curves(const char *path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw std::runtime_error(std::string{"failed to open "} + path); }
    std::stringstream bytes;
    bytes << input.rdbuf();
    const std::string storage = bytes.str();
    XGenSplineAPI::XgFnSpline splines;
    std::stringstream stream{storage};
    if (!splines.load(stream, storage.size(), 0.0f) || !splines.executeScript()) {
        throw std::runtime_error(std::string{"failed to evaluate "} + path);
    }
    if (splines.sampleCount() != 1u) {
        throw std::runtime_error("parity tool currently requires one motion sample");
    }

    std::vector<OfficialCurve> result;
    for (auto it = splines.iterator(); !it.isDone(); it.next()) {
        const unsigned int stride = it.primitiveInfoStride();
        const unsigned int *infos = it.primitiveInfos();
        const SgVec3f *positions = it.positions();
        const SgVec2f *patch_uvs = it.patchUVs();
        const SgVec2f *face_uvs = it.faceUV();
        const unsigned int *face_ids = it.faceId();
        const float *widths = it.width();
        if (stride < 2u || !infos || !positions || !patch_uvs || !face_uvs ||
            !face_ids || !widths) {
            throw std::runtime_error("XGen returned incomplete parity data");
        }
        for (unsigned int primitive = 0u; primitive < it.primitiveCount(); ++primitive) {
            const unsigned int offset = infos[primitive * stride];
            const unsigned int length = infos[primitive * stride + 1u];
            if (length < 2u || offset > it.vertexCount() || length > it.vertexCount() - offset) {
                throw std::runtime_error("XGen returned an invalid primitive range");
            }
            OfficialCurve curve{};
            curve.patch_uv = {patch_uvs[offset][0], patch_uvs[offset][1]};
            curve.face_uv = {face_uvs[primitive][0], face_uvs[primitive][1]};
            curve.face_id = face_ids[primitive];
            curve.points.reserve(length);
            curve.widths.reserve(length);
            for (unsigned int vertex = offset; vertex < offset + length; ++vertex) {
                curve.points.push_back({positions[vertex][0], positions[vertex][1], positions[vertex][2]});
                curve.widths.push_back(widths[vertex]);
            }
            result.emplace_back(std::move(curve));
        }
    }
    const auto key = [](const OfficialCurve &curve) {
        return std::tuple{curve.face_id, float_bits(curve.face_uv.x),
                          float_bits(curve.face_uv.y), float_bits(curve.patch_uv.x),
                          float_bits(curve.patch_uv.y)};
    };
    std::sort(result.begin(), result.end(), [&](const OfficialCurve &a, const OfficialCurve &b) {
        return key(a) < key(b);
    });
    return result;
}

std::uint32_t float_bits(float value) {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::uint32_t ordered_bits(float value) {
    const std::uint32_t bits = float_bits(value);
    return (bits & 0x80000000u) != 0u ? ~bits : bits | 0x80000000u;
}

std::uint32_t ulp_distance(float a, float b) {
    const std::uint32_t oa = ordered_bits(a);
    const std::uint32_t ob = ordered_bits(b);
    return oa > ob ? oa - ob : ob - oa;
}

bool same_bits(float a, float b) { return float_bits(a) == float_bits(b); }

float parse_float(const char *text, const char *name) {
    std::size_t used = 0u;
    const float value = std::stof(text, &used);
    if (text[used] != '\0' || !std::isfinite(value)) {
        throw std::invalid_argument(std::string{"invalid "} + name);
    }
    return value;
}

std::uint32_t parse_uint(const char *text, const char *name) {
    std::size_t used = 0u;
    const unsigned long value = std::stoul(text, &used);
    if (text[used] != '\0' || value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string{"invalid "} + name);
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace

int main(int argc, char **argv) try {
    if (argc < 3) {
        std::cerr << "usage: nanoxgen_xgen_parity <base.xgen> <target.xgen> "
                     "[--length-scale n] [--width-taper n] [--width-taper-start n] "
                     "[--position-abs n] [--position-ulp n]\n";
        return 2;
    }
    LinearModifierReferenceParams params{};
    float position_abs_tolerance = 5.0e-7f;
    std::uint32_t position_ulp_tolerance = 4u;
    for (int i = 3; i < argc; i += 2) {
        if (i + 1 >= argc) { throw std::invalid_argument("missing option value"); }
        const std::string option = argv[i];
        if (option == "--length-scale") {
            params.length_scale = parse_float(argv[i + 1], "length scale");
        } else if (option == "--width-taper") {
            params.width_taper = parse_float(argv[i + 1], "width taper");
        } else if (option == "--width-taper-start") {
            params.width_taper_start = parse_float(argv[i + 1], "width taper start");
        } else if (option == "--position-abs") {
            position_abs_tolerance = parse_float(argv[i + 1], "position tolerance");
        } else if (option == "--position-ulp") {
            position_ulp_tolerance = parse_uint(argv[i + 1], "ULP tolerance");
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }

    const std::vector<OfficialCurve> base = load_curves(argv[1]);
    const std::vector<OfficialCurve> target = load_curves(argv[2]);
    if (base.size() != target.size() || base.empty()) {
        throw std::runtime_error("base and target curve counts differ or are empty");
    }
    params.cvs_per_strand = static_cast<std::uint32_t>(target.front().points.size());
    std::vector<LinearCurveSeed> seeds;
    seeds.reserve(base.size());
    std::uint64_t metadata_mismatches = 0u;
    for (std::size_t i = 0u; i < base.size(); ++i) {
        if (base[i].points.size() != params.cvs_per_strand ||
            target[i].points.size() != params.cvs_per_strand ||
            base[i].widths.empty()) {
            throw std::runtime_error("parity fixtures must use one fixed CV count");
        }
        metadata_mismatches += base[i].face_id != target[i].face_id;
        metadata_mismatches += !same_bits(base[i].face_uv.x, target[i].face_uv.x);
        metadata_mismatches += !same_bits(base[i].face_uv.y, target[i].face_uv.y);
        metadata_mismatches += !same_bits(base[i].patch_uv.x, target[i].patch_uv.x);
        metadata_mismatches += !same_bits(base[i].patch_uv.y, target[i].patch_uv.y);
        seeds.push_back({base[i].points.front(), base[i].points.back(),
                         base[i].patch_uv, base[i].widths.front()});
    }

    const GeneratedCurves generated = generate_linear_modifier_reference_cpu(seeds, params);
    std::uint64_t position_values = 0u;
    std::uint64_t position_exact = 0u;
    std::uint64_t position_failures = 0u;
    std::uint64_t width_values = 0u;
    std::uint64_t width_exact = 0u;
    float max_position_abs = 0.0f;
    float max_width_abs = 0.0f;
    std::uint32_t max_position_ulp = 0u;
    std::uint32_t max_width_ulp = 0u;
    long double squared_error = 0.0;
    for (std::size_t strand = 0u; strand < target.size(); ++strand) {
        for (std::uint32_t cv = 0u; cv < params.cvs_per_strand; ++cv) {
            const std::size_t index = strand * params.cvs_per_strand + cv;
            const float actual[3] = {generated.points[index].x,
                                     generated.points[index].y,
                                     generated.points[index].z};
            const float expected[3] = {target[strand].points[cv].x,
                                       target[strand].points[cv].y,
                                       target[strand].points[cv].z};
            for (unsigned int axis = 0u; axis < 3u; ++axis) {
                const float error = std::abs(actual[axis] - expected[axis]);
                const std::uint32_t ulp = ulp_distance(actual[axis], expected[axis]);
                max_position_abs = std::max(max_position_abs, error);
                max_position_ulp = std::max(max_position_ulp, ulp);
                squared_error += static_cast<long double>(error) * error;
                position_exact += same_bits(actual[axis], expected[axis]);
                position_failures += error > position_abs_tolerance &&
                    ulp > position_ulp_tolerance;
                ++position_values;
            }
            const float width_error = std::abs(generated.widths[index] - target[strand].widths[cv]);
            const std::uint32_t width_ulp = ulp_distance(generated.widths[index], target[strand].widths[cv]);
            max_width_abs = std::max(max_width_abs, width_error);
            max_width_ulp = std::max(max_width_ulp, width_ulp);
            width_exact += same_bits(generated.widths[index], target[strand].widths[cv]);
            ++width_values;
        }
    }
    const double rms = std::sqrt(static_cast<double>(squared_error / position_values));
    const bool passed = metadata_mismatches == 0u && position_failures == 0u &&
        width_exact == width_values;
    std::cout << std::setprecision(9)
              << "{\n"
              << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
              << "  \"curves\": " << target.size() << ",\n"
              << "  \"cvs_per_curve\": " << params.cvs_per_strand << ",\n"
              << "  \"metadata_mismatches\": " << metadata_mismatches << ",\n"
              << "  \"positions\": {\"values\": " << position_values
              << ", \"bit_exact\": " << position_exact
              << ", \"failures\": " << position_failures
              << ", \"max_abs\": " << max_position_abs
              << ", \"rms\": " << rms
              << ", \"max_ulp\": " << max_position_ulp << "},\n"
              << "  \"widths\": {\"values\": " << width_values
              << ", \"bit_exact\": " << width_exact
              << ", \"max_abs\": " << max_width_abs
              << ", \"max_ulp\": " << max_width_ulp << "}\n"
              << "}\n";
    return passed ? 0 : 1;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
