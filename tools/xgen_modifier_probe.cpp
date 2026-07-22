#include "nanoxgen/types.h"

#if __has_include(<xgen/src/xgsculptcore/api/XgSplineAPI.h>)
#include <xgen/src/xgsculptcore/api/XgSplineAPI.h>
#elif __has_include(<XGen/XgSplineAPI.h>)
#include <XGen/XgSplineAPI.h>
#else
#error "Autodesk XgSplineAPI.h was not found"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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

struct ScalarStats {
    std::uint64_t count{};
    double minimum{std::numeric_limits<double>::infinity()};
    double maximum{-std::numeric_limits<double>::infinity()};
    long double sum{};
    long double squared_sum{};

    void add(double value) {
        if (!std::isfinite(value)) { throw std::runtime_error("non-finite modifier metric"); }
        ++count;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
        sum += value;
        squared_sum += static_cast<long double>(value) * value;
    }

    [[nodiscard]] double mean() const {
        return count == 0u ? 0.0 : static_cast<double>(sum / count);
    }

    [[nodiscard]] double rms() const {
        return count == 0u ? 0.0 : std::sqrt(static_cast<double>(squared_sum / count));
    }
};

std::uint32_t float_bits(float value) {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

auto curve_key(const OfficialCurve &curve) {
    return std::tuple{curve.face_id, float_bits(curve.face_uv.x),
                      float_bits(curve.face_uv.y), float_bits(curve.patch_uv.x),
                      float_bits(curve.patch_uv.y)};
}

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
        throw std::runtime_error("modifier probe requires one motion sample per BLOB");
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
            throw std::runtime_error("XGen returned incomplete modifier-probe data");
        }
        const unsigned int batch_vertices = it.vertexCount();
        for (unsigned int primitive = 0u; primitive < it.primitiveCount(); ++primitive) {
            const unsigned int offset = infos[primitive * stride];
            const unsigned int count = infos[primitive * stride + 1u];
            if (count < 2u || offset > batch_vertices || count > batch_vertices - offset) {
                throw std::runtime_error("XGen returned an invalid primitive range");
            }
            OfficialCurve curve{};
            curve.patch_uv = {patch_uvs[offset][0], patch_uvs[offset][1]};
            curve.face_uv = {face_uvs[primitive][0], face_uvs[primitive][1]};
            curve.face_id = face_ids[primitive];
            curve.points.reserve(count);
            curve.widths.reserve(count);
            for (unsigned int vertex = offset; vertex < offset + count; ++vertex) {
                curve.points.push_back(
                    {positions[vertex][0], positions[vertex][1], positions[vertex][2]});
                curve.widths.push_back(widths[vertex]);
            }
            result.emplace_back(std::move(curve));
        }
    }
    std::sort(result.begin(), result.end(), [](const OfficialCurve &a, const OfficialCurve &b) {
        return curve_key(a) < curve_key(b);
    });
    for (std::size_t i = 1u; i < result.size(); ++i) {
        if (curve_key(result[i - 1u]) == curve_key(result[i])) {
            throw std::runtime_error("duplicate face/UV curve identity in modifier fixture");
        }
    }
    return result;
}

float norm(Vec3 value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

Vec3 unit(Vec3 value) {
    const float length = norm(value);
    return length > 1.0e-20f ? value / length : Vec3{};
}

float arc_length(const OfficialCurve &curve) {
    float result = 0.0f;
    for (std::size_t i = 1u; i < curve.points.size(); ++i) {
        result += norm(curve.points[i] - curve.points[i - 1u]);
    }
    return result;
}

Vec3 tangent(const OfficialCurve &curve, std::size_t cv) {
    const std::size_t first = cv == 0u ? 0u : cv - 1u;
    const std::size_t last = std::min(cv + 1u, curve.points.size() - 1u);
    return unit(curve.points[last] - curve.points[first]);
}

void hash_float(std::uint64_t &hash, float value) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(&value);
    for (std::size_t i = 0u; i < sizeof(value); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
}

void write_stats(const char *name, const ScalarStats &stats) {
    std::cout << '"' << name << "\":{\"count\":" << stats.count
              << ",\"min\":" << (stats.count == 0u ? 0.0 : stats.minimum)
              << ",\"max\":" << (stats.count == 0u ? 0.0 : stats.maximum)
              << ",\"mean\":" << stats.mean() << ",\"rms\":" << stats.rms() << '}';
}

struct SpatialBin {
    std::uint64_t pairs{};
    long double cosine_sum{};
};

} // namespace

int main(int argc, char **argv) try {
    if (argc != 3) {
        std::cerr << "usage: nanoxgen_xgen_modifier_probe <base.xgen> <target.xgen>\n";
        return 2;
    }
    const std::vector<OfficialCurve> base = load_curves(argv[1]);
    const std::vector<OfficialCurve> target = load_curves(argv[2]);
    if (base.empty() || base.size() != target.size()) {
        throw std::runtime_error("base and target curve counts differ or are empty");
    }
    const std::size_t cv_count = base.front().points.size();
    std::uint64_t metadata_mismatches = 0u;
    for (std::size_t curve = 0u; curve < base.size(); ++curve) {
        metadata_mismatches += curve_key(base[curve]) != curve_key(target[curve]);
        if (base[curve].points.size() != cv_count || target[curve].points.size() != cv_count ||
            base[curve].widths.size() != cv_count || target[curve].widths.size() != cv_count) {
            throw std::runtime_error("modifier fixture topology differs between curves or BLOBs");
        }
    }

    ScalarStats root_displacement;
    ScalarStats tip_displacement;
    ScalarStats relative_displacement;
    ScalarStats tangential_displacement;
    ScalarStats normal_displacement;
    ScalarStats width_absolute_delta;
    ScalarStats arc_length_ratio;
    ScalarStats arc_length_relative_error;
    std::vector<ScalarStats> per_cv(cv_count);
    std::vector<Vec3> tip_vectors(base.size());
    std::uint64_t displacement_hash = 1469598103934665603ull;

    Vec3 root_min = base.front().points.front();
    Vec3 root_max = root_min;
    for (std::size_t curve = 0u; curve < base.size(); ++curve) {
        const Vec3 base_root = base[curve].points.front();
        const Vec3 target_root = target[curve].points.front();
        root_min = {std::min(root_min.x, base_root.x), std::min(root_min.y, base_root.y),
                    std::min(root_min.z, base_root.z)};
        root_max = {std::max(root_max.x, base_root.x), std::max(root_max.y, base_root.y),
                    std::max(root_max.z, base_root.z)};
        root_displacement.add(norm(target_root - base_root));
        for (std::size_t cv = 0u; cv < cv_count; ++cv) {
            const Vec3 displacement =
                (target[curve].points[cv] - target_root) -
                (base[curve].points[cv] - base_root);
            const float magnitude = norm(displacement);
            relative_displacement.add(magnitude);
            per_cv[cv].add(magnitude);
            const float tangent_component = dot(displacement, tangent(base[curve], cv));
            tangential_displacement.add(std::abs(tangent_component));
            normal_displacement.add(std::sqrt(std::max(
                0.0f, magnitude * magnitude - tangent_component * tangent_component)));
            width_absolute_delta.add(
                std::abs(target[curve].widths[cv] - base[curve].widths[cv]));
            hash_float(displacement_hash, displacement.x);
            hash_float(displacement_hash, displacement.y);
            hash_float(displacement_hash, displacement.z);
        }
        tip_vectors[curve] =
            (target[curve].points.back() - target_root) -
            (base[curve].points.back() - base_root);
        tip_displacement.add(norm(tip_vectors[curve]));
        const float base_length = arc_length(base[curve]);
        const float target_length = arc_length(target[curve]);
        if (base_length > 1.0e-20f) {
            const double ratio = target_length / base_length;
            arc_length_ratio.add(ratio);
            arc_length_relative_error.add(std::abs(ratio - 1.0));
        }
    }

    constexpr std::array<double, 7u> relative_edges = {
        0.0, 0.025, 0.05, 0.1, 0.2, 0.4, std::numeric_limits<double>::infinity()};
    std::array<SpatialBin, relative_edges.size() - 1u> spatial{};
    const double root_diagonal = norm(root_max - root_min);
    const std::size_t pair_stride = base.size() > 4096u ? base.size() / 4096u : 1u;
    for (std::size_t a = 0u; a < base.size(); a += pair_stride) {
        for (std::size_t b = a + pair_stride; b < base.size(); b += pair_stride) {
            const double distance = norm(base[a].points.front() - base[b].points.front());
            const double relative_distance = root_diagonal > 0.0 ? distance / root_diagonal : 0.0;
            std::size_t bin = spatial.size() - 1u;
            for (std::size_t i = 0u; i + 1u < relative_edges.size(); ++i) {
                if (relative_distance >= relative_edges[i] &&
                    relative_distance < relative_edges[i + 1u]) {
                    bin = i;
                    break;
                }
            }
            const double a_length = norm(tip_vectors[a]);
            const double b_length = norm(tip_vectors[b]);
            if (a_length > 1.0e-20 && b_length > 1.0e-20) {
                spatial[bin].cosine_sum +=
                    dot(tip_vectors[a], tip_vectors[b]) / (a_length * b_length);
                ++spatial[bin].pairs;
            }
        }
    }

    std::cout << std::setprecision(10) << "{\n"
              << "  \"curves\":" << base.size() << ",\n"
              << "  \"cvs_per_curve\":" << cv_count << ",\n"
              << "  \"metadata_mismatches\":" << metadata_mismatches << ",\n"
              << "  \"displacement_hash\":\"0x" << std::hex << displacement_hash
              << std::dec << "\",\n  ";
    write_stats("root_displacement", root_displacement);
    std::cout << ",\n  ";
    write_stats("tip_displacement", tip_displacement);
    std::cout << ",\n  ";
    write_stats("relative_displacement", relative_displacement);
    std::cout << ",\n  ";
    write_stats("tangential_displacement", tangential_displacement);
    std::cout << ",\n  ";
    write_stats("normal_displacement", normal_displacement);
    std::cout << ",\n  ";
    write_stats("width_absolute_delta", width_absolute_delta);
    std::cout << ",\n  ";
    write_stats("arc_length_ratio", arc_length_ratio);
    std::cout << ",\n  ";
    write_stats("arc_length_relative_error", arc_length_relative_error);
    std::cout << ",\n  \"per_cv_displacement\":[\n";
    for (std::size_t cv = 0u; cv < per_cv.size(); ++cv) {
        std::cout << "    {\"cv\":" << cv << ',';
        write_stats("magnitude", per_cv[cv]);
        std::cout << '}' << (cv + 1u == per_cv.size() ? "\n" : ",\n");
    }
    std::cout << "  ],\n  \"tip_spatial_cosine\":[\n";
    for (std::size_t bin = 0u; bin < spatial.size(); ++bin) {
        const double mean = spatial[bin].pairs == 0u
            ? 0.0 : static_cast<double>(spatial[bin].cosine_sum / spatial[bin].pairs);
        std::cout << "    {\"relative_distance_min\":" << relative_edges[bin]
                  << ",\"relative_distance_max\":";
        if (std::isinf(relative_edges[bin + 1u])) { std::cout << "null"; }
        else { std::cout << relative_edges[bin + 1u]; }
        std::cout << ",\"pairs\":" << spatial[bin].pairs
                  << ",\"mean_cosine\":" << mean << '}'
                  << (bin + 1u == spatial.size() ? "\n" : ",\n");
    }
    std::cout << "  ]\n}\n";
    return metadata_mismatches == 0u ? 0 : 1;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
