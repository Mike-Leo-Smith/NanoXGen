#include "nanoxgen/curve_cache.h"
#include "nanoxgen/luisa/generate.h"
#include "nanoxgen/luisa/xgen_classic_collection.h"
#include "nanoxgen/luisa/xgen_classic_runtime.h"
#include "nanoxgen/xgen_classic.h"
#include "nanoxgen/xgen_classic_alembic.h"
#include "nanoxgen/xgen_classic_clump.h"
#include "nanoxgen/xgen_classic_collection.h"
#include "nanoxgen/xgen_classic_ptex.h"
#include "nanoxgen/xgen_classic_roots.h"
#include "nanoxgen/xgen_classic_runtime.h"

#include <luisa/runtime/context.h>
#include <luisa/runtime/device.h>
#include <luisa/runtime/stream.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace luisa;
using namespace luisa::compute;

double milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

struct CompileTiming {
    Clock::time_point begin;
    Clock::time_point end;
};

template<typename Shader>
struct TimedShader {
    Shader shader;
    CompileTiming timing;
};

template<typename Kernel>
auto compile_timed(Device &device, Kernel kernel, ShaderOption option) {
    const Clock::time_point begin = Clock::now();
    auto shader = device.compile(kernel, option);
    const Clock::time_point end = Clock::now();
    return TimedShader<decltype(shader)>{
        std::move(shader), {begin, end}};
}

double timing_union_milliseconds(
    std::vector<CompileTiming> timings,
    std::optional<Clock::time_point> clip_begin = std::nullopt,
    std::optional<Clock::time_point> clip_end = std::nullopt) {
    if (timings.empty()) { return 0.0; }
    for (CompileTiming &timing : timings) {
        if (clip_begin) { timing.begin = std::max(timing.begin, *clip_begin); }
        if (clip_end) { timing.end = std::min(timing.end, *clip_end); }
    }
    timings.erase(
        std::remove_if(
            timings.begin(), timings.end(),
            [](const CompileTiming &timing) {
                return timing.end <= timing.begin;
            }),
        timings.end());
    if (timings.empty()) { return 0.0; }
    std::sort(
        timings.begin(), timings.end(),
        [](const CompileTiming &a, const CompileTiming &b) {
            return a.begin < b.begin;
        });
    Clock::time_point begin = timings.front().begin;
    Clock::time_point end = timings.front().end;
    double result = 0.0;
    for (std::size_t index = 1u; index < timings.size(); ++index) {
        if (timings[index].begin <= end) {
            end = std::max(end, timings[index].end);
            continue;
        }
        result += milliseconds(begin, end);
        begin = timings[index].begin;
        end = timings[index].end;
    }
    return result + milliseconds(begin, end);
}

std::uint32_t parse_u32(std::string_view text, const char *label,
                        bool allow_zero = false) {
    std::size_t consumed{};
    const unsigned long value = std::stoul(std::string{text}, &consumed);
    if (consumed != text.size() || (!allow_zero && value == 0u) ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string{label} + " is invalid");
    }
    return static_cast<std::uint32_t>(value);
}

struct Options {
    std::filesystem::path runtime_directory;
    std::string backend;
    std::filesystem::path collection;
    std::filesystem::path archive;
    std::filesystem::path descriptions_root;
    std::string description;
    std::uint32_t warmup{3u};
    std::uint32_t repeats{11u};
    bool base_only{};
    std::uint32_t effect_count{std::numeric_limits<std::uint32_t>::max()};
    std::optional<std::filesystem::path> reference_nxc;
    std::optional<std::filesystem::path> output_nxc;
    bool fast_math{true};
    bool cpu_validation{true};
    std::uint32_t threads{};
};

Options parse_options(int argc, char **argv) {
    if (argc < 6) {
        throw std::invalid_argument(
            "usage: nanoxgen_xgen_classic_luisa_benchmark RUNTIME_DIR "
            "BACKEND COLLECTION.xgen PATCHES.abc DESCRIPTIONS_ROOT "
            "[DESCRIPTION] [--warmup N] [--repeats N] [--base-only] "
            "[--effect-count N] [--threads N] "
            "[--strict-math] "
            "[--no-cpu-validation] "
            "[--reference-nxc FILE] [--output-nxc FILE]\n"
            "Omit DESCRIPTION to execute every description from the "
            "collection on one device.");
    }
    Options result{argv[1], argv[2], argv[3], argv[4], argv[5], {}};
    int first_option = 6;
    if (first_option < argc &&
        !std::string_view{argv[first_option]}.starts_with("--")) {
        result.description = argv[first_option++];
    }
    for (int index = first_option; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--base-only") {
            result.base_only = true;
            continue;
        }
        if (argument == "--strict-math") {
            result.fast_math = false;
            continue;
        }
        if (argument == "--no-cpu-validation") {
            result.cpu_validation = false;
            continue;
        }
        if (argument != "--warmup" && argument != "--repeats" &&
            argument != "--effect-count" && argument != "--reference-nxc" &&
            argument != "--output-nxc" && argument != "--threads") {
            throw std::invalid_argument(
                "unknown argument: " + std::string{argument});
        }
        if (++index >= argc) {
            throw std::invalid_argument(
                "missing value after " + std::string{argument});
        }
        if (argument == "--reference-nxc") {
            result.reference_nxc = argv[index];
        } else if (argument == "--output-nxc") {
            result.output_nxc = argv[index];
        } else if (argument == "--warmup") {
            result.warmup = parse_u32(argv[index], "warmup", true);
        } else if (argument == "--effect-count") {
            result.effect_count = parse_u32(argv[index], "effect count", true);
        } else if (argument == "--threads") {
            result.threads = parse_u32(argv[index], "threads", true);
        } else {
            result.repeats = parse_u32(argv[index], "repeats");
        }
    }
    if (result.description.empty() &&
        (result.reference_nxc || result.output_nxc)) {
        throw std::invalid_argument(
            "collection mode does not accept a single reference/output cache");
    }
    return result;
}

double percentile(std::vector<double> samples, double fraction) {
    std::sort(samples.begin(), samples.end());
    const std::size_t index = std::min(
        samples.size() - 1u,
        static_cast<std::size_t>(fraction * samples.size()));
    return samples[index];
}

std::uint64_t checksum(const nanoxgen::PackedGeneratedCurves &curves) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const nanoxgen::PackedCurvePoint point : curves.points) {
        for (const float value : {point.x, point.y, point.z, point.radius}) {
            hash ^= std::bit_cast<std::uint32_t>(value);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

nanoxgen::PackedGeneratedCurves compact_gpu_output(
    std::span<const luisa::float4> raw_points,
    std::span<const luisa::float4> states,
    const nanoxgen::ClassicRootPlan &root_plan,
    std::uint32_t cvs_per_strand) {
    if (states.size() != root_plan.roots.size() ||
        raw_points.size() != states.size() * cvs_per_strand) {
        throw std::runtime_error("Luisa output dimensions are invalid");
    }
    nanoxgen::PackedGeneratedCurves result{};
    result.cvs_per_strand = cvs_per_strand;
    result.points.reserve(raw_points.size());
    result.roots.reserve(states.size());
    result.root_uvs.reserve(states.size());
    result.point_counts.reserve(states.size());
    for (std::size_t strand = 0u; strand < states.size(); ++strand) {
        if (states[strand].x < 0.0f) { continue; }
        const auto begin = raw_points.begin() + strand * cvs_per_strand;
        const nanoxgen::Vec3 root = root_plan.roots[strand].position;
        for (std::uint32_t cv = 0u; cv < cvs_per_strand; ++cv) {
            const luisa::float4 point = begin[cv];
            result.points.push_back({
                point.x + root.x, point.y + root.y, point.z + root.z,
                point.w});
        }
        result.roots.push_back(root_plan.roots[strand]);
        result.root_uvs.push_back(root_plan.roots[strand].uv);
        result.point_counts.push_back(cvs_per_strand);
    }
    result.strand_count = static_cast<std::uint32_t>(result.roots.size());
    nanoxgen::add_xgen_classic_renderer_endpoints(result);
    return result;
}

struct ErrorStats {
    float position{};
    float radius{};
    std::uint64_t bit_mismatches{};
};

struct ClumpHostData {
    std::vector<luisa::float4> axes;
    std::vector<luisa::float4> frames;
    std::vector<std::uint32_t> runtime;
    std::vector<std::uint32_t> strand_guides;
    std::uint32_t guide_count{};
};

struct DescriptionRunResult {
    std::uint64_t strands{};
    std::uint64_t points{};
    std::uint64_t output_checksum{};
    double cold_ms{};
    double native_ms{};
    double jit_ms{};
    double first_dispatch_ms{};
};

ErrorStats compare(const nanoxgen::PackedGeneratedCurves &a,
                   const nanoxgen::PackedGeneratedCurves &b) {
    if (a.strand_count != b.strand_count ||
        a.point_counts != b.point_counts || a.points.size() != b.points.size()) {
        throw std::runtime_error("Luisa/CPU Classic topology mismatch");
    }
    ErrorStats result{};
    for (std::size_t index = 0u; index < a.points.size(); ++index) {
        const auto &x = a.points[index];
        const auto &y = b.points[index];
        result.position = std::max({result.position,
            std::abs(x.x - y.x), std::abs(x.y - y.y), std::abs(x.z - y.z)});
        result.radius = std::max(result.radius,
                                 std::abs(x.radius - y.radius));
        for (const auto [lhs, rhs] : {std::pair{x.x, y.x},
                                      std::pair{x.y, y.y},
                                      std::pair{x.z, y.z},
                                      std::pair{x.radius, y.radius}}) {
            result.bit_mismatches +=
                std::bit_cast<std::uint32_t>(lhs) !=
                std::bit_cast<std::uint32_t>(rhs);
        }
    }
    return result;
}

ErrorStats compare_cache(const nanoxgen::PackedGeneratedCurves &generated,
                         const nanoxgen::CurveCache &reference) {
    const nanoxgen::CurveCacheView view = reference.view();
    if (view.header().strand_count != generated.strand_count ||
        view.header().point_count != generated.points.size() ||
        !view.face_ids() || !view.face_uvs() ||
        generated.roots.size() != generated.strand_count) {
        throw std::runtime_error("Luisa/renderer-reference topology mismatch");
    }
    using Identity =
        std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
    struct IndexedCurve {
        Identity identity;
        std::uint64_t point_offset{};
        std::uint32_t point_count{};
    };
    std::vector<IndexedCurve> generated_curves;
    std::vector<IndexedCurve> reference_curves;
    generated_curves.reserve(generated.strand_count);
    reference_curves.reserve(generated.strand_count);
    std::uint64_t generated_offset{};
    std::uint64_t reference_offset{};
    for (std::uint32_t strand = 0u; strand < generated.strand_count; ++strand) {
        const nanoxgen::RootSample root = generated.roots[strand];
        generated_curves.push_back({
            {root.surface_face_id, std::bit_cast<std::uint32_t>(root.uv.x),
             std::bit_cast<std::uint32_t>(root.uv.y)},
            generated_offset, generated.point_counts[strand]});
        const nanoxgen::Vec2 uv = view.face_uvs()[strand];
        reference_curves.push_back({
            {view.face_ids()[strand], std::bit_cast<std::uint32_t>(uv.x),
             std::bit_cast<std::uint32_t>(uv.y)},
            reference_offset, view.point_counts()[strand]});
        generated_offset += generated.point_counts[strand];
        reference_offset += view.point_counts()[strand];
    }
    const auto less = [](const IndexedCurve &a, const IndexedCurve &b) {
        return a.identity < b.identity;
    };
    std::sort(generated_curves.begin(), generated_curves.end(), less);
    std::sort(reference_curves.begin(), reference_curves.end(), less);
    ErrorStats result{};
    for (std::size_t strand = 0u; strand < generated_curves.size(); ++strand) {
        if ((strand != 0u &&
             (generated_curves[strand - 1u].identity ==
                  generated_curves[strand].identity ||
              reference_curves[strand - 1u].identity ==
                  reference_curves[strand].identity)) ||
            generated_curves[strand].identity !=
                reference_curves[strand].identity ||
            generated_curves[strand].point_count !=
                reference_curves[strand].point_count) {
            throw std::runtime_error(
                "Luisa/renderer-reference canonical identity mismatch");
        }
        for (std::uint32_t cv = 0u;
             cv < generated_curves[strand].point_count; ++cv) {
            const nanoxgen::PackedCurvePoint &a = generated.points[
                generated_curves[strand].point_offset + cv];
            const nanoxgen::PackedCurvePoint &b = view.points()[
                reference_curves[strand].point_offset + cv];
            result.position = std::max({
                result.position, std::abs(a.x - b.x),
                std::abs(a.y - b.y), std::abs(a.z - b.z)});
            result.radius = std::max(
                result.radius, std::abs(a.radius - b.radius));
            for (const auto [lhs, rhs] : {
                     std::pair{a.x, b.x}, std::pair{a.y, b.y},
                     std::pair{a.z, b.z}, std::pair{a.radius, b.radius}}) {
                result.bit_mismatches +=
                    std::bit_cast<std::uint32_t>(lhs) !=
                    std::bit_cast<std::uint32_t>(rhs);
            }
        }
    }
    return result;
}

struct PreparedCollectionDescription {
    const nanoxgen::ClassicDescription *description{};
    nanoxgen::ClassicFloatRuntimePlan runtime;
    nanoxgen::ClassicAlembicAssetInput imported;
    nanoxgen::ClassicRootPlan roots;
    nanoxgen::ClassicRuntimeInputData runtime_inputs;
    std::vector<nanoxgen::ClassicClumpRuntimeData> clump_data;
    std::vector<luisa::float3> guides;
    std::vector<luisa::float3> tangents;
    std::vector<luisa::float3> noise_domain_positions;
    std::vector<std::uint32_t> root_runtime;
    std::vector<ClumpHostData> clumps;
    std::vector<std::uint32_t> clump_guide_counts;
    std::vector<float> runtime_upload;
};

PreparedCollectionDescription prepare_collection_description(
    const nanoxgen::ClassicDescription &description,
    nanoxgen::ClassicCollectionExecutionDescription host_plan) {
    PreparedCollectionDescription result{};
    result.description = &description;
    result.runtime = std::move(host_plan.runtime);
    result.imported = std::move(host_plan.surface);
    result.roots = std::move(host_plan.roots);
    result.runtime_inputs = std::move(host_plan.runtime_inputs);
    result.clump_data = std::move(host_plan.clumps);
    const std::uint32_t cvs = result.runtime.fx_cv_count;

    const std::vector<nanoxgen::Vec3> &rebuilt = host_plan.rebuilt_guides;
    result.guides.reserve(rebuilt.size());
    for (const nanoxgen::Vec3 value : rebuilt) {
        result.guides.emplace_back(value.x, value.y, value.z);
    }
    result.tangents.reserve(result.roots.surface_tangents.size());
    for (const nanoxgen::Vec3 value : result.roots.surface_tangents) {
        result.tangents.emplace_back(value.x, value.y, value.z);
    }
    result.noise_domain_positions.reserve(
        result.roots.reference_positions.size());
    for (const nanoxgen::Vec3 value : result.roots.reference_positions) {
        result.noise_domain_positions.emplace_back(
            value.x, value.y, value.z);
    }
    result.root_runtime.resize(result.roots.roots.size() * 2u);
    for (std::size_t strand = 0u; strand < result.roots.roots.size();
         ++strand) {
        result.root_runtime[strand * 2u] =
            result.roots.primitive_ids[strand];
        result.root_runtime[strand * 2u + 1u] =
            result.roots.random_prefixes[strand];
    }
    result.clumps.reserve(result.clump_data.size());
    result.clump_guide_counts.reserve(result.clump_data.size());
    for (const nanoxgen::ClassicClumpRuntimeData &binding :
         result.clump_data) {
        ClumpHostData host{};
        host.guide_count = static_cast<std::uint32_t>(
            binding.guide_axes.size() / cvs);
        if (binding.guide_render_axes.size() != binding.guide_axes.size() ||
            binding.guide_local_axes.size() != binding.guide_axes.size() ||
            binding.guide_local_render_axes.size() !=
                binding.guide_axes.size() ||
            binding.guide_spline_lengths.size() != host.guide_count) {
            throw std::runtime_error(
                "Classic clump binding has no prepared local render guides");
        }
        host.axes.reserve(binding.guide_local_render_axes.size());
        for (std::uint32_t guide = 0u; guide < host.guide_count; ++guide) {
            float distance = 0.0f;
            for (std::uint32_t cv = 0u; cv < cvs; ++cv) {
                const std::size_t index =
                    static_cast<std::size_t>(guide) * cvs + cv;
                if (cv != 0u) {
                    const nanoxgen::Vec3 delta =
                        binding.guide_local_axes[index] -
                        binding.guide_local_axes[index - 1u];
                    distance += std::sqrt(
                        delta.x * delta.x + delta.y * delta.y +
                        delta.z * delta.z);
                }
                const nanoxgen::Vec3 value =
                    binding.guide_local_render_axes[index];
                host.axes.emplace_back(
                    value.x, value.y, value.z, distance);
            }
        }
        host.frames.reserve(static_cast<std::size_t>(
            host.guide_count) * 4u);
        host.runtime.reserve(static_cast<std::size_t>(
            host.guide_count) * 2u);
        for (std::uint32_t guide = 0u; guide < host.guide_count; ++guide) {
            const nanoxgen::Vec3 normal = binding.guide_normals[guide];
            const nanoxgen::Vec3 tangent = binding.guide_tangents[guide];
            const nanoxgen::Vec2 uv = binding.guide_uvs[guide];
            host.frames.emplace_back(normal.x, normal.y, normal.z, uv.x);
            host.frames.emplace_back(tangent.x, tangent.y, tangent.z, uv.y);
            const nanoxgen::Vec3 domain_position =
                binding.guide_reference_positions.empty()
                ? binding.guide_axes[
                    static_cast<std::size_t>(guide) * cvs]
                : binding.guide_reference_positions[guide];
            host.frames.emplace_back(
                domain_position.x, domain_position.y, domain_position.z,
                binding.guide_spline_lengths[guide]);
            const nanoxgen::Vec3 guide_root = binding.guide_axes[
                static_cast<std::size_t>(guide) * cvs];
            host.frames.emplace_back(
                guide_root.x, guide_root.y, guide_root.z, 0.0f);
            host.runtime.push_back(binding.guide_face_ids[guide]);
            host.runtime.push_back(binding.guide_random_prefixes[guide]);
        }
        host.strand_guides = binding.strand_guide_indices;
        result.clump_guide_counts.push_back(host.guide_count);
        result.clumps.emplace_back(std::move(host));
    }
    result.runtime_upload = result.runtime_inputs.values;
    if (result.runtime_upload.empty()) {
        result.runtime_upload.push_back(0.0f);
    }
    return result;
}

struct CollectionDeviceResources {
    ByteBuffer roots;
    Buffer<std::uint32_t> offsets;
    ByteBuffer influences;
    Buffer<luisa::float3> guides;
    Buffer<std::uint32_t> root_runtime;
    Buffer<float> runtime_inputs;
    Buffer<luisa::float3> tangents;
    Buffer<luisa::float3> noise_domain_positions;
    Buffer<luisa::float4> points_a;
    Buffer<luisa::float4> points_b;
    Buffer<luisa::float4> states;
    std::vector<Buffer<luisa::float4>> clump_axes;
    std::vector<Buffer<luisa::float4>> clump_frames;
    std::vector<Buffer<std::uint32_t>> clump_runtime;
    std::vector<Buffer<std::uint32_t>> clump_strand_guides;
    std::vector<BufferView<luisa::float4>> clump_axis_views;
    std::vector<BufferView<luisa::float4>> clump_frame_views;
    std::vector<BufferView<std::uint32_t>> clump_runtime_views;
    std::vector<BufferView<std::uint32_t>> clump_strand_guide_views;
    std::vector<luisa::float4> raw_points;
    std::vector<luisa::float4> raw_states;
};

int run_collection_mode(
    const Options &options,
    Device &device,
    Stream &stream,
    const nanoxgen::ClassicCollection &collection,
    Clock::time_point process_begin,
    Clock::time_point device_end,
    Clock::time_point collection_load_end) {
    const Clock::time_point native_begin = Clock::now();
    nanoxgen::NanoXGenContext context{
        options.threads == 0u
            ? nanoxgen::available_worker_count()
            : static_cast<std::size_t>(options.threads)};
    nanoxgen::ClassicCollectionExecutionOptions host_options{};
    host_options.effect_count = options.effect_count;
    host_options.context = &context;
    nanoxgen::ClassicCollectionExecutionPlan host_plan =
        nanoxgen::build_xgen_classic_collection_execution_plan(
            collection, options.collection, options.archive,
            options.descriptions_root, host_options);
    const std::size_t host_worker_count =
        host_plan.context_worker_count;
    std::vector<PreparedCollectionDescription> prepared;
    prepared.reserve(host_plan.descriptions.size());
    for (std::size_t index = 0u;
         index < host_plan.descriptions.size(); ++index) {
        const nanoxgen::ClassicDescription &description =
            collection.descriptions.at(index);
        if (host_plan.descriptions[index].name != description.name) {
            throw std::runtime_error(
                "Classic collection host plan changed description order");
        }
        prepared.push_back(prepare_collection_description(
            description, std::move(host_plan.descriptions[index])));
    }
    const Clock::time_point native_end = Clock::now();

    std::vector<nanoxgen::luisa_backend::ClassicCollectionCompileInput>
        compile_inputs;
    compile_inputs.reserve(prepared.size());
    for (const PreparedCollectionDescription &description : prepared) {
        compile_inputs.push_back({
            &description.runtime, description.runtime.fx_cv_count,
            description.clump_guide_counts, true});
    }
    nanoxgen::luisa_backend::ClassicCollectionCompileOptions compile_options{};
    compile_options.fast_math = options.fast_math;
    compile_options.enable_cache = false;
    compile_options.context = &context;
    auto pipeline = nanoxgen::luisa_backend::compile_classic_collection(
        device, compile_inputs, compile_options);
    const Clock::time_point compile_end = Clock::now();

    const Clock::time_point allocate_begin = Clock::now();
    std::vector<CollectionDeviceResources> resources(prepared.size());
    for (std::size_t index = 0u; index < prepared.size(); ++index) {
        const PreparedCollectionDescription &host = prepared[index];
        CollectionDeviceResources &gpu = resources[index];
        const std::size_t strands = host.roots.roots.size();
        const std::size_t points =
            strands * host.runtime.fx_cv_count;
        gpu.roots = device.create_byte_buffer(
            strands * sizeof(nanoxgen::RootSample));
        gpu.offsets = device.create_buffer<std::uint32_t>(
            host.roots.influence_offsets.size());
        gpu.influences = device.create_byte_buffer(
            host.roots.influences.size() *
            sizeof(nanoxgen::ClassicGuideInfluence));
        gpu.guides = device.create_buffer<luisa::float3>(
            host.guides.size());
        gpu.root_runtime = device.create_buffer<std::uint32_t>(
            host.root_runtime.size());
        gpu.runtime_inputs = device.create_buffer<float>(
            host.runtime_upload.size());
        gpu.tangents = device.create_buffer<luisa::float3>(
            host.tangents.size());
        gpu.noise_domain_positions = device.create_buffer<luisa::float3>(
            host.noise_domain_positions.size());
        gpu.points_a = device.create_buffer<luisa::float4>(points);
        gpu.points_b = device.create_buffer<luisa::float4>(points);
        gpu.states = device.create_buffer<luisa::float4>(strands);
        for (const ClumpHostData &clump : host.clumps) {
            gpu.clump_axes.emplace_back(
                device.create_buffer<luisa::float4>(clump.axes.size()));
            gpu.clump_frames.emplace_back(
                device.create_buffer<luisa::float4>(clump.frames.size()));
            gpu.clump_runtime.emplace_back(
                device.create_buffer<std::uint32_t>(clump.runtime.size()));
            gpu.clump_strand_guides.emplace_back(
                device.create_buffer<std::uint32_t>(
                    clump.strand_guides.size()));
        }
        for (const auto &buffer : gpu.clump_axes) {
            gpu.clump_axis_views.push_back(buffer.view());
        }
        for (const auto &buffer : gpu.clump_frames) {
            gpu.clump_frame_views.push_back(buffer.view());
        }
        for (const auto &buffer : gpu.clump_runtime) {
            gpu.clump_runtime_views.push_back(buffer.view());
        }
        for (const auto &buffer : gpu.clump_strand_guides) {
            gpu.clump_strand_guide_views.push_back(buffer.view());
        }
        gpu.raw_points.resize(points);
        gpu.raw_states.assign(
            strands, luisa::make_float4(0.0f));
    }
    const Clock::time_point allocate_end = Clock::now();

    for (std::size_t index = 0u; index < prepared.size(); ++index) {
        const PreparedCollectionDescription &host = prepared[index];
        CollectionDeviceResources &gpu = resources[index];
        stream << gpu.roots.copy_from(host.roots.roots.data())
               << gpu.offsets.copy_from(
                      host.roots.influence_offsets.data())
               << gpu.influences.copy_from(host.roots.influences.data())
               << gpu.guides.copy_from(host.guides.data())
               << gpu.root_runtime.copy_from(host.root_runtime.data())
               << gpu.runtime_inputs.copy_from(host.runtime_upload.data())
               << gpu.tangents.copy_from(host.tangents.data())
               << gpu.noise_domain_positions.copy_from(
                      host.noise_domain_positions.data());
        for (std::size_t module = 0u; module < host.clumps.size(); ++module) {
            stream << gpu.clump_axes[module].copy_from(
                          host.clumps[module].axes.data())
                   << gpu.clump_frames[module].copy_from(
                          host.clumps[module].frames.data())
                   << gpu.clump_runtime[module].copy_from(
                          host.clumps[module].runtime.data())
                   << gpu.clump_strand_guides[module].copy_from(
                          host.clumps[module].strand_guides.data());
        }
    }
    stream << synchronize();
    const Clock::time_point upload_end = Clock::now();

    const auto encode_collection = [&] {
        for (std::size_t index = 0u; index < prepared.size(); ++index) {
            const PreparedCollectionDescription &host = prepared[index];
            CollectionDeviceResources &gpu = resources[index];
            const nanoxgen::luisa_backend::ClassicCollectionDispatchResources
                bindings{
                    gpu.roots.view(), gpu.offsets.view(),
                    gpu.influences.view(), gpu.guides.view(),
                    gpu.root_runtime.view(), gpu.runtime_inputs.view(),
                    gpu.tangents.view(), gpu.noise_domain_positions.view(),
                    gpu.points_a.view(), gpu.points_b.view(),
                    gpu.states.view(), gpu.clump_axis_views,
                    gpu.clump_frame_views, gpu.clump_runtime_views,
                    gpu.clump_strand_guide_views};
            pipeline.encode(
                stream, index, bindings,
                static_cast<std::uint32_t>(host.roots.roots.size()),
                options.base_only);
        }
    };
    const Clock::time_point first_begin = Clock::now();
    encode_collection();
    for (std::size_t index = 0u; index < prepared.size(); ++index) {
        CollectionDeviceResources &gpu = resources[index];
        const bool output_a = pipeline.output_is_points_a(
            index, options.base_only);
        if (output_a) {
            stream << gpu.points_a.copy_to(luisa::span{gpu.raw_points});
        } else {
            stream << gpu.points_b.copy_to(luisa::span{gpu.raw_points});
        }
        if (!options.base_only) {
            stream << gpu.states.copy_to(luisa::span{gpu.raw_states});
        }
    }
    stream << synchronize();
    std::vector<nanoxgen::PackedGeneratedCurves> outputs;
    outputs.reserve(prepared.size());
    for (std::size_t index = 0u; index < prepared.size(); ++index) {
        outputs.push_back(compact_gpu_output(
            resources[index].raw_points, resources[index].raw_states,
            prepared[index].roots, prepared[index].runtime.fx_cv_count));
    }
    const Clock::time_point first_end = Clock::now();

    for (std::uint32_t repeat = 0u; repeat < options.warmup; ++repeat) {
        encode_collection();
        stream << synchronize();
    }
    std::vector<double> warm_samples;
    warm_samples.reserve(options.repeats);
    for (std::uint32_t repeat = 0u; repeat < options.repeats; ++repeat) {
        const Clock::time_point begin = Clock::now();
        encode_collection();
        stream << synchronize();
        warm_samples.push_back(milliseconds(begin, Clock::now()));
    }

    std::uint64_t total_strands{};
    std::uint64_t total_points{};
    std::uint64_t collection_checksum = 1469598103934665603ull;
    for (std::size_t index = 0u; index < outputs.size(); ++index) {
        const auto &output = outputs[index];
        const std::uint64_t output_checksum = checksum(output);
        if (options.cpu_validation) {
            nanoxgen::PackedGeneratedCurves cpu =
                nanoxgen::generate_xgen_classic_base_curves_cpu(
                    prepared[index].imported.asset,
                    prepared[index].roots,
                    prepared[index].runtime.fx_cv_count,
                    0.0f, 1.0f, true);
            if (!options.base_only) {
                nanoxgen::apply_xgen_classic_float_runtime_plan_cpu(
                    cpu, prepared[index].runtime, 1.0f,
                    prepared[index].roots.surface_tangents,
                    prepared[index].roots.random_prefixes,
                    prepared[index].roots.primitive_ids,
                    prepared[index].clump_data,
                    prepared[index].runtime_inputs.values,
                    prepared[index].roots.reference_positions, true);
            }
            nanoxgen::make_xgen_classic_curves_world_space(cpu);
            nanoxgen::add_xgen_classic_renderer_endpoints(cpu);
            (void)compare(output, cpu);
        }
        total_strands += output.strand_count;
        total_points += output.points.size();
        collection_checksum ^= output_checksum;
        collection_checksum *= 1099511628211ull;
        std::cout << std::setprecision(9)
                  << "{\"backend\":\"" << options.backend
                  << "\",\"description\":\""
                  << prepared[index].description->name
                  << "\",\"output_strands\":" << output.strand_count
                  << ",\"output_points\":" << output.points.size()
                  << ",\"checksum\":" << output_checksum
                  << ",\"fallback_count\":0}\n";
    }
    const auto &compile_stats = pipeline.compile_stats();
    std::cout << std::setprecision(9)
              << "{\"collection_summary\":true,\"backend\":\""
              << options.backend << "\",\"description_count\":"
              << prepared.size() << ",\"single_device\":true"
              << ",\"external_device_api\":true"
              << ",\"parallel_host_across_descriptions\":"
              << (host_worker_count > 1u ? "true" : "false")
              << ",\"parallel_jit_across_descriptions\":"
              << (compile_stats.worker_limit > 1u
                      ? "true" : "false")
              << ",\"shader_cache\":false,\"strands\":"
              << total_strands << ",\"points\":" << total_points
              << ",\"device_create_ms\":"
              << milliseconds(process_begin, device_end)
              << ",\"collection_parse_ms\":"
              << milliseconds(device_end, collection_load_end)
              << ",\"native_prepare_ms\":"
              << milliseconds(native_begin, native_end)
              << ",\"jit_compile_wall_ms\":" << compile_stats.wall_ms
              << ",\"context_workers\":" << context.worker_count()
              << ",\"host_workers\":" << host_worker_count
              << ",\"jit_workers\":" << compile_stats.worker_limit
              << ",\"jit_kernel_count\":" << compile_stats.kernel_count
              << ",\"jit_task_sum_ms\":" << compile_stats.task_sum_ms
              << ",\"jit_task_max_ms\":" << compile_stats.task_max_ms
              << ",\"buffer_allocate_ms\":"
              << milliseconds(allocate_begin, allocate_end)
              << ",\"upload_ms\":" << milliseconds(compile_end, upload_end)
              << ",\"first_dispatch_download_pack_ms\":"
              << milliseconds(first_begin, first_end)
              << ",\"warm_median_ms\":"
              << percentile(warm_samples, 0.5)
              << ",\"warm_p90_ms\":" << percentile(warm_samples, 0.9)
              << ",\"cold_end_to_end_ms\":"
              << milliseconds(process_begin, first_end)
              << ",\"checksum\":" << collection_checksum << "}\n";
    return 0;
}

} // namespace

int main(int argc, char **argv) try {
    const Options options = parse_options(argc, argv);
    const Clock::time_point process_begin = Clock::now();
    Context context{options.runtime_directory.c_str()};
    Device device = context.create_device(options.backend.c_str());
    if (device.backend_name() != options.backend) {
        throw std::runtime_error("Luisa loaded an unexpected backend");
    }
    Stream stream = device.create_stream();
    const Clock::time_point device_end = Clock::now();

    const nanoxgen::ClassicCollection collection =
        nanoxgen::load_xgen_classic_collection(options.collection);
    const Clock::time_point collection_load_end = Clock::now();
    const bool collection_mode = options.description.empty();
    if (collection_mode) {
        return run_collection_mode(
            options, device, stream, collection,
            process_begin, device_end, collection_load_end);
    }
    const auto run_description = [&](
        const nanoxgen::ClassicDescription *description,
        bool include_shared_start) -> DescriptionRunResult {
    const Clock::time_point description_begin = Clock::now();
    const Clock::time_point cold_begin =
        include_shared_start ? process_begin : description_begin;
    const Clock::time_point native_begin =
        include_shared_start ? device_end : description_begin;
    const Clock::time_point runtime_plan_begin = Clock::now();
    nanoxgen::ClassicFloatRuntimePlan runtime =
        nanoxgen::compile_xgen_classic_float_runtime_plan(
            *description, collection.palette_attributes);
    if (!runtime.lowering_complete()) {
        throw std::runtime_error(
            "description needs fallback: " + runtime.fallback_reasons.front());
    }
    const Clock::time_point runtime_plan_end = Clock::now();
    const std::uint32_t cvs = runtime.fx_cv_count;
    const std::size_t active_effect_count = std::min<std::size_t>(
        options.effect_count, runtime.effects.size());

    ShaderOption shader_option{};
    shader_option.enable_cache = false;
    shader_option.enable_fast_math = options.fast_math;
    // All supported Luisa backends are validated for concurrent compilation.
    // The renderer-facing collection API applies a native-thread-count
    // semaphore; this legacy single-description path has at most one task per
    // specialized kernel.
    constexpr std::launch jit_launch = std::launch::async;
    using NoiseShader = Shader1D<Buffer<luisa::float4>, Buffer<luisa::float4>,
        ByteBuffer, Buffer<std::uint32_t>, Buffer<float>,
        Buffer<luisa::float3>, Buffer<luisa::float3>,
        Buffer<luisa::float4>>;
    using CutShader = Shader1D<Buffer<luisa::float4>, Buffer<luisa::float4>,
        ByteBuffer, Buffer<std::uint32_t>, Buffer<float>,
        Buffer<luisa::float4>>;
    using ClumpShader = Shader1D<
        Buffer<luisa::float4>, Buffer<luisa::float4>, ByteBuffer,
        Buffer<std::uint32_t>, Buffer<float>, Buffer<luisa::float4>,
        Buffer<luisa::float4>, Buffer<luisa::float4>,
        Buffer<std::uint32_t>, Buffer<std::uint32_t>>;

    const nanoxgen::ClassicAlembicAssetInput imported =
        nanoxgen::build_xgen_classic_alembic_asset_input(
            *description, options.archive);
    const Clock::time_point alembic_import_end = Clock::now();
    const nanoxgen::ClassicRootPlan root_plan =
        nanoxgen::build_xgen_classic_random_root_plan(
            *description, imported,
            options.descriptions_root / description->name);
    const Clock::time_point root_plan_end = Clock::now();
    if (root_plan.roots.empty() || root_plan.influence_offsets.empty()) {
        throw std::runtime_error("Classic root plan has no guide associations");
    }
    if (description->patches.empty()) {
        throw std::runtime_error("Classic PTEX runtime needs one patch");
    }
    const nanoxgen::ClassicRuntimeInputData runtime_inputs =
        nanoxgen::build_xgen_classic_runtime_input_data(
            runtime, options.descriptions_root / description->name,
            description->patches.front().name, root_plan);
    const Clock::time_point runtime_inputs_end = Clock::now();
    if (root_plan.primitive_ids.size() != root_plan.roots.size() ||
        root_plan.random_prefixes.size() != root_plan.roots.size() ||
        root_plan.surface_tangents.size() != root_plan.roots.size() ||
        root_plan.reference_positions.size() != root_plan.roots.size() ||
        root_plan.influence_offsets.size() != root_plan.roots.size() + 1u) {
        throw std::runtime_error("Classic root plan metadata is inconsistent");
    }
    const std::vector<nanoxgen::ClassicClumpRuntimeData> clump_data =
        nanoxgen::build_xgen_classic_clump_runtime_data_parallel(
            *description, imported,
            options.descriptions_root / description->name,
            root_plan, runtime, cvs);
    const Clock::time_point clump_data_end = Clock::now();
    runtime.effects.resize(active_effect_count);
    const std::vector<nanoxgen::Vec3> rebuilt =
        nanoxgen::rebuild_xgen_classic_guides_for_device(imported.asset, cvs);
    std::vector<luisa::float3> rebuilt_gpu;
    rebuilt_gpu.reserve(rebuilt.size());
    for (const nanoxgen::Vec3 value : rebuilt) {
        rebuilt_gpu.emplace_back(value.x, value.y, value.z);
    }
    const Clock::time_point guide_rebuild_end = Clock::now();
    std::vector<luisa::float3> tangents;
    tangents.reserve(root_plan.surface_tangents.size());
    for (const nanoxgen::Vec3 value : root_plan.surface_tangents) {
        tangents.emplace_back(value.x, value.y, value.z);
    }
    std::vector<luisa::float3> noise_domain_positions;
    noise_domain_positions.reserve(root_plan.reference_positions.size());
    for (const nanoxgen::Vec3 value : root_plan.reference_positions) {
        noise_domain_positions.emplace_back(value.x, value.y, value.z);
    }
    std::vector<std::uint32_t> root_runtime(root_plan.roots.size() * 2u);
    for (std::size_t strand = 0u; strand < root_plan.roots.size(); ++strand) {
        root_runtime[strand * 2u] = root_plan.primitive_ids[strand];
        root_runtime[strand * 2u + 1u] = root_plan.random_prefixes[strand];
    }
    std::vector<ClumpHostData> clump_host;
    clump_host.reserve(clump_data.size());
    for (const nanoxgen::ClassicClumpRuntimeData &binding : clump_data) {
        ClumpHostData host{};
        host.guide_count = static_cast<std::uint32_t>(
            binding.guide_axes.size() / cvs);
        if (binding.guide_render_axes.size() != binding.guide_axes.size() ||
            binding.guide_local_axes.size() != binding.guide_axes.size() ||
            binding.guide_local_render_axes.size() !=
                binding.guide_axes.size() ||
            binding.guide_spline_lengths.size() != host.guide_count) {
            throw std::runtime_error(
                "Classic clump binding has no prepared local render guides");
        }
        host.axes.reserve(binding.guide_local_render_axes.size());
        for (std::uint32_t guide = 0u; guide < host.guide_count; ++guide) {
            float distance = 0.0f;
            for (std::uint32_t cv = 0u; cv < cvs; ++cv) {
                const std::size_t index =
                    static_cast<std::size_t>(guide) * cvs + cv;
                if (cv != 0u) {
                    const nanoxgen::Vec3 delta =
                        binding.guide_local_axes[index] -
                        binding.guide_local_axes[index - 1u];
                    distance += std::sqrt(
                        delta.x * delta.x + delta.y * delta.y +
                        delta.z * delta.z);
                }
                const nanoxgen::Vec3 value =
                    binding.guide_local_render_axes[index];
                host.axes.emplace_back(
                    value.x, value.y, value.z, distance);
            }
        }
        host.frames.reserve(static_cast<std::size_t>(host.guide_count) * 4u);
        host.runtime.reserve(static_cast<std::size_t>(host.guide_count) * 2u);
        for (std::uint32_t guide = 0u; guide < host.guide_count; ++guide) {
            const nanoxgen::Vec3 normal = binding.guide_normals[guide];
            const nanoxgen::Vec3 tangent = binding.guide_tangents[guide];
            const nanoxgen::Vec2 uv = binding.guide_uvs[guide];
            host.frames.emplace_back(normal.x, normal.y, normal.z, uv.x);
            host.frames.emplace_back(tangent.x, tangent.y, tangent.z, uv.y);
            const nanoxgen::Vec3 domain_position =
                binding.guide_reference_positions.empty()
                ? binding.guide_axes[static_cast<std::size_t>(guide) * cvs]
                : binding.guide_reference_positions[guide];
            host.frames.emplace_back(
                domain_position.x, domain_position.y, domain_position.z,
                binding.guide_spline_lengths[guide]);
            const nanoxgen::Vec3 guide_root =
                binding.guide_axes[static_cast<std::size_t>(guide) * cvs];
            host.frames.emplace_back(
                guide_root.x, guide_root.y, guide_root.z, 0.0f);
            host.runtime.push_back(binding.guide_face_ids[guide]);
            host.runtime.push_back(binding.guide_random_prefixes[guide]);
        }
        host.strand_guides = binding.strand_guide_indices;
        clump_host.emplace_back(std::move(host));
    }
    const Clock::time_point native_prepare_end = Clock::now();

    auto base_kernel =
        nanoxgen::luisa_backend::make_classic_base_generate_kernel(
            cvs, 0.0f, 1.0f, true);
    auto primitive_kernel =
        nanoxgen::luisa_backend::make_classic_runtime_primitive_kernel(
            runtime, cvs);
    auto width_kernel =
        nanoxgen::luisa_backend::make_classic_runtime_width_kernel(
            runtime, cvs);
    auto base_future = std::async(
        jit_launch,
        [&device, shader_option, kernel = std::move(base_kernel)]() mutable {
            return compile_timed(
                device, std::move(kernel), shader_option);
        });
    auto primitive_future = std::async(
        jit_launch,
        [&device, shader_option,
         kernel = std::move(primitive_kernel)]() mutable {
            return compile_timed(
                device, std::move(kernel), shader_option);
        });
    auto width_future = std::async(
        jit_launch,
        [&device, shader_option, kernel = std::move(width_kernel)]() mutable {
            return compile_timed(
                device, std::move(kernel), shader_option);
        });
    std::vector<std::optional<NoiseShader>> noises(runtime.noises.size());
    std::vector<std::optional<CutShader>> cuts(runtime.cuts.size());
    std::vector<std::optional<ClumpShader>> clumps(runtime.clumps.size());
    std::vector<std::optional<std::future<TimedShader<NoiseShader>>>>
        noise_futures(runtime.noises.size());
    std::vector<std::optional<std::future<TimedShader<CutShader>>>>
        cut_futures(runtime.cuts.size());
    std::vector<std::optional<std::future<TimedShader<ClumpShader>>>>
        clump_futures(runtime.clumps.size());
    for (const nanoxgen::ClassicFloatEffect effect : runtime.effects) {
        if (effect.type == nanoxgen::ClassicFloatEffectType::Noise) {
            auto &future = noise_futures.at(effect.module_index);
            if (!future) {
                auto kernel =
                    nanoxgen::luisa_backend::make_classic_runtime_noise_kernel(
                        runtime, runtime.noises[effect.module_index], cvs);
                future.emplace(std::async(
                    jit_launch,
                    [&device, shader_option,
                     kernel = std::move(kernel)]() mutable {
                        return compile_timed(
                            device, std::move(kernel), shader_option);
                    }));
            }
        } else if (effect.type == nanoxgen::ClassicFloatEffectType::Cut) {
            auto &future = cut_futures.at(effect.module_index);
            if (!future) {
                auto kernel =
                    nanoxgen::luisa_backend::make_classic_runtime_cut_kernel(
                        runtime, runtime.cuts[effect.module_index], cvs);
                future.emplace(std::async(
                    jit_launch,
                    [&device, shader_option,
                     kernel = std::move(kernel)]() mutable {
                        return compile_timed(
                            device, std::move(kernel), shader_option);
                    }));
            }
        } else {
            auto &future = clump_futures.at(effect.module_index);
            if (!future) {
                auto kernel =
                    nanoxgen::luisa_backend::make_classic_runtime_clump_kernel(
                        runtime, runtime.clumps[effect.module_index], cvs,
                        clump_host[effect.module_index].guide_count, true);
                future.emplace(std::async(
                    jit_launch,
                    [&device, shader_option,
                     kernel = std::move(kernel)]() mutable {
                        return compile_timed(
                            device, std::move(kernel), shader_option);
                    }));
            }
        }
    }

    const Clock::time_point device_buffer_allocate_begin = Clock::now();
    const std::size_t strand_count = root_plan.roots.size();
    const std::size_t point_count = strand_count * cvs;
    ByteBuffer roots = device.create_byte_buffer(
        strand_count * sizeof(nanoxgen::RootSample));
    Buffer<std::uint32_t> offsets = device.create_buffer<std::uint32_t>(
        root_plan.influence_offsets.size());
    ByteBuffer influences = device.create_byte_buffer(
        root_plan.influences.size() * sizeof(nanoxgen::ClassicGuideInfluence));
    Buffer<luisa::float3> guides =
        device.create_buffer<luisa::float3>(rebuilt_gpu.size());
    Buffer<std::uint32_t> runtime_data =
        device.create_buffer<std::uint32_t>(root_runtime.size());
    std::vector<float> ptex_upload = runtime_inputs.values;
    if (ptex_upload.empty()) { ptex_upload.push_back(0.0f); }
    Buffer<float> ptex_buffer =
        device.create_buffer<float>(ptex_upload.size());
    Buffer<luisa::float3> tangent_buffer =
        device.create_buffer<luisa::float3>(tangents.size());
    Buffer<luisa::float3> noise_domain_buffer =
        device.create_buffer<luisa::float3>(noise_domain_positions.size());
    Buffer<luisa::float4> a = device.create_buffer<luisa::float4>(point_count);
    Buffer<luisa::float4> b = device.create_buffer<luisa::float4>(point_count);
    Buffer<luisa::float4> states =
        device.create_buffer<luisa::float4>(strand_count);
    std::vector<Buffer<luisa::float4>> clump_axes;
    std::vector<Buffer<luisa::float4>> clump_frames;
    std::vector<Buffer<std::uint32_t>> clump_runtime;
    std::vector<Buffer<std::uint32_t>> clump_strand_guides;
    clump_axes.reserve(clump_host.size());
    clump_frames.reserve(clump_host.size());
    clump_runtime.reserve(clump_host.size());
    clump_strand_guides.reserve(clump_host.size());
    for (const ClumpHostData &host : clump_host) {
        clump_axes.emplace_back(
            device.create_buffer<luisa::float4>(host.axes.size()));
        clump_frames.emplace_back(
            device.create_buffer<luisa::float4>(host.frames.size()));
        clump_runtime.emplace_back(
            device.create_buffer<std::uint32_t>(host.runtime.size()));
        clump_strand_guides.emplace_back(
            device.create_buffer<std::uint32_t>(host.strand_guides.size()));
    }
    const Clock::time_point device_buffer_allocate_end = Clock::now();

    const Clock::time_point jit_wait_begin = Clock::now();
    std::vector<CompileTiming> compile_timings;
    auto timed_base = base_future.get();
    compile_timings.push_back(timed_base.timing);
    auto base = std::move(timed_base.shader);
    auto timed_primitive = primitive_future.get();
    compile_timings.push_back(timed_primitive.timing);
    auto primitive = std::move(timed_primitive.shader);
    auto timed_width = width_future.get();
    compile_timings.push_back(timed_width.timing);
    auto width = std::move(timed_width.shader);
    for (std::size_t module = 0u; module < noise_futures.size(); ++module) {
        if (noise_futures[module]) {
            auto timed = noise_futures[module]->get();
            compile_timings.push_back(timed.timing);
            noises[module].emplace(std::move(timed.shader));
        }
    }
    for (std::size_t module = 0u; module < cut_futures.size(); ++module) {
        if (cut_futures[module]) {
            auto timed = cut_futures[module]->get();
            compile_timings.push_back(timed.timing);
            cuts[module].emplace(std::move(timed.shader));
        }
    }
    for (std::size_t module = 0u; module < clump_futures.size(); ++module) {
        if (clump_futures[module]) {
            auto timed = clump_futures[module]->get();
            compile_timings.push_back(timed.timing);
            clumps[module].emplace(std::move(timed.shader));
        }
    }
    const Clock::time_point compile_end = Clock::now();
    double jit_compile_task_sum_ms = 0.0;
    double jit_compile_task_max_ms = 0.0;
    for (const CompileTiming timing : compile_timings) {
        const double elapsed = milliseconds(timing.begin, timing.end);
        jit_compile_task_sum_ms += elapsed;
        jit_compile_task_max_ms =
            std::max(jit_compile_task_max_ms, elapsed);
    }
    const double jit_compile_active_wall_ms =
        timing_union_milliseconds(compile_timings);
    const double jit_device_buffer_overlap_ms = timing_union_milliseconds(
        compile_timings, device_buffer_allocate_begin,
        device_buffer_allocate_end);

    stream << roots.copy_from(root_plan.roots.data())
           << offsets.copy_from(root_plan.influence_offsets.data())
           << influences.copy_from(root_plan.influences.data())
           << guides.copy_from(rebuilt_gpu.data())
           << runtime_data.copy_from(root_runtime.data())
           << ptex_buffer.copy_from(ptex_upload.data())
           << tangent_buffer.copy_from(tangents.data())
           << noise_domain_buffer.copy_from(noise_domain_positions.data());
    for (std::size_t module = 0u; module < clump_host.size(); ++module) {
        stream << clump_axes[module].copy_from(clump_host[module].axes.data())
               << clump_frames[module].copy_from(
                      clump_host[module].frames.data())
               << clump_runtime[module].copy_from(
                      clump_host[module].runtime.data())
               << clump_strand_guides[module].copy_from(
                      clump_host[module].strand_guides.data());
    }
    stream << synchronize();
    const Clock::time_point upload_end = Clock::now();

    bool final_is_a = (runtime.effects.size() % 2u) != 0u;
    const auto dispatch = [&] {
        stream << base(roots, offsets, influences, guides, a)
                      .dispatch(static_cast<std::uint32_t>(strand_count));
        if (options.base_only) { return; }
        stream << primitive(a, b, roots, runtime_data, ptex_buffer, states)
                      .dispatch(static_cast<std::uint32_t>(strand_count));
        bool source_is_b = true;
        for (const nanoxgen::ClassicFloatEffect effect : runtime.effects) {
            if (effect.type == nanoxgen::ClassicFloatEffectType::Clump) {
                auto &shader = clumps.at(effect.module_index).value();
                stream << (source_is_b
                    ? shader(b, a, roots, runtime_data, ptex_buffer, states,
                             clump_axes[effect.module_index],
                             clump_frames[effect.module_index],
                             clump_runtime[effect.module_index],
                             clump_strand_guides[effect.module_index])
                    : shader(a, b, roots, runtime_data, ptex_buffer, states,
                             clump_axes[effect.module_index],
                             clump_frames[effect.module_index],
                             clump_runtime[effect.module_index],
                             clump_strand_guides[effect.module_index]))
                    .dispatch(static_cast<std::uint32_t>(strand_count));
            } else if (effect.type == nanoxgen::ClassicFloatEffectType::Noise) {
                auto &shader = noises.at(effect.module_index).value();
                stream << (source_is_b
                    ? shader(b, a, roots, runtime_data, ptex_buffer,
                             tangent_buffer, noise_domain_buffer, states)
                    : shader(a, b, roots, runtime_data, ptex_buffer,
                             tangent_buffer, noise_domain_buffer, states))
                    .dispatch(static_cast<std::uint32_t>(strand_count));
            } else {
                auto &shader = cuts.at(effect.module_index).value();
                stream << (source_is_b
                    ? shader(b, a, roots, runtime_data, ptex_buffer, states)
                    : shader(a, b, roots, runtime_data, ptex_buffer, states))
                    .dispatch(static_cast<std::uint32_t>(strand_count));
            }
            source_is_b = !source_is_b;
        }
        stream << width(final_is_a ? a : b, roots, runtime_data,
                        ptex_buffer, states)
                      .dispatch(static_cast<std::uint32_t>(strand_count));
    };

    const Clock::time_point host_output_allocate_begin = Clock::now();
    std::vector<luisa::float4> raw_points(point_count);
    std::vector<luisa::float4> raw_states(
        strand_count, luisa::make_float4(0.0f));
    const Clock::time_point host_output_allocate_end = Clock::now();
    const Clock::time_point first_begin = Clock::now();
    dispatch();
    stream << (options.base_only ? a : (final_is_a ? a : b))
                  .copy_to(luisa::span{raw_points});
    if (!options.base_only) {
        stream << states.copy_to(luisa::span{raw_states});
    }
    stream << synchronize();
    nanoxgen::PackedGeneratedCurves gpu = compact_gpu_output(
        raw_points, raw_states, root_plan, cvs);
    const Clock::time_point first_output_end = Clock::now();

    for (std::uint32_t i = 0u; i < options.warmup; ++i) {
        dispatch();
        stream << synchronize();
    }
    std::vector<double> warm_samples;
    warm_samples.reserve(options.repeats);
    for (std::uint32_t i = 0u; i < options.repeats; ++i) {
        const Clock::time_point begin = Clock::now();
        dispatch();
        stream << synchronize();
        warm_samples.push_back(milliseconds(begin, Clock::now()));
    }

    const Clock::time_point cpu_begin = Clock::now();
    std::optional<nanoxgen::PackedGeneratedCurves> cpu;
    std::optional<ErrorStats> error;
    if (options.cpu_validation) {
        cpu.emplace(nanoxgen::generate_xgen_classic_base_curves_cpu(
            imported.asset, root_plan, cvs, 0.0f, 1.0f, true));
        if (!options.base_only) {
            nanoxgen::apply_xgen_classic_float_runtime_plan_cpu(
                *cpu, runtime, 1.0f, root_plan.surface_tangents,
                root_plan.random_prefixes, root_plan.primitive_ids, clump_data,
                runtime_inputs.values, root_plan.reference_positions, true);
        }
        nanoxgen::make_xgen_classic_curves_world_space(*cpu);
        nanoxgen::add_xgen_classic_renderer_endpoints(*cpu);
        error = compare(gpu, *cpu);
    }
    const Clock::time_point cpu_end = Clock::now();
    std::optional<ErrorStats> reference_error;
    std::optional<ErrorStats> cpu_reference_error;
    if (options.reference_nxc) {
        const nanoxgen::CurveCache reference =
            nanoxgen::load_curve_cache(*options.reference_nxc);
        reference_error = compare_cache(gpu, reference);
        if (cpu) { cpu_reference_error = compare_cache(*cpu, reference); }
    }
    if (options.output_nxc) {
        std::vector<std::uint32_t> face_ids(gpu.strand_count);
        std::vector<nanoxgen::Vec2> face_uvs(gpu.strand_count);
        for (std::uint32_t strand = 0u; strand < gpu.strand_count; ++strand) {
            face_ids[strand] = gpu.roots[strand].surface_face_id;
            face_uvs[strand] = gpu.roots[strand].uv;
        }
        nanoxgen::save_curve_cache(
            nanoxgen::build_curve_cache(
                {gpu.point_counts, gpu.points, {}, {}, face_uvs, face_ids,
                 {}, {}}),
            *options.output_nxc);
    }
    constexpr float oracle_position_tolerance = 1.0e-3f;
    constexpr float oracle_radius_tolerance = 1.0e-7f;
    const bool oracle_within_tolerance = reference_error &&
        reference_error->position <= oracle_position_tolerance &&
        reference_error->radius <= oracle_radius_tolerance;
    const std::uint64_t output_checksum = checksum(gpu);

    std::cout << std::setprecision(9)
              << "{\"backend\":\"" << options.backend
              << "\",\"description\":\"" << description->name
              << "\",\"base_only\":"
              << (options.base_only ? "true" : "false")
              << ",\"effect_count\":" << runtime.effects.size()
              << ",\"parallel_jit\":true"
              << ",\"shader_cache\":false,\"fast_math\":"
              << (options.fast_math ? "true" : "false")
              << ",\"cpu_validation\":"
              << (options.cpu_validation ? "true" : "false")
              << ",\"includes_file_io\":true"
              << ",\"includes_autodesk_serialization\":false"
              << ",\"reference_comparison_order\":\"canonical-face-uv\""
              << ",\"input_roots\":" << strand_count
              << ",\"patch_culled\":" << root_plan.patch_culled_count
              << ",\"output_strands\":" << gpu.strand_count
              << ",\"output_points\":" << gpu.points.size()
              << ",\"device_create_ms\":"
              << (include_shared_start
                      ? milliseconds(process_begin, device_end) : 0.0)
              << ",\"native_parse_import_root_rebuild_ms\":"
              << milliseconds(native_begin, native_prepare_end)
              << ",\"collection_parse_ms\":"
              << (include_shared_start
                      ? milliseconds(device_end, collection_load_end) : 0.0)
              << ",\"runtime_plan_lower_ms\":"
              << milliseconds(runtime_plan_begin, runtime_plan_end)
              << ",\"alembic_import_ms\":"
              << milliseconds(runtime_plan_end, alembic_import_end)
              << ",\"root_plan_ms\":"
              << milliseconds(alembic_import_end, root_plan_end)
              << ",\"runtime_inputs_ms\":"
              << milliseconds(root_plan_end, runtime_inputs_end)
              << ",\"clump_data_ms\":"
              << milliseconds(runtime_inputs_end, clump_data_end)
              << ",\"guide_rebuild_ms\":"
              << milliseconds(clump_data_end, guide_rebuild_end)
              << ",\"native_host_pack_ms\":"
              << milliseconds(guide_rebuild_end, native_prepare_end)
              << ",\"jit_compile_allocate_ms\":"
              << milliseconds(native_prepare_end, compile_end)
              << ",\"device_buffer_allocate_ms\":"
              << milliseconds(
                     device_buffer_allocate_begin,
                     device_buffer_allocate_end)
              << ",\"jit_wait_after_native_ms\":"
              << milliseconds(jit_wait_begin, compile_end)
              << ",\"jit_kernel_count\":" << compile_timings.size()
              << ",\"jit_compile_active_wall_ms\":"
              << jit_compile_active_wall_ms
              << ",\"jit_compile_task_sum_ms\":"
              << jit_compile_task_sum_ms
              << ",\"jit_compile_task_max_ms\":"
              << jit_compile_task_max_ms
              << ",\"jit_device_buffer_overlap_ms\":"
              << jit_device_buffer_overlap_ms
              << ",\"upload_ms\":" << milliseconds(compile_end, upload_end)
              << ",\"host_output_allocate_ms\":"
              << milliseconds(
                     host_output_allocate_begin, host_output_allocate_end)
              << ",\"first_dispatch_download_pack_ms\":"
              << milliseconds(first_begin, first_output_end)
              << ",\"cold_end_to_end_ms\":"
              << milliseconds(cold_begin, first_output_end)
              << ",\"warm_median_ms\":"
              << percentile(warm_samples, 0.5)
              << ",\"warm_p90_ms\":"
              << percentile(warm_samples, 0.9)
              << ",\"cpu_generation_only_ms\":"
              << (options.cpu_validation
                      ? milliseconds(cpu_begin, cpu_end) : -1.0)
              << ",\"max_position_error_vs_cpu\":"
              << (error ? error->position : -1.0f)
              << ",\"max_radius_error_vs_cpu\":"
              << (error ? error->radius : -1.0f)
              << ",\"bit_mismatches_vs_cpu\":"
              << (error
                      ? static_cast<long long>(error->bit_mismatches) : -1ll)
              << ",\"max_position_error_vs_reference\":"
              << (reference_error ? reference_error->position : -1.0f)
              << ",\"max_radius_error_vs_reference\":"
              << (reference_error ? reference_error->radius : -1.0f)
              << ",\"cpu_max_position_error_vs_reference\":"
              << (cpu_reference_error ? cpu_reference_error->position : -1.0f)
              << ",\"cpu_max_radius_error_vs_reference\":"
              << (cpu_reference_error ? cpu_reference_error->radius : -1.0f)
              << ",\"oracle_position_tolerance\":"
              << oracle_position_tolerance
              << ",\"oracle_radius_tolerance\":"
              << oracle_radius_tolerance
              << ",\"oracle_within_tolerance\":"
              << (oracle_within_tolerance ? "true" : "false")
              << ",\"checksum\":" << output_checksum
              << ",\"fallback_count\":0"
              << ",\"handwritten_gpu_api\":false}\n";
    return {
        gpu.strand_count, gpu.points.size(), output_checksum,
        milliseconds(cold_begin, first_output_end),
        milliseconds(native_begin, native_prepare_end),
        jit_compile_active_wall_ms,
        milliseconds(first_begin, first_output_end)};
    };

    std::vector<const nanoxgen::ClassicDescription *> descriptions;
    if (collection_mode) {
        descriptions.reserve(collection.descriptions.size());
        for (const nanoxgen::ClassicDescription &description :
             collection.descriptions) {
            descriptions.push_back(&description);
        }
    } else {
        const nanoxgen::ClassicDescription *description =
            nanoxgen::find_classic_description(
                collection, options.description);
        if (description == nullptr) {
            throw std::runtime_error("Classic description was not found");
        }
        descriptions.push_back(description);
    }
    if (descriptions.empty()) {
        throw std::runtime_error("Classic collection has no descriptions");
    }
    std::uint64_t total_strands{};
    std::uint64_t total_points{};
    std::uint64_t collection_checksum = 1469598103934665603ull;
    double native_sum{};
    double jit_sum{};
    double dispatch_sum{};
    for (std::size_t index = 0u; index < descriptions.size(); ++index) {
        const DescriptionRunResult result = run_description(
            descriptions[index], index == 0u);
        total_strands += result.strands;
        total_points += result.points;
        collection_checksum ^= result.output_checksum;
        collection_checksum *= 1099511628211ull;
        native_sum += result.native_ms;
        jit_sum += result.jit_ms;
        dispatch_sum += result.first_dispatch_ms;
    }
    if (collection_mode) {
        std::cout << std::setprecision(9)
                  << "{\"collection_summary\":true,\"backend\":\""
                  << options.backend << "\",\"description_count\":"
                  << descriptions.size() << ",\"single_device\":true"
                  << ",\"parallel_jit_within_description\":true"
                  << ",\"parallel_jit_across_descriptions\":false"
                  << ",\"shader_cache\":false,\"strands\":"
                  << total_strands << ",\"points\":" << total_points
                  << ",\"native_sum_ms\":" << native_sum
                  << ",\"jit_active_sum_ms\":" << jit_sum
                  << ",\"first_dispatch_sum_ms\":" << dispatch_sum
                  << ",\"cold_end_to_end_ms\":"
                  << milliseconds(process_begin, Clock::now())
                  << ",\"checksum\":" << collection_checksum << "}\n";
    }
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
