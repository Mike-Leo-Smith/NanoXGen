#if __has_include(<xgen/src/xgsculptcore/api/XgSplineAPI.h>)
#include <xgen/src/xgsculptcore/api/XgSplineAPI.h>
#elif __has_include(<XGen/XgSplineAPI.h>)
#include <XGen/XgSplineAPI.h>
#else
#error "Autodesk XgSplineAPI.h was not found"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Summary {
    double minimum{};
    double median{};
    double p90{};
};

struct PackedPoint {
    float x{};
    float y{};
    float z{};
    float radius{};
};

Summary summarize(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    const std::size_t p90 = std::min(samples.size() - 1u,
        static_cast<std::size_t>(std::ceil(samples.size() * 0.9)) - 1u);
    return {samples.front(), samples[samples.size() / 2u], samples[p90]};
}

void write_summary(const char *name, const Summary &summary) {
    std::cout << "\"" << name << "\": {\"min\": " << summary.minimum
              << ", \"median\": " << summary.median
              << ", \"p90\": " << summary.p90 << "}";
}

} // namespace

int main(int argc, char **argv) try {
    std::uint32_t repeats = 7u;
    std::vector<std::filesystem::path> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--repeats") {
            if (++i >= argc) { throw std::invalid_argument("missing repeat count"); }
            repeats = static_cast<std::uint32_t>(std::stoul(argv[i]));
        } else {
            paths.emplace_back(argument);
        }
    }
    if (repeats == 0u || paths.empty()) {
        throw std::invalid_argument("provide BLOB paths and a positive repeat count");
    }
    std::cout << std::setprecision(9) << "{\n  \"repeats\": " << repeats
              << ",\n  \"cases\": [\n";
    for (std::size_t case_index = 0u; case_index < paths.size(); ++case_index) {
        std::ifstream input(paths[case_index], std::ios::binary);
        if (!input) { throw std::runtime_error("failed to open " + paths[case_index].string()); }
        std::stringstream buffer;
        buffer << input.rdbuf();
        const std::string storage = buffer.str();
        std::vector<double> load_samples;
        std::vector<double> execute_samples;
        std::vector<double> iterate_samples;
        std::vector<double> materialize_samples;
        std::vector<double> accessor_samples;
        std::vector<double> copy_samples;
        std::uint64_t curves = 0u;
        std::uint64_t vertices = 0u;
        std::uint64_t batches = 0u;
        double checksum = 0.0;
        for (std::uint32_t repeat = 0u; repeat < repeats; ++repeat) {
            XGenSplineAPI::XgFnSpline splines;
            std::stringstream stream{storage};
            auto begin = std::chrono::steady_clock::now();
            const bool loaded = splines.load(stream, storage.size(), 0.0f);
            auto end = std::chrono::steady_clock::now();
            load_samples.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
            if (!loaded) { throw std::runtime_error("XgFnSpline::load failed"); }

            begin = std::chrono::steady_clock::now();
            const bool executed = splines.executeScript();
            end = std::chrono::steady_clock::now();
            execute_samples.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
            if (!executed) { throw std::runtime_error("XgFnSpline::executeScript failed"); }

            std::uint64_t current_curves = 0u;
            std::uint64_t current_vertices = 0u;
            std::uint64_t current_batches = 0u;
            begin = std::chrono::steady_clock::now();
            for (auto it = splines.iterator(); !it.isDone(); it.next()) {
                ++current_batches;
                current_curves += it.primitiveCount();
                current_vertices += it.vertexCount();
            }
            end = std::chrono::steady_clock::now();
            iterate_samples.push_back(std::chrono::duration<double, std::milli>(end - begin).count());

            std::vector<std::uint32_t> point_counts;
            std::vector<PackedPoint> materialized;
            begin = std::chrono::steady_clock::now();
            point_counts.reserve(current_curves);
            materialized.reserve(current_vertices);
            double accessor_ms = 0.0;
            double copy_ms = 0.0;
            for (auto it = splines.iterator(); !it.isDone(); it.next()) {
                const auto accessor_begin = std::chrono::steady_clock::now();
                const unsigned int stride = it.primitiveInfoStride();
                const unsigned int *infos = it.primitiveInfos();
                const SgVec3f *points = it.positions();
                const float *widths = it.width();
                const unsigned int primitive_count = it.primitiveCount();
                const unsigned int vertex_count = it.vertexCount();
                const auto accessor_end = std::chrono::steady_clock::now();
                accessor_ms += std::chrono::duration<double, std::milli>(
                    accessor_end - accessor_begin).count();
                if (stride < 2u || !infos || !points || !widths) {
                    throw std::runtime_error("XGen returned incomplete materialization data");
                }
                const auto copy_begin = std::chrono::steady_clock::now();
                for (unsigned int primitive = 0u; primitive < primitive_count; ++primitive) {
                    const unsigned int offset = infos[primitive * stride];
                    const unsigned int length = infos[primitive * stride + 1u];
                    if (offset > vertex_count || length > vertex_count - offset) {
                        throw std::runtime_error("XGen returned an invalid primitive range");
                    }
                    point_counts.push_back(length);
                    for (unsigned int vertex = offset; vertex < offset + length; ++vertex) {
                        materialized.push_back({points[vertex][0], points[vertex][1],
                                                points[vertex][2], 0.5f * widths[vertex]});
                    }
                }
                const auto copy_end = std::chrono::steady_clock::now();
                copy_ms += std::chrono::duration<double, std::milli>(
                    copy_end - copy_begin).count();
            }
            end = std::chrono::steady_clock::now();
            materialize_samples.push_back(
                std::chrono::duration<double, std::milli>(end - begin).count());
            accessor_samples.push_back(accessor_ms);
            copy_samples.push_back(copy_ms);
            if (point_counts.size() != current_curves || materialized.size() != current_vertices) {
                throw std::runtime_error("XGen materialization topology mismatch");
            }
            checksum += materialized[(repeat * 17u) % materialized.size()].radius;
            if (repeat == 0u) {
                curves = current_curves;
                vertices = current_vertices;
                batches = current_batches;
            } else if (curves != current_curves || vertices != current_vertices ||
                       batches != current_batches) {
                throw std::runtime_error("XGen decode topology changed between repeats");
            }
        }
        std::cout << "    {\"file\": \"" << paths[case_index].filename().string()
                  << "\", \"blob_bytes\": " << storage.size()
                  << ", \"batches\": " << batches
                  << ", \"curves\": " << curves << ", \"vertices\": " << vertices << ", ";
        write_summary("load_ms", summarize(std::move(load_samples)));
        std::cout << ", ";
        write_summary("execute_ms", summarize(std::move(execute_samples)));
        std::cout << ", ";
        write_summary("iterate_ms", summarize(std::move(iterate_samples)));
        std::cout << ", ";
        write_summary("materialize_ms", summarize(std::move(materialize_samples)));
        std::cout << ", ";
        write_summary("accessor_ms", summarize(std::move(accessor_samples)));
        std::cout << ", ";
        write_summary("copy_ms", summarize(std::move(copy_samples)));
        std::cout << ", \"checksum\": " << checksum
                  << "}" << (case_index + 1u == paths.size() ? "\n" : ",\n");
    }
    std::cout << "  ]\n}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
