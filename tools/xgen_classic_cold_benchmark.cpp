#include "nanoxgen/asset.h"
#include "nanoxgen/curve_cache.h"
#include "nanoxgen/xgen_classic.h"
#include "nanoxgen/xgen_classic_alembic.h"
#include "nanoxgen/xgen_classic_clump.h"
#include "nanoxgen/xgen_classic_collection.h"
#include "nanoxgen/xgen_classic_ptex.h"
#include "nanoxgen/xgen_classic_roots.h"
#include "nanoxgen/xgen_classic_runtime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using Clock = std::chrono::steady_clock;

struct ModuleAttributeOverride {
    std::string module;
    std::string attribute;
    std::string value;
};

double milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

struct Options {
    std::filesystem::path collection;
    std::filesystem::path archive_directory;
    std::filesystem::path descriptions_root;
    std::string description;
    std::uint32_t cvs{};
    std::uint32_t dump_roots{};
    std::optional<std::uint32_t> dump_face;
    std::optional<std::filesystem::path> output_nxc;
    std::optional<std::string> generator_mask;
    std::vector<ModuleAttributeOverride> module_attribute_overrides;
    std::uint32_t dump_guides{};
    std::optional<std::uint32_t> dump_runtime;
    std::optional<std::uint32_t> effect_count;
    std::uint32_t probe_face{};
    nanoxgen::Vec2 probe_uv{};
    bool probe{};
    bool face_counts{};
    bool generate{};
    bool base_only{};
};

std::uint32_t parse_u32(std::string_view value, const char *label) {
    std::size_t consumed{};
    const unsigned long parsed = std::stoul(std::string{value}, &consumed);
    if (consumed != value.size() || parsed > 0xfffffffful) {
        throw std::invalid_argument(std::string{label} + " is invalid");
    }
    return static_cast<std::uint32_t>(parsed);
}

Options parse_options(int argc, char **argv) {
    if (argc < 4) {
        throw std::invalid_argument(
            "usage: nanoxgen_xgen_classic_cold_benchmark COLLECTION.xgen "
            "ARCHIVE_DIRECTORY DESCRIPTIONS_ROOT [--description NAME] "
            "[--generate] [--base-only] [--cvs N] [--face-counts] [--dump-roots N] "
            "[--dump-face ID] [--dump-guides N] [--probe FACE,U,V] "
            "[--dump-runtime N] [--effect-count N] [--generator-mask EXPR] "
            "[--module-attr MODULE ATTRIBUTE VALUE ...] "
            "[--nxc OUTPUT]");
    }
    Options result{};
    result.collection = argv[1];
    result.archive_directory = argv[2];
    result.descriptions_root = argv[3];
    for (int index = 4; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--generate") {
            result.generate = true;
        } else if (argument == "--base-only") {
            result.generate = true;
            result.base_only = true;
        } else if (argument == "--face-counts") {
            result.face_counts = true;
        } else if (argument == "--module-attr") {
            if (index + 3 >= argc) {
                throw std::invalid_argument(
                    "--module-attr needs MODULE ATTRIBUTE VALUE");
            }
            result.module_attribute_overrides.push_back(
                {argv[++index], argv[++index], argv[++index]});
        } else if (argument == "--description" || argument == "--cvs" ||
                   argument == "--dump-roots" || argument == "--dump-face" ||
                   argument == "--dump-guides" ||
                   argument == "--dump-runtime" ||
                   argument == "--effect-count" ||
                   argument == "--generator-mask" ||
                   argument == "--probe" || argument == "--nxc") {
            if (++index >= argc) {
                throw std::invalid_argument(
                    "missing value after " + std::string{argument});
            }
            if (argument == "--description") {
                result.description = argv[index];
            } else if (argument == "--generator-mask") {
                result.generator_mask = argv[index];
            } else if (argument == "--nxc") {
                result.output_nxc = std::filesystem::path{argv[index]};
            } else if (argument == "--cvs") {
                result.cvs = parse_u32(argv[index], "CV count");
                if (result.cvs == 1u) {
                    throw std::invalid_argument("CV count must be zero or at least two");
                }
            } else if (argument == "--dump-roots") {
                result.dump_roots = parse_u32(argv[index], "root count");
            } else if (argument == "--dump-face") {
                result.dump_face = parse_u32(argv[index], "face id");
            } else if (argument == "--dump-guides") {
                result.dump_guides = parse_u32(argv[index], "guide count");
            } else if (argument == "--dump-runtime") {
                result.dump_runtime = parse_u32(argv[index], "runtime strand");
            } else if (argument == "--effect-count") {
                result.effect_count = parse_u32(argv[index], "effect count");
            } else {
                const std::string value{argv[index]};
                const std::size_t first = value.find(',');
                const std::size_t second = value.find(',', first + 1u);
                if (first == std::string::npos || second == std::string::npos ||
                    value.find(',', second + 1u) != std::string::npos) {
                    throw std::invalid_argument(
                        "probe must be FACE,U,V");
                }
                result.probe_face = parse_u32(
                    std::string_view{value}.substr(0u, first), "probe face");
                result.probe_uv = {
                    std::stof(value.substr(first + 1u, second - first - 1u)),
                    std::stof(value.substr(second + 1u))};
                result.probe = true;
            }
        } else {
            throw std::invalid_argument(
                "unknown argument: " + std::string{argument});
        }
    }
    if (result.output_nxc && (result.description.empty() || !result.generate)) {
        throw std::invalid_argument(
            "--nxc requires --generate and exactly one --description");
    }
    return result;
}

std::uint64_t checksum(const nanoxgen::PackedGeneratedCurves &curves) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](std::uint32_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    for (const nanoxgen::PackedCurvePoint &point : curves.points) {
        mix(std::bit_cast<std::uint32_t>(point.x));
        mix(std::bit_cast<std::uint32_t>(point.y));
        mix(std::bit_cast<std::uint32_t>(point.z));
        mix(std::bit_cast<std::uint32_t>(point.radius));
    }
    return hash;
}

} // namespace

int main(int argc, char **argv) try {
    const Options options = parse_options(argc, argv);
    const Clock::time_point total_begin = Clock::now();
    const Clock::time_point parse_begin = Clock::now();
    const nanoxgen::ClassicCollection collection =
        nanoxgen::load_xgen_classic_collection(options.collection);
    const Clock::time_point parse_end = Clock::now();
    const std::filesystem::path descriptions_root =
        nanoxgen::resolve_xgen_classic_descriptions_root(
            collection, options.collection, options.descriptions_root);
    std::uint64_t total_candidates{};
    std::uint64_t total_strands{};
    std::uint64_t total_points{};
    std::uint64_t aggregate_checksum{};
    std::uint32_t processed{};
    std::cout << std::setprecision(9);
    for (const nanoxgen::ClassicDescription &source_description :
         collection.descriptions) {
        if (!options.description.empty() &&
            source_description.name != options.description) {
            continue;
        }
        nanoxgen::ClassicDescription description = source_description;
        if (options.generator_mask) {
            auto generator = std::find_if(
                description.objects.begin(), description.objects.end(),
                [](const nanoxgen::ClassicObject &object) {
                    return object.type == "RandomGenerator";
                });
            if (generator == description.objects.end()) {
                throw std::runtime_error(
                    "generator mask override needs a RandomGenerator");
            }
            auto mask = std::find_if(
                generator->attributes.begin(), generator->attributes.end(),
                [](const nanoxgen::ClassicAttribute &attribute) {
                    return attribute.name == "mask";
                });
            if (mask == generator->attributes.end()) {
                generator->attributes.push_back(
                    {"mask", *options.generator_mask});
            } else {
                mask->value = *options.generator_mask;
            }
        }
        for (const ModuleAttributeOverride &override_value :
             options.module_attribute_overrides) {
            const auto object = std::find_if(
                description.objects.begin(), description.objects.end(),
                [&](const nanoxgen::ClassicObject &candidate) {
                    const nanoxgen::ClassicAttribute *name =
                        nanoxgen::find_classic_attribute(
                            candidate.attributes, "name");
                    return name && name->value == override_value.module;
                });
            if (object == description.objects.end()) {
                throw std::runtime_error(
                    "module override did not match " + override_value.module);
            }
            auto attribute = std::find_if(
                object->attributes.begin(), object->attributes.end(),
                [&](const nanoxgen::ClassicAttribute &candidate) {
                    return candidate.name == override_value.attribute;
                });
            if (attribute == object->attributes.end()) {
                object->attributes.push_back(
                    {override_value.attribute, override_value.value});
            } else {
                attribute->value = override_value.value;
            }
        }
        ++processed;
        const std::filesystem::path archive =
            std::filesystem::is_regular_file(options.archive_directory)
                ? options.archive_directory
                : options.archive_directory / (description.name + ".abc");
        const std::filesystem::path description_directory =
            descriptions_root / description.name;
        const Clock::time_point import_begin = Clock::now();
        const nanoxgen::ClassicAlembicAssetInput imported =
            nanoxgen::build_xgen_classic_alembic_asset_input(
                description, archive);
        const Clock::time_point import_end = Clock::now();
        if (options.probe) {
            if (!imported.reference_surface || description.patches.empty()) {
                throw std::runtime_error(
                    "probe requires an imported subdivision surface");
            }
            const nanoxgen::ClassicReferenceSurfaceSample sample =
                imported.reference_surface->evaluate(
                    description.patches.front().name, options.probe_face,
                    options.probe_uv.x, options.probe_uv.y);
            const nanoxgen::ClassicReferenceSurfaceSample current =
                imported.reference_surface->evaluate_current(
                    description.patches.front().name, options.probe_face,
                    options.probe_uv.x, options.probe_uv.y);
            std::cout << "probe " << options.probe_face << ' '
                      << options.probe_uv.x << ' ' << options.probe_uv.y
                      << " reference_position " << sample.position.x << ' '
                      << sample.position.y << ' ' << sample.position.z
                      << " reference_normal " << sample.normal.x << ' '
                      << sample.normal.y << ' ' << sample.normal.z;
            std::cout << " reference_tangent " << sample.tangent.x << ' '
                      << sample.tangent.y << ' ' << sample.tangent.z
                      << " current_position " << current.position.x << ' '
                      << current.position.y << ' ' << current.position.z
                      << " current_normal " << current.normal.x << ' '
                      << current.normal.y << ' ' << current.normal.z
                      << " current_tangent " << current.tangent.x << ' '
                      << current.tangent.y << ' ' << current.tangent.z;
            const float mask = nanoxgen::evaluate_xgen_classic_random_mask(
                description, description_directory,
                description.patches.front().name, options.probe_face,
                options.probe_uv.x, options.probe_uv.y);
            std::cout << " mask " << mask;
            nanoxgen::ClassicRootPlan probe_roots{};
            probe_roots.roots.push_back({
                current.position, current.normal, options.probe_uv, 0u, {},
                options.probe_face});
            probe_roots.influence_offsets.push_back(0u);
            for (std::size_t index = 0u;
                 index < imported.asset.guides.size(); ++index) {
                const float weight =
                    nanoxgen::evaluate_xgen_classic_guide_weight(
                        imported.asset.guides[index], sample.position,
                        sample.normal);
                if (weight > 0.0f) {
                    std::cout << ' ' << index << ':' << weight;
                    probe_roots.influences.push_back({
                        static_cast<std::uint32_t>(index), weight});
                }
            }
            std::cout << '\n';
            probe_roots.influence_offsets.push_back(
                static_cast<std::uint32_t>(probe_roots.influences.size()));
            if (!probe_roots.influences.empty()) {
                const std::uint32_t probe_cvs = options.cvs == 0u
                    ? nanoxgen::compile_xgen_classic_float_runtime_plan(
                          description, collection.palette_attributes).fx_cv_count
                    : options.cvs;
                const nanoxgen::PackedGeneratedCurves curve =
                    nanoxgen::generate_xgen_classic_base_curves_cpu(
                        imported.asset, probe_roots, probe_cvs);
                std::cout << "probe_curve " << options.probe_face << ' '
                          << options.probe_uv.x << ' ' << options.probe_uv.y
                          << " count " << curve.points.size();
                for (const nanoxgen::PackedCurvePoint &point : curve.points) {
                    std::cout << ' ' << point.x << ' ' << point.y << ' '
                              << point.z;
                }
                std::cout << '\n';
            }
        }
        double surface_area = 0.0;
        for (const auto &face : imported.surface_faces) {
            surface_area += face.surface_area;
        }
        const nanoxgen::ClassicRootPlan roots =
            nanoxgen::build_xgen_classic_random_root_plan(
                description, imported, description_directory);
        const Clock::time_point roots_end = Clock::now();
        const nanoxgen::Asset asset = nanoxgen::build_asset(imported.asset);
        const Clock::time_point asset_end = Clock::now();
        nanoxgen::ClassicFloatRuntimePlan runtime =
            nanoxgen::compile_xgen_classic_float_runtime_plan(
                description, collection.palette_attributes);
        const Clock::time_point runtime_plan_end = Clock::now();
        if (description.patches.empty()) {
            throw std::runtime_error(
                "Classic runtime PTEX binding needs one patch");
        }
        const nanoxgen::ClassicRuntimeInputData runtime_inputs =
            nanoxgen::build_xgen_classic_runtime_input_data(
                runtime, description_directory,
                description.patches.front().name, roots);
        const Clock::time_point runtime_inputs_end = Clock::now();
        const std::uint32_t runtime_cvs = options.cvs != 0u
            ? options.cvs
            : (runtime.fx_cv_count >= 2u ? runtime.fx_cv_count : 8u);
        const std::vector<nanoxgen::ClassicClumpRuntimeData> clump_data =
            nanoxgen::build_xgen_classic_clump_runtime_data_parallel(
                description, imported, description_directory, roots,
                runtime, runtime_cvs);
        const Clock::time_point clump_data_end = Clock::now();
        if (options.effect_count) {
            if (*options.effect_count > runtime.effects.size()) {
                throw std::runtime_error(
                    "effect count exceeds the compiled runtime plan");
            }
            runtime.effects.resize(*options.effect_count);
        }
        if (options.dump_runtime) {
            if (*options.dump_runtime >= roots.roots.size()) {
                throw std::runtime_error("runtime strand is out of range");
            }
            const std::uint32_t strand = *options.dump_runtime;
            const nanoxgen::RootSample &root = roots.roots[strand];
            const nanoxgen::Vec3 reference_position =
                roots.reference_positions[strand];
            const nanoxgen::Vec3 noise_domain =
                (reference_position +
                 nanoxgen::Vec3{0.419276f, 0.184247f, 0.805721f}) * 100.0f;
            nanoxgen::ClassicFloatRuntimeContext context{};
            context.id = roots.primitive_ids[strand];
            context.u = root.uv.x;
            context.v = root.uv.y;
            context.face_seed = nanoxgen::xgen_runtime_face_seed(
                runtime.description_id, runtime.description_name,
                root.surface_face_id);
            context.c_length = 1.0f;
            context.random_prefix = roots.random_prefixes[strand];
            context.has_random_prefix = true;
            if (runtime_inputs.values_per_strand != 0u) {
                const auto row = std::span{runtime_inputs.values}.subspan(
                    static_cast<std::size_t>(strand) *
                        runtime_inputs.values_per_strand,
                    runtime_inputs.values_per_strand);
                context.ptex_values = row.first(runtime.ptex_paths.size());
                context.custom_values = row.subspan(
                    runtime.ptex_paths.size(), runtime.custom_inputs.size());
                context.pref_noise_values = row.subspan(
                    runtime.ptex_paths.size() + runtime.custom_inputs.size(),
                    runtime.pref_noise_inputs.size());
            }
            const std::array<float, 1u> runtime_hash_input{
                static_cast<float>(context.id)};
            const std::array<double, 1u> exact_hash_input{
                static_cast<double>(context.id)};
            std::cout << "runtime " << strand << " primitive_id "
                      << context.id << " prefix " << context.random_prefix
                      << " face " << root.surface_face_id << " uv "
                      << root.uv.x << ' ' << root.uv.y << " position "
                      << root.position.x << ' ' << root.position.y << ' '
                      << root.position.z << " normal " << root.normal.x << ' '
                      << root.normal.y << ' ' << root.normal.z << " tangent "
                      << roots.surface_tangents[strand].x << ' '
                      << roots.surface_tangents[strand].y << ' '
                      << roots.surface_tangents[strand].z << " hash "
                      << nanoxgen::xgen_runtime_hash(runtime_hash_input)
                      << " exact_hash "
                      << nanoxgen::xgen_seexpr_hash(exact_hash_input)
                      << " component "
                      << nanoxgen::xgen_runtime_hash_component(
                             runtime_hash_input.front())
                      << " exact_component "
                      << nanoxgen::xgen_seexpr_component(
                             exact_hash_input.front())
                      << '\n';
            std::cout << "runtime " << strand << " noise_domain "
                      << noise_domain.x << ' ' << noise_domain.y << ' '
                      << noise_domain.z << " noise "
                      << nanoxgen::xgen_classic_noise_float(noise_domain)
                      << '\n';
            const std::uint32_t influence_begin =
                roots.influence_offsets[strand];
            const std::uint32_t influence_end =
                roots.influence_offsets[strand + 1u];
            std::cout << "runtime " << strand << " influences "
                      << (influence_end - influence_begin);
            for (std::uint32_t index = influence_begin;
                 index < influence_end; ++index) {
                const nanoxgen::ClassicGuideInfluence influence =
                    roots.influences[index];
                std::cout << ' ' << influence.guide_index << ':'
                          << influence.weight;
            }
            std::cout << '\n';
            for (std::uint32_t index = influence_begin;
                 index < influence_end; ++index) {
                const nanoxgen::ClassicGuideInfluence influence =
                    roots.influences[index];
                const nanoxgen::GuideInput &guide =
                    imported.asset.guides[influence.guide_index];
                std::cout << "runtime " << strand << " influence_guide "
                          << influence.guide_index << " root "
                          << guide.cvs.front().x << ' ' << guide.cvs.front().y
                          << ' ' << guide.cvs.front().z << " reference_root "
                          << guide.reference_root_position.x << ' '
                          << guide.reference_root_position.y << ' '
                          << guide.reference_root_position.z << " normal "
                          << guide.reference_root_normal.x << ' '
                          << guide.reference_root_normal.y << ' '
                          << guide.reference_root_normal.z << " tangent "
                          << guide.reference_root_tangent.x << ' '
                          << guide.reference_root_tangent.y << ' '
                          << guide.reference_root_tangent.z << " binormal "
                          << guide.reference_root_binormal.x << ' '
                          << guide.reference_root_binormal.y << ' '
                          << guide.reference_root_binormal.z << '\n';
            }
            const auto emit = [&](std::string_view name,
                                  const auto &expression) {
                if (!expression) { return; }
                std::cout << "runtime " << strand << ' ' << name << ' '
                          << nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                                 *expression, context)
                          << '\n';
            };
            emit("width", runtime.width);
            emit("widthRamp0", runtime.width_ramp);
            context.t = 1.0f;
            emit("widthRamp1", runtime.width_ramp);
            for (const auto &cut : runtime.cuts) {
                std::cout << "runtime " << strand << " cut "
                          << nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                                 cut.amount, context)
                          << '\n';
            }
            for (const auto &noise : runtime.noises) {
                std::cout << "runtime " << strand << " noise"
                          << " mask "
                          << nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                                 noise.mask, context)
                          << " magnitude "
                          << nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                                 noise.magnitude, context)
                          << " frequency "
                          << nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                                 noise.frequency, context)
                          << " correlation "
                          << nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                                 noise.correlation, context)
                          << " preserve_length "
                          << nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                                 noise.preserve_length, context)
                          << '\n';
            }
            for (std::size_t module_index = 0u;
                 module_index < runtime.clumps.size(); ++module_index) {
                const auto &clump = runtime.clumps[module_index];
                const auto &binding = clump_data[module_index];
                const std::uint32_t guide =
                    binding.strand_guide_indices[strand];
                std::cout << "runtime " << strand << " clump "
                          << clump.name << " guide " << guide << " mask "
                          << nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                                 clump.mask, context)
                          << " amount "
                          << nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                                 clump.clump, context)
                          << " noise "
                          << nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                                 clump.noise, context)
                          << " noise_frequency "
                          << nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                                 clump.noise_frequency, context)
                          << " noise_correlation "
                          << nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                                 clump.noise_correlation, context)
                          << '\n';
                if (guide != nanoxgen::kInvalidIndex) {
                    const nanoxgen::ClassicFloatRuntimeContext strand_context =
                        context;
                    context.u = binding.guide_uvs[guide].x;
                    context.v = binding.guide_uvs[guide].y;
                    context.face_seed = nanoxgen::xgen_runtime_face_seed(
                        runtime.description_id, runtime.description_name,
                        binding.guide_face_ids[guide]);
                    context.random_prefix =
                        binding.guide_random_prefixes[guide];
                    std::cout << "runtime " << strand
                              << " clump_guide_noise " << clump.name
                              << " prefix "
                              << binding.guide_random_prefixes[guide]
                              << " face " << binding.guide_face_ids[guide]
                              << " uv " << binding.guide_uvs[guide].x << ' '
                              << binding.guide_uvs[guide].y << ' '
                              << "normal "
                              << binding.guide_normals[guide].x << ' '
                              << binding.guide_normals[guide].y << ' '
                              << binding.guide_normals[guide].z << " tangent "
                              << binding.guide_tangents[guide].x << ' '
                              << binding.guide_tangents[guide].y << ' '
                              << binding.guide_tangents[guide].z
                              << " reference_position "
                              << binding.guide_reference_positions[guide].x
                              << ' '
                              << binding.guide_reference_positions[guide].y
                              << ' '
                              << binding.guide_reference_positions[guide].z
                              << " noise "
                              << nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                                     clump.noise, context)
                              << " frequency "
                              << nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                                     clump.noise_frequency, context)
                              << '\n';
                    context = strand_context;
                    const std::size_t offset =
                        static_cast<std::size_t>(guide) *
                        binding.cvs_per_guide;
                    for (std::uint32_t cv = 0u;
                         cv < binding.cvs_per_guide; ++cv) {
                        const nanoxgen::Vec3 point =
                            binding.guide_axes[offset + cv];
                        std::cout << "runtime " << strand
                                  << " clump_axis " << cv << ' '
                                  << point.x << ' ' << point.y << ' '
                                  << point.z << '\n';
                    }
                }
            }
        }
        const Clock::time_point compile_end = Clock::now();
        std::uint64_t output_checksum{};
        std::uint64_t point_count{};
        std::uint64_t output_strand_count = roots.roots.size();
        if (options.generate) {
            if (roots.roots.size() >
                std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error("root count exceeds generation ABI");
            }
            nanoxgen::GenerationParams params{};
            params.strand_count =
                static_cast<std::uint32_t>(roots.roots.size());
            params.cvs_per_strand = runtime_cvs;
            const bool root_relative = !roots.influence_offsets.empty();
            nanoxgen::PackedGeneratedCurves curves =
                !root_relative
                    ? nanoxgen::generate_packed_roots_cpu(
                          asset, params, roots.roots)
                    : nanoxgen::generate_xgen_classic_base_curves_cpu(
                          imported.asset, roots, params.cvs_per_strand,
                          0.0f, 1.0f, true);
            if (!options.base_only) {
                nanoxgen::apply_xgen_classic_float_runtime_plan_cpu(
                    curves, runtime, 1.0f, roots.surface_tangents,
                    roots.random_prefixes, roots.primitive_ids, clump_data,
                    runtime_inputs.values, roots.reference_positions,
                    root_relative);
            }
            if (root_relative) {
                nanoxgen::make_xgen_classic_curves_world_space(curves);
            }
            nanoxgen::add_xgen_classic_renderer_endpoints(curves);
            if (options.output_nxc) {
                std::vector<std::uint32_t> face_ids(curves.strand_count);
                std::vector<nanoxgen::Vec2> face_uvs(curves.strand_count);
                for (std::uint32_t strand = 0u;
                     strand < curves.strand_count; ++strand) {
                    face_ids[strand] = curves.roots[strand].surface_face_id;
                    face_uvs[strand] = curves.roots[strand].uv;
                }
                const nanoxgen::CurveCache cache = nanoxgen::build_curve_cache(
                    {curves.point_counts, curves.points, {}, {}, face_uvs,
                     face_ids, {}, {}});
                nanoxgen::save_curve_cache(cache, *options.output_nxc);
            }
            output_checksum = checksum(curves);
            point_count = curves.points.size();
            output_strand_count = curves.strand_count;
        }
        const Clock::time_point generate_end = Clock::now();
        total_candidates += roots.candidate_count;
        total_strands += output_strand_count;
        total_points += point_count;
        aggregate_checksum ^= output_checksum;
        std::cout << "{\"description\":\"" << description.name
                  << "\",\"candidates\":" << roots.candidate_count
                  << ",\"roots\":" << roots.roots.size()
                  << ",\"strands\":" << output_strand_count
                  << ",\"mask_rejected\":" << roots.mask_rejected_count
                  << ",\"patch_culled\":" << roots.patch_culled_count
                  << ",\"guide_rejected\":" << roots.guide_rejected_count
                  << ",\"vertices\":" << imported.asset.positions.size()
                  << ",\"triangles\":" << imported.asset.triangles.size()
                  << ",\"guides\":" << imported.asset.guides.size()
                  << ",\"surface_area\":" << surface_area
                  << ",\"points\":" << point_count
                  << ",\"fallback_count\":"
                  << runtime.fallback_reasons.size()
                  << ",\"import_ms\":"
                  << milliseconds(import_begin, import_end)
                  << ",\"roots_ms\":"
                  << milliseconds(import_end, roots_end)
                  << ",\"asset_build_ms\":"
                  << milliseconds(roots_end, asset_end)
                  << ",\"runtime_compile_ms\":"
                  << milliseconds(asset_end, compile_end)
                  << ",\"runtime_plan_lower_ms\":"
                  << milliseconds(asset_end, runtime_plan_end)
                  << ",\"runtime_inputs_ms\":"
                  << milliseconds(runtime_plan_end, runtime_inputs_end)
                  << ",\"clump_data_ms\":"
                  << milliseconds(runtime_inputs_end, clump_data_end)
                  << ",\"runtime_misc_ms\":"
                  << milliseconds(clump_data_end, compile_end)
                  << ",\"generate_apply_checksum_ms\":"
                  << milliseconds(compile_end, generate_end)
                  << ",\"checksum\":" << output_checksum << "}\n";
        for (const std::string &reason : runtime.fallback_reasons) {
            std::cout << "fallback " << description.name << ' ' << reason
                      << '\n';
        }
        if (options.face_counts) {
            for (const nanoxgen::ClassicRootFaceStats &stats :
                 roots.face_stats) {
                const std::uint32_t face = stats.face_id;
                const auto metadata = std::find_if(
                    imported.surface_faces.begin(), imported.surface_faces.end(),
                    [face](const auto &entry) { return entry.face_id == face; });
                std::cout << "face " << face << ' ' << stats.root_count
                          << " candidates " << stats.candidate_count
                          << " mask " << stats.mask_accepted_count;
                if (metadata != imported.surface_faces.end()) {
                    std::cout << " area " << metadata->surface_area
                              << " ulen " << metadata->center_u_length
                              << " vlen " << metadata->center_v_length
                              << " reference_bounds "
                              << metadata->reference_bounds_min.x << ' '
                              << metadata->reference_bounds_min.y << ' '
                              << metadata->reference_bounds_min.z << ' '
                              << metadata->reference_bounds_max.x << ' '
                              << metadata->reference_bounds_max.y << ' '
                              << metadata->reference_bounds_max.z;
                }
                std::cout << '\n';
            }
        }
        std::uint32_t dumped_roots = 0u;
        for (std::uint32_t index = 0u;
             index < roots.roots.size() &&
             dumped_roots < options.dump_roots;
             ++index) {
            const nanoxgen::RootSample &root = roots.roots[index];
            if (options.dump_face &&
                root.surface_face_id != *options.dump_face) {
                continue;
            }
            std::cout << "root " << index << ' ' << root.surface_face_id
                      << ' ' << root.uv.x << ' ' << root.uv.y
                      << " bits " << std::bit_cast<std::uint32_t>(root.uv.x)
                      << ' ' << std::bit_cast<std::uint32_t>(root.uv.y)
                      << " primitive_id " << roots.primitive_ids[index]
                      << '\n';
            ++dumped_roots;
        }
        for (std::uint32_t index = 0u;
             index < std::min<std::size_t>(
                         options.dump_guides, imported.asset.guides.size());
             ++index) {
            const nanoxgen::GuideInput &guide = imported.asset.guides[index];
            const auto print_vec = [](nanoxgen::Vec3 value) {
                std::cout << ' ' << value.x << ' ' << value.y << ' ' << value.z;
            };
            std::cout << "guide " << index << " face "
                      << guide.surface_face_id << " position";
            print_vec(guide.reference_root_position);
            std::cout << " normal";
            print_vec(guide.reference_root_normal);
            std::cout << " tangent";
            print_vec(guide.reference_root_tangent);
            std::cout << " binormal";
            print_vec(guide.reference_root_binormal);
            std::cout << " radii";
            for (const float radius : guide.support_radii) {
                std::cout << ' ' << radius;
            }
            std::cout << " angles";
            for (const float angle : guide.support_angles) {
                std::cout << ' ' << angle;
            }
            std::cout << " cvs";
            for (const nanoxgen::Vec3 cv : guide.cvs) {
                print_vec(cv);
            }
            std::cout << '\n';
        }
    }
    if (processed == 0u) {
        throw std::runtime_error("requested description was not found");
    }
    const Clock::time_point total_end = Clock::now();
    std::cout << "{\"summary\":true,\"descriptions\":" << processed
              << ",\"parse_ms\":" << milliseconds(parse_begin, parse_end)
              << ",\"candidates\":" << total_candidates
              << ",\"strands\":" << total_strands
              << ",\"points\":" << total_points
              << ",\"total_ms\":" << milliseconds(total_begin, total_end)
              << ",\"checksum\":" << aggregate_checksum << "}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
