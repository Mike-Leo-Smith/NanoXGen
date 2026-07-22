#include "nanoxgen/curve_cache.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {

using Identity = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;

} // namespace

int main(int argc, char **argv) try {
    bool face_counts = false;
    std::uint32_t dump_roots = 0u;
    std::optional<std::uint32_t> dump_face;
    std::optional<Identity> dump_identity;
    std::optional<std::filesystem::path> compare_path;
    std::filesystem::path path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument{argv[index]};
        if (argument == "--face-counts") {
            face_counts = true;
        } else if (argument == "--dump-roots") {
            if (++index >= argc) { throw std::runtime_error("missing root count"); }
            dump_roots = static_cast<std::uint32_t>(std::stoul(argv[index]));
        } else if (argument == "--dump-face") {
            if (++index >= argc) { throw std::runtime_error("missing face id"); }
            dump_face = static_cast<std::uint32_t>(std::stoul(argv[index]));
        } else if (argument == "--dump-identity") {
            if (++index >= argc) {
                throw std::runtime_error("missing face,u,v identity");
            }
            const std::string value{argv[index]};
            const std::size_t first = value.find(',');
            const std::size_t second = value.find(',', first + 1u);
            if (first == std::string::npos || second == std::string::npos ||
                value.find(',', second + 1u) != std::string::npos) {
                throw std::runtime_error("identity must be FACE,U,V");
            }
            const std::uint32_t face = static_cast<std::uint32_t>(
                std::stoul(value.substr(0u, first)));
            const float u = std::stof(
                value.substr(first + 1u, second - first - 1u));
            const float v = std::stof(value.substr(second + 1u));
            dump_identity = Identity{face, std::bit_cast<std::uint32_t>(u),
                                     std::bit_cast<std::uint32_t>(v)};
        } else if (argument == "--compare") {
            if (++index >= argc) {
                throw std::runtime_error("missing comparison cache");
            }
            compare_path = argv[index];
        } else if (path.empty()) {
            path = argument;
        } else {
            throw std::runtime_error("unexpected argument: " + argument);
        }
    }
    if (path.empty()) {
        throw std::runtime_error(
            "usage: nanoxgen_curve_cache_inspect [--face-counts] "
            "[--dump-roots N] [--dump-face ID] "
            "[--dump-identity FACE,U,V] [--compare CACHE.nxc] CACHE.nxc");
    }
    const nanoxgen::CurveCache cache = nanoxgen::load_curve_cache(path);
    const nanoxgen::CurveCacheView view = cache.view();
    const nanoxgen::CurveCacheHeader &header = view.header();
    std::map<std::uint32_t, std::uint32_t> counts;
    std::set<Identity> identities;
    std::uint64_t duplicate_identities{};
    if (view.face_ids() && view.face_uvs()) {
        for (std::uint32_t strand = 0u; strand < header.strand_count; ++strand) {
            ++counts[view.face_ids()[strand]];
            const nanoxgen::Vec2 uv = view.face_uvs()[strand];
            const Identity identity{view.face_ids()[strand],
                                    std::bit_cast<std::uint32_t>(uv.x),
                                    std::bit_cast<std::uint32_t>(uv.y)};
            duplicate_identities += identities.insert(identity).second ? 0u : 1u;
        }
    }
    std::cout << std::setprecision(9) << "{\"bytes\":" << header.byte_size
              << ",\"strands\":" << header.strand_count
              << ",\"points\":" << header.point_count
              << ",\"faces\":" << counts.size()
              << ",\"duplicate_face_uv_identities\":"
              << duplicate_identities << "}\n";
    if (compare_path) {
        const nanoxgen::CurveCache other_cache =
            nanoxgen::load_curve_cache(*compare_path);
        const nanoxgen::CurveCacheView other = other_cache.view();
        const bool topology_matches =
            header.strand_count == other.header().strand_count &&
            header.point_count == other.header().point_count &&
            std::equal(view.point_counts(),
                       view.point_counts() + header.strand_count,
                       other.point_counts());
        if (!topology_matches) {
            std::cout << "{\"compare\":\"" << compare_path->string()
                      << "\",\"topology_matches\":false}\n";
        } else {
            double position_squared_error{};
            double radius_squared_error{};
            float max_position_error{};
            float max_radius_error{};
            std::uint64_t bit_mismatches{};
            std::uint64_t max_position_point{};
            std::uint32_t max_position_axis{};
            for (std::uint64_t point = 0u; point < header.point_count; ++point) {
                const nanoxgen::PackedCurvePoint a = view.points()[point];
                const nanoxgen::PackedCurvePoint b = other.points()[point];
                const float values_a[4]{a.x, a.y, a.z, a.radius};
                const float values_b[4]{b.x, b.y, b.z, b.radius};
                for (std::uint32_t axis = 0u; axis < 3u; ++axis) {
                    const float error = std::abs(values_a[axis] - values_b[axis]);
                    position_squared_error += static_cast<double>(error) * error;
                    if (error > max_position_error) {
                        max_position_error = error;
                        max_position_point = point;
                        max_position_axis = axis;
                    }
                }
                const float radius_error = std::abs(a.radius - b.radius);
                radius_squared_error +=
                    static_cast<double>(radius_error) * radius_error;
                max_radius_error = std::max(max_radius_error, radius_error);
                for (std::uint32_t component = 0u; component < 4u; ++component) {
                    bit_mismatches +=
                        std::bit_cast<std::uint32_t>(values_a[component]) !=
                        std::bit_cast<std::uint32_t>(values_b[component]);
                }
            }
            const double position_rms = header.point_count == 0u
                ? 0.0
                : std::sqrt(position_squared_error /
                            (3.0 * header.point_count));
            const double radius_rms = header.point_count == 0u
                ? 0.0
                : std::sqrt(radius_squared_error / header.point_count);
            std::cout << "{\"compare\":\"" << compare_path->string()
                      << "\",\"topology_matches\":true"
                      << ",\"max_position_error\":" << max_position_error
                      << ",\"position_rms_error\":" << position_rms
                      << ",\"max_radius_error\":" << max_radius_error
                      << ",\"radius_rms_error\":" << radius_rms
                      << ",\"bit_mismatches\":" << bit_mismatches
                      << ",\"max_position_point\":" << max_position_point
                      << ",\"max_position_axis\":" << max_position_axis
                      << "}\n";
        }
    }
    if (face_counts) {
        for (const auto &[face, count] : counts) {
            std::cout << "face " << face << ' ' << count << '\n';
        }
    }
    if (dump_roots != 0u && view.face_ids() && view.face_uvs()) {
        std::uint32_t emitted = 0u;
        for (std::uint32_t strand = 0u;
             strand < header.strand_count && emitted < dump_roots; ++strand) {
            if (dump_face && view.face_ids()[strand] != *dump_face) { continue; }
            const nanoxgen::Vec2 uv = view.face_uvs()[strand];
            std::cout << "root " << strand << ' ' << view.face_ids()[strand]
                      << ' ' << uv.x << ' ' << uv.y << '\n';
            ++emitted;
        }
    }
    if (dump_identity) {
        if (!view.face_ids() || !view.face_uvs()) {
            throw std::runtime_error(
                "cache does not contain face identities");
        }
        std::uint64_t point_offset = 0u;
        bool found = false;
        for (std::uint32_t strand = 0u; strand < header.strand_count;
             ++strand) {
            const nanoxgen::Vec2 uv = view.face_uvs()[strand];
            const Identity identity{view.face_ids()[strand],
                                    std::bit_cast<std::uint32_t>(uv.x),
                                    std::bit_cast<std::uint32_t>(uv.y)};
            const std::uint32_t point_count = view.point_counts()[strand];
            if (identity == *dump_identity) {
                std::cout << "strand " << strand << " points "
                          << point_count << '\n';
                for (std::uint32_t cv = 0u; cv < point_count; ++cv) {
                    const nanoxgen::PackedCurvePoint point =
                        view.points()[point_offset + cv];
                    std::cout << "point " << cv << ' ' << point.x << ' '
                              << point.y << ' ' << point.z << ' '
                              << point.radius << '\n';
                }
                found = true;
                break;
            }
            point_offset += point_count;
        }
        if (!found) {
            throw std::runtime_error("curve identity was not found");
        }
    }
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
