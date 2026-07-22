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
        const std::size_t index = static_cast<std::size_t>(
            fraction * static_cast<double>(sorted.size() - 1u));
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
    Samples parse;
    Samples source_packed;
    Samples canonical_packed;
    Samples full_canonical;
    Samples end_to_end;
    std::uint64_t observed_checksum = 0u;
    std::size_t curve_count = 0u;
    std::size_t point_count = 0u;
    for (std::size_t iteration = 0u; iteration < warmup + iterations; ++iteration) {
        const auto file_begin = Clock::now();
        const std::vector<std::byte> file_bytes = read_bytes(path);
        const auto file_end = Clock::now();

        const auto parse_begin = Clock::now();
        const XGenDocument document = parse_xgen_document(resident);
        const auto parse_end = Clock::now();

        const auto source_begin = Clock::now();
        const XGenPackedCurves source = materialize_xgen_packed_curves(
            document, XGenCurveOrder::Source);
        const auto source_end = Clock::now();

        const auto canonical_begin = Clock::now();
        const XGenPackedCurves canonical = materialize_xgen_packed_curves(
            document, XGenCurveOrder::Canonical);
        const auto canonical_end = Clock::now();

        const auto full_begin = Clock::now();
        const XGenEvaluatedCurves full = materialize_xgen_curves(
            document, XGenCurveOrder::Canonical);
        const auto full_end = Clock::now();

        const auto e2e_begin = Clock::now();
        const XGenPackedCurves loaded = load_xgen_packed_curves(
            path, XGenCurveOrder::Source);
        const auto e2e_end = Clock::now();

        const auto mix_checksum = [&](std::uint64_t value) {
            observed_checksum ^= value;
            observed_checksum *= 0x100000001b3ull;
        };
        mix_checksum(checksum(source));
        mix_checksum(checksum(canonical));
        mix_checksum(checksum(loaded));
        mix_checksum(static_cast<std::uint64_t>(full.positions.size()));
        mix_checksum(file_bytes.size());
        curve_count = source.point_counts.size();
        point_count = source.points.size();
        if (iteration >= warmup) {
            file_read.add(milliseconds(file_begin, file_end));
            parse.add(milliseconds(parse_begin, parse_end));
            source_packed.add(milliseconds(source_begin, source_end));
            canonical_packed.add(milliseconds(canonical_begin, canonical_end));
            full_canonical.add(milliseconds(full_begin, full_end));
            end_to_end.add(milliseconds(e2e_begin, e2e_end));
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
              << ",\"iterations\":" << iterations
              << ",\"checksum\":\"0x" << std::hex << observed_checksum << std::dec
              << "\",";
    emit("file_read", file_read, true);
    emit("resident_document_parse", parse, true);
    emit("source_packed", source_packed, true);
    emit("canonical_packed", canonical_packed, true);
    emit("full_canonical", full_canonical, true);
    emit("source_end_to_end", end_to_end, false);
    std::cout << "}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
