#include "nanoxgen/curve_cache.h"

#if __has_include(<xgen/src/sggeom/SgCurve.h>)
#include <xgen/src/sggeom/SgCurve.h>
#else
#error "Autodesk SgCurve.h was not found"
#endif

#include <cstdint>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

std::uint32_t parse_u32(const std::string &value, const char *label) {
    std::size_t consumed{};
    const unsigned long parsed = std::stoul(value, &consumed);
    if (consumed != value.size() ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string{label} + " is invalid");
    }
    return static_cast<std::uint32_t>(parsed);
}

double sampled_length(const safevector<SgVec3d> &points,
                      std::uint32_t sample_count) {
    SgVec3d previous;
    SgCurve::eval(points, 0.0, previous);
    double result = 0.0;
    for (std::uint32_t sample = 1u; sample < sample_count; ++sample) {
        SgVec3d current;
        SgCurve::eval(
            points, static_cast<double>(sample) /
                        static_cast<double>(sample_count - 1u), current);
        const SgVec3d delta = current - previous;
        result += std::sqrt(delta.dot(delta));
        previous = current;
    }
    return result;
}

} // namespace

int main(int argc, char **argv) try {
    std::filesystem::path path;
    std::uint32_t strand{};
    std::optional<double> cut;
    for (int index = 1; index < argc; ++index) {
        const std::string argument{argv[index]};
        if ((argument == "--strand" || argument == "--cut") &&
            index + 1 < argc) {
            if (argument == "--strand") {
                strand = parse_u32(argv[++index], "strand");
            } else {
                cut = std::stod(argv[++index]);
            }
        } else if (path.empty()) {
            path = argument;
        } else {
            throw std::invalid_argument(
                "unknown or incomplete argument: " + argument);
        }
    }
    if (path.empty()) {
        throw std::invalid_argument(
            "usage: nanoxgen_xgen_curve_oracle CACHE.nxc "
            "[--strand N] [--cut AMOUNT]");
    }
    const nanoxgen::CurveCache cache = nanoxgen::load_curve_cache(path);
    const nanoxgen::CurveCacheView view = cache.view();
    if (strand >= view.header().strand_count) {
        throw std::out_of_range("strand is out of range");
    }
    std::uint64_t offset{};
    for (std::uint32_t index = 0u; index < strand; ++index) {
        offset += view.point_counts()[index];
    }
    const std::uint32_t renderer_count = view.point_counts()[strand];
    if (renderer_count < 4u) {
        throw std::runtime_error(
            "renderer curve cannot contain two extrapolated endpoints");
    }
    safevector<SgVec3d> points;
    points.reserve(renderer_count - 2u);
    for (std::uint32_t cv = 1u; cv + 1u < renderer_count; ++cv) {
        const nanoxgen::PackedCurvePoint point = view.points()[offset + cv];
        points.emplace_back(point.x, point.y, point.z);
    }
    safevector<double> segment_lengths;
    const double spline_length = SgCurve::length(points);
    const double polygon_length = SgCurve::cpolyLength(
        points, segment_lengths);
    std::cout << std::setprecision(17)
              << "{\"strand\":" << strand
              << ",\"cv_count\":" << points.size()
              << ",\"spline_length\":" << spline_length
              << ",\"polygon_length\":" << polygon_length
              << ",\"sampled_2x\":"
              << sampled_length(points, 2u * points.size())
              << ",\"sampled_4x\":"
              << sampled_length(points, 4u * points.size())
              << ",\"sampled_8x\":"
              << sampled_length(points, 8u * points.size());
    if (cut) {
        const double parameter = SgCurve::cutFromTip(points, *cut, true);
        std::cout << ",\"cut_amount\":" << *cut
                  << ",\"cut_parameter\":" << parameter
                  << ",\"cut_spline_length\":" << SgCurve::length(points);
    }
    std::cout << "}\n";
    if (cut) {
        for (std::size_t cv = 0u; cv < points.size(); ++cv) {
            std::cout << "point " << cv << ' ' << points[cv][0] << ' '
                      << points[cv][1] << ' ' << points[cv][2] << '\n';
        }
    }
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
