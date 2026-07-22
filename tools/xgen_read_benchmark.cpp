#include "nanoxgen/xgen.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nanoxgen;

namespace {

using Clock = std::chrono::steady_clock;

std::vector<std::byte> read_bytes(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) { throw std::runtime_error("cannot open input"); }
    const std::streamoff size = input.tellg();
    if (size < 0 || static_cast<std::uintmax_t>(size) >
                        std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("cannot address input size");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) { throw std::runtime_error("cannot read input"); }
    return bytes;
}

double milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

struct Samples {
    std::vector<double> values;
    void add(double value) { values.push_back(value); }
    double percentile(double fraction) const {
        std::vector<double> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t index = std::min(
            sorted.size() - 1u,
            static_cast<std::size_t>(std::ceil(
                fraction * static_cast<double>(sorted.size()))) - 1u);
        return sorted[index];
    }
};

std::uint64_t checksum(const XGenPackedCurves &curves) {
    std::uint64_t value = curves.point_counts.size() * 0x9e3779b185ebca87ull;
    if (!curves.points.empty()) {
        const std::size_t stride = std::max<std::size_t>(1u, curves.points.size() / 1024u);
        for (std::size_t index = 0u; index < curves.points.size(); index += stride) {
            value ^= std::bit_cast<std::uint32_t>(curves.points[index].x) +
                (static_cast<std::uint64_t>(
                     std::bit_cast<std::uint32_t>(curves.points[index].radius)) << 32u);
        }
    }
    return value;
}

std::uint64_t checksum(const XGenEvaluatedCurves &curves) {
    std::uint64_t value = curves.point_counts.size() * 0x9e3779b185ebca87ull;
    if (!curves.positions.empty()) {
        const std::size_t stride = std::max<std::size_t>(
            1u, curves.positions.size() / 1024u);
        for (std::size_t index = 0u; index < curves.positions.size(); index += stride) {
            value ^= std::bit_cast<std::uint32_t>(curves.positions[index].x) +
                (static_cast<std::uint64_t>(
                     std::bit_cast<std::uint32_t>(curves.widths[index])) << 32u);
        }
    }
    return value;
}

} // namespace

int main(int argc, char **argv) try {
    std::size_t warmup = 3u;
    std::size_t iterations = 15u;
    std::filesystem::path path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--warmup" && index + 1 < argc) {
            warmup = std::stoul(argv[++index]);
        } else if (argument == "--iterations" && index + 1 < argc) {
            iterations = std::stoul(argv[++index]);
        } else if (path.empty()) {
            path = argument;
        } else {
            throw std::invalid_argument("unexpected argument: " + argument);
        }
    }
    if (path.empty() || iterations == 0u) {
        std::cerr << "usage: nanoxgen_xgen_read_benchmark [--warmup N] "
                     "[--iterations N] <snapshot.xgen>\n";
        return 2;
    }
    const std::vector<std::byte> resident = read_bytes(path);
    Samples file_read;
    Samples resident_document_parse;
    Samples resident_source_packed;
    Samples resident_canonical_packed;
    Samples resident_full_canonical;
    Samples hot_file_source_packed;
    Samples hot_file_canonical_packed;
    Samples hot_file_full_canonical;
    std::uint64_t observed_checksum = 0u;
    std::size_t curve_count = 0u;
    std::size_t point_count = 0u;
    for (std::size_t iteration = 0u; iteration < warmup + iterations; ++iteration) {
        const auto file_begin = Clock::now();
        const std::vector<std::byte> file_bytes = read_bytes(path);
        const auto file_end = Clock::now();

        const auto document_begin = Clock::now();
        const XGenDocument document = parse_xgen_document(resident);
        const auto document_end = Clock::now();
        const std::size_t document_group_count = document.groups.size();

        const auto source_begin = Clock::now();
        const XGenPackedCurves source = parse_xgen_packed_curves(
            resident, XGenCurveOrder::Source);
        const auto source_end = Clock::now();

        const auto canonical_begin = Clock::now();
        const XGenPackedCurves canonical = parse_xgen_packed_curves(
            resident, XGenCurveOrder::Canonical);
        const auto canonical_end = Clock::now();

        const auto full_begin = Clock::now();
        const XGenEvaluatedCurves full = materialize_xgen_curves(
            parse_xgen_document(resident), XGenCurveOrder::Canonical);
        const auto full_end = Clock::now();

        const auto hot_source_begin = Clock::now();
        const XGenPackedCurves hot_source = load_xgen_packed_curves(
            path, XGenCurveOrder::Source);
        const auto hot_source_end = Clock::now();

        const auto hot_canonical_begin = Clock::now();
        const XGenPackedCurves hot_canonical = load_xgen_packed_curves(
            path, XGenCurveOrder::Canonical);
        const auto hot_canonical_end = Clock::now();

        const auto hot_full_begin = Clock::now();
        const XGenEvaluatedCurves hot_full = materialize_xgen_curves(
            load_xgen_document(path), XGenCurveOrder::Canonical);
        const auto hot_full_end = Clock::now();

        const auto mix_checksum = [&](std::uint64_t value) {
            observed_checksum ^= value;
            observed_checksum *= 0x100000001b3ull;
        };
        mix_checksum(checksum(source));
        mix_checksum(checksum(canonical));
        mix_checksum(checksum(full));
        mix_checksum(checksum(hot_source));
        mix_checksum(checksum(hot_canonical));
        mix_checksum(checksum(hot_full));
        mix_checksum(file_bytes.size());
        mix_checksum(document_group_count);
        curve_count = source.point_counts.size();
        point_count = source.points.size();
        if (iteration >= warmup) {
            file_read.add(milliseconds(file_begin, file_end));
            resident_document_parse.add(milliseconds(document_begin, document_end));
            resident_source_packed.add(milliseconds(source_begin, source_end));
            resident_canonical_packed.add(
                milliseconds(canonical_begin, canonical_end));
            resident_full_canonical.add(milliseconds(full_begin, full_end));
            hot_file_source_packed.add(
                milliseconds(hot_source_begin, hot_source_end));
            hot_file_canonical_packed.add(
                milliseconds(hot_canonical_begin, hot_canonical_end));
            hot_file_full_canonical.add(
                milliseconds(hot_full_begin, hot_full_end));
        }
    }
    const auto emit = [](const char *name, const Samples &samples, bool comma) {
        std::cout << "\"" << name << "\":{\"median_ms\":"
                  << samples.percentile(0.5) << ",\"p90_ms\":"
                  << samples.percentile(0.9) << "}" << (comma ? "," : "");
    };
    std::cout << std::fixed << std::setprecision(3)
              << "{\"file_bytes\":" << resident.size()
              << ",\"curves\":" << curve_count
              << ",\"points\":" << point_count
              << ",\"warmup\":" << warmup
              << ",\"iterations\":" << iterations
              << ",\"resident_includes_file_io\":false"
              << ",\"hot_file_includes_file_io\":true"
              << ",\"autodesk_serialization_included\":false"
              << ",\"packed_channels\":\"pointCounts+position+radius\""
              << ",\"full_channels\":\"packed+texcoord+patchUV+faceUV+faceId\""
              << ",\"checksum\":\"0x" << std::hex << observed_checksum << std::dec
              << "\",";
    emit("file_read", file_read, true);
    emit("resident_lossless_document_parse", resident_document_parse, true);
    emit("resident_source_packed", resident_source_packed, true);
    emit("resident_canonical_packed", resident_canonical_packed, true);
    emit("resident_full_canonical", resident_full_canonical, true);
    emit("hot_file_source_packed", hot_file_source_packed, true);
    emit("hot_file_canonical_packed", hot_file_canonical_packed, true);
    emit("hot_file_full_canonical", hot_file_full_canonical, false);
    std::cout << "}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
