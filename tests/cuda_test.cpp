#include "nanoxgen/asset.h"
#include "nanoxgen/cuda.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nanoxgen;

namespace {

void check_cuda(cudaError_t error, const char *operation) {
    if (error != cudaSuccess) {
        throw std::runtime_error(
            std::string{operation} + ": " + cudaGetErrorString(error));
    }
}

void require(bool condition, const char *message) {
    if (!condition) { throw std::runtime_error(message); }
}

template<typename T>
class DeviceBuffer {
public:
    DeviceBuffer() = default;

    explicit DeviceBuffer(std::size_t count) : _count(count) {
        if (_count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::overflow_error("device allocation size overflow");
        }
        if (_count != 0u) {
            check_cuda(cudaMalloc(reinterpret_cast<void **>(&_data), bytes()), "cudaMalloc");
        }
    }

    DeviceBuffer(const DeviceBuffer &) = delete;
    DeviceBuffer &operator=(const DeviceBuffer &) = delete;

    DeviceBuffer(DeviceBuffer &&other) noexcept
        : _data(other._data), _count(other._count) {
        other._data = nullptr;
        other._count = 0u;
    }

    DeviceBuffer &operator=(DeviceBuffer &&other) noexcept {
        if (this == &other) { return *this; }
        if (_data) { cudaFree(_data); }
        _data = other._data;
        _count = other._count;
        other._data = nullptr;
        other._count = 0u;
        return *this;
    }

    ~DeviceBuffer() {
        if (_data) { cudaFree(_data); }
    }

    [[nodiscard]] T *data() const noexcept { return _data; }
    [[nodiscard]] std::size_t size() const noexcept { return _count; }
    [[nodiscard]] std::size_t bytes() const noexcept { return _count * sizeof(T); }

    void upload(const T *source, std::size_t count) {
        require(count <= _count, "device upload exceeds allocation");
        check_cuda(cudaMemcpy(_data, source, count * sizeof(T), cudaMemcpyHostToDevice),
                   "cudaMemcpy host to device");
    }

    [[nodiscard]] std::vector<T> download() const {
        std::vector<T> result(_count);
        check_cuda(cudaMemcpy(result.data(), _data, bytes(), cudaMemcpyDeviceToHost),
                   "cudaMemcpy device to host");
        return result;
    }

private:
    T *_data{};
    std::size_t _count{};
};

AssetBuildInput fixture() {
    AssetBuildInput input{};
    input.positions = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f}, {1.0f, 0.15f, 1.0f}};
    input.normals.assign(input.positions.size(), {0.0f, 1.0f, 0.0f});
    input.texcoords = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
    input.triangles = {{0u, 1u, 2u}, {1u, 3u, 2u}};

    GuideInput first{};
    first.root_normal = {0.0f, 1.0f, 0.0f};
    first.triangle_index = 0u;
    first.barycentric = {0.2f, 0.3f};
    first.support_radius = 10.0f;
    first.cvs = {
        {0.2f, 0.0f, 0.3f}, {0.18f, 0.3f, 0.33f},
        {0.25f, 0.7f, 0.38f}, {0.35f, 1.1f, 0.45f}};
    input.guides.push_back(first);

    GuideInput second{};
    second.root_normal = {0.0f, 1.0f, 0.0f};
    second.triangle_index = 1u;
    second.barycentric = {0.35f, 0.25f};
    second.support_radius = 10.0f;
    second.cvs = {
        {0.75f, 0.05f, 0.6f}, {0.82f, 0.4f, 0.57f},
        {0.78f, 0.78f, 0.68f}, {0.68f, 1.2f, 0.76f}};
    input.guides.push_back(second);
    return input;
}

struct ErrorSummary {
    double squared_sum{};
    float maximum{};
    std::uint64_t count{};

    void add(float reference, float actual) {
        const float error = std::abs(reference - actual);
        squared_sum += static_cast<double>(error) * error;
        maximum = std::max(maximum, error);
        ++count;
    }

    [[nodiscard]] double rms() const {
        return count == 0u ? 0.0 : std::sqrt(squared_sum / static_cast<double>(count));
    }
};

void compare_vec3(ErrorSummary &errors, Vec3 reference, Vec3 actual) {
    errors.add(reference.x, actual.x);
    errors.add(reference.y, actual.y);
    errors.add(reference.z, actual.z);
}

void require_close(const ErrorSummary &errors, const char *label) {
    if (errors.maximum > 5.0e-5f || errors.rms() > 5.0e-6) {
        throw std::runtime_error(
            std::string{label} + " mismatch: max=" + std::to_string(errors.maximum) +
            ", RMS=" + std::to_string(errors.rms()));
    }
}

DeviceAssetDescriptor upload_asset(const Asset &asset, DeviceBuffer<std::byte> &device) {
    device.upload(asset.bytes().data(), asset.bytes().size());
    return make_device_asset_descriptor(asset, device.data(), device.bytes());
}

GenerationParams generation_params() {
    GenerationParams params{};
    params.strand_count = 513u;
    params.cvs_per_strand = 17u;
    params.seed = 0x13579bdu;
    params.guide_weight_power = 1.75f;
    params.length_scale = 0.93f;
    params.root_width = 0.032f;
    params.tip_width = 0.0015f;
    params.noise_amplitude = 0.047f;
    params.noise_frequency = 3.17f;
    params.noise_mask = 0.83f;
    params.noise_correlation = 0.35f;
    params.noise_preserve_length = 0.4f;
    return params;
}

void test_packed_generation(
    const Asset &asset, const DeviceAssetDescriptor &device_asset) {
    const GenerationParams params = generation_params();
    constexpr float radius_scale = 1.5f;
    const PackedGeneratedCurves reference =
        generate_packed_cpu(asset, params, radius_scale, {1u, 128u});

    DeviceBuffer<PackedCurvePoint> points(reference.points.size());
    DeviceBuffer<RootSample> roots(reference.roots.size());
    DeviceBuffer<Vec2> root_uvs(reference.root_uvs.size());
    DeviceBuffer<std::uint32_t> point_counts(reference.point_counts.size());
    const DevicePackedCurveOutputDescriptor output{
        {points.data(), roots.data(), root_uvs.data(), radius_scale, point_counts.data()},
        points.size(), roots.size(), root_uvs.size(), point_counts.size()};
    check_cuda(launch_generate_packed_cuda(
                   device_asset, {}, params, output, {127u}),
               "launch_generate_packed_cuda");
    check_cuda(cudaDeviceSynchronize(), "packed generation synchronize");

    const std::vector<PackedCurvePoint> actual_points = points.download();
    const std::vector<RootSample> actual_roots = roots.download();
    const std::vector<Vec2> actual_uvs = root_uvs.download();
    const std::vector<std::uint32_t> actual_counts = point_counts.download();
    require(actual_counts == reference.point_counts, "CUDA point counts must be exact");

    ErrorSummary point_errors{};
    ErrorSummary radius_errors{};
    for (std::size_t i = 0u; i < reference.points.size(); ++i) {
        compare_vec3(point_errors,
                     {reference.points[i].x, reference.points[i].y, reference.points[i].z},
                     {actual_points[i].x, actual_points[i].y, actual_points[i].z});
        radius_errors.add(reference.points[i].radius, actual_points[i].radius);
    }
    require_close(point_errors, "CUDA packed positions");
    require_close(radius_errors, "CUDA packed radii");

    ErrorSummary root_errors{};
    ErrorSummary uv_errors{};
    for (std::size_t i = 0u; i < reference.roots.size(); ++i) {
        require(actual_roots[i].triangle_index == reference.roots[i].triangle_index,
                "CUDA root triangle identity must be exact");
        compare_vec3(root_errors, reference.roots[i].position, actual_roots[i].position);
        compare_vec3(root_errors, reference.roots[i].normal, actual_roots[i].normal);
        uv_errors.add(reference.roots[i].barycentric.x, actual_roots[i].barycentric.x);
        uv_errors.add(reference.roots[i].barycentric.y, actual_roots[i].barycentric.y);
        uv_errors.add(reference.root_uvs[i].x, actual_uvs[i].x);
        uv_errors.add(reference.root_uvs[i].y, actual_uvs[i].y);
    }
    require_close(root_errors, "CUDA roots");
    require_close(uv_errors, "CUDA root attributes");
}

void test_deformed_motion(
    const Asset &asset, const DeviceAssetDescriptor &device_asset) {
    const DeviceAssetView view = asset.view();
    const Vec3 translation{1.25f, -0.5f, 2.0f};
    std::vector<Vec3> positions(
        view.positions(), view.positions() + view.header().vertex_count);
    std::vector<Vec3> normals(
        view.normals(), view.normals() + view.header().vertex_count);
    std::vector<Vec3> guide_cvs(
        view.guide_cvs(), view.guide_cvs() + view.header().guide_cv_count);
    for (Vec3 &point : positions) { point = point + translation; }
    for (Vec3 &point : guide_cvs) { point = point + translation; }

    DeviceBuffer<Vec3> device_positions(positions.size());
    DeviceBuffer<Vec3> device_normals(normals.size());
    DeviceBuffer<Vec3> device_guide_cvs(guide_cvs.size());
    device_positions.upload(positions.data(), positions.size());
    device_normals.upload(normals.data(), normals.size());
    device_guide_cvs.upload(guide_cvs.data(), guide_cvs.size());
    const DeviceDeformedGeometryDescriptor deformation{
        {device_positions.data(), device_normals.data(), device_guide_cvs.data()},
        device_positions.size(), device_normals.size(), device_guide_cvs.size()};

    const GenerationParams params = generation_params();
    const GeneratedCurves rest = generate_cpu(asset, params, {1u, 128u});
    const GeneratedCurves moved = generate_deformed_cpu(
        asset, params, {positions, normals, guide_cvs}, {1u, 128u});
    const std::size_t points_per_sample = rest.points.size();
    DeviceBuffer<Vec3> motion_points(points_per_sample * 2u);
    const DeviceMotionSampleDescriptor samples[2] = {
        {{}, 0.0f}, {deformation, 1.0f}};
    const DeviceMotionOutputDescriptor output{motion_points.data(), motion_points.size()};
    check_cuda(launch_generate_motion_cuda(
                   device_asset, samples, 2u, params, output, {128u}),
               "launch_generate_motion_cuda");
    check_cuda(cudaDeviceSynchronize(), "motion generation synchronize");
    const std::vector<Vec3> actual = motion_points.download();

    ErrorSummary rest_errors{};
    ErrorSummary moved_errors{};
    for (std::size_t i = 0u; i < points_per_sample; ++i) {
        compare_vec3(rest_errors, rest.points[i], actual[i]);
        compare_vec3(moved_errors, moved.points[i], actual[points_per_sample + i]);
    }
    require_close(rest_errors, "CUDA rest motion sample");
    require_close(moved_errors, "CUDA deformed motion sample");
}

} // namespace

int main() try {
    int device_count = 0;
    const cudaError_t device_error = cudaGetDeviceCount(&device_count);
    if (device_error != cudaSuccess || device_count == 0) {
        std::cout << "SKIP: no usable CUDA device ("
                  << cudaGetErrorString(device_error) << ")\n";
        return 77;
    }
    check_cuda(cudaSetDevice(0), "cudaSetDevice");

    const Asset asset = build_asset(fixture());
    DeviceBuffer<std::byte> device_asset_bytes(asset.bytes().size());
    const DeviceAssetDescriptor device_asset = upload_asset(asset, device_asset_bytes);
    test_packed_generation(asset, device_asset);
    test_deformed_motion(asset, device_asset);
    std::cout << "all NanoXGen CUDA parity tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "CUDA test failure: " << error.what() << '\n';
    return 1;
}
