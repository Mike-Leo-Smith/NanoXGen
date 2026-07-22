#include "nanoxgen/curve_cache.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

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
    float minimum_radius = std::numeric_limits<float>::infinity();
    float maximum_radius{};
    std::uint64_t zero_radius_points{};
    for (std::uint32_t point = 0u; point < header.point_count; ++point) {
        const float radius = view.points()[point].radius;
        minimum_radius = std::min(minimum_radius, radius);
        maximum_radius = std::max(maximum_radius, radius);
        zero_radius_points += radius == 0.0f;
    }
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
              << duplicate_identities
              << ",\"minimum_radius\":" << minimum_radius
              << ",\"maximum_radius\":" << maximum_radius
              << ",\"zero_radius_points\":" << zero_radius_points
              << "}\n";
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
            std::set<Identity> other_identities;
            using CurveRange = std::pair<std::uint64_t, std::uint32_t>;
            std::map<Identity, CurveRange> input_ranges;
            std::map<Identity, CurveRange> compare_ranges;
            std::uint64_t input_point_offset{};
            if (view.face_ids() && view.face_uvs()) {
                for (std::uint32_t strand = 0u;
                     strand < header.strand_count; ++strand) {
                    const nanoxgen::Vec2 uv = view.face_uvs()[strand];
                    const std::uint32_t count = view.point_counts()[strand];
                    input_ranges.emplace(
                        Identity{view.face_ids()[strand],
                                 std::bit_cast<std::uint32_t>(uv.x),
                                 std::bit_cast<std::uint32_t>(uv.y)},
                        CurveRange{input_point_offset, count});
                    input_point_offset += count;
                }
            }
            std::uint64_t compare_point_offset{};
            if (other.face_ids() && other.face_uvs()) {
                for (std::uint32_t strand = 0u;
                     strand < other.header().strand_count; ++strand) {
                    const nanoxgen::Vec2 uv = other.face_uvs()[strand];
                    const Identity identity{
                        other.face_ids()[strand],
                        std::bit_cast<std::uint32_t>(uv.x),
                        std::bit_cast<std::uint32_t>(uv.y)};
                    const std::uint32_t count = other.point_counts()[strand];
                    other_identities.insert(identity);
                    compare_ranges.emplace(
                        identity, CurveRange{compare_point_offset, count});
                    compare_point_offset += count;
                }
            }
            std::vector<Identity> only_input;
            std::vector<Identity> only_compare;
            std::set_difference(
                identities.begin(), identities.end(),
                other_identities.begin(), other_identities.end(),
                std::back_inserter(only_input));
            std::set_difference(
                other_identities.begin(), other_identities.end(),
                identities.begin(), identities.end(),
                std::back_inserter(only_compare));
            std::uint64_t common_curves{};
            std::uint64_t common_points{};
            std::uint64_t common_point_count_mismatches{};
            double common_position_squared_error{};
            double common_radius_squared_error{};
            float common_max_position_error{};
            float common_max_radius_error{};
            for (const auto &[identity, input_range] : input_ranges) {
                const auto found = compare_ranges.find(identity);
                if (found == compare_ranges.end()) { continue; }
                ++common_curves;
                const CurveRange compare_range = found->second;
                if (input_range.second != compare_range.second) {
                    ++common_point_count_mismatches;
                    continue;
                }
                common_points += input_range.second;
                for (std::uint32_t cv = 0u; cv < input_range.second; ++cv) {
                    const nanoxgen::PackedCurvePoint a =
                        view.points()[input_range.first + cv];
                    const nanoxgen::PackedCurvePoint b =
                        other.points()[compare_range.first + cv];
                    const float position_errors[]{
                        std::abs(a.x - b.x), std::abs(a.y - b.y),
                        std::abs(a.z - b.z)};
                    for (const float error : position_errors) {
                        common_position_squared_error +=
                            static_cast<double>(error) * error;
                        common_max_position_error = std::max(
                            common_max_position_error, error);
                    }
                    const float radius_error =
                        std::abs(a.radius - b.radius);
                    common_radius_squared_error +=
                        static_cast<double>(radius_error) * radius_error;
                    common_max_radius_error = std::max(
                        common_max_radius_error, radius_error);
                }
            }
            const double common_position_rms = common_points == 0u
                ? 0.0
                : std::sqrt(common_position_squared_error /
                            (3.0 * static_cast<double>(common_points)));
            const double common_radius_rms = common_points == 0u
                ? 0.0
                : std::sqrt(common_radius_squared_error /
                            static_cast<double>(common_points));
            const auto length_range = [](const nanoxgen::CurveCacheView &curves,
                                         const auto &ranges,
                                         const std::vector<Identity> &values) {
                float minimum = std::numeric_limits<float>::infinity();
                float maximum = 0.0f;
                for (const Identity &identity : values) {
                    const CurveRange range = ranges.at(identity);
                    // Renderer endpoints are not authored CVs and would add
                    // two extrapolated segments to a near-zero cut curve.
                    const std::uint32_t begin = range.second > 2u ? 1u : 0u;
                    const std::uint32_t end = range.second > 2u
                        ? range.second - 1u : range.second;
                    float length = 0.0f;
                    for (std::uint32_t cv = begin + 1u; cv < end; ++cv) {
                        const nanoxgen::PackedCurvePoint a =
                            curves.points()[range.first + cv - 1u];
                        const nanoxgen::PackedCurvePoint b =
                            curves.points()[range.first + cv];
                        const float dx = b.x - a.x;
                        const float dy = b.y - a.y;
                        const float dz = b.z - a.z;
                        length += std::sqrt(dx * dx + dy * dy + dz * dz);
                    }
                    minimum = std::min(minimum, length);
                    maximum = std::max(maximum, length);
                }
                if (values.empty()) { minimum = 0.0f; }
                return std::pair{minimum, maximum};
            };
            const auto input_lengths =
                length_range(view, input_ranges, only_input);
            const auto compare_lengths =
                length_range(other, compare_ranges, only_compare);
            std::cout << "{\"compare\":\"" << compare_path->string()
                      << "\",\"topology_matches\":false"
                      << ",\"only_in_input\":" << only_input.size()
                      << ",\"only_in_compare\":" << only_compare.size()
                      << ",\"common_curves\":" << common_curves
                      << ",\"common_points\":" << common_points
                      << ",\"common_point_count_mismatches\":"
                      << common_point_count_mismatches
                      << ",\"common_max_position_error\":"
                      << common_max_position_error
                      << ",\"common_position_rms_error\":"
                      << common_position_rms
                      << ",\"common_max_radius_error\":"
                      << common_max_radius_error
                      << ",\"common_radius_rms_error\":"
                      << common_radius_rms
                      << ",\"only_in_input_length_min\":"
                      << input_lengths.first
                      << ",\"only_in_input_length_max\":"
                      << input_lengths.second
                      << ",\"only_in_compare_length_min\":"
                      << compare_lengths.first
                      << ",\"only_in_compare_length_max\":"
                      << compare_lengths.second;
            const auto write_identity = [](std::string_view label,
                                           const Identity &identity) {
                std::cout << ",\"" << label << "\":["
                          << std::get<0>(identity) << ','
                          << std::get<1>(identity) << ','
                          << std::get<2>(identity) << ']';
            };
            if (!only_input.empty()) {
                write_identity("first_only_in_input", only_input.front());
            }
            if (!only_compare.empty()) {
                write_identity("first_only_in_compare", only_compare.front());
            }
            const auto write_identities = [](
                std::string_view label,
                const std::vector<Identity> &values) {
                std::cout << ",\"" << label << "\":[";
                const std::size_t count = std::min<std::size_t>(
                    values.size(), 16u);
                for (std::size_t index = 0u; index < count; ++index) {
                    if (index != 0u) { std::cout << ','; }
                    std::cout << '[' << std::get<0>(values[index]) << ','
                              << std::get<1>(values[index]) << ','
                              << std::get<2>(values[index]) << ']';
                }
                std::cout << ']';
            };
            const auto write_face_counts = [](
                std::string_view label,
                const std::vector<Identity> &values) {
                std::map<std::uint32_t, std::uint32_t> face_counts;
                for (const Identity &identity : values) {
                    ++face_counts[std::get<0>(identity)];
                }
                std::cout << ",\"" << label << "\":{";
                std::size_t index = 0u;
                for (const auto &[face, count] : face_counts) {
                    if (index++ != 0u) { std::cout << ','; }
                    std::cout << '\"' << face << "\":" << count;
                }
                std::cout << '}';
            };
            write_identities("input_identity_sample", only_input);
            write_identities("compare_identity_sample", only_compare);
            write_face_counts("input_only_face_counts", only_input);
            write_face_counts("compare_only_face_counts", only_compare);
            std::cout << "}\n";
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
