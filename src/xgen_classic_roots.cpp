#include "nanoxgen/xgen_classic_roots.h"

#include "xgen_classic_path.h"
#include "nanoxgen/generate.h"
#include "nanoxgen/xgen_expression.h"
#include "nanoxgen/xgen_ptex.h"
#include "nanoxgen/xgen_samples.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <memory>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nanoxgen {
namespace {

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error("Classic RandomGenerator: " + message);
}

const ClassicObject *random_generator(const ClassicDescription &description) {
    const ClassicObject *result = nullptr;
    for (const ClassicObject &object : description.objects) {
        if (object.type != "RandomGenerator") { continue; }
        if (result) { fail("description contains multiple RandomGenerator objects"); }
        result = &object;
    }
    if (!result) { fail("description has no RandomGenerator object"); }
    return result;
}

float parse_positive_float(const ClassicAttribute *attribute,
                           std::string_view label) {
    if (!attribute || attribute->value.empty()) {
        fail("missing " + std::string{label});
    }
    float result{};
    const char *begin = attribute->value.data();
    const char *end = begin + attribute->value.size();
    const auto converted = std::from_chars(begin, end, result);
    if (converted.ec != std::errc{} || converted.ptr != end ||
        !std::isfinite(result) || result < 0.0f) {
        fail(std::string{label} + " must be a finite non-negative float");
    }
    return result;
}

bool parse_optional_bool(const ClassicAttribute *attribute,
                         bool fallback, std::string_view label) {
    if (!attribute || attribute->value.empty()) { return fallback; }
    if (attribute->value == "true" || attribute->value == "1") { return true; }
    if (attribute->value == "false" || attribute->value == "0") { return false; }
    fail(std::string{label} + " must be true or false");
}

std::uint32_t description_id(const ClassicDescription &description) {
    const ClassicAttribute *attribute = find_classic_attribute(
        description.attributes, "descriptionId");
    if (!attribute) { return 0u; }
    std::uint32_t result{};
    const auto converted = std::from_chars(
        attribute->value.data(), attribute->value.data() + attribute->value.size(),
        result);
    if (converted.ec != std::errc{} ||
        converted.ptr != attribute->value.data() + attribute->value.size()) {
        fail("descriptionId must be an unsigned integer");
    }
    return result;
}

std::string uncomment(std::string_view source) {
    std::string result;
    result.reserve(source.size());
    bool quoted = false;
    char quote{};
    for (std::size_t index = 0u; index < source.size();) {
        char c = source[index];
        if (!quoted && c == '\\' && index + 1u < source.size() &&
            source[index + 1u] == 'n') {
            result.push_back('\n');
            index += 2u;
            continue;
        }
        if (!quoted && c == '#') {
            while (index < source.size() && source[index] != '\n') {
                if (source[index] == '\\' && index + 1u < source.size() &&
                    source[index + 1u] == 'n') {
                    index += 2u;
                    result.push_back('\n');
                    break;
                }
                ++index;
            }
            continue;
        }
        result.push_back(c);
        if (quoted) {
            if (c == quote && (index == 0u || source[index - 1u] != '\\')) {
                quoted = false;
            }
        } else if (c == '\'' || c == '"') {
            quoted = true;
            quote = c;
        }
        ++index;
    }
    return result;
}

std::filesystem::path map_path(
    std::string_view value, const std::filesystem::path &description_directory,
    std::string_view patch_name) {
    if (!detail::classic_safe_component(patch_name)) {
        fail("patch name is not a safe path component");
    }
    std::filesystem::path result =
        detail::resolve_classic_description_path(
            value, description_directory);
    if (!detail::classic_extension_equals(result, ".ptx")) {
        result /= std::string{patch_name} + ".ptx";
    }
    if (!std::filesystem::is_regular_file(result)) {
        fail("PTEX map does not exist: " + result.string());
    }
    return result;
}

struct BoundMapExpression {
    XgenExpressionProgram exact_program;
    XgenFloatExpressionProgram float_program;
    std::vector<std::filesystem::path> paths;
    std::vector<std::unique_ptr<XgenPtexMap>> maps;
};

float sample_xgen_generator_map(
    const XgenPtexMap &map, std::uint32_t face, float u, float v) {
    // XGen's scalar map() expression samples the authored texel directly.
    // Ptex's bilinear default is useful for general texture access but does
    // not match RandomGenerator density/mask evaluation.
    XgenPtexSampleOptions options{};
    options.filter = XgenPtexFilter::Point;
    return map.sample(face, u, v, 0u, options);
}

std::string replace_invert(std::string source) {
    for (std::size_t index = 0u; index + 2u < source.size(); ++index) {
        if (source[index] != '~' || source[index + 1u] != '$') { continue; }
        std::size_t end = index + 2u;
        while (end < source.size() &&
               ((source[end] >= 'a' && source[end] <= 'z') ||
                (source[end] >= 'A' && source[end] <= 'Z') ||
                (source[end] >= '0' && source[end] <= '9') ||
                source[end] == '_')) {
            ++end;
        }
        source.replace(index, end - index,
                       "(1.0-" + source.substr(index + 1u, end - index - 1u) + ")");
    }
    return source;
}

BoundMapExpression bind_maps(
    std::string_view source, const std::filesystem::path &description_directory,
    std::string_view patch_name) {
    const std::string cleaned = uncomment(source);
    std::string rewritten;
    rewritten.reserve(cleaned.size());
    BoundMapExpression result{};
    for (std::size_t index = 0u; index < cleaned.size();) {
        if (cleaned.compare(index, 3u, "map") != 0) {
            rewritten.push_back(cleaned[index++]);
            continue;
        }
        std::size_t cursor = index + 3u;
        while (cursor < cleaned.size() &&
               (cleaned[cursor] == ' ' || cleaned[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor >= cleaned.size() || cleaned[cursor++] != '(') {
            rewritten.push_back(cleaned[index++]);
            continue;
        }
        while (cursor < cleaned.size() &&
               (cleaned[cursor] == ' ' || cleaned[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor >= cleaned.size() ||
            (cleaned[cursor] != '\'' && cleaned[cursor] != '"')) {
            fail("map() path must be a quoted string");
        }
        const char quote = cleaned[cursor++];
        const std::size_t path_begin = cursor;
        while (cursor < cleaned.size() && cleaned[cursor] != quote) { ++cursor; }
        if (cursor >= cleaned.size()) { fail("unterminated map() path"); }
        const std::string_view value{cleaned.data() + path_begin,
                                     cursor - path_begin};
        ++cursor;
        while (cursor < cleaned.size() &&
               (cleaned[cursor] == ' ' || cleaned[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor >= cleaned.size() || cleaned[cursor++] != ')') {
            fail("map() is missing a closing parenthesis");
        }
        const std::string input = "__nxg_map_" +
                                  std::to_string(result.paths.size());
        rewritten.push_back('$');
        rewritten += input;
        result.paths.push_back(map_path(
            value, description_directory, patch_name));
        result.maps.push_back(std::make_unique<XgenPtexMap>(result.paths.back()));
        index = cursor;
    }
    rewritten = replace_invert(std::move(rewritten));
    XgenExpressionCompileOptions compile_options{};
    compile_options.expression_name = "mask";
    compile_options.object_type = "RandomGenerator";
    result.exact_program = compile_xgen_scalar_expression(
        rewritten, compile_options);
    result.float_program = make_xgen_float_expression_program(
        result.exact_program);
    if (result.exact_program.inputs.size() != result.maps.size()) {
        fail("mask contains variables other than bound map() calls");
    }
    for (std::size_t index = 0u;
         index < result.exact_program.inputs.size(); ++index) {
        if (result.exact_program.inputs[index] !=
            "__nxg_map_" + std::to_string(index)) {
            fail("map inputs have an unsupported evaluation order");
        }
    }
    return result;
}

XgenSample root_sequence(std::uint32_t description, std::uint32_t face,
                         std::string_view patch_name,
                         std::uint32_t candidate) noexcept {
    // Maya 2027 hashes (descriptionId, namespace-stripped patch-name seed,
    // faceId) with SeExpr, maps the result to one of 1600 logical groups, and
    // getSample then selects one of the sixteen resident base patterns.
    const std::uint32_t group = static_cast<std::uint32_t>(
        xgen_face_seed(description, patch_name, face) * 1600.0);
    return xgen_random_sample_exact(candidate, description, group);
}

struct GridCell {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};

    bool operator==(const GridCell &) const noexcept = default;
};

struct GridCellHash {
    std::size_t operator()(GridCell cell) const noexcept {
        std::uint64_t hash = 1469598103934665603ull;
        const auto mix = [&](std::uint32_t value) {
            hash ^= value;
            hash *= 1099511628211ull;
        };
        mix(static_cast<std::uint32_t>(cell.x));
        mix(static_cast<std::uint32_t>(cell.y));
        mix(static_cast<std::uint32_t>(cell.z));
        return static_cast<std::size_t>(hash);
    }
};

class GuideSupportIndex {
public:
    explicit GuideSupportIndex(const AssetBuildInput &asset)
        : _guides{asset.guides} {
        bool has_metadata = false;
        bool lacks_metadata = false;
        for (const GuideInput &guide : _guides) {
            has_metadata |= !guide.support_radii.empty();
            lacks_metadata |= guide.support_radii.empty();
            if (!guide.support_radii.empty()) {
                _cell_size = std::max(_cell_size, guide.support_radii.front());
            }
        }
        if (!has_metadata) { return; }
        if (lacks_metadata) {
            fail("guide support metadata is incomplete");
        }
        if (asset.reference_positions.size() != asset.positions.size() ||
            asset.reference_normals.size() != asset.positions.size()) {
            fail("reference surface data is required for guide association");
        }
        if (!std::isfinite(_cell_size) || !(_cell_size > 0.0f)) {
            fail("guide support domain is empty");
        }
        _enabled = true;
        _cells.reserve(_guides.size() * 2u);
        for (std::uint32_t index = 0u; index < _guides.size(); ++index) {
            _cells[cell(_guides[index].reference_root_position)].push_back(index);
        }
    }

    [[nodiscard]] bool enabled() const noexcept { return _enabled; }

    void gather(Vec3 position, Vec3 normal,
                const ClassicAlembicAssetInput::SurfaceFace &face,
                std::vector<ClassicGuideInfluence> &result) const {
        result.clear();
        if (!_enabled) { return; }
        const GridCell center = cell(position);
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dy = -1; dy <= 1; ++dy) {
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    const auto found = _cells.find(
                        {center.x + dx, center.y + dy, center.z + dz});
                    if (found == _cells.end()) { continue; }
                    for (const std::uint32_t index : found->second) {
                        const GuideInput &guide = _guides[index];
                        const Vec3 guide_root = guide.reference_root_position;
                        const float guide_radius = guide.support_radii.front();
                        // setupInterpolation uses the global maximum radius
                        // only for its coarse KD-tree query. It then performs
                        // this strict face-box test with each guide's own
                        // broad radius before retaining the candidate.
                        if (!(guide_root.x >
                                  face.reference_bounds_min.x - guide_radius &&
                              guide_root.x <
                                  face.reference_bounds_max.x + guide_radius &&
                              guide_root.y >
                                  face.reference_bounds_min.y - guide_radius &&
                              guide_root.y <
                                  face.reference_bounds_max.y + guide_radius &&
                              guide_root.z >
                                  face.reference_bounds_min.z - guide_radius &&
                              guide_root.z <
                                  face.reference_bounds_max.z + guide_radius)) {
                            continue;
                        }
                        const float weight =
                            guide_weight(guide, position, normal);
                        if (weight > 1.0e-5f) {
                            result.push_back({index, weight});
                        }
                    }
                }
            }
        }
        std::sort(result.begin(), result.end(),
                  [](const ClassicGuideInfluence &a,
                     const ClassicGuideInfluence &b) {
                      return a.guide_index < b.guide_index;
                  });
    }

private:
    [[nodiscard]] GridCell cell(Vec3 position) const {
        const auto coordinate = [&](float value) {
            const double scaled = std::floor(
                static_cast<double>(value) / static_cast<double>(_cell_size));
            if (scaled < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
                scaled > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
                fail("guide support grid coordinate exceeds int32");
            }
            return static_cast<std::int32_t>(scaled);
        };
        return {coordinate(position.x), coordinate(position.y),
                coordinate(position.z)};
    }

    [[nodiscard]] static float directional_radius(
        const GuideInput &guide, Vec3 direction) noexcept {
        if (guide.support_angles.empty() ||
            !(length_squared(direction) > 1.0e-20f)) {
            return guide.support_radii.front();
        }
        direction = normalize(direction);
        const float cosine = std::clamp(
            dot(direction, guide.reference_root_tangent), -1.0f, 1.0f);
        float angle = 1.0f - cosine * std::abs(cosine);
        if (dot(cross(direction, guide.reference_root_tangent),
                guide.reference_root_normal) < 0.0f) {
            angle = 4.0f - angle;
        }
        const auto upper = std::upper_bound(
            guide.support_angles.begin(), guide.support_angles.end(), angle);
        std::size_t lower_index{};
        std::size_t upper_index{};
        float lower_angle{};
        float upper_angle{};
        if (upper == guide.support_angles.begin()) {
            lower_index = guide.support_angles.size() - 1u;
            upper_index = 0u;
            lower_angle = guide.support_angles[lower_index] - 4.0f;
            upper_angle = guide.support_angles[upper_index];
        } else if (upper == guide.support_angles.end()) {
            lower_index = guide.support_angles.size() - 1u;
            upper_index = 0u;
            lower_angle = guide.support_angles[lower_index];
            upper_angle = guide.support_angles[upper_index] + 4.0f;
        } else {
            upper_index = static_cast<std::size_t>(
                upper - guide.support_angles.begin());
            lower_index = upper_index - 1u;
            lower_angle = guide.support_angles[lower_index];
            upper_angle = guide.support_angles[upper_index];
        }
        const float denominator = upper_angle - lower_angle;
        if (!(denominator > 0.0f)) {
            return guide.support_radii[upper_index + 1u];
        }
        const float blend = std::clamp(
            (angle - lower_angle) / denominator, 0.0f, 1.0f);
        return guide.support_radii[lower_index + 1u] * (1.0f - blend) +
               guide.support_radii[upper_index + 1u] * blend;
    }

public:
    [[nodiscard]] static float guide_weight(
        const GuideInput &guide, Vec3 position, Vec3 normal) noexcept {
        constexpr float kCosEightyDegrees = 0.1736481785774230957f;
        const float alignment = dot(normal, guide.reference_root_normal);
        if (!(alignment >= 0.0f)) { return 0.0f; }
        const Vec3 offset = position - guide.reference_root_position;
        const float distance_squared = length_squared(offset);
        const float broad_radius = guide.support_radii.front();
        if (!(distance_squared < broad_radius * broad_radius)) { return 0.0f; }
        const Vec3 projected = offset -
            guide.reference_root_normal *
                dot(offset, guide.reference_root_normal);
        const float radius = directional_radius(guide, projected);
        if (!(radius > 0.0f)) { return 0.0f; }
        const float distance = std::sqrt(distance_squared);
        const float falloff = std::max(0.0f, 1.0f - distance / radius);
        const float normal_fade = alignment < kCosEightyDegrees
            ? alignment / kCosEightyDegrees
            : 1.0f;
        return falloff * normal_fade;
    }

    std::span<const GuideInput> _guides;
    float _cell_size{};
    bool _enabled{};
    std::unordered_map<GridCell, std::vector<std::uint32_t>, GridCellHash> _cells;
};

} // namespace

float evaluate_xgen_classic_guide_weight(
    const GuideInput &guide, Vec3 reference_position,
    Vec3 reference_normal) noexcept {
    if (guide.support_radii.empty()) { return 0.0f; }
    return GuideSupportIndex::guide_weight(
        guide, reference_position, reference_normal);
}

float evaluate_xgen_classic_random_mask(
    const ClassicDescription &description,
    const std::filesystem::path &description_directory,
    std::string_view patch_name, std::uint32_t face_id,
    float u, float v) {
    const ClassicObject *generator = random_generator(description);
    const ClassicAttribute *mask_attribute = find_classic_attribute(
        generator->attributes, "mask");
    const std::string_view source = mask_attribute && !mask_attribute->value.empty()
        ? std::string_view{mask_attribute->value}
        : std::string_view{"1.0"};
    BoundMapExpression mask = bind_maps(
        source, description_directory, patch_name);
    std::vector<float> inputs(mask.maps.size());
    for (std::size_t index = 0u; index < mask.maps.size(); ++index) {
        if (face_id >= mask.maps[index]->info().face_count) {
            fail("PTEX map has fewer faces than the patch");
        }
        inputs[index] = sample_xgen_generator_map(
            *mask.maps[index], face_id, u, v);
    }
    std::vector<float> scratch(mask.float_program.instructions.size());
    return evaluate_xgen_scalar_expression_float(
        mask.float_program,
        {inputs, u, v,
         xgen_runtime_face_seed(
             description_id(description), patch_name, face_id),
         0.0f},
        scratch);
}

ClassicRootPlan build_xgen_classic_random_root_plan(
    const ClassicDescription &description,
    const ClassicAlembicAssetInput &surface,
    const std::filesystem::path &description_directory,
    const ClassicRootGenerationLimits &limits) {
    if (limits.max_candidates == 0u || limits.max_roots == 0u) {
        fail("limits must be nonzero");
    }
    const ClassicObject *generator = random_generator(description);
    const float density = parse_positive_float(
        find_classic_attribute(generator->attributes, "density"), "density");
    const bool surface_compensation_enabled = parse_optional_bool(
        find_classic_attribute(generator->attributes, "scFlag"), false,
        "scFlag");
    const ClassicAttribute *mask_attribute = find_classic_attribute(
        generator->attributes, "mask");
    const std::string_view mask_source = mask_attribute && !mask_attribute->value.empty()
        ? std::string_view{mask_attribute->value}
        : std::string_view{"1.0"};
    if (surface.surface_faces.empty()) { fail("surface has no selected faces"); }
    if (description.patches.size() != 1u) {
        fail("multi-patch RandomGenerator planning is not implemented");
    }
    const ClassicPatch &patch = description.patches.front();
    const std::string &patch_name = patch.name;
    BoundMapExpression mask = bind_maps(
        mask_source, description_directory, patch_name);
    ClassicRootPlan result{};
    result.ptex_maps = mask.paths;
    const std::uint32_t id = description_id(description);
    XgenExpressionCompileOptions rounding_options{};
    rounding_options.expression_name = "roundOffInternal";
    rounding_options.object_type = "RandomGenerator";
    const XgenExpressionProgram rounding = compile_xgen_scalar_expression(
        "rand()", rounding_options);
    std::vector<double> inputs(mask.maps.size());
    std::vector<double> scratch(mask.exact_program.instructions.size());
    const GuideSupportIndex guide_support{surface.asset};
    std::vector<ClassicGuideInfluence> guide_influences;
    if (guide_support.enabled()) {
        result.influence_offsets.push_back(0u);
    }
    for (const ClassicAlembicAssetInput::SurfaceFace &face :
         surface.surface_faces) {
        if (face.patch_name != patch_name || face.uv_resolution == 0u ||
            face.triangle_count != face.uv_resolution * face.uv_resolution * 2u) {
            fail("root planning currently requires one tessellated Subd patch");
        }
        // XGen evaluates density on the Alembic xgen_Pref control cage. It
        // stochastically rounds area*density per face with the private
        // roundOffInternal=rand() expression, quantizes to five decimals,
        // then applies the renderer's 100% interval as [0,1].
        double expected = face.surface_area * static_cast<double>(density);
        if (!std::isfinite(expected) || expected < 0.0f ||
            expected > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            fail("face density count is invalid");
        }
        const double integral = std::trunc(expected);
        const double fractional = expected - integral;
        const double round_threshold = evaluate_xgen_scalar_expression(
            rounding, {{}, 0.0, 0.0,
                       xgen_face_seed(id, patch_name, face.face_id), 0.0});
        if (fractional > round_threshold) { expected += 1.0; }
        expected = std::round(expected * 100000.0) / 100000.0;
        const std::uint32_t candidate_count =
            static_cast<std::uint32_t>(expected);
        if (candidate_count > limits.max_candidates - result.candidate_count) {
            fail("candidate limit exceeded");
        }
        result.candidate_count += candidate_count;
        result.face_stats.push_back({face.face_id, candidate_count, 0u, 0u});
        ClassicRootFaceStats &face_stats = result.face_stats.back();
        const auto culled_face = std::lower_bound(
            patch.culled_primitives.begin(), patch.culled_primitives.end(),
            face.face_id,
            [](const ClassicCulledPrimitiveFace &entry,
               std::uint32_t face_id) { return entry.face_id < face_id; });
        const std::span<const std::uint32_t> culled_primitives =
            culled_face != patch.culled_primitives.end() &&
                    culled_face->face_id == face.face_id
                ? std::span<const std::uint32_t>{culled_face->primitive_ids}
                : std::span<const std::uint32_t>{};
        double surface_compensation = 1.0;
        if (surface_compensation_enabled && face.center_u_length > 0.0) {
            surface_compensation = std::clamp(
                face.center_v_length / face.center_u_length, 0.1, 10.0);
            // XgRandomGenerator::getSCRatio keeps mildly anisotropic faces
            // unmodified. Maya 2027's defaults are a 10x hard clamp and a
            // symmetric 1.0001 + 0.1 deadband around one.
            constexpr double deadband = 1.1001;
            if (surface_compensation >= 1.0 / deadband &&
                surface_compensation <= deadband) {
                surface_compensation = 1.0;
            }
        }
        if (!std::isfinite(surface_compensation) ||
            surface_compensation <= 0.0) {
            fail("surface compensation ratio is invalid");
        }
        std::uint32_t accepted_candidate = 0u;
        std::uint32_t sample_index = 0u;
        double mask_threshold = 0.0;
        // Autodesk advances the density mask ramp using the five-decimal
        // stochastic expected count, before truncating it to the number of
        // candidates. Keeping the fractional denominator is observable near
        // mask boundaries (for example 92 candidates from 92.41738).
        const double mask_threshold_step = candidate_count != 0u
            ? 1.0 / expected
            : 0.0;
        while (accepted_candidate < candidate_count) {
            if (sample_index > kXgenMaximumUniqueSample) {
                fail("face exceeds the unique RandomGenerator sample range");
            }
            const XgenSample sample = root_sequence(
                id, face.face_id, patch_name, sample_index++);
            double u = sample.x;
            double v = sample.y;
            if (surface_compensation < 1.0) {
                u /= surface_compensation;
                if (u > 1.0) { continue; }
            } else if (surface_compensation > 1.0) {
                v *= surface_compensation;
                if (v > 1.0) { continue; }
            }
            ++accepted_candidate;
            const Vec2 uv{static_cast<float>(u), static_cast<float>(v)};
            for (std::size_t map_index = 0u; map_index < mask.maps.size();
                 ++map_index) {
                if (face.face_id >= mask.maps[map_index]->info().face_count) {
                    fail("PTEX map has fewer faces than the patch");
                }
                inputs[map_index] = static_cast<double>(
                    sample_xgen_generator_map(
                        *mask.maps[map_index], face.face_id, uv.x, uv.y));
            }
            const double value = evaluate_xgen_scalar_expression(
                mask.exact_program,
                {inputs, u, v,
                 xgen_face_seed(id, patch_name, face.face_id), 0.0},
                scratch);
            if (!std::isfinite(value)) { fail("mask produced a non-finite value"); }
            const double probability = std::clamp(value, 0.0, 1.0);
            // XGen walks a deterministic [0,1) ramp over the accepted surface
            // samples for a face. It does not draw an independent hash for the
            // mask test. This preserves stable density as map values animate.
            const double threshold = mask_threshold;
            mask_threshold += mask_threshold_step;
            if (threshold >= probability) {
                ++result.mask_rejected_count;
                continue;
            }
            ++face_stats.mask_accepted_count;
            // XgGenerator::cullPreGeometry consults authored Patch exclusions
            // after the density mask and before guide interpolation.
            if (std::binary_search(culled_primitives.begin(),
                                   culled_primitives.end(),
                                   accepted_candidate)) {
                ++result.patch_culled_count;
                continue;
            }
            const std::uint32_t resolution = face.uv_resolution;
            const float scaled_u = uv.x * static_cast<float>(resolution);
            const float scaled_v = uv.y * static_cast<float>(resolution);
            const std::uint32_t cell_u = std::min(
                static_cast<std::uint32_t>(scaled_u), resolution - 1u);
            const std::uint32_t cell_v = std::min(
                static_cast<std::uint32_t>(scaled_v), resolution - 1u);
            const float local_u = scaled_u - static_cast<float>(cell_u);
            const float local_v = scaled_v - static_cast<float>(cell_v);
            std::uint32_t triangle_index = face.first_triangle +
                (cell_v * resolution + cell_u) * 2u;
            Vec2 barycentric{};
            if (local_u >= local_v) {
                barycentric = {local_u - local_v, local_v};
            } else {
                ++triangle_index;
                barycentric = {local_u, local_v - local_u};
            }
            const UInt3 triangle = surface.asset.triangles[triangle_index];
            const float b0 = 1.0f - barycentric.x - barycentric.y;
            Vec3 position{};
            Vec3 normal{};
            Vec3 surface_tangent{};
            Vec3 reference_position{};
            Vec3 reference_normal{};
            if (surface.reference_surface) {
                const ClassicReferenceSurfaceSample current =
                    surface.reference_surface->evaluate_current(
                        face.patch_name, face.face_id, uv.x, uv.y);
                position = current.position;
                normal = current.normal;
                surface_tangent = current.tangent;
                const ClassicReferenceSurfaceSample reference =
                    surface.reference_surface->evaluate(
                        face.patch_name, face.face_id, uv.x, uv.y);
                reference_position = reference.position;
                reference_normal = reference.normal;
            } else {
                position = surface.asset.positions[triangle.x] * b0 +
                    surface.asset.positions[triangle.y] * barycentric.x +
                    surface.asset.positions[triangle.z] * barycentric.y;
                normal = normalize(cross(
                    surface.asset.positions[triangle.y] -
                        surface.asset.positions[triangle.x],
                    surface.asset.positions[triangle.z] -
                        surface.asset.positions[triangle.x]));
                surface_tangent = normalize(
                    surface.asset.positions[triangle.y] -
                        surface.asset.positions[triangle.x]);
                if (surface.asset.reference_positions.size() ==
                    surface.asset.positions.size()) {
                    reference_position =
                        surface.asset.reference_positions[triangle.x] * b0 +
                        surface.asset.reference_positions[triangle.y] *
                            barycentric.x +
                        surface.asset.reference_positions[triangle.z] *
                            barycentric.y;
                } else {
                    reference_position = position;
                }
                if (surface.asset.reference_normals.size() ==
                    surface.asset.positions.size()) {
                    reference_normal = normalize(
                        surface.asset.reference_normals[triangle.x] * b0 +
                        surface.asset.reference_normals[triangle.y] *
                            barycentric.x +
                        surface.asset.reference_normals[triangle.z] *
                            barycentric.y);
                } else {
                    reference_normal = normal;
                }
            }
            if (guide_support.enabled()) {
                guide_support.gather(
                    reference_position, reference_normal, face,
                    guide_influences);
                if (guide_influences.empty()) {
                    ++result.guide_rejected_count;
                    continue;
                }
            }
            if (result.roots.size() >= limits.max_roots) {
                fail("root limit exceeded");
            }
            result.roots.push_back({position, normal, uv, triangle_index,
                                    barycentric, face.face_id});
            result.patch_names.push_back(face.patch_name);
            result.reference_positions.push_back(reference_position);
            result.surface_tangents.push_back(surface_tangent);
            result.primitive_ids.push_back(accepted_candidate);
            const std::array random_arguments{
                u, v, xgen_face_seed(id, patch_name, face.face_id)};
            result.random_prefixes.push_back(
                xgen_seexpr_hash_prefix(random_arguments));
            if (guide_support.enabled()) {
                if (guide_influences.size() >
                    std::numeric_limits<std::uint32_t>::max() -
                        result.influences.size()) {
                    fail("guide influence count exceeds uint32");
                }
                result.influences.insert(
                    result.influences.end(), guide_influences.begin(),
                    guide_influences.end());
                result.influence_offsets.push_back(
                    static_cast<std::uint32_t>(result.influences.size()));
            }
            ++face_stats.root_count;
        }
    }
    return result;
}

ClassicRootPlan build_xgen_classic_explicit_root_plan(
    const ClassicDescription &description,
    const ClassicAlembicAssetInput &surface,
    std::string_view patch_name,
    std::span<const ClassicExplicitRoot> samples) {
    if (!surface.reference_surface) {
        fail("explicit roots require an imported subdivision surface");
    }
    GuideSupportIndex guide_support{surface.asset};
    if (!guide_support.enabled()) {
        fail("explicit roots require Classic guide support metadata");
    }
    ClassicRootPlan result{};
    result.roots.reserve(samples.size());
    result.reference_positions.reserve(samples.size());
    result.surface_tangents.reserve(samples.size());
    result.primitive_ids.reserve(samples.size());
    result.random_prefixes.reserve(samples.size());
    result.influence_offsets.reserve(samples.size() + 1u);
    result.influence_offsets.push_back(0u);
    std::vector<ClassicGuideInfluence> influences;
    for (const ClassicExplicitRoot &sample : samples) {
        if (!std::isfinite(sample.uv.x) || !std::isfinite(sample.uv.y) ||
            sample.uv.x < 0.0f || sample.uv.x > 1.0f ||
            sample.uv.y < 0.0f || sample.uv.y > 1.0f ||
            !std::isfinite(sample.position.x) ||
            !std::isfinite(sample.position.y) ||
            !std::isfinite(sample.position.z)) {
            fail("explicit root contains a non-finite or out-of-range value");
        }
        const ClassicReferenceSurfaceSample reference =
            surface.reference_surface->evaluate(
                patch_name, sample.face_id, sample.uv.x, sample.uv.y);
        const ClassicReferenceSurfaceSample current =
            surface.reference_surface->evaluate_current(
                patch_name, sample.face_id, sample.uv.x, sample.uv.y);
        const auto face = std::find_if(
            surface.surface_faces.begin(), surface.surface_faces.end(),
            [&](const ClassicAlembicAssetInput::SurfaceFace &candidate) {
                return candidate.patch_name == patch_name &&
                    candidate.face_id == sample.face_id;
            });
        if (face == surface.surface_faces.end()) {
            fail("explicit root face is not present in the imported surface");
        }
        guide_support.gather(
            reference.position, reference.normal, *face, influences);
        if (influences.empty()) {
            fail("explicit root has no associated guide");
        }
        // XPD Location xyz records the surface position at the time the point
        // map was authored. Maya's SESubd evaluation and OpenSubdiv can differ
        // by a few float ulps even on an unchanged patch; those ulps become
        // visible when a clump guide has a reversing terminal segment and
        // stacked NoiseFX transports a frame through it. XGen retains the
        // authored location in that static case. Snap only within a bounded
        // float-roundoff envelope so a genuinely deformed patch still follows
        // its current face/u/v evaluation.
        Vec3 current_position = current.position;
        const float position_scale = std::max({
            1.0f, std::abs(sample.position.x), std::abs(sample.position.y),
            std::abs(sample.position.z)});
        const float snap_tolerance =
            32.0f * std::numeric_limits<float>::epsilon() * position_scale;
        if (length_squared(sample.position - current.position) <=
            snap_tolerance * snap_tolerance) {
            current_position = sample.position;
        }
        result.roots.push_back({
            current_position, current.normal, sample.uv, 0u, {},
            sample.face_id});
        result.patch_names.emplace_back(patch_name);
        result.reference_positions.push_back(reference.position);
        result.surface_tangents.push_back(current.tangent);
        result.primitive_ids.push_back(sample.primitive_id);
        const std::array random_arguments{
            static_cast<double>(sample.uv.x),
            static_cast<double>(sample.uv.y),
            xgen_face_seed(description_id(description), patch_name,
                           sample.face_id)};
        result.random_prefixes.push_back(
            xgen_seexpr_hash_prefix(random_arguments));
        result.influences.insert(
            result.influences.end(), influences.begin(), influences.end());
        if (result.influences.size() >
            std::numeric_limits<std::uint32_t>::max()) {
            fail("explicit guide influence count exceeds uint32");
        }
        result.influence_offsets.push_back(
            static_cast<std::uint32_t>(result.influences.size()));
    }
    return result;
}

ClassicRootDeformationTopology prepare_xgen_classic_root_deformation(
    const ClassicRootPlan &reference,
    const ClassicAlembicAssetInput &reference_surface) {
    const std::size_t count = reference.roots.size();
    if (count == 0u || reference.patch_names.size() != count ||
        reference.surface_tangents.size() != count ||
        reference.reference_positions.size() != count ||
        reference.primitive_ids.size() != count ||
        reference.random_prefixes.size() != count ||
        reference.influence_offsets.size() != count + 1u) {
        fail("motion reference root plan is inconsistent");
    }
    struct FaceIdentity {
        std::string_view patch;
        std::uint32_t face{};
        bool operator==(const FaceIdentity &) const noexcept = default;
    };
    struct FaceIdentityHash {
        std::size_t operator()(const FaceIdentity &value) const noexcept {
            std::size_t hash = std::hash<std::string_view>{}(value.patch);
            hash ^= static_cast<std::size_t>(value.face) +
                0x9e3779b9u + (hash << 6u) + (hash >> 2u);
            return hash;
        }
    };
    struct RootIdentity {
        std::uint32_t face_index{};
        std::uint32_t u{};
        std::uint32_t v{};
        bool operator==(const RootIdentity &) const noexcept = default;
    };
    if (reference_surface.surface_faces.size() >
        std::numeric_limits<std::uint32_t>::max()) {
        fail("motion surface face count exceeds uint32");
    }
    std::unordered_map<FaceIdentity, std::uint32_t, FaceIdentityHash>
        face_indices;
    face_indices.reserve(reference_surface.surface_faces.size());
    for (std::size_t index = 0u;
         index < reference_surface.surface_faces.size(); ++index) {
        const auto &face = reference_surface.surface_faces[index];
        if (!face_indices.emplace(
                FaceIdentity{face.patch_name, face.face_id},
                static_cast<std::uint32_t>(index)).second) {
            fail("motion surface contains a duplicate coarse face identity");
        }
    }
    ClassicRootDeformationTopology result{};
    result.surface_face_indices.resize(count);
    std::vector<RootIdentity> identities;
    identities.reserve(count);
    for (std::size_t index = 0u; index < count; ++index) {
        const RootSample &root = reference.roots[index];
        const FaceIdentity face{
            reference.patch_names[index], root.surface_face_id};
        const auto found = face_indices.find(face);
        if (found == face_indices.end()) {
            fail("motion sample is missing a reference root face");
        }
        result.surface_face_indices[index] = found->second;
        identities.push_back({
            found->second, std::bit_cast<std::uint32_t>(root.uv.x),
            std::bit_cast<std::uint32_t>(root.uv.y)});
    }
    std::sort(
        identities.begin(), identities.end(),
        [](const RootIdentity &a, const RootIdentity &b) {
            if (a.face_index != b.face_index) {
                return a.face_index < b.face_index;
            }
            if (a.u != b.u) { return a.u < b.u; }
            return a.v < b.v;
        });
    if (std::adjacent_find(identities.begin(), identities.end()) !=
        identities.end()) {
        fail("motion reference contains a duplicate root identity");
    }
    return result;
}

ClassicDeformedRootPlan deform_xgen_classic_root_plan(
    const ClassicRootPlan &reference,
    const ClassicRootDeformationTopology &topology,
    const ClassicAlembicAssetInput &surface) {
    const std::size_t count = reference.roots.size();
    if (topology.surface_face_indices.size() != count) {
        fail("motion root deformation topology is inconsistent");
    }
    ClassicDeformedRootPlan result{};
    result.roots.resize(count);
    result.surface_tangents.resize(count);
    for (std::size_t index = 0u; index < count; ++index) {
        const RootSample &reference_root = reference.roots[index];
        RootSample root = reference_root;
        const std::uint32_t face_index =
            topology.surface_face_indices[index];
        if (face_index >= surface.surface_faces.size()) {
            fail("motion sample is missing a reference root face");
        }
        const auto &face = surface.surface_faces[face_index];
        if (face.patch_name != reference.patch_names[index] ||
            face.face_id != reference_root.surface_face_id) {
            fail("motion surface coarse face topology changed");
        }
        if (face.uv_resolution != 0u) {
            if (!surface.reference_surface) {
                fail("motion subdivision sample has no surface evaluator");
            }
            const ClassicReferenceSurfaceSample current =
                surface.reference_surface->evaluate_current(
                    face.patch_name, face.face_id,
                    reference_root.uv.x, reference_root.uv.y);
            root.position = current.position;
            root.normal = current.normal;
            result.surface_tangents[index] = current.tangent;

            const std::uint32_t resolution = face.uv_resolution;
            const float scaled_u =
                reference_root.uv.x * static_cast<float>(resolution);
            const float scaled_v =
                reference_root.uv.y * static_cast<float>(resolution);
            const std::uint32_t cell_u = std::min(
                static_cast<std::uint32_t>(scaled_u), resolution - 1u);
            const std::uint32_t cell_v = std::min(
                static_cast<std::uint32_t>(scaled_v), resolution - 1u);
            const float local_u = scaled_u - static_cast<float>(cell_u);
            const float local_v = scaled_v - static_cast<float>(cell_v);
            root.triangle_index = face.first_triangle +
                (cell_v * resolution + cell_u) * 2u;
            if (local_u >= local_v) {
                root.barycentric = {local_u - local_v, local_v};
            } else {
                ++root.triangle_index;
                root.barycentric = {local_u, local_v - local_u};
            }
        } else {
            if (reference_root.triangle_index >=
                surface.asset.triangles.size()) {
                fail("motion polygon root triangle is out of range");
            }
            const UInt3 triangle =
                surface.asset.triangles[reference_root.triangle_index];
            if (triangle.x >= surface.asset.positions.size() ||
                triangle.y >= surface.asset.positions.size() ||
                triangle.z >= surface.asset.positions.size()) {
                fail("motion polygon root topology changed");
            }
            const float b0 =
                1.0f - reference_root.barycentric.x -
                reference_root.barycentric.y;
            root.position =
                surface.asset.positions[triangle.x] * b0 +
                surface.asset.positions[triangle.y] *
                    reference_root.barycentric.x +
                surface.asset.positions[triangle.z] *
                    reference_root.barycentric.y;
            root.normal = normalize(
                surface.asset.normals[triangle.x] * b0 +
                surface.asset.normals[triangle.y] *
                    reference_root.barycentric.x +
                surface.asset.normals[triangle.z] *
                    reference_root.barycentric.y);
            result.surface_tangents[index] = normalize(
                surface.asset.positions[triangle.y] -
                surface.asset.positions[triangle.x]);
        }
        if (!std::isfinite(root.position.x) ||
            !std::isfinite(root.position.y) ||
            !std::isfinite(root.position.z) ||
            !std::isfinite(root.normal.x) ||
            !std::isfinite(root.normal.y) ||
            !std::isfinite(root.normal.z)) {
            fail("motion root evaluation produced a non-finite value");
        }
        result.roots[index] = root;
    }
    return result;
}

ClassicDeformedRootPlan deform_xgen_classic_root_plan(
    const ClassicRootPlan &reference,
    const ClassicAlembicAssetInput &surface) {
    return deform_xgen_classic_root_plan(
        reference,
        prepare_xgen_classic_root_deformation(reference, surface),
        surface);
}

namespace {

struct ClassicDouble3 {
    double x{};
    double y{};
    double z{};
};

ClassicDouble3 operator+(ClassicDouble3 a, ClassicDouble3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

ClassicDouble3 operator-(ClassicDouble3 a, ClassicDouble3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

ClassicDouble3 operator*(ClassicDouble3 a, double scale) noexcept {
    return {a.x * scale, a.y * scale, a.z * scale};
}

ClassicDouble3 xgen_curve_eval(
    std::span<const ClassicDouble3> points, double t) {
    constexpr double epsilon = 1.0e-7;
    if (t < epsilon) { return points.front(); }
    if (t > 1.0 - epsilon) { return points.back(); }
    const std::uint32_t spans =
        static_cast<std::uint32_t>(points.size() - 1u);
    const double scaled = t * static_cast<double>(spans);
    const std::uint32_t span = static_cast<std::uint32_t>(scaled);
    const double f = scaled - static_cast<double>(span);
    const double f2 = f * f;
    const double f3 = f * f2;
    const double one_minus_f = 1.0 - f;
    const double b0 = one_minus_f * one_minus_f * one_minus_f;
    const double b1 = 3.0 * f3 - 6.0 * f2 + 4.0;
    const double b2 = -3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0;
    const double b3 = f3;
    const ClassicDouble3 p0 = span == 0u
        ? points[0u] * 2.0 - points[1u]
        : points[span - 1u];
    const ClassicDouble3 p1 = points[span];
    const ClassicDouble3 p2 = points[span + 1u];
    const ClassicDouble3 p3 = span + 2u == points.size()
        ? points.back() * 2.0 - points[points.size() - 2u]
        : points[span + 2u];
    return (p0 * b0 + p1 * b1 + p2 * b2 + p3 * b3) * (1.0 / 6.0);
}

std::vector<ClassicDouble3> xgen_rebuild_uniform(
    std::span<const Vec3> source, std::uint32_t output_count) {
    if (source.empty()) { fail("Classic guide has no CVs"); }
    output_count = std::max(output_count, 3u);
    std::vector<ClassicDouble3> input;
    input.reserve(source.size());
    for (const Vec3 point : source) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
            !std::isfinite(point.z)) {
            fail("Classic guide has a non-finite CV");
        }
        input.push_back({point.x, point.y, point.z});
    }
    if (input.size() == 1u) {
        return std::vector<ClassicDouble3>(output_count, input.front());
    }
    std::vector<double> lengths(input.size() - 1u);
    double total_length = 0.0;
    for (std::size_t index = input.size() - 1u; index > 0u; --index) {
        const ClassicDouble3 delta = input[index] - input[index - 1u];
        const double length = std::sqrt(
            delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        lengths[index - 1u] = length;
        total_length += length;
    }
    if (total_length < 1.0e-7) {
        // This is the fallback used by rebuildUniform for degenerate control
        // polygons; rebuildNonUniform still preserves both endpoints.
        std::vector<ClassicDouble3> result(output_count);
        for (std::uint32_t index = 0u; index < output_count; ++index) {
            const double t = static_cast<double>(index) /
                             static_cast<double>(output_count - 1u);
            result[index] = xgen_curve_eval(input, t);
        }
        return result;
    }
    std::vector<ClassicDouble3> result(output_count);
    result.front() = input.front();
    const double step = total_length /
                        static_cast<double>(output_count - 1u);
    double remaining = 0.0;
    std::size_t segment = 0u;
    for (std::uint32_t index = 1u; index + 1u < output_count; ++index) {
        remaining += step;
        while (segment + 1u < lengths.size() &&
               remaining > lengths[segment]) {
            remaining -= lengths[segment++];
        }
        const double fraction = lengths[segment] > 0.0
            ? remaining / lengths[segment]
            : 0.0;
        const double t = (static_cast<double>(segment) + fraction) /
                         static_cast<double>(input.size() - 1u);
        result[index] = xgen_curve_eval(input, t);
    }
    result.back() = input.back();
    return result;
}

} // namespace

std::vector<Vec3> rebuild_xgen_classic_guides_for_device(
    const AssetBuildInput &asset,
    std::uint32_t cvs_per_guide) {
    if (cvs_per_guide < 3u || asset.guides.empty()) {
        throw std::invalid_argument(
            "Classic guide rebuild needs guides and at least three CVs");
    }
    std::vector<Vec3> result;
    if (asset.guides.size() >
        std::numeric_limits<std::size_t>::max() / cvs_per_guide) {
        throw std::overflow_error("Classic rebuilt guide count is too large");
    }
    result.reserve(asset.guides.size() * cvs_per_guide);
    for (const GuideInput &guide : asset.guides) {
        const std::vector<ClassicDouble3> rebuilt =
            xgen_rebuild_uniform(guide.cvs, cvs_per_guide);
        for (const ClassicDouble3 point : rebuilt) {
            result.push_back({static_cast<float>(point.x),
                              static_cast<float>(point.y),
                              static_cast<float>(point.z)});
        }
    }
    return result;
}

PackedGeneratedCurves generate_xgen_classic_base_curves_cpu(
    const AssetBuildInput &asset,
    const ClassicRootPlan &roots,
    std::uint32_t cvs_per_strand,
    float diameter,
    float radius_scale,
    bool root_relative) {
    if (cvs_per_strand < 3u || roots.roots.empty() ||
        roots.influence_offsets.size() != roots.roots.size() + 1u ||
        roots.influence_offsets.front() != 0u ||
        roots.influence_offsets.back() != roots.influences.size()) {
        throw std::invalid_argument(
            "Classic base generation needs roots with guide associations");
    }
    if (!std::isfinite(diameter) || diameter < 0.0f ||
        !std::isfinite(radius_scale) || radius_scale < 0.0f) {
        throw std::invalid_argument("Classic base curve width is invalid");
    }
    std::vector<std::vector<ClassicDouble3>> rebuilt_guides;
    rebuilt_guides.reserve(asset.guides.size());
    for (const GuideInput &guide : asset.guides) {
        rebuilt_guides.push_back(
            xgen_rebuild_uniform(guide.cvs, cvs_per_strand));
    }
    PackedGeneratedCurves result{};
    result.strand_count = static_cast<std::uint32_t>(roots.roots.size());
    result.cvs_per_strand = cvs_per_strand;
    result.point_counts.assign(result.strand_count, cvs_per_strand);
    result.roots = roots.roots;
    result.root_uvs.reserve(result.strand_count);
    for (const RootSample &root : roots.roots) {
        result.root_uvs.push_back(root.uv);
    }
    result.points.resize(
        static_cast<std::size_t>(result.strand_count) * cvs_per_strand);
    const float radius = 0.5f * diameter * radius_scale;
    for (std::uint32_t strand = 0u; strand < result.strand_count; ++strand) {
        const std::uint32_t begin = roots.influence_offsets[strand];
        const std::uint32_t end = roots.influence_offsets[strand + 1u];
        if (begin >= end || end > roots.influences.size()) {
            throw std::invalid_argument(
                "Classic guide association range is invalid");
        }
        double weight_sum = 0.0;
        for (std::uint32_t index = begin; index < end; ++index) {
            const ClassicGuideInfluence influence = roots.influences[index];
            if (influence.guide_index >= rebuilt_guides.size() ||
                !std::isfinite(influence.weight) ||
                !(influence.weight > 0.0f)) {
                throw std::invalid_argument(
                    "Classic guide association is invalid");
            }
            weight_sum += influence.weight;
        }
        if (!(weight_sum > 0.0) || !std::isfinite(weight_sum)) {
            throw std::invalid_argument("Classic guide weight sum is invalid");
        }
        const RootSample &root = roots.roots[strand];
        for (std::uint32_t cv = 0u; cv < cvs_per_strand; ++cv) {
            ClassicDouble3 offset{};
            for (std::uint32_t index = begin; index < end; ++index) {
                const ClassicGuideInfluence influence = roots.influences[index];
                const std::vector<ClassicDouble3> &guide =
                    rebuilt_guides[influence.guide_index];
                offset = offset + (guide[cv] - guide.front()) *
                    static_cast<double>(influence.weight);
            }
            offset = offset * (1.0 / weight_sum);
            PackedCurvePoint &point = result.points[
                static_cast<std::size_t>(strand) * cvs_per_strand + cv];
            point = {static_cast<float>(
                         offset.x +
                         (root_relative
                              ? 0.0
                              : static_cast<double>(root.position.x))),
                     static_cast<float>(
                         offset.y +
                         (root_relative
                              ? 0.0
                              : static_cast<double>(root.position.y))),
                     static_cast<float>(
                         offset.z +
                         (root_relative
                              ? 0.0
                              : static_cast<double>(root.position.z))),
                     radius};
        }
    }
    return result;
}

void make_xgen_classic_curves_world_space(PackedGeneratedCurves &curves) {
    if (curves.roots.size() != curves.strand_count ||
        curves.point_counts.size() != curves.strand_count) {
        throw std::invalid_argument(
            "Classic world translation needs one root per strand");
    }
    std::size_t point_offset = 0u;
    for (std::uint32_t strand = 0u; strand < curves.strand_count; ++strand) {
        const RootSample &root = curves.roots[strand];
        const std::size_t count = curves.point_counts[strand];
        if (point_offset > curves.points.size() ||
            count > curves.points.size() - point_offset) {
            throw std::invalid_argument(
                "Classic world translation point counts are inconsistent");
        }
        for (std::size_t cv = 0u; cv < count; ++cv) {
            PackedCurvePoint &point = curves.points[point_offset + cv];
            point.x += root.position.x;
            point.y += root.position.y;
            point.z += root.position.z;
        }
        point_offset += count;
    }
    if (point_offset != curves.points.size()) {
        throw std::invalid_argument(
            "Classic world translation did not consume every point");
    }
}

} // namespace nanoxgen
