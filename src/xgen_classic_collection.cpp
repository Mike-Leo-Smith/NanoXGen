#include "nanoxgen/xgen_classic_collection.h"

#include "nanoxgen/xgen_classic.h"
#include "nanoxgen/xgen_classic_clump.h"
#include "nanoxgen/xgen_classic_roots.h"
#include "xgen_classic_path.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace nanoxgen {
namespace {

bool descriptions_exist(
    const ClassicCollection &collection,
    const std::filesystem::path &root) {
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error) {
        return false;
    }
    for (const ClassicDescription &description :
         collection.descriptions) {
        error.clear();
        if (!std::filesystem::is_directory(
                root / description.name, error) || error) {
            return false;
        }
    }
    return !collection.descriptions.empty();
}

std::string portable_path_string(std::string_view value) {
    std::string result{value};
    for (char &character : result) {
        if (character == '\\') { character = '/'; }
    }
    while (result.size() > 1u && result.back() == '/') {
        result.pop_back();
    }
    return result;
}

bool portable_path_prefix(
    std::string_view path, std::string_view prefix) {
    const std::string normalized_path = portable_path_string(path);
    const std::string normalized_prefix = portable_path_string(prefix);
    if (normalized_prefix.empty() ||
        normalized_path.size() < normalized_prefix.size()) {
        return false;
    }
    const auto equal_ascii = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
            std::tolower(static_cast<unsigned char>(b));
    };
    if (!std::equal(
            normalized_prefix.begin(), normalized_prefix.end(),
            normalized_path.begin(), equal_ascii) ||
        (normalized_path.size() != normalized_prefix.size() &&
         normalized_path[normalized_prefix.size()] != '/')) {
        return false;
    }
    return true;
}

} // namespace

std::filesystem::path resolve_xgen_classic_descriptions_root(
    const ClassicCollection &collection,
    const std::filesystem::path &collection_path,
    const std::filesystem::path &root_or_project) {
    if (collection.descriptions.empty()) {
        throw std::invalid_argument(
            "cannot resolve a description root for an empty collection");
    }
    std::vector<std::filesystem::path> candidates;
    const auto add = [&](std::filesystem::path candidate) {
        candidate = candidate.lexically_normal();
        if (candidate.empty()) { return; }
        if (std::find(candidates.begin(), candidates.end(), candidate) ==
            candidates.end()) {
            candidates.emplace_back(std::move(candidate));
        }
    };
    add(root_or_project);

    const ClassicAttribute *palette_name =
        find_classic_attribute(collection.palette_attributes, "name");
    if (palette_name && !palette_name->value.empty()) {
        const auto relative =
            std::filesystem::path{"xgen"} / "collections" /
            detail::classic_path(palette_name->value);
        add(root_or_project / relative);
        add(collection_path.parent_path() / relative);
    }

    const ClassicAttribute *data_path =
        find_classic_attribute(collection.palette_attributes, "xgDataPath");
    const ClassicAttribute *project_path =
        find_classic_attribute(collection.palette_attributes, "xgProjectPath");
    if (data_path && !data_path->value.empty()) {
        std::size_t begin{};
        while (begin <= data_path->value.size()) {
            const std::size_t end =
                data_path->value.find(';', begin);
            std::string_view value{
                data_path->value.data() + begin,
                (end == std::string::npos
                     ? data_path->value.size() : end) - begin};
            while (!value.empty() &&
                   std::isspace(
                       static_cast<unsigned char>(value.front()))) {
                value.remove_prefix(1u);
            }
            while (!value.empty() &&
                   std::isspace(
                       static_cast<unsigned char>(value.back()))) {
                value.remove_suffix(1u);
            }
            constexpr std::string_view project_token{"${PROJECT}"};
            if (value.starts_with(project_token)) {
                value.remove_prefix(project_token.size());
                add(root_or_project / detail::classic_path(
                    detail::strip_classic_root_separators(value)));
            } else if (!value.empty()) {
                add(detail::classic_path(value));
                if (project_path && !project_path->value.empty()) {
                    const std::string normalized_value =
                        portable_path_string(value);
                    const std::string normalized_project =
                        portable_path_string(project_path->value);
                    if (portable_path_prefix(
                            normalized_value, normalized_project)) {
                        std::string suffix =
                            normalized_value.substr(
                                normalized_value.size() ==
                                        normalized_project.size()
                                ? normalized_value.size()
                                : normalized_project.size() + 1u);
                        add(root_or_project /
                            detail::classic_path(suffix));
                        add(collection_path.parent_path() /
                            detail::classic_path(suffix));
                    }
                }
            }
            if (end == std::string::npos) { break; }
            begin = end + 1u;
        }
    }
    for (const std::filesystem::path &candidate : candidates) {
        if (descriptions_exist(collection, candidate)) {
            return candidate;
        }
    }
    // Preserve the explicit-root diagnostic for callers that intentionally
    // materialize a description lazily after planning.
    return root_or_project.lexically_normal();
}

const ClassicCollectionMotionSampleDescription &
resolve_xgen_classic_motion_deformation(
    const ClassicCollectionMotionExecutionDescription &description,
    std::size_t sample) {
    if (sample >= description.samples.size()) {
        throw std::out_of_range("Classic motion sample is invalid");
    }
    const std::size_t source =
        description.samples[sample].deformation_source_index;
    if (source > sample || source >= description.samples.size() ||
        description.samples[source].deformation_source_index != source) {
        throw std::logic_error(
            "Classic motion deformation sharing is inconsistent");
    }
    return description.samples[source];
}

void validate_xgen_classic_motion_sampling(
    const ClassicMotionSampling &sampling) {
    constexpr std::size_t maximum_motion_samples = 20u;
    if (!std::isfinite(sampling.frame) ||
        !std::isfinite(sampling.frames_per_second) ||
        !(sampling.frames_per_second > 0.0)) {
        throw std::invalid_argument(
            "Classic motion frame and FPS must be finite with positive FPS");
    }
    if (sampling.lookup_offsets.empty() ||
        sampling.lookup_offsets.size() > maximum_motion_samples ||
        sampling.lookup_offsets.size() != sampling.placements.size()) {
        throw std::invalid_argument(
            "Classic motion needs 1-20 matching lookup and placement samples");
    }
    for (std::size_t index = 0u;
         index < sampling.lookup_offsets.size(); ++index) {
        if (!std::isfinite(sampling.lookup_offsets[index]) ||
            !std::isfinite(sampling.placements[index])) {
            throw std::invalid_argument(
                "Classic motion samples must be finite");
        }
        if (index != 0u &&
            !(sampling.placements[index] >
              sampling.placements[index - 1u])) {
            throw std::invalid_argument(
                "Classic motion placements must be strictly increasing");
        }
    }
}

ClassicCollectionExecutionPlan
build_xgen_classic_collection_execution_plan(
    const std::filesystem::path &collection_path,
    const std::filesystem::path &archive_path,
    const std::filesystem::path &descriptions_root,
    const ClassicCollectionExecutionOptions &options) {
    const ClassicCollection collection =
        load_xgen_classic_collection(collection_path);
    return build_xgen_classic_collection_execution_plan(
        collection, collection_path, archive_path, descriptions_root,
        options);
}

ClassicCollectionExecutionPlan
build_xgen_classic_collection_execution_plan(
    const ClassicCollection &collection,
    const std::filesystem::path &collection_path,
    const std::filesystem::path &archive_path,
    const std::filesystem::path &descriptions_root,
    const ClassicCollectionExecutionOptions &options) {
    if (collection.descriptions.empty()) {
        throw std::runtime_error(
            "Classic collection main file has no descriptions");
    }
    std::unique_ptr<NanoXGenContext> owned_context;
    NanoXGenContext *context = options.context;
    if (!context) {
        owned_context = std::make_unique<NanoXGenContext>();
        context = owned_context.get();
    }
    TaskExecutor &executor = context->executor();
    const std::filesystem::path resolved_descriptions_root =
        resolve_xgen_classic_descriptions_root(
            collection, collection_path, descriptions_root);
    ClassicCollectionExecutionPlan result{};
    result.collection_path = collection_path;
    result.archive_path = archive_path;
    result.descriptions_root = resolved_descriptions_root;
    result.descriptions.resize(collection.descriptions.size());
    const auto prepare_description = [&](std::size_t index) {
        const ClassicDescription &description =
            collection.descriptions[index];
        ClassicCollectionExecutionDescription output{};
        output.name = description.name;
        output.runtime = compile_xgen_classic_float_runtime_plan(
            description, collection.palette_attributes);
        if (!output.runtime.lowering_complete()) {
            throw std::runtime_error(
                "Classic collection description '" + description.name +
                "' needs fallback: " +
                output.runtime.fallback_reasons.front());
        }
        const std::uint32_t cvs = output.runtime.fx_cv_count;
        output.surface = build_xgen_classic_alembic_asset_input(
            description, archive_path);
        output.roots = build_xgen_classic_random_root_plan(
            description, output.surface,
            resolved_descriptions_root / description.name);
        if (output.roots.roots.empty() ||
            output.roots.influence_offsets.empty()) {
            throw std::runtime_error(
                "Classic collection description '" + description.name +
                "' has no generated roots");
        }
        if (description.patches.empty()) {
            throw std::runtime_error(
                "Classic collection description '" + description.name +
                "' has no patch");
        }
        output.runtime_inputs = build_xgen_classic_runtime_input_data(
            output.runtime, resolved_descriptions_root / description.name,
            description.patches.front().name, output.roots,
            context);
        output.clumps = build_xgen_classic_clump_runtime_data_parallel(
            description, output.surface,
            resolved_descriptions_root / description.name, output.roots,
            output.runtime, cvs, context);
        output.runtime.effects.resize(std::min<std::size_t>(
            options.effect_count, output.runtime.effects.size()));
        output.rebuilt_guides = rebuild_xgen_classic_guides_for_device(
            output.surface.asset, cvs);
        result.descriptions[index] = std::move(output);
    };
    result.context_worker_count = executor.worker_count();
    if (collection.descriptions.size() <= 1u ||
        executor.worker_count() <= 1u) {
        for (std::size_t index = 0u;
             index < collection.descriptions.size(); ++index) {
            prepare_description(index);
        }
    } else {
        executor.parallel_for(
            collection.descriptions.size(), prepare_description);
    }
    return result;
}

ClassicCollectionMotionExecutionPlan
build_xgen_classic_collection_motion_execution_plan(
    const std::filesystem::path &collection_path,
    const std::filesystem::path &archive_path,
    const std::filesystem::path &descriptions_root,
    const ClassicMotionSampling &sampling,
    const ClassicCollectionExecutionOptions &options) {
    const ClassicCollection collection =
        load_xgen_classic_collection(collection_path);
    return build_xgen_classic_collection_motion_execution_plan(
        collection, collection_path, archive_path, descriptions_root,
        sampling, options);
}

ClassicCollectionMotionExecutionPlan
build_xgen_classic_collection_motion_execution_plan(
    const ClassicCollection &collection,
    const std::filesystem::path &collection_path,
    const std::filesystem::path &archive_path,
    const std::filesystem::path &descriptions_root,
    const ClassicMotionSampling &sampling,
    const ClassicCollectionExecutionOptions &options) {
    validate_xgen_classic_motion_sampling(sampling);
    if (collection.descriptions.empty()) {
        throw std::runtime_error(
            "Classic collection main file has no descriptions");
    }
    std::unique_ptr<NanoXGenContext> owned_context;
    NanoXGenContext *context = options.context;
    if (!context) {
        owned_context = std::make_unique<NanoXGenContext>();
        context = owned_context.get();
    }
    TaskExecutor &executor = context->executor();
    const std::filesystem::path resolved_descriptions_root =
        resolve_xgen_classic_descriptions_root(
            collection, collection_path, descriptions_root);
    ClassicCollectionMotionExecutionPlan result{};
    result.collection_path = collection_path;
    result.archive_path = archive_path;
    result.descriptions_root = resolved_descriptions_root;
    result.sampling = sampling;
    result.context_worker_count = executor.worker_count();
    result.descriptions.resize(collection.descriptions.size());

    const auto prepare_description = [&](std::size_t description_index) {
        const ClassicDescription &description =
            collection.descriptions[description_index];
        ClassicCollectionMotionExecutionDescription output{};
        output.name = description.name;
        output.runtime = compile_xgen_classic_float_runtime_plan(
            description, collection.palette_attributes);
        if (!output.runtime.lowering_complete()) {
            throw std::runtime_error(
                "Classic collection description '" + description.name +
                "' needs fallback: " +
                output.runtime.fallback_reasons.front());
        }
        if (description.patches.empty()) {
            throw std::runtime_error(
                "Classic collection description '" + description.name +
                "' has no patch");
        }
        const std::uint32_t cvs = output.runtime.fx_cv_count;
        output.samples.resize(sampling.lookup_offsets.size());
        for (std::size_t sample_index = 0u;
             sample_index < output.samples.size(); ++sample_index) {
            auto &sample = output.samples[sample_index];
            sample.lookup_offset = sampling.lookup_offsets[sample_index];
            sample.placement = sampling.placements[sample_index];
            sample.deformation_source_index = sample_index;
        }
        const bool static_deformation =
            output.samples.size() > 1u &&
            xgen_classic_alembic_deformation_is_static(
                description, archive_path);
        const auto import_sample = [&](std::size_t sample_index) {
            auto &sample = output.samples[sample_index];
            sample.surface = build_xgen_classic_alembic_asset_input(
                description, archive_path,
                {sampling.frame, sample.lookup_offset,
                 sampling.frames_per_second,
                 sampling.interpolation});
        };
        if (static_deformation) {
            import_sample(0u);
            for (std::size_t sample_index = 1u;
                 sample_index < output.samples.size(); ++sample_index) {
                output.samples[sample_index].deformation_source_index = 0u;
            }
        } else {
            executor.parallel_for(output.samples.size(), import_sample);
        }

        ClassicRootPlan reference_roots =
            build_xgen_classic_random_root_plan(
                description, output.samples.front().surface,
                resolved_descriptions_root / description.name);
        if (reference_roots.roots.empty() ||
            reference_roots.influence_offsets.empty()) {
            throw std::runtime_error(
                "Classic collection description '" + description.name +
                "' has no generated roots");
        }
        const ClassicRootDeformationTopology deformation_topology =
            prepare_xgen_classic_root_deformation(
                reference_roots, output.samples.front().surface);
        output.runtime_inputs = build_xgen_classic_runtime_input_data(
            output.runtime, resolved_descriptions_root / description.name,
            description.patches.front().name, reference_roots, context);

        // Re-evaluation preserves reference identity and guide association;
        // no RandomGenerator/PTEX work is repeated for later samples.
        if (!static_deformation && output.samples.size() > 1u) {
            executor.parallel_for(
                output.samples.size() - 1u, [&](std::size_t relative_index) {
                    const std::size_t sample_index =
                        relative_index + 1u;
                    output.samples[sample_index].roots =
                        deform_xgen_classic_root_plan(
                            reference_roots, deformation_topology,
                            output.samples[sample_index].surface);
                });
        }
        output.samples.front().roots.roots =
            std::move(reference_roots.roots);
        output.samples.front().roots.surface_tangents =
            std::move(reference_roots.surface_tangents);

        std::vector<std::size_t> imported_samples;
        imported_samples.reserve(output.samples.size());
        for (std::size_t sample_index = 0u;
             sample_index < output.samples.size(); ++sample_index) {
            if (output.samples[sample_index].deformation_source_index ==
                sample_index) {
                imported_samples.push_back(sample_index);
            }
        }
        executor.parallel_for(
            imported_samples.size(), [&](std::size_t imported_index) {
                auto &sample =
                    output.samples[imported_samples[imported_index]];
                sample.rebuilt_guides =
                    rebuild_xgen_classic_guides_for_device(
                        sample.surface.asset, cvs);
            });
        const auto equal_bytes = []<typename T>(
                                     const std::vector<T> &a,
                                     const std::vector<T> &b) {
            return a.size() == b.size() &&
                (a.empty() ||
                 std::memcmp(
                     a.data(), b.data(), a.size() * sizeof(T)) == 0);
        };
        for (std::size_t sample_index = 1u;
             sample_index < output.samples.size(); ++sample_index) {
            auto &sample = output.samples[sample_index];
            for (std::size_t candidate = 0u;
                 candidate < sample_index; ++candidate) {
                const auto &source = output.samples[candidate];
                if (source.deformation_source_index != candidate) {
                    continue;
                }
                if (equal_bytes(sample.roots.roots, source.roots.roots) &&
                    equal_bytes(
                        sample.roots.surface_tangents,
                        source.roots.surface_tangents) &&
                    equal_bytes(
                        sample.rebuilt_guides,
                        source.rebuilt_guides)) {
                    sample.deformation_source_index = candidate;
                    sample.surface = {};
                    sample.roots = {};
                    sample.rebuilt_guides.clear();
                    break;
                }
            }
        }
        std::vector<std::size_t> unique_samples;
        for (std::size_t sample_index = 0u;
             sample_index < output.samples.size(); ++sample_index) {
            if (output.samples[sample_index].deformation_source_index ==
                sample_index) {
                unique_samples.push_back(sample_index);
            }
        }
        // Clump guide axes genuinely deform with the patch. Static archives
        // and repeated/strobe lookups reach this expensive path only once.
        executor.parallel_for(
            unique_samples.size(), [&](std::size_t unique_index) {
                auto &sample =
                    output.samples[unique_samples[unique_index]];
                sample.clumps =
                    build_xgen_classic_clump_runtime_data_parallel(
                        description, sample.surface,
                        resolved_descriptions_root / description.name,
                        std::span<const RootSample>{sample.roots.roots},
                        output.runtime, cvs, context);
            });
        output.runtime.effects.resize(std::min<std::size_t>(
            options.effect_count, output.runtime.effects.size()));

        output.root_topology.patch_names =
            std::move(reference_roots.patch_names);
        output.root_topology.reference_positions =
            std::move(reference_roots.reference_positions);
        output.root_topology.primitive_ids =
            std::move(reference_roots.primitive_ids);
        output.root_topology.random_prefixes =
            std::move(reference_roots.random_prefixes);
        output.root_topology.influence_offsets =
            std::move(reference_roots.influence_offsets);
        output.root_topology.influences =
            std::move(reference_roots.influences);
        output.root_topology.ptex_maps =
            std::move(reference_roots.ptex_maps);
        output.root_topology.face_stats =
            std::move(reference_roots.face_stats);
        output.root_topology.candidate_count =
            reference_roots.candidate_count;
        output.root_topology.mask_rejected_count =
            reference_roots.mask_rejected_count;
        output.root_topology.patch_culled_count =
            reference_roots.patch_culled_count;
        output.root_topology.guide_rejected_count =
            reference_roots.guide_rejected_count;
        result.descriptions[description_index] = std::move(output);
    };
    if (collection.descriptions.size() <= 1u ||
        executor.worker_count() <= 1u) {
        for (std::size_t index = 0u;
             index < collection.descriptions.size(); ++index) {
            prepare_description(index);
        }
    } else {
        executor.parallel_for(
            collection.descriptions.size(), prepare_description);
    }
    return result;
}

} // namespace nanoxgen
