#include "nanoxgen/asset.h"
#include "nanoxgen/hip.h"

#include <hip/hip_runtime_api.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nanoxgen;

namespace {

void check_hip(hipError_t error, const char *operation) {
    if (error != hipSuccess) {
        throw std::runtime_error(
            std::string{operation} + ": " + hipGetErrorString(error));
    }
}

template<typename T>
class DeviceBuffer {
public:
    explicit DeviceBuffer(std::size_t count) : _count(count) {
        if (_count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::overflow_error("device allocation size overflow");
        }
        if (_count != 0u) {
            check_hip(hipMalloc(reinterpret_cast<void **>(&_data), bytes()), "hipMalloc");
        }
    }

    DeviceBuffer(const DeviceBuffer &) = delete;
    DeviceBuffer &operator=(const DeviceBuffer &) = delete;

    ~DeviceBuffer() {
        if (_data) { (void)hipFree(_data); }
    }

    [[nodiscard]] T *data() const noexcept { return _data; }
    [[nodiscard]] std::size_t size() const noexcept { return _count; }
    [[nodiscard]] std::size_t bytes() const noexcept { return _count * sizeof(T); }

    void upload(const T *source, std::size_t count) {
        if (count > _count) { throw std::runtime_error("device upload exceeds allocation"); }
        check_hip(hipMemcpy(_data, source, count * sizeof(T), hipMemcpyHostToDevice),
                   "hipMemcpy host to device");
    }

private:
    T *_data{};
    std::size_t _count{};
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

std::uint32_t parse_positive_u32(const char *text, const char *name) {
    const unsigned long value = std::stoul(text);
    if (value == 0u || value > 0xfffffffful) {
        throw std::invalid_argument(std::string{name} + " must be in [1, 2^32-1]");
    }
    return static_cast<std::uint32_t>(value);
}

struct Options {
    std::filesystem::path asset_path;
    std::uint32_t strands{100000u};
    std::uint32_t cvs{12u};
    std::uint32_t repeats{31u};
    std::uint32_t block_size{128u};
    bool noise{};
};

Options parse_options(int argc, char **argv) {
    if (argc < 2) {
        throw std::invalid_argument(
            "usage: nanoxgen_hip_benchmark ASSET.nxg [--strands N] [--cvs N] "
            "[--repeats N] [--block-size N] [--noise]");
    }
    Options options{};
    options.asset_path = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--noise") {
            options.noise = true;
            continue;
        }
        if (i + 1 >= argc) {
            throw std::invalid_argument("missing value after " + argument);
        }
        if (argument == "--strands") {
            options.strands = parse_positive_u32(argv[++i], "strand count");
        } else if (argument == "--cvs") {
            options.cvs = parse_positive_u32(argv[++i], "CV count");
            if (options.cvs < 2u) { throw std::invalid_argument("CV count must be at least 2"); }
        } else if (argument == "--repeats") {
            options.repeats = parse_positive_u32(argv[++i], "repeat count");
        } else if (argument == "--block-size") {
            options.block_size = parse_positive_u32(argv[++i], "block size");
            if (options.block_size > 1024u) {
                throw std::invalid_argument("block size must not exceed 1024");
            }
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    return options;
}

float percentile(const std::vector<float> &sorted, double fraction) {
    const std::size_t index = static_cast<std::size_t>(
        fraction * static_cast<double>(sorted.size() - 1u));
    return sorted[index];
}

} // namespace

int main(int argc, char **argv) try {
    const Options options = parse_options(argc, argv);
    int device_count = 0;
    const hipError_t device_error = hipGetDeviceCount(&device_count);
    if (device_error != hipSuccess || device_count == 0) {
        std::cerr << "no usable HIP device: " << hipGetErrorString(device_error) << '\n';
        return 77;
    }
    check_hip(hipSetDevice(0), "hipSetDevice");
    hipDeviceProp_t device_properties{};
    check_hip(hipGetDeviceProperties(&device_properties, 0), "hipGetDeviceProperties");

    const Asset asset = load_asset(options.asset_path);
    DeviceBuffer<std::byte> device_asset_bytes(asset.bytes().size());
    device_asset_bytes.upload(asset.bytes().data(), asset.bytes().size());
    const DeviceAssetDescriptor device_asset = make_device_asset_descriptor(
        asset, device_asset_bytes.data(), device_asset_bytes.bytes());

    GenerationParams params{};
    params.strand_count = options.strands;
    params.cvs_per_strand = options.cvs;
    params.seed = 0x13579bdu;
    if (options.noise) {
        params.noise_amplitude = 0.043f;
        params.noise_frequency = 3.17f;
        params.noise_mask = 0.83f;
        params.noise_correlation = 0.35f;
        params.noise_preserve_length = 0.4f;
    }

    const std::uint64_t point_count =
        static_cast<std::uint64_t>(params.strand_count) * params.cvs_per_strand;
    if (point_count > std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error("point allocation exceeds size_t");
    }
    DeviceBuffer<PackedCurvePoint> points(static_cast<std::size_t>(point_count));
    const DevicePackedCurveOutputDescriptor output{
        {points.data(), nullptr, nullptr, 1.0f, nullptr},
        points.size(), 0u, 0u, 0u};
    const DeviceLaunchConfig launch_config{options.block_size};

    for (std::uint32_t warmup = 0u; warmup < 3u; ++warmup) {
        check_hip(launch_generate_packed_hip(
                       device_asset, {}, params, output, launch_config),
                   "warm-up launch_generate_packed_hip");
    }
    check_hip(hipDeviceSynchronize(), "warm-up synchronize");

    HipEvent start;
    HipEvent stop;
    std::vector<float> milliseconds;
    milliseconds.reserve(options.repeats);
    for (std::uint32_t repeat = 0u; repeat < options.repeats; ++repeat) {
        check_hip(hipEventRecord(start), "hipEventRecord start");
        check_hip(launch_generate_packed_hip(
                       device_asset, {}, params, output, launch_config),
                   "launch_generate_packed_hip");
        check_hip(hipEventRecord(stop), "hipEventRecord stop");
        check_hip(hipEventSynchronize(stop), "hipEventSynchronize");
        float elapsed = 0.0f;
        check_hip(hipEventElapsedTime(&elapsed, start, stop), "hipEventElapsedTime");
        milliseconds.push_back(elapsed);
    }
    std::sort(milliseconds.begin(), milliseconds.end());
    const float median_ms = percentile(milliseconds, 0.5);
    const float p90_ms = percentile(milliseconds, 0.90);
    const double million_cvs_per_second =
        static_cast<double>(point_count) / (static_cast<double>(median_ms) * 1000.0);

    std::cout << std::fixed << std::setprecision(4)
              << "{\n"
              << "  \"device\": \"" << device_properties.name << "\",\n"
              << "  \"architecture\": \"" << device_properties.gcnArchName << "\",\n"
              << "  \"asset\": \"" << options.asset_path.string() << "\",\n"
              << "  \"strands\": " << params.strand_count << ",\n"
              << "  \"cvs_per_strand\": " << params.cvs_per_strand << ",\n"
              << "  \"noise\": " << (options.noise ? "true" : "false") << ",\n"
              << "  \"block_size\": " << options.block_size << ",\n"
              << "  \"warmup\": 3,\n"
              << "  \"repeats\": " << options.repeats << ",\n"
              << "  \"median_ms\": " << median_ms << ",\n"
              << "  \"p90_ms\": " << p90_ms << ",\n"
              << "  \"million_cvs_per_second\": " << million_cvs_per_second << "\n"
              << "}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "HIP benchmark failure: " << error.what() << '\n';
    return 1;
}
