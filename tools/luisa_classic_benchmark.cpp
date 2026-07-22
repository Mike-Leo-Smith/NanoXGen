#include "nanoxgen/asset.h"
#include "nanoxgen/hip.h"
#include "nanoxgen/luisa/xgen_classic_runtime.h"
#include "nanoxgen/xgen_classic.h"
#include "nanoxgen/xgen_classic_runtime.h"

#include <hip/hip_runtime_api.h>
#include <luisa/runtime/context.h>
#include <luisa/runtime/device.h>
#include <luisa/runtime/stream.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Float4Buffer = luisa::compute::Buffer<luisa::float4>;
using PrimitiveShader = luisa::compute::Shader1D<
    Float4Buffer, Float4Buffer, luisa::compute::ByteBuffer, Float4Buffer>;
using CutShader = PrimitiveShader;
using WidthShader = luisa::compute::Shader1D<
    Float4Buffer, luisa::compute::ByteBuffer, Float4Buffer>;

void check_hip(hipError_t error, const char *operation) {
    if (error != hipSuccess) {
        throw std::runtime_error(
            std::string{operation} + ": " + hipGetErrorString(error));
    }
}

std::uint32_t parse_u32(std::string_view text, const char *label) {
    std::size_t consumed = 0u;
    const unsigned long value = std::stoul(std::string{text}, &consumed);
    if (consumed != text.size() || value == 0u || value > 0xfffffffful) {
        throw std::invalid_argument(std::string{label} + " is invalid");
    }
    return static_cast<std::uint32_t>(value);
}

struct CaseSpec {
    std::string description;
    std::filesystem::path asset;
    std::uint32_t strands{};
    std::uint32_t cvs{};
};

CaseSpec parse_case(std::string_view value) {
    std::vector<std::string_view> fields;
    std::size_t begin = 0u;
    for (;;) {
        const std::size_t separator = value.find(',', begin);
        fields.emplace_back(value.substr(begin, separator - begin));
        if (separator == std::string_view::npos) { break; }
        begin = separator + 1u;
    }
    if (fields.size() != 4u || fields[0].empty() || fields[1].empty()) {
        throw std::invalid_argument(
            "--case must be DESCRIPTION,ASSET.nxg,STRANDS,CVS");
    }
    CaseSpec result{};
    result.description = fields[0];
    result.asset = fields[1];
    result.strands = parse_u32(fields[2], "case strand count");
    result.cvs = parse_u32(fields[3], "case CV count");
    if (result.cvs < 2u) {
        throw std::invalid_argument("case CV count must be at least two");
    }
    return result;
}

struct Options {
    std::filesystem::path runtime_dir;
    std::string backend;
    std::filesystem::path collection;
    std::uint32_t warmup{3u};
    std::uint32_t repeats{15u};
    std::uint32_t cpu_repeats{};
    std::uint32_t block_size{128u};
    std::vector<CaseSpec> cases;
};

Options parse_options(int argc, char **argv) {
    if (argc < 6) {
        throw std::invalid_argument(
            "usage: nanoxgen_luisa_classic_benchmark RUNTIME_DIR BACKEND "
            "COLLECTION.xgen --case DESCRIPTION,ASSET.nxg,STRANDS,CVS "
            "[--case ...] [--warmup N] [--repeats N] [--cpu-repeats N] "
            "[--block-size N]");
    }
    Options options{};
    options.runtime_dir = argv[1];
    options.backend = argv[2];
    options.collection = argv[3];
    for (int i = 4; i < argc; ++i) {
        const std::string argument = argv[i];
        if (i + 1 >= argc) {
            throw std::invalid_argument("missing value after " + argument);
        }
        const std::string_view value = argv[++i];
        if (argument == "--case") {
            options.cases.emplace_back(parse_case(value));
        } else if (argument == "--warmup") {
            options.warmup = parse_u32(value, "warmup count");
        } else if (argument == "--repeats") {
            options.repeats = parse_u32(value, "repeat count");
        } else if (argument == "--cpu-repeats") {
            options.cpu_repeats = parse_u32(value, "CPU repeat count");
        } else if (argument == "--block-size") {
            options.block_size = parse_u32(value, "block size");
            if (options.block_size > 1024u) {
                throw std::invalid_argument("block size must not exceed 1024");
            }
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (options.cases.empty()) {
        throw std::invalid_argument("at least one --case is required");
    }
    return options;
}

class HipAllocation {
public:
    explicit HipAllocation(std::size_t bytes) : _bytes{bytes} {
        if (bytes != 0u) { check_hip(hipMalloc(&_data, bytes), "hipMalloc"); }
    }
    HipAllocation(const HipAllocation &) = delete;
    HipAllocation &operator=(const HipAllocation &) = delete;
    ~HipAllocation() {
        if (_data != nullptr) { (void)hipFree(_data); }
    }
    [[nodiscard]] void *data() const noexcept { return _data; }
    [[nodiscard]] std::size_t bytes() const noexcept { return _bytes; }
private:
    void *_data{};
    std::size_t _bytes{};
};

class HipEvent {
public:
    HipEvent() { check_hip(hipEventCreate(&_event), "hipEventCreate"); }
    HipEvent(const HipEvent &) = delete;
    HipEvent &operator=(const HipEvent &) = delete;
    ~HipEvent() { (void)hipEventDestroy(_event); }
    [[nodiscard]] operator hipEvent_t() const noexcept { return _event; }
private:
    hipEvent_t _event{};
};

std::size_t checked_points(const CaseSpec &spec) {
    const std::uint64_t count =
        static_cast<std::uint64_t>(spec.strands) * spec.cvs;
    if (count > std::numeric_limits<std::size_t>::max() /
                    sizeof(nanoxgen::PackedCurvePoint)) {
        throw std::overflow_error("case point allocation is too large");
    }
    return static_cast<std::size_t>(count);
}

struct RuntimeCase {
    CaseSpec spec;
    nanoxgen::Asset asset;
    nanoxgen::ClassicFloatRuntimePlan plan;
    HipAllocation asset_bytes;
    HipAllocation points_a;
    HipAllocation points_b;
    HipAllocation roots;
    Float4Buffer a;
    Float4Buffer b;
    luisa::compute::ByteBuffer root_bytes;
    Float4Buffer states;
    PrimitiveShader primitive;
    std::vector<CutShader> cuts;
    WidthShader width;
    nanoxgen::DeviceAssetDescriptor asset_descriptor;
    nanoxgen::DevicePackedCurveOutputDescriptor output;
    bool final_is_a{};

    RuntimeCase(const CaseSpec &case_spec,
                const nanoxgen::ClassicCollection &collection,
                luisa::compute::Device &device)
        : spec{case_spec}, asset{nanoxgen::load_asset(spec.asset)},
          plan{[&] {
              const auto *description = nanoxgen::find_classic_description(
                  collection, spec.description);
              if (description == nullptr) {
                  throw std::runtime_error(
                      "Classic description not found: " + spec.description);
              }
              return nanoxgen::compile_xgen_classic_float_runtime_plan(
                  *description);
          }()},
          asset_bytes{asset.bytes().size()},
          points_a{checked_points(spec) * sizeof(nanoxgen::PackedCurvePoint)},
          points_b{checked_points(spec) * sizeof(nanoxgen::PackedCurvePoint)},
          roots{static_cast<std::size_t>(spec.strands) *
                sizeof(nanoxgen::RootSample)},
          a{device.import_external_buffer<luisa::float4>(
              points_a.data(), checked_points(spec))},
          b{device.import_external_buffer<luisa::float4>(
              points_b.data(), checked_points(spec))},
          root_bytes{device.import_external_byte_buffer(
              roots.data(), roots.bytes())},
          states{device.create_buffer<luisa::float4>(spec.strands)},
          primitive{device.compile(
              nanoxgen::luisa_backend::make_classic_runtime_primitive_kernel(
                  plan, spec.cvs))},
          width{device.compile(
              nanoxgen::luisa_backend::make_classic_runtime_width_kernel(
                  plan, spec.cvs))} {
        check_hip(hipMemcpy(asset_bytes.data(), asset.bytes().data(),
                            asset.bytes().size(), hipMemcpyHostToDevice),
                  "upload Classic benchmark asset");
        asset_descriptor = nanoxgen::make_device_asset_descriptor(
            asset, asset_bytes.data(), asset_bytes.bytes());
        output = {{static_cast<nanoxgen::PackedCurvePoint *>(points_a.data()),
                   static_cast<nanoxgen::RootSample *>(roots.data()),
                   nullptr, 1.0f, nullptr},
                  checked_points(spec), spec.strands, 0u, 0u};
        cuts.reserve(plan.cuts.size());
        for (const auto &cut : plan.cuts) {
            cuts.emplace_back(device.compile(
                nanoxgen::luisa_backend::make_classic_runtime_cut_kernel(
                    plan, cut, spec.cvs)));
        }
        final_is_a = (cuts.size() % 2u) != 0u;
    }

    [[nodiscard]] const HipAllocation &final_points() const noexcept {
        return final_is_a ? points_a : points_b;
    }
};

void dispatch_case(RuntimeCase &runtime, luisa::compute::Stream &stream,
                   hipStream_t hip_stream, std::uint32_t block_size) {
    nanoxgen::GenerationParams params{};
    params.strand_count = runtime.spec.strands;
    params.cvs_per_strand = runtime.spec.cvs;
    params.seed = 0x13579bdu;
    check_hip(nanoxgen::launch_generate_packed_hip(
                  runtime.asset_descriptor, {}, params, runtime.output,
                  {block_size}, hip_stream),
              "launch Classic benchmark generation");
    stream << runtime.primitive(runtime.a, runtime.b, runtime.root_bytes,
                                runtime.states)
                  .dispatch(runtime.spec.strands);
    bool source_is_a = false;
    for (auto &cut : runtime.cuts) {
        if (source_is_a) {
            stream << cut(runtime.a, runtime.b, runtime.root_bytes,
                          runtime.states)
                          .dispatch(runtime.spec.strands);
        } else {
            stream << cut(runtime.b, runtime.a, runtime.root_bytes,
                          runtime.states)
                          .dispatch(runtime.spec.strands);
        }
        source_is_a = !source_is_a;
    }
    if (source_is_a) {
        stream << runtime.width(runtime.a, runtime.root_bytes, runtime.states)
                      .dispatch(runtime.spec.strands);
    } else {
        stream << runtime.width(runtime.b, runtime.root_bytes, runtime.states)
                      .dispatch(runtime.spec.strands);
    }
}

double percentile(const std::vector<double> &sorted, double fraction) {
    return sorted[static_cast<std::size_t>(
        fraction * static_cast<double>(sorted.size() - 1u))];
}

std::uint64_t checksum(RuntimeCase &runtime) {
    const std::size_t point_count = checked_points(runtime.spec);
    std::vector<nanoxgen::PackedCurvePoint> points(point_count);
    check_hip(hipMemcpy(points.data(), runtime.final_points().data(),
                        runtime.final_points().bytes(), hipMemcpyDeviceToHost),
              "download benchmark checksum points");
    std::uint64_t result = 1469598103934665603ull;
    const auto mix = [&](float value) {
        result ^= std::bit_cast<std::uint32_t>(value);
        result *= 1099511628211ull;
    };
    for (const auto &point : points) {
        mix(point.x); mix(point.y); mix(point.z); mix(point.radius);
    }
    return result;
}

std::uint64_t benchmark_cpu_once(
    const std::vector<std::unique_ptr<RuntimeCase>> &cases) {
    std::uint64_t result = 1469598103934665603ull;
    for (const auto &runtime : cases) {
        nanoxgen::GenerationParams params{};
        params.strand_count = runtime->spec.strands;
        params.cvs_per_strand = runtime->spec.cvs;
        params.seed = 0x13579bdu;
        auto curves = nanoxgen::generate_packed_cpu(
            runtime->asset, params, 1.0f, {});
        nanoxgen::apply_xgen_classic_float_runtime_plan_cpu(
            curves, runtime->plan, 1.0f);
        const auto &point = curves.points[
            (static_cast<std::size_t>(runtime->spec.strands) * 17u) %
            curves.points.size()];
        result ^= std::bit_cast<std::uint32_t>(point.x);
        result *= 1099511628211ull;
        result ^= std::bit_cast<std::uint32_t>(point.radius);
        result *= 1099511628211ull;
    }
    return result;
}

struct DifferentialSummary {
    long double squared_error{};
    std::uint64_t component_count{};
    float maximum{};
    std::uint64_t above_1e5{};
    std::uint64_t above_1e4{};

    void add(float expected, float actual) {
        if (!std::isfinite(expected) || !std::isfinite(actual)) {
            throw std::runtime_error(
                "CPU/GPU Classic differential found a non-finite value");
        }
        const float error = std::abs(expected - actual);
        maximum = std::max(maximum, error);
        above_1e5 += error > 1.0e-5f ? 1u : 0u;
        above_1e4 += error > 1.0e-4f ? 1u : 0u;
        squared_error += static_cast<long double>(error) * error;
        ++component_count;
    }

    [[nodiscard]] double rms() const {
        return component_count == 0u ? 0.0 : std::sqrt(
            static_cast<double>(squared_error / component_count));
    }
};

struct CaseDifferential {
    std::string description;
    DifferentialSummary root_position;
    DifferentialSummary root_normal;
    DifferentialSummary root_uv;
    std::uint64_t root_triangle_mismatches{};
    DifferentialSummary base_position;
    DifferentialSummary base_radius;
    DifferentialSummary final_position;
    DifferentialSummary final_radius;
};

struct DifferentialReport {
    DifferentialSummary final;
    std::vector<CaseDifferential> cases;
};

void add_point(DifferentialSummary &position,
               DifferentialSummary &radius,
               const nanoxgen::PackedCurvePoint &expected,
               const nanoxgen::PackedCurvePoint &actual) {
    position.add(expected.x, actual.x);
    position.add(expected.y, actual.y);
    position.add(expected.z, actual.z);
    radius.add(expected.radius, actual.radius);
}

DifferentialReport validate_cpu_gpu(
    const std::vector<std::unique_ptr<RuntimeCase>> &cases,
    luisa::compute::Stream &stream, hipStream_t hip_stream,
    std::uint32_t block_size) {
    DifferentialReport report{};
    report.cases.reserve(cases.size());
    for (const auto &runtime : cases) {
        nanoxgen::GenerationParams params{};
        params.strand_count = runtime->spec.strands;
        params.cvs_per_strand = runtime->spec.cvs;
        params.seed = 0x13579bdu;
        auto expected = nanoxgen::generate_packed_cpu(
            runtime->asset, params, 1.0f, {});
        check_hip(nanoxgen::launch_generate_packed_hip(
                      runtime->asset_descriptor, {}, params, runtime->output,
                      {block_size}, hip_stream),
                  "launch base CPU/GPU differential");
        check_hip(hipStreamSynchronize(hip_stream),
                  "synchronize base CPU/GPU differential");
        std::vector<nanoxgen::PackedCurvePoint> actual(expected.points.size());
        check_hip(hipMemcpy(actual.data(), runtime->points_a.data(),
                            runtime->points_a.bytes(), hipMemcpyDeviceToHost),
                  "download base CPU/GPU differential points");
        std::vector<nanoxgen::RootSample> actual_roots(expected.roots.size());
        check_hip(hipMemcpy(actual_roots.data(), runtime->roots.data(),
                            runtime->roots.bytes(), hipMemcpyDeviceToHost),
                  "download base CPU/GPU differential roots");
        CaseDifferential current{};
        current.description = runtime->spec.description;
        for (std::size_t i = 0u; i < actual_roots.size(); ++i) {
            current.root_position.add(
                expected.roots[i].position.x, actual_roots[i].position.x);
            current.root_position.add(
                expected.roots[i].position.y, actual_roots[i].position.y);
            current.root_position.add(
                expected.roots[i].position.z, actual_roots[i].position.z);
            current.root_normal.add(
                expected.roots[i].normal.x, actual_roots[i].normal.x);
            current.root_normal.add(
                expected.roots[i].normal.y, actual_roots[i].normal.y);
            current.root_normal.add(
                expected.roots[i].normal.z, actual_roots[i].normal.z);
            current.root_uv.add(expected.roots[i].uv.x, actual_roots[i].uv.x);
            current.root_uv.add(expected.roots[i].uv.y, actual_roots[i].uv.y);
            current.root_uv.add(expected.roots[i].barycentric.x,
                                actual_roots[i].barycentric.x);
            current.root_uv.add(expected.roots[i].barycentric.y,
                                actual_roots[i].barycentric.y);
            current.root_triangle_mismatches +=
                expected.roots[i].triangle_index !=
                actual_roots[i].triangle_index ? 1u : 0u;
        }
        for (std::size_t i = 0u; i < actual.size(); ++i) {
            add_point(current.base_position, current.base_radius,
                      expected.points[i], actual[i]);
        }
        nanoxgen::apply_xgen_classic_float_runtime_plan_cpu(
            expected, runtime->plan, 1.0f);
        dispatch_case(*runtime, stream, hip_stream, block_size);
        check_hip(hipStreamSynchronize(hip_stream),
                  "synchronize final CPU/GPU differential");
        check_hip(hipMemcpy(actual.data(), runtime->final_points().data(),
                            runtime->final_points().bytes(),
                            hipMemcpyDeviceToHost),
                  "download CPU/GPU differential points");
        for (std::size_t i = 0u; i < actual.size(); ++i) {
            add_point(current.final_position, current.final_radius,
                      expected.points[i], actual[i]);
            report.final.add(expected.points[i].x, actual[i].x);
            report.final.add(expected.points[i].y, actual[i].y);
            report.final.add(expected.points[i].z, actual[i].z);
            report.final.add(expected.points[i].radius, actual[i].radius);
        }
        report.cases.emplace_back(std::move(current));
    }
    return report;
}

std::string json_escape(std::string_view value) {
    std::string result;
    for (const char c : value) {
        if (c == '\\' || c == '"') { result.push_back('\\'); }
        result.push_back(c);
    }
    return result;
}

} // namespace

int main(int argc, char **argv) try {
    const Options options = parse_options(argc, argv);
    int device_count = 0;
    const hipError_t device_error = hipGetDeviceCount(&device_count);
    if (device_error != hipSuccess || device_count == 0) {
        std::cerr << "no usable HIP device: "
                  << hipGetErrorString(device_error) << '\n';
        return 77;
    }
    check_hip(hipSetDevice(0), "hipSetDevice");
    hipDeviceProp_t properties{};
    check_hip(hipGetDeviceProperties(&properties, 0), "hipGetDeviceProperties");

    const auto io_start = std::chrono::steady_clock::now();
    const nanoxgen::ClassicCollection collection =
        nanoxgen::load_xgen_classic_collection(options.collection);
    const auto io_end = std::chrono::steady_clock::now();
    luisa::compute::Context context{options.runtime_dir.string().c_str()};
    luisa::compute::Device device = context.create_device(options.backend.c_str());
    if (device.backend_name() != options.backend) {
        throw std::runtime_error("LuisaCompute loaded an unexpected backend");
    }
    luisa::compute::Stream stream = device.create_stream();
    const auto hip_stream = static_cast<hipStream_t>(stream.native_handle());

    const auto setup_start = std::chrono::steady_clock::now();
    std::vector<std::unique_ptr<RuntimeCase>> cases;
    cases.reserve(options.cases.size());
    for (const auto &spec : options.cases) {
        cases.emplace_back(std::make_unique<RuntimeCase>(
            spec, collection, device));
    }
    const auto setup_end = std::chrono::steady_clock::now();

    const auto dispatch_all = [&] {
        for (auto &runtime : cases) {
            dispatch_case(*runtime, stream, hip_stream, options.block_size);
        }
    };
    for (std::uint32_t i = 0u; i < options.warmup; ++i) { dispatch_all(); }
    check_hip(hipStreamSynchronize(hip_stream), "warmup synchronize");

    HipEvent start_event;
    HipEvent stop_event;
    std::vector<double> gpu_samples;
    std::vector<double> wall_samples;
    gpu_samples.reserve(options.repeats);
    wall_samples.reserve(options.repeats);
    for (std::uint32_t i = 0u; i < options.repeats; ++i) {
        const auto wall_start = std::chrono::steady_clock::now();
        check_hip(hipEventRecord(start_event, hip_stream),
                  "record benchmark start");
        dispatch_all();
        check_hip(hipEventRecord(stop_event, hip_stream),
                  "record benchmark stop");
        check_hip(hipEventSynchronize(stop_event),
                  "synchronize benchmark stop");
        const auto wall_stop = std::chrono::steady_clock::now();
        float gpu_ms = 0.0f;
        check_hip(hipEventElapsedTime(&gpu_ms, start_event, stop_event),
                  "measure benchmark events");
        gpu_samples.push_back(gpu_ms);
        wall_samples.push_back(std::chrono::duration<double, std::milli>(
            wall_stop - wall_start).count());
    }
    std::sort(gpu_samples.begin(), gpu_samples.end());
    std::sort(wall_samples.begin(), wall_samples.end());

    std::vector<double> cpu_samples;
    std::uint64_t cpu_checksum = 0u;
    DifferentialReport differential{};
    if (options.cpu_repeats != 0u) {
        static_cast<void>(benchmark_cpu_once(cases));
        cpu_samples.reserve(options.cpu_repeats);
        for (std::uint32_t i = 0u; i < options.cpu_repeats; ++i) {
            const auto start = std::chrono::steady_clock::now();
            cpu_checksum ^= benchmark_cpu_once(cases);
            const auto stop = std::chrono::steady_clock::now();
            cpu_samples.push_back(std::chrono::duration<double, std::milli>(
                stop - start).count());
        }
        std::sort(cpu_samples.begin(), cpu_samples.end());
        differential = validate_cpu_gpu(
            cases, stream, hip_stream, options.block_size);
    }

    std::uint64_t total_strands = 0u;
    std::uint64_t total_points = 0u;
    std::uint64_t combined_checksum = 1469598103934665603ull;
    for (auto &runtime : cases) {
        total_strands += runtime->spec.strands;
        total_points += checked_points(runtime->spec);
        combined_checksum ^= checksum(*runtime);
        combined_checksum *= 1099511628211ull;
    }

    const auto duration_ms = [](auto begin, auto end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    std::cout << std::fixed << std::setprecision(6)
              << "{\n"
              << "  \"backend\": \"" << json_escape(options.backend) << "\",\n"
              << "  \"device\": \"" << json_escape(properties.name) << "\",\n"
              << "  \"architecture\": \"" << json_escape(properties.gcnArchName) << "\",\n"
              << "  \"semantics\": \"nanoxgen-native-partial-not-autodesk-equivalent\",\n"
              << "  \"file_io_in_timing\": false,\n"
              << "  \"jit_compile_in_timing\": false,\n"
              << "  \"checksum_download_in_timing\": false,\n"
              << "  \"host_device_round_trip_between_stages\": false,\n"
              << "  \"collection_parse_ms\": " << duration_ms(io_start, io_end) << ",\n"
              << "  \"gpu_allocation_asset_upload_and_jit_compile_ms\": "
              << duration_ms(setup_start, setup_end) << ",\n"
              << "  \"warmup\": " << options.warmup << ",\n"
              << "  \"repeats\": " << options.repeats << ",\n"
              << "  \"strand_count\": " << total_strands << ",\n"
              << "  \"point_count\": " << total_points << ",\n"
              << "  \"gpu_ms\": {\"median\": " << percentile(gpu_samples, 0.5)
              << ", \"p90\": " << percentile(gpu_samples, 0.9) << "},\n"
              << "  \"wall_ms\": {\"median\": " << percentile(wall_samples, 0.5)
              << ", \"p90\": " << percentile(wall_samples, 0.9) << "},\n"
              << "  \"million_points_per_second\": "
              << static_cast<double>(total_points) /
                     (percentile(gpu_samples, 0.5) * 1000.0) << ",\n";
    if (cpu_samples.empty()) {
        std::cout << "  \"cpu\": null,\n";
    } else {
        std::cout << "  \"cpu\": {\"repeats\":" << options.cpu_repeats
                  << ",\"warmup\":1,\"file_io_in_timing\":false"
                  << ",\"output_allocation_in_timing\":true"
                  << ",\"median_ms\":" << percentile(cpu_samples, 0.5)
                  << ",\"p90_ms\":" << percentile(cpu_samples, 0.9)
                  << ",\"checksum\":\"0x" << std::hex << cpu_checksum
                  << std::dec << "\"},\n"
                  << "  \"cpu_gpu_differential\": {\"channels\":"
                     "\"position-radius\",\"component_count\":"
                  << differential.final.component_count
                  << ",\"maximum_absolute_error\":"
                  << differential.final.maximum
                  << ",\"rms_error\":" << differential.final.rms()
                  << ",\"above_1e-5\":" << differential.final.above_1e5
                  << ",\"above_1e-4\":" << differential.final.above_1e4
                  << "},\n"
                  << "  \"cpu_gpu_case_differential\": [\n";
        for (std::size_t i = 0u; i < differential.cases.size(); ++i) {
            const auto &d = differential.cases[i];
            const auto emit = [](const char *name,
                                 const DifferentialSummary &value) {
                std::cout << "\"" << name << "\":{\"max\":"
                          << value.maximum << ",\"rms\":" << value.rms()
                          << ",\"above_1e-5\":" << value.above_1e5
                          << ",\"above_1e-4\":" << value.above_1e4 << "}";
            };
            std::cout << "    {\"description\":\""
                      << json_escape(d.description) << "\",";
            emit("root_position", d.root_position); std::cout << ',';
            emit("root_normal", d.root_normal); std::cout << ',';
            emit("root_uv_barycentric", d.root_uv); std::cout << ',';
            std::cout << "\"root_triangle_mismatches\":"
                      << d.root_triangle_mismatches << ',';
            emit("base_position", d.base_position); std::cout << ',';
            emit("base_radius", d.base_radius); std::cout << ',';
            emit("final_position", d.final_position); std::cout << ',';
            emit("final_radius", d.final_radius);
            std::cout << "}" << (i + 1u == differential.cases.size()
                                      ? "\n" : ",\n");
        }
        std::cout << "  ],\n";
    }
    std::cout
              << "  \"cases\": [\n";
    for (std::size_t i = 0u; i < cases.size(); ++i) {
        const auto &runtime = *cases[i];
        std::cout << "    {\"description\":\""
                  << json_escape(runtime.spec.description)
                  << "\",\"strands\":" << runtime.spec.strands
                  << ",\"cvs\":" << runtime.spec.cvs
                  << ",\"points\":" << checked_points(runtime.spec)
                  << ",\"cut_kernels\":" << runtime.cuts.size()
                  << ",\"lowering_complete\":"
                  << (runtime.plan.lowering_complete() ? "true" : "false")
                  << ",\"fallback_count\":"
                  << runtime.plan.fallback_reasons.size() << "}"
                  << (i + 1u == cases.size() ? "\n" : ",\n");
    }
    std::cout << "  ],\n"
              << "  \"checksum\": \"0x" << std::hex << combined_checksum
              << std::dec << "\"\n"
              << "}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "Luisa Classic benchmark failure: " << error.what() << '\n';
    return 1;
}
