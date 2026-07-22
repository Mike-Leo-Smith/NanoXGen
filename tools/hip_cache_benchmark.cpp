#include "nanoxgen/curve_cache.h"

#include <hip/hip_runtime.h>

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
#include <stdexcept>
#include <string>
#include <vector>

using namespace nanoxgen;

namespace {

constexpr std::uint32_t kBlockSize = 256u;

void check_hip(hipError_t error, const char *operation) {
    if (error != hipSuccess) {
        throw std::runtime_error(
            std::string{operation} + ": " + hipGetErrorString(error));
    }
}

std::uint32_t parse_positive_u32(const char *text, const char *name) {
    const unsigned long value = std::stoul(text);
    if (value == 0u || value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string{name} + " must be positive");
    }
    return static_cast<std::uint32_t>(value);
}

struct Options {
    std::vector<std::filesystem::path> paths;
    std::uint32_t warmup{3u};
    std::uint32_t repeats{15u};
};

Options parse_options(int argc, char **argv) {
    Options options{};
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--warmup" || argument == "--repeats") {
            if (++i >= argc) { throw std::invalid_argument("missing value after " + argument); }
            const std::uint32_t value = parse_positive_u32(
                argv[i], argument == "--warmup" ? "warm-up count" : "repeat count");
            if (argument == "--warmup") {
                options.warmup = value;
            } else {
                options.repeats = value;
            }
        } else if (argument.starts_with('-')) {
            throw std::invalid_argument("unknown option: " + argument);
        } else {
            options.paths.emplace_back(argument);
        }
    }
    if (options.paths.empty()) {
        throw std::invalid_argument(
            "usage: nanoxgen_hip_cache_benchmark [--warmup N] [--repeats N] CACHE.nxc...");
    }
    return options;
}

struct Summary {
    double median{};
    double p90{};
};

Summary summarize(std::vector<double> samples) {
    if (samples.empty()) { throw std::invalid_argument("cannot summarize empty samples"); }
    std::sort(samples.begin(), samples.end());
    const std::size_t p90 = std::min(
        samples.size() - 1u,
        static_cast<std::size_t>(std::ceil(samples.size() * 0.9)) - 1u);
    return {samples[samples.size() / 2u], samples[p90]};
}

template<typename T>
class DeviceBuffer {
public:
    explicit DeviceBuffer(std::size_t count) : _count(count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::overflow_error("HIP allocation size overflow");
        }
        if (count != 0u) {
            check_hip(hipMalloc(reinterpret_cast<void **>(&_data), bytes()), "hipMalloc");
        }
    }

    DeviceBuffer(const DeviceBuffer &) = delete;
    DeviceBuffer &operator=(const DeviceBuffer &) = delete;

    ~DeviceBuffer() {
        if (_data) { (void)hipFree(_data); }
    }

    [[nodiscard]] T *data() const noexcept { return _data; }
    [[nodiscard]] std::size_t bytes() const noexcept { return _count * sizeof(T); }

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

struct ValidationResult {
    unsigned long long point_sum{};
    unsigned long long checksum{};
    unsigned int invalid_points{};
};

__host__ __device__ unsigned long long mix64(unsigned long long value) noexcept {
    value ^= value >> 30u;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27u;
    value *= 0x94d049bb133111ebull;
    value ^= value >> 31u;
    return value;
}

__host__ __device__ unsigned long long point_checksum(
    std::uint64_t index,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t z,
    std::uint32_t radius) noexcept {
    unsigned long long value = mix64(index + 0x9e3779b97f4a7c15ull);
    value ^= mix64((static_cast<unsigned long long>(x) << 32u) | y);
    value ^= mix64((static_cast<unsigned long long>(z) << 32u) | radius);
    return mix64(value);
}

__global__ void validate_counts_kernel(
    const std::uint32_t *counts,
    std::uint64_t count,
    ValidationResult *result) {
    __shared__ unsigned long long partial[kBlockSize];
    unsigned long long sum = 0u;
    for (std::uint64_t index =
             static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
         index < count;
         index += static_cast<std::uint64_t>(blockDim.x) * gridDim.x) {
        sum += counts[index];
    }
    partial[threadIdx.x] = sum;
    __syncthreads();
    for (std::uint32_t stride = blockDim.x / 2u; stride != 0u; stride /= 2u) {
        if (threadIdx.x < stride) { partial[threadIdx.x] += partial[threadIdx.x + stride]; }
        __syncthreads();
    }
    if (threadIdx.x == 0u) { atomicAdd(&result->point_sum, partial[0]); }
}

__global__ void validate_points_kernel(
    const PackedCurvePoint *points,
    std::uint64_t count,
    ValidationResult *result) {
    __shared__ unsigned long long partial_checksum[kBlockSize];
    __shared__ unsigned int partial_invalid[kBlockSize];
    unsigned long long checksum = 0u;
    unsigned int invalid = 0u;
    for (std::uint64_t index =
             static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
         index < count;
         index += static_cast<std::uint64_t>(blockDim.x) * gridDim.x) {
        const PackedCurvePoint point = points[index];
        if (!isfinite(point.x) || !isfinite(point.y) || !isfinite(point.z) ||
            !isfinite(point.radius) || point.radius < 0.0f) {
            invalid = 1u;
        }
        checksum ^= point_checksum(
            index, __float_as_uint(point.x), __float_as_uint(point.y),
            __float_as_uint(point.z), __float_as_uint(point.radius));
    }
    partial_checksum[threadIdx.x] = checksum;
    partial_invalid[threadIdx.x] = invalid;
    __syncthreads();
    for (std::uint32_t stride = blockDim.x / 2u; stride != 0u; stride /= 2u) {
        if (threadIdx.x < stride) {
            partial_checksum[threadIdx.x] ^= partial_checksum[threadIdx.x + stride];
            partial_invalid[threadIdx.x] |= partial_invalid[threadIdx.x + stride];
        }
        __syncthreads();
    }
    if (threadIdx.x == 0u) {
        atomicXor(&result->checksum, partial_checksum[0]);
        atomicOr(&result->invalid_points, partial_invalid[0]);
    }
}

double elapsed_ms(hipEvent_t start, hipEvent_t stop) {
    float milliseconds = 0.0f;
    check_hip(hipEventElapsedTime(&milliseconds, start, stop), "hipEventElapsedTime");
    return milliseconds;
}

std::vector<CurveCache> load_all(const std::vector<std::filesystem::path> &paths) {
    std::vector<CurveCache> caches;
    caches.reserve(paths.size());
    for (const std::filesystem::path &path : paths) { caches.push_back(load_curve_cache(path)); }
    return caches;
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

    for (std::uint32_t i = 0u; i < options.warmup; ++i) { (void)load_all(options.paths); }
    std::vector<double> read_samples;
    read_samples.reserve(options.repeats);
    std::vector<CurveCache> caches;
    for (std::uint32_t i = 0u; i < options.repeats; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        std::vector<CurveCache> loaded = load_all(options.paths);
        const auto end = std::chrono::steady_clock::now();
        read_samples.push_back(
            std::chrono::duration<double, std::milli>(end - begin).count());
        caches = std::move(loaded);
    }

    std::uint64_t input_bytes = 0u;
    std::uint64_t curve_count = 0u;
    std::uint64_t point_count = 0u;
    for (std::size_t i = 0u; i < caches.size(); ++i) {
        input_bytes += std::filesystem::file_size(options.paths[i]);
        const CurveCacheHeader &header = caches[i].view().header();
        curve_count += header.strand_count;
        point_count += header.point_count;
    }
    if (curve_count > std::numeric_limits<std::size_t>::max() ||
        point_count > std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error("aggregate Rabbit cache exceeds size_t");
    }

    const auto pack_begin = std::chrono::steady_clock::now();
    std::vector<std::uint32_t> point_counts;
    std::vector<PackedCurvePoint> points;
    point_counts.reserve(static_cast<std::size_t>(curve_count));
    points.reserve(static_cast<std::size_t>(point_count));
    for (const CurveCache &cache : caches) {
        const CurveCacheView view = cache.view();
        const CurveCacheHeader &header = view.header();
        point_counts.insert(
            point_counts.end(), view.point_counts(), view.point_counts() + header.strand_count);
        points.insert(points.end(), view.points(), view.points() + header.point_count);
    }
    const auto pack_end = std::chrono::steady_clock::now();
    const double host_pack_ms =
        std::chrono::duration<double, std::milli>(pack_end - pack_begin).count();

    unsigned long long expected_point_sum = 0u;
    for (std::uint32_t count : point_counts) { expected_point_sum += count; }
    if (expected_point_sum != points.size()) {
        throw std::runtime_error("aggregate pointCounts topology does not match points");
    }
    unsigned long long expected_checksum = 0u;
    for (std::size_t i = 0u; i < points.size(); ++i) {
        const PackedCurvePoint &point = points[i];
        expected_checksum ^= point_checksum(
            i, std::bit_cast<std::uint32_t>(point.x), std::bit_cast<std::uint32_t>(point.y),
            std::bit_cast<std::uint32_t>(point.z), std::bit_cast<std::uint32_t>(point.radius));
    }

    DeviceBuffer<std::uint32_t> device_counts(point_counts.size());
    DeviceBuffer<PackedCurvePoint> device_points(points.size());
    DeviceBuffer<ValidationResult> device_result(1u);
    HipEvent start;
    HipEvent stop;
    std::vector<double> h2d_device_samples;
    std::vector<double> h2d_wall_samples;
    for (std::uint32_t i = 0u; i < options.warmup + options.repeats; ++i) {
        const auto wall_begin = std::chrono::steady_clock::now();
        check_hip(hipEventRecord(start), "hipEventRecord H2D start");
        check_hip(hipMemcpyAsync(
                      device_counts.data(), point_counts.data(), device_counts.bytes(),
                      hipMemcpyHostToDevice),
                  "hipMemcpyAsync pointCounts");
        check_hip(hipMemcpyAsync(
                      device_points.data(), points.data(), device_points.bytes(),
                      hipMemcpyHostToDevice),
                  "hipMemcpyAsync points");
        check_hip(hipEventRecord(stop), "hipEventRecord H2D stop");
        check_hip(hipEventSynchronize(stop), "hipEventSynchronize H2D");
        const auto wall_end = std::chrono::steady_clock::now();
        if (i >= options.warmup) {
            h2d_device_samples.push_back(elapsed_ms(start, stop));
            h2d_wall_samples.push_back(
                std::chrono::duration<double, std::milli>(wall_end - wall_begin).count());
        }
    }

    const std::uint32_t grid_size = std::max(
        1u, std::min(1024u, static_cast<std::uint32_t>(device_properties.multiProcessorCount) * 8u));
    std::vector<double> kernel_samples;
    for (std::uint32_t i = 0u; i < options.warmup + options.repeats; ++i) {
        check_hip(hipMemset(device_result.data(), 0, device_result.bytes()), "hipMemset result");
        check_hip(hipEventRecord(start), "hipEventRecord kernel start");
        validate_counts_kernel<<<grid_size, kBlockSize>>>(
            device_counts.data(), point_counts.size(), device_result.data());
        check_hip(hipGetLastError(), "validate_counts_kernel launch");
        validate_points_kernel<<<grid_size, kBlockSize>>>(
            device_points.data(), points.size(), device_result.data());
        check_hip(hipGetLastError(), "validate_points_kernel launch");
        check_hip(hipEventRecord(stop), "hipEventRecord kernel stop");
        check_hip(hipEventSynchronize(stop), "hipEventSynchronize kernel");
        ValidationResult result{};
        check_hip(hipMemcpy(
                      &result, device_result.data(), sizeof(result), hipMemcpyDeviceToHost),
                  "hipMemcpy validation result");
        if (result.point_sum != expected_point_sum || result.checksum != expected_checksum ||
            result.invalid_points != 0u) {
            throw std::runtime_error("HIP validation result does not match the host cache");
        }
        if (i >= options.warmup) { kernel_samples.push_back(elapsed_ms(start, stop)); }
    }

    const Summary read = summarize(std::move(read_samples));
    const Summary h2d_device = summarize(std::move(h2d_device_samples));
    const Summary h2d_wall = summarize(std::move(h2d_wall_samples));
    const Summary kernel = summarize(std::move(kernel_samples));
    const std::uint64_t resident_bytes =
        static_cast<std::uint64_t>(device_counts.bytes()) + device_points.bytes();

    std::cout << std::fixed << std::setprecision(4)
              << "{\n"
              << "  \"device\": \"" << device_properties.name << "\",\n"
              << "  \"architecture\": \"" << device_properties.gcnArchName << "\",\n"
              << "  \"files\": " << options.paths.size() << ",\n"
              << "  \"input_bytes\": " << input_bytes << ",\n"
              << "  \"curves\": " << curve_count << ",\n"
              << "  \"points\": " << point_count << ",\n"
              << "  \"gpu_resident_bytes\": " << resident_bytes << ",\n"
              << "  \"warmup\": " << options.warmup << ",\n"
              << "  \"repeats\": " << options.repeats << ",\n"
              << "  \"cpu_read_validate_ms\": {\"median\": " << read.median
              << ", \"p90\": " << read.p90 << "},\n"
              << "  \"host_concatenate_once_ms\": " << host_pack_ms << ",\n"
              << "  \"h2d_device_ms\": {\"median\": " << h2d_device.median
              << ", \"p90\": " << h2d_device.p90 << "},\n"
              << "  \"h2d_wall_ms\": {\"median\": " << h2d_wall.median
              << ", \"p90\": " << h2d_wall.p90 << "},\n"
              << "  \"gpu_validate_checksum_ms\": {\"median\": " << kernel.median
              << ", \"p90\": " << kernel.p90 << "},\n"
              << "  \"checksum\": \"0x" << std::hex << expected_checksum << std::dec << "\"\n"
              << "}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "HIP cache benchmark failure: " << error.what() << '\n';
    return 1;
}
