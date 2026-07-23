#include "nanoxgen/luisa/xgen_classic_collection.h"

#include "nanoxgen/luisa/generate.h"
#include "nanoxgen/luisa/xgen_classic_runtime.h"

#include <luisa/runtime/shader.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <utility>

namespace nanoxgen::luisa_backend {
namespace {

using Clock = std::chrono::steady_clock;
using namespace luisa;
using namespace luisa::compute;

using BaseShader = Shader1D<
    ByteBuffer, Buffer<uint>, ByteBuffer, Buffer<float3>, Buffer<float4>>;
using PrimitiveShader = Shader1D<
    Buffer<float4>, Buffer<float4>, ByteBuffer, Buffer<uint>, Buffer<float>,
    Buffer<float4>>;
using WidthShader = Shader1D<
    Buffer<float4>, ByteBuffer, Buffer<uint>, Buffer<float>, Buffer<float4>>;
using NoiseShader = Shader1D<
    Buffer<float4>, Buffer<float4>, ByteBuffer, Buffer<uint>, Buffer<float>,
    Buffer<float3>, Buffer<float3>, Buffer<float4>>;
using CutShader = PrimitiveShader;
using ClumpShader = Shader1D<
    Buffer<float4>, Buffer<float4>, ByteBuffer, Buffer<uint>, Buffer<float>,
    Buffer<float4>, Buffer<float4>, Buffer<float4>, Buffer<uint>,
    Buffer<uint>>;

double milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

struct TaskTiming {
    Clock::time_point begin;
    Clock::time_point end;
};

} // namespace

struct ClassicCollectionPipeline::Impl {
    struct Description {
        std::string name;
        std::uint32_t cvs_per_strand{};
        std::vector<ClassicFloatEffect> effects;
        std::vector<std::optional<NoiseShader>> noises;
        std::vector<std::optional<CutShader>> cuts;
        std::vector<std::optional<ClumpShader>> clumps;
        std::optional<BaseShader> base;
        std::optional<PrimitiveShader> primitive;
        std::optional<WidthShader> width;
    };

    std::vector<Description> descriptions;
    ClassicCollectionCompileStats stats;
};

ClassicCollectionPipeline::ClassicCollectionPipeline() noexcept = default;
ClassicCollectionPipeline::~ClassicCollectionPipeline() = default;
ClassicCollectionPipeline::ClassicCollectionPipeline(
    ClassicCollectionPipeline &&) noexcept = default;
ClassicCollectionPipeline &ClassicCollectionPipeline::operator=(
    ClassicCollectionPipeline &&) noexcept = default;

ClassicCollectionPipeline::ClassicCollectionPipeline(
    std::unique_ptr<Impl> impl) noexcept
    : _impl{std::move(impl)} {}

std::size_t ClassicCollectionPipeline::description_count() const noexcept {
    return _impl ? _impl->descriptions.size() : 0u;
}

std::string_view ClassicCollectionPipeline::description_name(
    std::size_t description) const {
    if (!_impl || description >= _impl->descriptions.size()) {
        throw std::out_of_range("Classic collection description is invalid");
    }
    return _impl->descriptions[description].name;
}

bool ClassicCollectionPipeline::output_is_points_a(
    std::size_t description, bool base_only) const {
    if (!_impl || description >= _impl->descriptions.size()) {
        throw std::out_of_range("Classic collection description is invalid");
    }
    return base_only ||
        (_impl->descriptions[description].effects.size() % 2u) != 0u;
}

const ClassicCollectionCompileStats &
ClassicCollectionPipeline::compile_stats() const noexcept {
    static const ClassicCollectionCompileStats empty{};
    return _impl ? _impl->stats : empty;
}

void ClassicCollectionPipeline::encode(
    Stream &stream,
    std::size_t description,
    const ClassicCollectionDispatchResources &resources,
    std::uint32_t strand_count,
    bool base_only) const {
    if (!_impl || description >= _impl->descriptions.size()) {
        throw std::out_of_range("Classic collection description is invalid");
    }
    if (strand_count == 0u) {
        throw std::invalid_argument(
            "Classic collection dispatch needs nonzero strands");
    }
    const Impl::Description &pipeline = _impl->descriptions[description];
    if (!pipeline.base || !pipeline.primitive || !pipeline.width ||
        resources.clump_axes.size() != pipeline.clumps.size() ||
        resources.clump_frames.size() != pipeline.clumps.size() ||
        resources.clump_runtime.size() != pipeline.clumps.size() ||
        resources.clump_strand_guides.size() != pipeline.clumps.size()) {
        throw std::invalid_argument(
            "Classic collection dispatch resources are inconsistent");
    }
    stream << (*pipeline.base)(
                  resources.roots, resources.influence_offsets,
                  resources.influences, resources.guides, resources.points_a)
                  .dispatch(strand_count);
    if (base_only) { return; }
    stream << (*pipeline.primitive)(
                  resources.points_a, resources.points_b, resources.roots,
                  resources.root_runtime, resources.runtime_inputs,
                  resources.states)
                  .dispatch(strand_count);
    bool source_is_b = true;
    for (const ClassicFloatEffect effect : pipeline.effects) {
        if (effect.type == ClassicFloatEffectType::Clump) {
            if (effect.module_index >= pipeline.clumps.size() ||
                !pipeline.clumps[effect.module_index]) {
                throw std::logic_error(
                    "Classic collection clump shader is missing");
            }
            const ClumpShader &shader =
                *pipeline.clumps[effect.module_index];
            stream << (source_is_b
                ? shader(
                    resources.points_b, resources.points_a, resources.roots,
                    resources.root_runtime, resources.runtime_inputs,
                    resources.states,
                    resources.clump_axes[effect.module_index],
                    resources.clump_frames[effect.module_index],
                    resources.clump_runtime[effect.module_index],
                    resources.clump_strand_guides[effect.module_index])
                : shader(
                    resources.points_a, resources.points_b, resources.roots,
                    resources.root_runtime, resources.runtime_inputs,
                    resources.states,
                    resources.clump_axes[effect.module_index],
                    resources.clump_frames[effect.module_index],
                    resources.clump_runtime[effect.module_index],
                    resources.clump_strand_guides[effect.module_index]))
                .dispatch(strand_count);
        } else if (effect.type == ClassicFloatEffectType::Noise) {
            if (effect.module_index >= pipeline.noises.size() ||
                !pipeline.noises[effect.module_index]) {
                throw std::logic_error(
                    "Classic collection noise shader is missing");
            }
            const NoiseShader &shader =
                *pipeline.noises[effect.module_index];
            stream << (source_is_b
                ? shader(
                    resources.points_b, resources.points_a, resources.roots,
                    resources.root_runtime, resources.runtime_inputs,
                    resources.surface_tangents,
                    resources.noise_domain_positions, resources.states)
                : shader(
                    resources.points_a, resources.points_b, resources.roots,
                    resources.root_runtime, resources.runtime_inputs,
                    resources.surface_tangents,
                    resources.noise_domain_positions, resources.states))
                .dispatch(strand_count);
        } else {
            if (effect.module_index >= pipeline.cuts.size() ||
                !pipeline.cuts[effect.module_index]) {
                throw std::logic_error(
                    "Classic collection cut shader is missing");
            }
            const CutShader &shader = *pipeline.cuts[effect.module_index];
            stream << (source_is_b
                ? shader(
                    resources.points_b, resources.points_a, resources.roots,
                    resources.root_runtime, resources.runtime_inputs,
                    resources.states)
                : shader(
                    resources.points_a, resources.points_b, resources.roots,
                    resources.root_runtime, resources.runtime_inputs,
                    resources.states))
                .dispatch(strand_count);
        }
        source_is_b = !source_is_b;
    }
    if (output_is_points_a(description)) {
        stream << (*pipeline.width)(
                      resources.points_a, resources.roots,
                      resources.root_runtime, resources.runtime_inputs,
                      resources.states)
                      .dispatch(strand_count);
    } else {
        stream << (*pipeline.width)(
                      resources.points_b, resources.roots,
                      resources.root_runtime, resources.runtime_inputs,
                      resources.states)
                      .dispatch(strand_count);
    }
}

ClassicCollectionPipeline compile_classic_collection(
    Device &device,
    std::span<const ClassicCollectionCompileInput> descriptions,
    const ClassicCollectionCompileOptions &options) {
    if (descriptions.empty()) {
        throw std::invalid_argument(
            "Classic collection compile input is empty");
    }
    auto impl = std::make_unique<ClassicCollectionPipeline::Impl>();
    impl->descriptions.resize(descriptions.size());
    for (std::size_t index = 0u; index < descriptions.size(); ++index) {
        const ClassicCollectionCompileInput &input = descriptions[index];
        if (!input.runtime || !input.runtime->lowering_complete() ||
            input.cvs_per_strand < 2u ||
            input.clump_guide_counts.size() !=
                input.runtime->clumps.size()) {
            throw std::invalid_argument(
                "Classic collection compile input is inconsistent");
        }
        auto &output = impl->descriptions[index];
        output.name = input.runtime->description_name;
        output.cvs_per_strand = input.cvs_per_strand;
        output.effects = input.runtime->effects;
        output.noises.resize(input.runtime->noises.size());
        output.cuts.resize(input.runtime->cuts.size());
        output.clumps.resize(input.runtime->clumps.size());
    }

    const std::size_t hardware_workers = std::max<std::size_t>(
        std::thread::hardware_concurrency(), 1u);
    const std::size_t worker_limit = options.max_parallel_compiles == 0u
        ? hardware_workers
        : options.max_parallel_compiles;
    if (worker_limit == 0u ||
        worker_limit > static_cast<std::size_t>(
            std::numeric_limits<std::ptrdiff_t>::max())) {
        throw std::invalid_argument(
            "Classic collection compile worker limit is invalid");
    }
    std::counting_semaphore<> slots(
        static_cast<std::ptrdiff_t>(worker_limit));
    impl->stats.worker_limit = worker_limit;
    std::mutex timing_mutex;
    std::vector<TaskTiming> timings;
    std::vector<std::future<void>> tasks;
    ShaderOption shader_option{};
    shader_option.enable_cache = options.enable_cache;
    shader_option.enable_fast_math = options.fast_math;
    const Clock::time_point compile_begin = Clock::now();

    const auto launch = [&](auto kernel, auto &shader_slot) {
        tasks.emplace_back(std::async(
            std::launch::async,
            [&, kernel = std::move(kernel),
             slot = &shader_slot]() mutable {
                slots.acquire();
                const Clock::time_point begin = Clock::now();
                try {
                    slot->emplace(device.compile(
                        std::move(kernel), shader_option));
                } catch (...) {
                    slots.release();
                    throw;
                }
                const Clock::time_point end = Clock::now();
                slots.release();
                std::scoped_lock lock{timing_mutex};
                timings.push_back({begin, end});
            }));
    };

    for (std::size_t index = 0u; index < descriptions.size(); ++index) {
        const ClassicCollectionCompileInput &input = descriptions[index];
        const ClassicFloatRuntimePlan &runtime = *input.runtime;
        auto &output = impl->descriptions[index];
        launch(
            make_classic_base_generate_kernel(
                input.cvs_per_strand, 0.0f, 1.0f, input.root_relative),
            output.base);
        launch(
            make_classic_runtime_primitive_kernel(
                runtime, input.cvs_per_strand),
            output.primitive);
        launch(
            make_classic_runtime_width_kernel(
                runtime, input.cvs_per_strand),
            output.width);
        std::vector<bool> scheduled_noises(output.noises.size());
        std::vector<bool> scheduled_cuts(output.cuts.size());
        std::vector<bool> scheduled_clumps(output.clumps.size());
        for (const ClassicFloatEffect effect : runtime.effects) {
            if (effect.type == ClassicFloatEffectType::Noise) {
                auto &slot = output.noises.at(effect.module_index);
                if (!scheduled_noises.at(effect.module_index)) {
                    scheduled_noises[effect.module_index] = true;
                    launch(
                        make_classic_runtime_noise_kernel(
                            runtime, runtime.noises.at(effect.module_index),
                            input.cvs_per_strand),
                        slot);
                }
            } else if (effect.type == ClassicFloatEffectType::Cut) {
                auto &slot = output.cuts.at(effect.module_index);
                if (!scheduled_cuts.at(effect.module_index)) {
                    scheduled_cuts[effect.module_index] = true;
                    launch(
                        make_classic_runtime_cut_kernel(
                            runtime, runtime.cuts.at(effect.module_index),
                            input.cvs_per_strand),
                        slot);
                }
            } else {
                auto &slot = output.clumps.at(effect.module_index);
                if (!scheduled_clumps.at(effect.module_index)) {
                    scheduled_clumps[effect.module_index] = true;
                    launch(
                        make_classic_runtime_clump_kernel(
                            runtime, runtime.clumps.at(effect.module_index),
                            input.cvs_per_strand,
                            input.clump_guide_counts[effect.module_index],
                            input.root_relative),
                        slot);
                }
            }
        }
    }
    for (std::future<void> &task : tasks) { task.get(); }
    const Clock::time_point compile_end = Clock::now();
    impl->stats.kernel_count = timings.size();
    impl->stats.wall_ms = milliseconds(compile_begin, compile_end);
    for (const TaskTiming timing : timings) {
        const double elapsed = milliseconds(timing.begin, timing.end);
        impl->stats.task_sum_ms += elapsed;
        impl->stats.task_max_ms =
            std::max(impl->stats.task_max_ms, elapsed);
    }
    return ClassicCollectionPipeline{std::move(impl)};
}

} // namespace nanoxgen::luisa_backend
