#pragma once

#include "nanoxgen/xgen_classic_runtime.h"

#include <luisa/runtime/buffer.h>
#include <luisa/runtime/byte_buffer.h>
#include <luisa/runtime/device.h>
#include <luisa/runtime/stream.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nanoxgen::luisa_backend {

// Host-side inputs needed to specialize one Classic description. The caller
// retains ownership only for the duration of compile_classic_collection().
struct ClassicCollectionCompileInput {
    const ClassicFloatRuntimePlan *runtime{};
    std::uint32_t cvs_per_strand{};
    std::span<const std::uint32_t> clump_guide_counts;
    bool root_relative{true};
};

struct ClassicCollectionCompileOptions {
    bool fast_math{true};
    bool enable_cache{};
    // Device::compile is entered concurrently only when this is greater than
    // one. Zero selects the machine's native logical-thread count for every
    // backend; callers may set a lower limit to share CPU capacity.
    std::size_t max_parallel_compiles{};
};

struct ClassicCollectionCompileStats {
    std::size_t worker_limit{};
    std::size_t kernel_count{};
    double wall_ms{};
    double task_sum_ms{};
    double task_max_ms{};
};

// All buffers and their lifetimes belong to the renderer. NanoXGen only
// records work into the supplied Stream; it never synchronizes or downloads.
struct ClassicCollectionDispatchResources {
    luisa::compute::ByteBufferView roots;
    luisa::compute::BufferView<luisa::uint> influence_offsets;
    luisa::compute::ByteBufferView influences;
    luisa::compute::BufferView<luisa::float3> guides;
    luisa::compute::BufferView<luisa::uint> root_runtime;
    luisa::compute::BufferView<float> runtime_inputs;
    luisa::compute::BufferView<luisa::float3> surface_tangents;
    luisa::compute::BufferView<luisa::float3> noise_domain_positions;
    luisa::compute::BufferView<luisa::float4> points_a;
    luisa::compute::BufferView<luisa::float4> points_b;
    luisa::compute::BufferView<luisa::float4> states;
    std::span<const luisa::compute::BufferView<luisa::float4>> clump_axes;
    std::span<const luisa::compute::BufferView<luisa::float4>> clump_frames;
    std::span<const luisa::compute::BufferView<luisa::uint>> clump_runtime;
    std::span<const luisa::compute::BufferView<luisa::uint>>
        clump_strand_guides;
};

class ClassicCollectionPipeline {
public:
    ClassicCollectionPipeline() noexcept;
    ~ClassicCollectionPipeline();
    ClassicCollectionPipeline(ClassicCollectionPipeline &&) noexcept;
    ClassicCollectionPipeline &operator=(
        ClassicCollectionPipeline &&) noexcept;
    ClassicCollectionPipeline(const ClassicCollectionPipeline &) = delete;
    ClassicCollectionPipeline &operator=(
        const ClassicCollectionPipeline &) = delete;

    [[nodiscard]] std::size_t description_count() const noexcept;
    [[nodiscard]] std::string_view description_name(
        std::size_t description) const;
    [[nodiscard]] bool output_is_points_a(
        std::size_t description, bool base_only = false) const;
    [[nodiscard]] const ClassicCollectionCompileStats &
    compile_stats() const noexcept;

    // Encode one description into an externally owned stream. No implicit
    // synchronization, allocation, upload, or readback is performed.
    void encode(
        luisa::compute::Stream &stream,
        std::size_t description,
        const ClassicCollectionDispatchResources &resources,
        std::uint32_t strand_count,
        bool base_only = false) const;

private:
    struct Impl;
    explicit ClassicCollectionPipeline(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> _impl;

    friend ClassicCollectionPipeline compile_classic_collection(
        luisa::compute::Device &,
        std::span<const ClassicCollectionCompileInput>,
        const ClassicCollectionCompileOptions &);
};

// Compile every description as one batch on a renderer-owned Device. The
// returned pipeline borrows no host input; it owns shaders, not the Device.
[[nodiscard]] ClassicCollectionPipeline compile_classic_collection(
    luisa::compute::Device &device,
    std::span<const ClassicCollectionCompileInput> descriptions,
    const ClassicCollectionCompileOptions &options = {});

} // namespace nanoxgen::luisa_backend
