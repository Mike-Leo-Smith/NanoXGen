#include "nanoxgen/xgen_classic_alembic.h"

#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcGeom/All.h>
#include <opensubdiv/far/patchMap.h>
#include <opensubdiv/far/patchTableFactory.h>
#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/ptexIndices.h>
#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyRefinerFactory.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nanoxgen {
namespace {

namespace Abc = Alembic::Abc;
namespace AbcGeom = Alembic::AbcGeom;
namespace AbcFactory = Alembic::AbcCoreFactory;
namespace AbcCore = Alembic::AbcCoreAbstract;
namespace Far = OpenSubdiv::Far;
namespace Sdc = OpenSubdiv::Sdc;

struct LoadedMesh {
    std::vector<Imath::V3f> positions;
    std::vector<Imath::V3f> reference_positions;
    std::vector<std::int32_t> face_counts;
    std::vector<std::int32_t> face_indices;
    Imath::M44d transform{};
};

struct ArchiveSampleSelection {
    bool first_sample{true};
    double time_seconds{};
    ClassicAlembicInterpolation interpolation{
        ClassicAlembicInterpolation::None};
};

struct SubdPosition {
    float x{};
    float y{};
    float z{};

    void Clear() noexcept { x = y = z = 0.0f; }
    void AddWithWeight(const SubdPosition &source, float weight) noexcept {
        x += source.x * weight;
        y += source.y * weight;
        z += source.z * weight;
    }
};

struct Double3 {
    double x{};
    double y{};
    double z{};
};

struct SubdSample {
    Vec3 position{};
    Vec3 du{};
    Vec3 dv{};
};

struct SurfaceFrame {
    Vec3 normal{};
    Vec3 tangent{};
    Vec3 binormal{};
};

SurfaceFrame xgen_surface_frame(Vec3 du, Vec3 dv) {
    // XGen's patch v increases in the opposite direction from OpenSubdiv's
    // Ptex v. XgPatch::evalFrame then rotates the two unit parameter tangents
    // away from one another by half of their deviation from 90 degrees. The
    // normalized sum/difference form performs the same symmetric
    // orthogonalization without trigonometry.
    using D3 = std::array<double, 3u>;
    const auto add = [](D3 a, D3 b) {
        return D3{a[0] + b[0], a[1] + b[1], a[2] + b[2]};
    };
    const auto subtract = [](D3 a, D3 b) {
        return D3{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    const auto scale = [](D3 value, double amount) {
        return D3{
            value[0] * amount, value[1] * amount, value[2] * amount};
    };
    const auto normalized = [&](D3 value) {
        const double length_squared =
            value[0] * value[0] + value[1] * value[1] +
            value[2] * value[2];
        if (!(length_squared > 1.0e-30)) {
            throw std::runtime_error(
                "Classic Alembic import: zero surface parameter tangent");
        }
        return scale(value, 1.0 / std::sqrt(length_squared));
    };
    const auto crossed = [](D3 a, D3 b) {
        return D3{
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
    };
    const D3 u_tangent = normalized({
        static_cast<double>(du.x), static_cast<double>(du.y),
        static_cast<double>(du.z)});
    const D3 v_tangent = normalized({
        -static_cast<double>(dv.x), -static_cast<double>(dv.y),
        -static_cast<double>(dv.z)});
    const D3 sum = add(u_tangent, v_tangent);
    const D3 difference = subtract(u_tangent, v_tangent);
    const double sum_length_squared =
        sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2];
    const double difference_length_squared =
        difference[0] * difference[0] +
        difference[1] * difference[1] +
        difference[2] * difference[2];
    if (!(sum_length_squared > 1.0e-30) ||
        !(difference_length_squared > 1.0e-30)) {
        throw std::runtime_error(
            "Classic Alembic import: surface parameter tangents cannot form an XGen frame");
    }
    constexpr double kInverseSqrtTwo = 0.707106781186547524400844362104849;
    const D3 tangent = scale(
        add(normalized(sum), normalized(difference)), kInverseSqrtTwo);
    const D3 binormal = scale(
        subtract(normalized(sum), normalized(difference)), kInverseSqrtTwo);
    const D3 normal = normalized(crossed(tangent, binormal));
    const auto to_float = [](D3 value) {
        return Vec3{
            static_cast<float>(value[0]), static_cast<float>(value[1]),
            static_cast<float>(value[2])};
    };
    return {to_float(normal), to_float(tangent), to_float(binormal)};
}

Double3 to_double(Vec3 value) noexcept {
    return {static_cast<double>(value.x), static_cast<double>(value.y),
            static_cast<double>(value.z)};
}

Double3 operator-(Double3 lhs, Double3 rhs) noexcept {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

Double3 operator+(Double3 lhs, Double3 rhs) noexcept {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

Double3 operator*(Double3 value, double scale) noexcept {
    return {value.x * scale, value.y * scale, value.z * scale};
}

Double3 cross(Double3 lhs, Double3 rhs) noexcept {
    return {lhs.y * rhs.z - lhs.z * rhs.y,
            lhs.z * rhs.x - lhs.x * rhs.z,
            lhs.x * rhs.y - lhs.y * rhs.x};
}

double length(Double3 value) noexcept {
    return std::sqrt(value.x * value.x + value.y * value.y +
                     value.z * value.z);
}

double triangle_double_area(Double3 a, Double3 b, Double3 c) noexcept {
    return length(cross(b - a, c - a));
}

// SgSubdSurface::area evaluates the float control cage, not the OpenSubdiv
// limit surface. For a quad it averages the double-areas of all four corner
// triples. This is symmetric for non-planar cages and equals the usual area
// for a planar quad.
double xgen_quad_area(const Double3 (&p)[4]) noexcept {
    return 0.25 * (triangle_double_area(p[0], p[1], p[2]) +
                   triangle_double_area(p[0], p[1], p[3]) +
                   triangle_double_area(p[0], p[2], p[3]) +
                   triangle_double_area(p[1], p[2], p[3]));
}

// SgSubdSurface::lengthU/V use straight spans between opposite control-cage
// edges. XGen requests both at 0.5 for surface compensation.
double xgen_quad_length_u(const Double3 (&p)[4]) noexcept {
    return length((p[3] + p[2]) * 0.5 - (p[0] + p[1]) * 0.5);
}

double xgen_quad_length_v(const Double3 (&p)[4]) noexcept {
    return length((p[1] + p[2]) * 0.5 - (p[0] + p[3]) * 0.5);
}

struct MeshSearch {
    std::string target;
    std::size_t visited{};
    std::size_t max_objects{};
    ArchiveSampleSelection selection;
    std::vector<std::pair<Abc::IObject, Imath::M44d>> matches;
};

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error("Classic Alembic import: " + message);
}

bool finite(const Imath::V3d &value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

struct SampleBlend {
    AbcCore::index_t lower{};
    AbcCore::index_t upper{};
    double alpha{};
};

template<typename Schema>
SampleBlend select_samples(
    const Schema &schema, const ArchiveSampleSelection &selection) {
    const std::size_t sample_count = schema.getNumSamples();
    if (sample_count == 0u) { fail("schema has no samples"); }
    if (selection.first_sample || sample_count == 1u) {
        return {};
    }
    const auto sampling = schema.getTimeSampling();
    if (!sampling) { fail("schema has no time sampling"); }
    const auto lower = sampling->getFloorIndex(
        selection.time_seconds,
        static_cast<AbcCore::index_t>(sample_count));
    if (selection.interpolation == ClassicAlembicInterpolation::None) {
        return {lower.first, lower.first, 0.0};
    }
    const auto upper = sampling->getCeilIndex(
        selection.time_seconds,
        static_cast<AbcCore::index_t>(sample_count));
    if (lower.first == upper.first || !(upper.second > lower.second)) {
        return {lower.first, lower.first, 0.0};
    }
    const double alpha = std::clamp(
        (selection.time_seconds - lower.second) /
            (upper.second - lower.second),
        0.0, 1.0);
    return {lower.first, upper.first, alpha};
}

Imath::M44d sample_transform(
    const AbcGeom::IXformSchema &schema,
    const ArchiveSampleSelection &selection) {
    const SampleBlend blend = select_samples(schema, selection);
    AbcGeom::XformSample lower;
    schema.get(lower, Abc::ISampleSelector{blend.lower});
    Imath::M44d result = lower.getMatrix();
    if (blend.lower == blend.upper) { return result; }
    AbcGeom::XformSample upper;
    schema.get(upper, Abc::ISampleSelector{blend.upper});
    if (!lower.isTopologyEqual(upper) ||
        lower.getInheritsXforms() != upper.getInheritsXforms()) {
        fail("transform topology changes between motion samples");
    }
    AbcGeom::XformSample interpolated;
    interpolated.setInheritsXforms(lower.getInheritsXforms());
    for (std::size_t op_index = 0u;
         op_index < lower.getNumOps(); ++op_index) {
        AbcGeom::XformOp op = lower.getOp(op_index);
        const AbcGeom::XformOp upper_op = upper.getOp(op_index);
        for (std::size_t channel = 0u;
             channel < op.getNumChannels(); ++channel) {
            const double value =
                op.getChannelValue(channel) * (1.0 - blend.alpha) +
                upper_op.getChannelValue(channel) * blend.alpha;
            if (!std::isfinite(value)) {
                fail("transform interpolation produced a non-finite value");
            }
            op.setChannelValue(channel, value);
        }
        interpolated.addOp(op);
    }
    return interpolated.getMatrix();
}

void find_meshes(const Abc::IObject &object, const Imath::M44d &parent_transform,
                 bool inside_target, MeshSearch &search) {
    if (++search.visited > search.max_objects) {
        fail("object limit exceeded");
    }
    Imath::M44d transform = parent_transform;
    if (AbcGeom::IXform::matches(object.getHeader())) {
        AbcGeom::IXform xform{object, Abc::kWrapExisting};
        if (xform.getSchema().getNumSamples() != 0u) {
            transform = sample_transform(
                xform.getSchema(), search.selection) * parent_transform;
        }
    }
    const bool selected = inside_target || object.getName() == search.target;
    if (selected && (AbcGeom::IPolyMesh::matches(object.getHeader()) ||
                     AbcGeom::ISubD::matches(object.getHeader()))) {
        search.matches.emplace_back(object, transform);
    }
    for (std::size_t index = 0u; index < object.getNumChildren(); ++index) {
        const Abc::ObjectHeader &header = object.getChildHeader(index);
        find_meshes(Abc::IObject{object, header.getName()}, transform,
                    selected, search);
    }
}

void inspect_static_deformation(
    const Abc::IObject &object, std::string_view target,
    bool inside_target, bool animated_ancestor,
    std::size_t &visited, std::size_t max_objects,
    std::size_t &matches, bool &is_static) {
    if (++visited > max_objects) { fail("object limit exceeded"); }
    bool animated = animated_ancestor;
    if (AbcGeom::IXform::matches(object.getHeader())) {
        AbcGeom::IXform xform{object, Abc::kWrapExisting};
        animated |= xform.getSchema().getNumSamples() > 1u;
    }
    const bool selected = inside_target || object.getName() == target;
    if (selected && (AbcGeom::IPolyMesh::matches(object.getHeader()) ||
                     AbcGeom::ISubD::matches(object.getHeader()))) {
        ++matches;
        std::size_t samples{};
        if (AbcGeom::IPolyMesh::matches(object.getHeader())) {
            AbcGeom::IPolyMesh mesh{object, Abc::kWrapExisting};
            samples = mesh.getSchema().getNumSamples();
        } else {
            AbcGeom::ISubD mesh{object, Abc::kWrapExisting};
            samples = mesh.getSchema().getNumSamples();
        }
        is_static &= !animated && samples <= 1u;
    }
    for (std::size_t index = 0u; index < object.getNumChildren(); ++index) {
        const Abc::ObjectHeader &header = object.getChildHeader(index);
        inspect_static_deformation(
            Abc::IObject{object, header.getName()}, target, selected,
            animated, visited, max_objects, matches, is_static);
    }
}

template<typename Sample>
LoadedMesh copy_mesh_sample(const Sample &sample, const Imath::M44d &transform,
                            const ClassicAlembicLimits &limits) {
    const auto positions = sample.getPositions();
    const auto counts = sample.getFaceCounts();
    const auto indices = sample.getFaceIndices();
    if (!positions || !counts || !indices) {
        fail("mesh sample is missing positions or topology");
    }
    if (positions->size() == 0u || positions->size() > limits.max_vertices) {
        fail("vertex count is empty or exceeds the limit");
    }
    if (counts->size() == 0u || counts->size() > limits.max_faces) {
        fail("face count is empty or exceeds the limit");
    }
    if (indices->size() > limits.max_face_vertices) {
        fail("face-vertex count exceeds the limit");
    }
    LoadedMesh result{};
    result.positions.assign(positions->get(), positions->get() + positions->size());
    result.reference_positions = result.positions;
    result.face_counts.assign(counts->get(), counts->get() + counts->size());
    result.face_indices.assign(indices->get(), indices->get() + indices->size());
    result.transform = transform;
    return result;
}

template<typename Schema, typename Sample>
LoadedMesh load_mesh_sample(
    const Schema &schema, const Imath::M44d &transform,
    const ClassicAlembicLimits &limits,
    const ArchiveSampleSelection &selection) {
    const SampleBlend blend = select_samples(schema, selection);
    Sample lower;
    schema.get(lower, Abc::ISampleSelector{blend.lower});
    LoadedMesh result = copy_mesh_sample(lower, transform, limits);
    if (blend.lower == blend.upper) { return result; }
    Sample upper;
    schema.get(upper, Abc::ISampleSelector{blend.upper});
    const auto upper_positions = upper.getPositions();
    const auto upper_counts = upper.getFaceCounts();
    const auto upper_indices = upper.getFaceIndices();
    if (!upper_positions || !upper_counts || !upper_indices ||
        upper_positions->size() != result.positions.size() ||
        upper_counts->size() != result.face_counts.size() ||
        upper_indices->size() != result.face_indices.size() ||
        !std::equal(
            upper_counts->get(),
            upper_counts->get() + upper_counts->size(),
            result.face_counts.begin()) ||
        !std::equal(
            upper_indices->get(),
            upper_indices->get() + upper_indices->size(),
            result.face_indices.begin())) {
        fail("mesh topology changes between motion samples");
    }
    const float alpha = static_cast<float>(blend.alpha);
    for (std::size_t index = 0u; index < result.positions.size(); ++index) {
        const Imath::V3f a = result.positions[index];
        const Imath::V3f b = (*upper_positions)[index];
        const Imath::V3f value = a * (1.0f - alpha) + b * alpha;
        if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
            !std::isfinite(value.z)) {
            fail("position interpolation produced a non-finite value");
        }
        result.positions[index] = value;
    }
    return result;
}

template<typename Schema>
void load_reference_positions(const Schema &schema, LoadedMesh &mesh) {
    const Abc::ICompoundProperty arbitrary = schema.getArbGeomParams();
    if (!arbitrary.valid() ||
        !arbitrary.getPropertyHeader("xgen_Pref")) {
        return;
    }
    const AbcGeom::IV3dGeomParam pref{arbitrary, "xgen_Pref"};
    const AbcGeom::IV3dGeomParam::Sample sample = pref.getExpandedValue(
        Abc::ISampleSelector{AbcCore::index_t{0}});
    const auto values = sample.getVals();
    if (!values || values->size() != mesh.positions.size()) {
        fail("xgen_Pref must contain one value per mesh vertex");
    }
    mesh.reference_positions.resize(values->size());
    for (std::size_t index = 0u; index < values->size(); ++index) {
        const Imath::V3d &value = (*values)[index];
        if (!finite(value)) { fail("xgen_Pref contains a non-finite value"); }
        const Imath::V3f converted{
            static_cast<float>(value.x), static_cast<float>(value.y),
            static_cast<float>(value.z)};
        if (!std::isfinite(converted.x) || !std::isfinite(converted.y) ||
            !std::isfinite(converted.z)) {
            fail("xgen_Pref value cannot be represented as float");
        }
        mesh.reference_positions[index] = converted;
    }
}

LoadedMesh load_patch_mesh(const Abc::IArchive &archive, std::string_view name,
                           const ClassicAlembicLimits &limits,
                           const ArchiveSampleSelection &selection) {
    MeshSearch search{
        std::string{name}, 0u, limits.max_objects, selection, {}};
    find_meshes(archive.getTop(), Imath::M44d{}, false, search);
    if (search.matches.empty()) {
        fail("patch object not found: " + std::string{name});
    }
    if (search.matches.size() != 1u) {
        fail("patch object resolves to multiple meshes: " + std::string{name});
    }
    const auto &[object, transform] = search.matches.front();
    if (AbcGeom::IPolyMesh::matches(object.getHeader())) {
        AbcGeom::IPolyMesh mesh{object, Abc::kWrapExisting};
        if (mesh.getSchema().getNumSamples() == 0u) {
            fail("polygon mesh has no samples: " + object.getFullName());
        }
        LoadedMesh result =
            load_mesh_sample<
                AbcGeom::IPolyMeshSchema,
                AbcGeom::IPolyMeshSchema::Sample>(
                mesh.getSchema(), transform, limits, selection);
        load_reference_positions(mesh.getSchema(), result);
        return result;
    }
    AbcGeom::ISubD mesh{object, Abc::kWrapExisting};
    if (mesh.getSchema().getNumSamples() == 0u) {
        fail("subdivision mesh has no samples: " + object.getFullName());
    }
    LoadedMesh result =
        load_mesh_sample<AbcGeom::ISubDSchema, AbcGeom::ISubDSchema::Sample>(
            mesh.getSchema(), transform, limits, selection);
    load_reference_positions(mesh.getSchema(), result);
    return result;
}

Vec3 transformed_position(std::span<const Imath::V3f> positions,
                          const Imath::M44d &transform,
                          std::uint32_t index) {
    if (index >= positions.size()) { fail("face vertex index is out of range"); }
    const Imath::V3f &source = positions[index];
    Imath::V3d transformed;
    transform.multVecMatrix(
        Imath::V3d{source.x, source.y, source.z}, transformed);
    if (!finite(transformed)) { fail("mesh contains a non-finite position"); }
    const Vec3 result{static_cast<float>(transformed.x),
                      static_cast<float>(transformed.y),
                      static_cast<float>(transformed.z)};
    if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
        !std::isfinite(result.z)) {
        fail("mesh position cannot be represented as float");
    }
    return result;
}

Vec3 position(const LoadedMesh &mesh, std::uint32_t index) {
    return transformed_position(mesh.positions, mesh.transform, index);
}

Vec3 reference_position(const LoadedMesh &mesh, std::uint32_t index) {
    return transformed_position(
        mesh.reference_positions, mesh.transform, index);
}

class SubdEvaluator {
public:
    SubdEvaluator(const LoadedMesh &mesh,
                  std::span<const std::uint32_t> selected_faces) {
        if (mesh.positions.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max()) ||
            mesh.face_counts.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max()) ||
            mesh.face_indices.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max()) ||
            selected_faces.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max())) {
            fail("subdivision topology exceeds int indexing");
        }
        std::vector<int> counts(mesh.face_counts.begin(), mesh.face_counts.end());
        std::vector<int> indices(mesh.face_indices.begin(), mesh.face_indices.end());
        Far::TopologyDescriptor descriptor{};
        descriptor.numVertices = static_cast<int>(mesh.positions.size());
        descriptor.numFaces = static_cast<int>(mesh.face_counts.size());
        descriptor.numVertsPerFace = counts.data();
        descriptor.vertIndicesPerFace = indices.data();
        Sdc::Options scheme_options;
        scheme_options.SetVtxBoundaryInterpolation(
            Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
        typename Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options
            refiner_options{Sdc::SCHEME_CATMARK, scheme_options};
        refiner_options.validateFullTopology = true;
        _base_refiner.reset(
            Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
                descriptor, refiner_options));
        if (!_base_refiner) { fail("OpenSubdiv rejected the patch topology"); }
        _ptex_indices = std::make_unique<Far::PtexIndices>(*_base_refiner);

        _base_positions.resize(mesh.positions.size());
        _reference_base_positions.resize(mesh.positions.size());
        for (std::uint32_t index = 0u; index < mesh.positions.size(); ++index) {
            const Vec3 value = position(mesh, index);
            _base_positions[index] = {value.x, value.y, value.z};
            const Vec3 reference = reference_position(mesh, index);
            _reference_base_positions[index] = {
                reference.x, reference.y, reference.z};
        }
        std::vector<Far::Index> faces;
        faces.reserve(selected_faces.size());
        for (const std::uint32_t face : selected_faces) {
            if (face >= mesh.face_counts.size()) {
                fail("selected subdivision face is out of range");
            }
            faces.push_back(static_cast<Far::Index>(face));
        }
        Far::ConstIndexArray group_faces{
            faces.data(), static_cast<int>(faces.size())};
        std::unique_ptr<Far::TopologyRefiner> local_refiner{
            Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
                *_base_refiner)};
        if (!local_refiner) { fail("cannot clone OpenSubdiv topology"); }
        Far::PatchTableFactory::Options patch_options{8u};
        patch_options.SetEndCapType(
            Far::PatchTableFactory::Options::ENDCAP_GREGORY_BASIS);
        local_refiner->RefineAdaptive(
            patch_options.GetRefineAdaptiveOptions(), group_faces);
        _patch_table.reset(Far::PatchTableFactory::Create(
            *local_refiner, patch_options, group_faces));
        if (!_patch_table) { fail("cannot create OpenSubdiv patch table"); }
        _patch_map = std::make_unique<Far::PatchMap>(*_patch_table);

        const int base_vertices =
            local_refiner->GetLevel(0).GetNumVertices();
        const int refined_vertices =
            local_refiner->GetNumVerticesTotal() - base_vertices;
        const int local_points = _patch_table->GetNumLocalPoints();
        if (refined_vertices < 0 || local_points < 0) {
            fail("OpenSubdiv returned invalid local point counts");
        }
        _local_positions.resize(
            static_cast<std::size_t>(refined_vertices + local_points));
        _reference_local_positions.resize(
            static_cast<std::size_t>(refined_vertices + local_points));
        const auto refine = [&](const auto &base, auto &local) {
            if (refined_vertices != 0) {
                Far::PrimvarRefiner primvar_refiner{*local_refiner};
                const auto *source = base.data();
                auto *destination = local.data();
                for (int level = 1; level < local_refiner->GetNumLevels(); ++level) {
                    primvar_refiner.Interpolate(level, source, destination);
                    source = destination;
                    destination +=
                        local_refiner->GetLevel(level).GetNumVertices();
                }
            }
            if (local_points != 0) {
                const Far::StencilTable *stencils =
                    _patch_table->GetLocalPointStencilTable();
                if (!stencils) {
                    fail("OpenSubdiv local point stencils are missing");
                }
                stencils->UpdateValues(
                    base.data(), base_vertices, local.data(),
                    local.data() + refined_vertices);
            }
        };
        refine(_base_positions, _local_positions);
        refine(_reference_base_positions, _reference_local_positions);
    }

    [[nodiscard]] SubdSample evaluate(std::uint32_t face, float u, float v) const {
        return evaluate_positions(
            _base_positions, _local_positions, face, u, v);
    }

    [[nodiscard]] SubdSample evaluate_reference(
        std::uint32_t face, float u, float v) const {
        return evaluate_positions(
            _reference_base_positions, _reference_local_positions,
            face, u, v);
    }

private:
    [[nodiscard]] SubdSample evaluate_positions(
        const std::vector<SubdPosition> &base_positions,
        const std::vector<SubdPosition> &local_positions,
        std::uint32_t face, float u, float v) const {
        const int ptex_face = _ptex_indices->GetFaceId(static_cast<int>(face));
        const Far::PatchTable::PatchHandle *handle =
            _patch_map->FindPatch(ptex_face, u, v);
        if (!handle) { fail("cannot locate OpenSubdiv patch for guide coordinate"); }
        float weights[20u]{};
        float du_weights[20u]{};
        float dv_weights[20u]{};
        _patch_table->EvaluateBasis(
            *handle, u, v, weights, du_weights, dv_weights);
        const Far::ConstIndexArray vertices =
            _patch_table->GetPatchVertices(*handle);
        if (vertices.size() <= 0 || vertices.size() > 20) {
            fail("unsupported OpenSubdiv patch control point count");
        }
        SubdPosition p{};
        SubdPosition du{};
        SubdPosition dv{};
        for (int index = 0; index < vertices.size(); ++index) {
            const Far::Index vertex = vertices[index];
            const SubdPosition *source = nullptr;
            if (vertex < static_cast<Far::Index>(base_positions.size())) {
                source = &base_positions[vertex];
            } else {
                const std::size_t local = static_cast<std::size_t>(vertex) -
                                          base_positions.size();
                if (local >= local_positions.size()) {
                    fail("OpenSubdiv patch control point is out of range");
                }
                source = &local_positions[local];
            }
            p.AddWithWeight(*source, weights[index]);
            du.AddWithWeight(*source, du_weights[index]);
            dv.AddWithWeight(*source, dv_weights[index]);
        }
        const SubdSample result{{p.x, p.y, p.z},
                                {du.x, du.y, du.z},
                                {dv.x, dv.y, dv.z}};
        if (!std::isfinite(result.position.x) ||
            !std::isfinite(result.position.y) ||
            !std::isfinite(result.position.z) ||
            !std::isfinite(result.du.x) || !std::isfinite(result.du.y) ||
            !std::isfinite(result.du.z) || !std::isfinite(result.dv.x) ||
            !std::isfinite(result.dv.y) || !std::isfinite(result.dv.z)) {
            fail("OpenSubdiv evaluation produced a non-finite value");
        }
        return result;
    }

    std::unique_ptr<Far::TopologyRefiner> _base_refiner;
    std::unique_ptr<Far::PtexIndices> _ptex_indices;
    std::unique_ptr<Far::PatchTable> _patch_table;
    std::unique_ptr<Far::PatchMap> _patch_map;
    std::vector<SubdPosition> _base_positions;
    std::vector<SubdPosition> _local_positions;
    std::vector<SubdPosition> _reference_base_positions;
    std::vector<SubdPosition> _reference_local_positions;
};

class ImportedReferenceSurface final : public ClassicReferenceSurfaceEvaluator {
public:
    void add(std::string patch_name,
             std::unique_ptr<SubdEvaluator> evaluator) {
        if (!evaluator ||
            !_evaluators.emplace(std::move(patch_name), std::move(evaluator)).second) {
            fail("duplicate reference-surface evaluator");
        }
    }

    [[nodiscard]] ClassicReferenceSurfaceSample evaluate_current(
        std::string_view patch_name, std::uint32_t face_id,
        float u, float v) const override {
        return evaluate_impl(patch_name, face_id, u, v, false);
    }

    [[nodiscard]] ClassicReferenceSurfaceSample evaluate(
        std::string_view patch_name, std::uint32_t face_id,
        float u, float v) const override {
        return evaluate_impl(patch_name, face_id, u, v, true);
    }

private:
    [[nodiscard]] ClassicReferenceSurfaceSample evaluate_impl(
        std::string_view patch_name, std::uint32_t face_id,
        float u, float v, bool reference) const {
        const auto found = _evaluators.find(std::string{patch_name});
        if (found == _evaluators.end()) {
            fail("reference-surface patch was not imported: " +
                 std::string{patch_name});
        }
        if (!std::isfinite(u) || !std::isfinite(v) ||
            u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
            fail("reference-surface coordinate is outside [0,1]");
        }
        const SubdSample sample = reference
            ? found->second->evaluate_reference(face_id, u, 1.0f - v)
            : found->second->evaluate(face_id, u, 1.0f - v);
        const SurfaceFrame frame = xgen_surface_frame(sample.du, sample.dv);
        return {sample.position, frame.normal, frame.tangent};
    }
    std::unordered_map<std::string, std::unique_ptr<SubdEvaluator>> _evaluators;
};

struct FaceRange {
    std::size_t offset{};
    std::size_t count{};
    std::uint32_t first_triangle{};
    std::uint32_t first_vertex{};
};

std::vector<std::size_t> face_offsets(const LoadedMesh &mesh) {
    std::vector<std::size_t> result(mesh.face_counts.size() + 1u);
    for (std::size_t face = 0u; face < mesh.face_counts.size(); ++face) {
        const std::int32_t count = mesh.face_counts[face];
        if (count < 3) { fail("mesh contains a face with fewer than three vertices"); }
        const std::size_t next = result[face] + static_cast<std::size_t>(count);
        if (next > mesh.face_indices.size()) {
            fail("face counts exceed the face-index array");
        }
        result[face + 1u] = next;
    }
    if (result.back() != mesh.face_indices.size()) {
        fail("face counts do not consume the face-index array");
    }
    return result;
}

struct LimitVertexApproximation {
    std::vector<Vec3> positions;
    std::vector<bool> supported_neighborhood;
};

LimitVertexApproximation xgen_limit_vertices(const LoadedMesh &mesh) {
    const std::vector<std::size_t> offsets = face_offsets(mesh);
    struct Edge {
        std::uint32_t first{};
        std::uint32_t second{};
        std::uint32_t face_count{};
    };
    std::vector<Vec3> neighbor_sums(mesh.positions.size());
    std::vector<Vec3> boundary_neighbor_sums(mesh.positions.size());
    std::vector<Vec3> face_point_sums(mesh.positions.size());
    std::vector<std::uint32_t> incident_faces(mesh.positions.size());
    std::vector<std::uint32_t> boundary_edges(mesh.positions.size());
    std::vector<Edge> edges;
    edges.reserve(mesh.face_indices.size() / 2u);
    std::unordered_map<std::uint64_t, std::size_t> edge_indices;
    edge_indices.reserve(mesh.face_indices.size());
    for (std::size_t face = 0u; face < mesh.face_counts.size(); ++face) {
        const std::size_t count = static_cast<std::size_t>(mesh.face_counts[face]);
        const std::size_t offset = offsets[face];
        Vec3 face_point{};
        for (std::size_t corner = 0u; corner < count; ++corner) {
            // Autodesk's Alembic bridge reverses each face's index run before
            // constructing SESubd. The winding does not change the limit
            // stencil mathematically, but it does change float accumulation
            // order in computeFacePoints and is observable in RandomGenerator
            // surface-compensation UVs.
            const std::size_t reversed = count - 1u - corner;
            const std::int32_t raw = mesh.face_indices[offset + reversed];
            const std::int32_t raw_next =
                mesh.face_indices[offset + (reversed + count - 1u) % count];
            if (raw < 0 || raw_next < 0) {
                fail("mesh contains a negative vertex index");
            }
            const std::uint32_t vertex = static_cast<std::uint32_t>(raw);
            const std::uint32_t next = static_cast<std::uint32_t>(raw_next);
            if (vertex >= mesh.positions.size() ||
                next >= mesh.positions.size()) {
                fail("mesh contains an out-of-range vertex index");
            }
            face_point = face_point + reference_position(mesh, vertex);
            const std::uint32_t first = std::min(vertex, next);
            const std::uint32_t second = std::max(vertex, next);
            const std::uint64_t key =
                (static_cast<std::uint64_t>(first) << 32u) | second;
            const auto [found, inserted] =
                edge_indices.try_emplace(key, edges.size());
            if (inserted) {
                edges.push_back({first, second, 1u});
            } else {
                ++edges[found->second].face_count;
            }
        }
        face_point = face_point / static_cast<float>(count);
        for (std::size_t corner = 0u; corner < count; ++corner) {
            const std::uint32_t vertex = static_cast<std::uint32_t>(
                mesh.face_indices[offset + count - 1u - corner]);
            face_point_sums[vertex] = face_point_sums[vertex] + face_point;
            ++incident_faces[vertex];
        }
    }
    std::vector<std::uint32_t> valence(mesh.positions.size());
    for (const Edge &edge : edges) {
        if (edge.face_count != 2u) {
            boundary_neighbor_sums[edge.first] =
                boundary_neighbor_sums[edge.first] +
                reference_position(mesh, edge.second);
            boundary_neighbor_sums[edge.second] =
                boundary_neighbor_sums[edge.second] +
                reference_position(mesh, edge.first);
            ++boundary_edges[edge.first];
            ++boundary_edges[edge.second];
        }
        neighbor_sums[edge.first] = neighbor_sums[edge.first] +
            reference_position(mesh, edge.second);
        neighbor_sums[edge.second] = neighbor_sums[edge.second] +
            reference_position(mesh, edge.first);
        ++valence[edge.first];
        ++valence[edge.second];
    }
    LimitVertexApproximation result{};
    result.positions.resize(mesh.positions.size());
    result.supported_neighborhood.resize(mesh.positions.size());
    for (std::uint32_t vertex = 0u; vertex < mesh.positions.size(); ++vertex) {
        const std::uint32_t n = valence[vertex];
        const Vec3 own = reference_position(mesh, vertex);
        if (boundary_edges[vertex] == 2u) {
            // SESubd's smooth-boundary limit rule. It retains only the two
            // boundary neighbours in the vertex edge accumulator and applies
            // (4*P + E0 + E1) / 6 using float arithmetic.
            result.positions[vertex] =
                (own * 4.0f + boundary_neighbor_sums[vertex]) *
                (1.0f / 6.0f);
            result.supported_neighborhood[vertex] = true;
            continue;
        }
        const bool regular = boundary_edges[vertex] == 0u && n >= 3u &&
            incident_faces[vertex] == n;
        if (!regular) {
            // SESubd marks non-smooth boundary junctions as corners and their
            // limit value is the authored cage vertex.
            result.positions[vertex] = own;
            result.supported_neighborhood[vertex] = true;
            continue;
        }
        result.supported_neighborhood[vertex] = true;
        const double dn = static_cast<double>(n);
        const float vertex_weight = static_cast<float>((dn - 1.0) / (dn + 5.0));
        const float neighbor_weight = static_cast<float>(
            2.0 / (dn * (dn + 5.0)));
        const float face_weight = static_cast<float>(
            4.0 / (dn * (dn + 5.0)));
        const Vec3 vertex_term = own * vertex_weight;
        const Vec3 neighbor_term = neighbor_sums[vertex] * neighbor_weight;
        const Vec3 face_term = face_point_sums[vertex] * face_weight;
        result.positions[vertex] = vertex_term + neighbor_term + face_term;
    }
    return result;
}

std::vector<Vec3> smooth_vertex_normals(const LoadedMesh &mesh,
                                        bool reference) {
    std::vector<Vec3> result(mesh.positions.size());
    const std::vector<std::size_t> offsets = face_offsets(mesh);
    for (std::size_t face = 0u; face < mesh.face_counts.size(); ++face) {
        const std::size_t count = static_cast<std::size_t>(mesh.face_counts[face]);
        const std::size_t offset = offsets[face];
        const auto sample_position = [&](std::size_t corner) {
            const std::int32_t raw = mesh.face_indices[offset + corner];
            if (raw < 0) { fail("mesh contains a negative vertex index"); }
            const std::uint32_t index = static_cast<std::uint32_t>(raw);
            return reference ? reference_position(mesh, index)
                             : position(mesh, index);
        };
        const Vec3 first = sample_position(0u);
        for (std::size_t corner = 1u; corner + 1u < count; ++corner) {
            const Vec3 normal = cross(
                sample_position(corner) - first,
                sample_position(corner + 1u) - first);
            const std::uint32_t i0 = static_cast<std::uint32_t>(
                mesh.face_indices[offset]);
            const std::uint32_t i1 = static_cast<std::uint32_t>(
                mesh.face_indices[offset + corner]);
            const std::uint32_t i2 = static_cast<std::uint32_t>(
                mesh.face_indices[offset + corner + 1u]);
            result[i0] = result[i0] + normal;
            result[i1] = result[i1] + normal;
            result[i2] = result[i2] + normal;
        }
    }
    for (Vec3 &normal : result) {
        if (!(length_squared(normal) > 0.0f)) {
            fail("mesh contains a vertex without a valid normal");
        }
        normal = normalize(normal);
    }
    return result;
}

std::uint32_t vertex_index(const LoadedMesh &mesh, std::size_t index,
                           std::uint32_t vertex_base) {
    if (index >= mesh.face_indices.size() || mesh.face_indices[index] < 0) {
        fail("mesh contains an invalid face vertex index");
    }
    const std::uint64_t value = static_cast<std::uint64_t>(vertex_base) +
        static_cast<std::uint32_t>(mesh.face_indices[index]);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        fail("combined vertex index exceeds uint32");
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace

ClassicAlembicAssetInput build_xgen_classic_alembic_asset_input_impl(
    const ClassicDescription &description,
    const std::filesystem::path &archive_path,
    const ClassicAlembicLimits &limits,
    const ArchiveSampleSelection &selection) {
    if (description.patches.empty()) {
        fail("description has no patches");
    }
    if (limits.max_objects == 0u || limits.max_vertices == 0u ||
        limits.max_faces == 0u || limits.max_face_vertices == 0u ||
        limits.max_triangles == 0u) {
        fail("limits must be nonzero");
    }
    AbcFactory::IFactory factory;
    const Abc::IArchive archive = factory.getArchive(archive_path.string());
    if (!archive.valid()) { fail("cannot open archive: " + archive_path.string()); }

    ClassicAlembicAssetInput result{};
    const auto reference_surface =
        std::make_shared<ImportedReferenceSurface>();
    result.reference_surface = reference_surface;
    float guide_cage_root_squared_distance_sum = 0.0f;
    std::size_t subdivision_guide_count = 0u;
    for (const ClassicPatch &patch : description.patches) {
        const LoadedMesh mesh = load_patch_mesh(
            archive, patch.name, limits, selection);
        result.source_vertex_count += mesh.positions.size();
        result.source_face_count += mesh.face_counts.size();
        const std::vector<std::size_t> offsets = face_offsets(mesh);
        const bool subdivide = patch.type == "Subd";
        std::vector<std::vector<std::uint32_t>> vertex_faces(
            mesh.positions.size());
        for (std::uint32_t face_id = 0u;
             face_id < mesh.face_counts.size(); ++face_id) {
            const std::size_t offset = offsets[face_id];
            const std::size_t count = static_cast<std::size_t>(
                mesh.face_counts[face_id]);
            for (std::size_t corner = 0u; corner < count; ++corner) {
                const std::int32_t raw = mesh.face_indices[offset + corner];
                if (raw < 0 ||
                    static_cast<std::size_t>(raw) >= mesh.positions.size()) {
                    fail("mesh contains an invalid face vertex index");
                }
                vertex_faces[static_cast<std::uint32_t>(raw)].push_back(
                    face_id);
            }
        }
        const LimitVertexApproximation limit_vertices = subdivide
            ? xgen_limit_vertices(mesh)
            : LimitVertexApproximation{};
        if (limits.subd_face_resolution == 0u ||
            limits.subd_face_resolution > 64u) {
            fail("subdivision face resolution must be in [1, 64]");
        }
        std::unique_ptr<SubdEvaluator> subd;
        if (subdivide) {
            result.subdivision_face_count += patch.face_ids.size();
            for (const std::uint32_t face_id : patch.face_ids) {
                if (face_id >= mesh.face_counts.size()) {
                    fail("patch face ID is outside the Alembic mesh: " +
                         std::to_string(face_id));
                }
                if (mesh.face_counts[face_id] != 4) {
                    fail("subdivision import currently requires quad patch faces");
                }
            }
            subd = std::make_unique<SubdEvaluator>(mesh, patch.face_ids);
        }
        auto append_vertex = [&](Vec3 value, Vec3 normal,
                                 Vec3 reference_value,
                                 Vec3 reference_normal) -> std::uint32_t {
            if (result.asset.positions.size() >= limits.max_vertices ||
                result.asset.positions.size() >=
                    std::numeric_limits<std::uint32_t>::max()) {
                fail("combined vertex count exceeds the limit");
            }
            const std::uint32_t index =
                static_cast<std::uint32_t>(result.asset.positions.size());
            result.asset.positions.push_back(value);
            result.asset.normals.push_back(normal);
            result.asset.reference_positions.push_back(reference_value);
            result.asset.reference_normals.push_back(reference_normal);
            return index;
        };
        std::uint32_t vertex_base = 0u;
        if (!subdivide) {
            if (mesh.positions.size() >
                limits.max_vertices - result.asset.positions.size() ||
                mesh.positions.size() >
                    std::numeric_limits<std::uint32_t>::max() -
                        result.asset.positions.size()) {
                fail("combined vertex count exceeds the limit");
            }
            vertex_base = static_cast<std::uint32_t>(
                result.asset.positions.size());
            result.asset.positions.reserve(
                result.asset.positions.size() + mesh.positions.size());
            const std::vector<Vec3> current_normals =
                smooth_vertex_normals(mesh, false);
            const std::vector<Vec3> reference_normals =
                smooth_vertex_normals(mesh, true);
            for (std::uint32_t index = 0u; index < mesh.positions.size(); ++index) {
                append_vertex(position(mesh, index), current_normals[index],
                              reference_position(mesh, index),
                              reference_normals[index]);
            }
        }

        std::unordered_map<std::uint32_t, FaceRange> selected;
        selected.reserve(patch.face_ids.size());
        for (const std::uint32_t face_id : patch.face_ids) {
            if (face_id >= mesh.face_counts.size()) {
                fail("patch face ID is outside the Alembic mesh: " +
                     std::to_string(face_id));
            }
            const std::size_t offset = offsets[face_id];
            const std::size_t count = static_cast<std::size_t>(
                mesh.face_counts[face_id]);
            const std::size_t triangle_count = subdivide
                ? static_cast<std::size_t>(limits.subd_face_resolution) *
                      limits.subd_face_resolution * 2u
                : count - 2u;
            if (triangle_count > limits.max_triangles - result.asset.triangles.size()) {
                fail("triangle count exceeds the limit");
            }
            if (triangle_count >
                std::numeric_limits<std::uint32_t>::max() -
                    result.asset.triangles.size()) {
                fail("triangle count exceeds uint32");
            }
            const std::uint32_t first_triangle =
                static_cast<std::uint32_t>(result.asset.triangles.size());
            const std::uint32_t first_vertex = subdivide
                ? static_cast<std::uint32_t>(result.asset.positions.size())
                : vertex_base;
            if (subdivide) {
                const std::uint32_t resolution = limits.subd_face_resolution;
                for (std::uint32_t y = 0u; y <= resolution; ++y) {
                    for (std::uint32_t x = 0u; x <= resolution; ++x) {
                        const float u = static_cast<float>(x) /
                                        static_cast<float>(resolution);
                        const float v = static_cast<float>(y) /
                                        static_cast<float>(resolution);
                        const float surface_v = 1.0f - v;
                        const SubdSample current =
                            subd->evaluate(face_id, u, surface_v);
                        const SubdSample reference =
                            subd->evaluate_reference(face_id, u, surface_v);
                        append_vertex(
                            current.position,
                            xgen_surface_frame(current.du, current.dv).normal,
                            reference.position,
                            xgen_surface_frame(reference.du, reference.dv).normal);
                    }
                }
                const std::uint32_t row = resolution + 1u;
                for (std::uint32_t y = 0u; y < resolution; ++y) {
                    for (std::uint32_t x = 0u; x < resolution; ++x) {
                        const std::uint32_t a = first_vertex + y * row + x;
                        const std::uint32_t b = a + 1u;
                        const std::uint32_t d = a + row;
                        const std::uint32_t c = d + 1u;
                        result.asset.triangles.push_back({a, b, c});
                        result.asset.triangles.push_back({a, c, d});
                    }
                }
            } else {
                const std::uint32_t first =
                    vertex_index(mesh, offset, vertex_base);
                for (std::size_t corner = 1u; corner + 1u < count; ++corner) {
                    result.asset.triangles.push_back({
                        first,
                        vertex_index(mesh, offset + corner, vertex_base),
                        vertex_index(mesh, offset + corner + 1u, vertex_base)});
                }
            }
            double surface_area = 0.0;
            double center_u_length = 0.0;
            double center_v_length = 0.0;
            if (subdivide) {
                Double3 cage[4u]{};
                bool supported_neighborhood = true;
                for (std::size_t corner = 0u; corner < 4u; ++corner) {
                    const std::int32_t source_index =
                        mesh.face_indices[offset + corner];
                    if (source_index < 0) {
                        fail("selected face has a negative vertex index");
                    }
                    const std::uint32_t vertex =
                        static_cast<std::uint32_t>(source_index);
                    supported_neighborhood = supported_neighborhood &&
                        limit_vertices.supported_neighborhood[vertex];
                    cage[corner] = to_double(limit_vertices.positions[vertex]);
                }
                if (!supported_neighborhood) {
                    cage[0] = to_double(subd->evaluate_reference(
                        face_id, 0.0f, 0.0f).position);
                    cage[1] = to_double(subd->evaluate_reference(
                        face_id, 1.0f, 0.0f).position);
                    cage[2] = to_double(subd->evaluate_reference(
                        face_id, 1.0f, 1.0f).position);
                    cage[3] = to_double(subd->evaluate_reference(
                        face_id, 0.0f, 1.0f).position);
                }
                surface_area = xgen_quad_area(cage);
                center_u_length = xgen_quad_length_u(cage);
                center_v_length = xgen_quad_length_v(cage);
            } else {
                for (std::uint32_t triangle_index = first_triangle;
                     triangle_index < result.asset.triangles.size();
                     ++triangle_index) {
                    const UInt3 triangle = result.asset.triangles[triangle_index];
                    surface_area += 0.5 * triangle_double_area(
                        to_double(result.asset.positions[triangle.x]),
                        to_double(result.asset.positions[triangle.y]),
                        to_double(result.asset.positions[triangle.z]));
                }
                center_u_length = 1.0;
                center_v_length = 1.0;
            }
            if (!std::isfinite(surface_area) || surface_area <= 0.0f) {
                fail("selected face has invalid surface area");
            }
            if (!std::isfinite(center_u_length) ||
                !std::isfinite(center_v_length) ||
                center_u_length <= 0.0 || center_v_length <= 0.0) {
                fail("selected face has invalid surface-compensation lengths");
            }
            Vec3 reference_bounds_min{
                std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::infinity()};
            Vec3 reference_bounds_max{
                -std::numeric_limits<float>::infinity(),
                -std::numeric_limits<float>::infinity(),
                -std::numeric_limits<float>::infinity()};
            const auto include_reference_vertex = [&](std::uint32_t vertex) {
                const Vec3 value = reference_position(mesh, vertex);
                reference_bounds_min.x = std::min(reference_bounds_min.x, value.x);
                reference_bounds_min.y = std::min(reference_bounds_min.y, value.y);
                reference_bounds_min.z = std::min(reference_bounds_min.z, value.z);
                reference_bounds_max.x = std::max(reference_bounds_max.x, value.x);
                reference_bounds_max.y = std::max(reference_bounds_max.y, value.y);
                reference_bounds_max.z = std::max(reference_bounds_max.z, value.z);
            };
            if (subdivide) {
                // SESubdImpl::faceBoundingBox starts with the active cage face,
                // then visits getFaceUmbrella for every corner and includes all
                // vertices of every incident face.
                for (std::size_t corner = 0u; corner < count; ++corner) {
                    const std::uint32_t vertex = static_cast<std::uint32_t>(
                        mesh.face_indices[offset + corner]);
                    for (const std::uint32_t incident_face :
                         vertex_faces[vertex]) {
                        const std::size_t incident_offset =
                            offsets[incident_face];
                        const std::size_t incident_count =
                            static_cast<std::size_t>(
                                mesh.face_counts[incident_face]);
                        for (std::size_t incident_corner = 0u;
                             incident_corner < incident_count;
                             ++incident_corner) {
                            include_reference_vertex(
                                static_cast<std::uint32_t>(mesh.face_indices[
                                    incident_offset + incident_corner]));
                        }
                    }
                }
            } else {
                for (std::size_t corner = 0u; corner < count; ++corner) {
                    include_reference_vertex(static_cast<std::uint32_t>(
                        mesh.face_indices[offset + corner]));
                }
            }
            if (!std::isfinite(reference_bounds_min.x) ||
                !std::isfinite(reference_bounds_min.y) ||
                !std::isfinite(reference_bounds_min.z) ||
                !std::isfinite(reference_bounds_max.x) ||
                !std::isfinite(reference_bounds_max.y) ||
                !std::isfinite(reference_bounds_max.z) ||
                reference_bounds_min.x > reference_bounds_max.x ||
                reference_bounds_min.y > reference_bounds_max.y ||
                reference_bounds_min.z > reference_bounds_max.z) {
                fail("selected face has invalid reference bounds");
            }
            result.surface_faces.push_back({
                patch.name, face_id, first_triangle,
                static_cast<std::uint32_t>(triangle_count),
                subdivide ? limits.subd_face_resolution : 0u, surface_area,
                center_u_length, center_v_length, reference_bounds_min,
                reference_bounds_max});
            selected.emplace(face_id, FaceRange{
                offset, count, first_triangle, first_vertex});
            ++result.selected_face_count;
        }

        for (const ClassicGuide &guide : patch.guides) {
            const auto found = selected.find(guide.face_id);
            if (found == selected.end()) {
                fail("guide face was not selected");
            }
            const FaceRange face = found->second;
            if (face.count != 4u) {
                fail("guide root evaluation currently requires quad patch faces");
            }
            const float u = static_cast<float>(guide.patch_u);
            const float v = static_cast<float>(guide.patch_v);
            const float surface_v = 1.0f - v;
            if (!std::isfinite(u) || !std::isfinite(v) ||
                u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
                fail("guide patch coordinates must be finite and in [0, 1]");
            }
            Vec3 root{};
            Vec3 du{};
            Vec3 dv{};
            Vec3 reference_root{};
            Vec3 reference_du{};
            Vec3 reference_dv{};
            Vec3 corners[4u]{};
            Vec3 reference_corners[4u]{};
            for (std::size_t corner = 0u; corner < 4u; ++corner) {
                const std::int32_t source_index =
                    mesh.face_indices[face.offset + corner];
                if (source_index < 0) {
                    fail("guide face has a negative vertex index");
                }
                corners[corner] = position(
                    mesh, static_cast<std::uint32_t>(source_index));
                reference_corners[corner] = reference_position(
                    mesh, static_cast<std::uint32_t>(source_index));
            }
            const Vec3 cage_root =
                corners[0] * ((1.0f - u) * (1.0f - surface_v)) +
                corners[1] * (u * (1.0f - surface_v)) +
                corners[2] * (u * surface_v) +
                corners[3] * ((1.0f - u) * surface_v);
            const Vec3 reference_cage_root =
                reference_corners[0] * ((1.0f - u) * (1.0f - surface_v)) +
                reference_corners[1] * (u * (1.0f - surface_v)) +
                reference_corners[2] * (u * surface_v) +
                reference_corners[3] * ((1.0f - u) * surface_v);
            if (subdivide) {
                const SubdSample sample =
                    subd->evaluate(guide.face_id, u, surface_v);
                const SubdSample reference_sample =
                    subd->evaluate_reference(guide.face_id, u, surface_v);
                root = sample.position;
                du = sample.du;
                dv = sample.dv;
                reference_root = reference_sample.position;
                reference_du = reference_sample.du;
                reference_dv = reference_sample.dv;
                const float distance_squared = length_squared(root - cage_root);
                guide_cage_root_squared_distance_sum += distance_squared;
                result.guide_cage_root_max_distance = std::max(
                    result.guide_cage_root_max_distance,
                    std::sqrt(distance_squared));
                ++subdivision_guide_count;
            } else {
                root = cage_root;
                du = (corners[1] - corners[0]) * (1.0f - surface_v) +
                     (corners[2] - corners[3]) * surface_v;
                dv = (corners[3] - corners[0]) * (1.0f - u) +
                     (corners[2] - corners[1]) * u;
                reference_root = reference_cage_root;
                reference_du =
                    (reference_corners[1] - reference_corners[0]) *
                        (1.0f - surface_v) +
                    (reference_corners[2] - reference_corners[3]) * surface_v;
                reference_dv =
                    (reference_corners[3] - reference_corners[0]) * (1.0f - u) +
                    (reference_corners[2] - reference_corners[1]) * u;
            }
            GuideInput output{};
            const SurfaceFrame current_frame = xgen_surface_frame(du, dv);
            const SurfaceFrame reference_frame =
                xgen_surface_frame(reference_du, reference_dv);
            output.root_normal = current_frame.normal;
            output.root_uv = {u, v};
            output.surface_face_id = guide.face_id;
            output.reference_root_position = reference_root;
            output.reference_root_normal = reference_frame.normal;
            output.reference_root_tangent = reference_frame.tangent;
            output.reference_root_binormal = reference_frame.binormal;
            if (((guide.interpolation_count & 1u) == 0u &&
                 guide.interpolation_count != 0u) ||
                guide.interpolation_offset > patch.guide_interpolation.size() ||
                guide.interpolation_count > patch.guide_interpolation.size() -
                                                guide.interpolation_offset) {
                fail("guide interpolation payload is invalid");
            }
            if (guide.interpolation_count != 0u) {
                const auto interpolation = std::span{
                    patch.guide_interpolation}.subspan(
                        guide.interpolation_offset, guide.interpolation_count);
                const auto checked_float = [](double value) {
                    const float converted = static_cast<float>(value);
                    if (!std::isfinite(value) || !std::isfinite(converted)) {
                        fail("guide interpolation contains a non-finite value");
                    }
                    return converted;
                };
                output.support_radii.reserve((interpolation.size() + 1u) / 2u);
                output.support_angles.reserve(interpolation.size() / 2u);
                // XgGuide::setInterpolation applies max(1 + blend, 1) to
                // every authored support radius. Keep the source/parse side
                // in double, then narrow the already-scaled value at the
                // float-only runtime boundary.
                const double radius_scale = std::max(1.0 + guide.blend, 1.0);
                output.support_radii.push_back(
                    checked_float(interpolation[0u] * radius_scale));
                for (std::size_t index = 1u; index < interpolation.size(); index += 2u) {
                    output.support_radii.push_back(
                        checked_float(interpolation[index] * radius_scale));
                    output.support_angles.push_back(checked_float(interpolation[index + 1u]));
                }
                output.support_radius = output.support_radii.front();
            }
            float local_u = u;
            float local_v = v;
            std::uint32_t triangle = face.first_triangle;
            if (subdivide) {
                const std::uint32_t resolution = limits.subd_face_resolution;
                const float scaled_u = u * static_cast<float>(resolution);
                const float scaled_v = v * static_cast<float>(resolution);
                const std::uint32_t cell_u = std::min(
                    static_cast<std::uint32_t>(scaled_u), resolution - 1u);
                const std::uint32_t cell_v = std::min(
                    static_cast<std::uint32_t>(scaled_v), resolution - 1u);
                local_u = scaled_u - static_cast<float>(cell_u);
                local_v = scaled_v - static_cast<float>(cell_v);
                triangle += (cell_v * resolution + cell_u) * 2u;
            }
            if (local_u >= local_v) {
                output.triangle_index = triangle;
                output.barycentric = {local_u - local_v, local_v};
            } else {
                output.triangle_index = triangle + 1u;
                output.barycentric = {local_u, local_v - local_u};
            }
            output.cvs.reserve(guide.cv_count);
            for (std::size_t index = 0u; index < guide.cv_count; ++index) {
                if (guide.cv_offset + index >= patch.guide_cvs.size()) {
                    fail("guide CV range is invalid");
                }
                const ClassicFloat3 local =
                    patch.guide_cvs[guide.cv_offset + index];
                const Vec3 local_float{
                    static_cast<float>(local.x),
                    static_cast<float>(local.y),
                    static_cast<float>(local.z)};
                if (!std::isfinite(local.x) || !std::isfinite(local.y) ||
                    !std::isfinite(local.z) ||
                    !std::isfinite(local_float.x) ||
                    !std::isfinite(local_float.y) ||
                    !std::isfinite(local_float.z)) {
                    fail("guide CV cannot be represented as float");
                }
                // Classic guide CVs are authored in the guide's patch frame,
                // not as world-space displacement vectors. This mirrors
                // XgBasePrimitive::transformGuidesToSurface(): local x is cU,
                // local y is cN, and local z is the frame binormal.
                output.cvs.push_back(
                    root + current_frame.tangent * local_float.x +
                    current_frame.normal * local_float.y +
                    current_frame.binormal * local_float.z);
            }
            result.asset.guides.emplace_back(std::move(output));
        }
        if (subd) {
            reference_surface->add(patch.name, std::move(subd));
        }
    }
    if (subdivision_guide_count != 0u) {
        result.guide_cage_root_rms_distance = std::sqrt(
            guide_cage_root_squared_distance_sum /
            static_cast<float>(subdivision_guide_count));
    }
    return result;
}

ClassicAlembicAssetInput build_xgen_classic_alembic_asset_input(
    const ClassicDescription &description,
    const std::filesystem::path &archive_path,
    const ClassicAlembicLimits &limits) {
    return build_xgen_classic_alembic_asset_input_impl(
        description, archive_path, limits, {});
}

ClassicAlembicAssetInput build_xgen_classic_alembic_asset_input(
    const ClassicDescription &description,
    const std::filesystem::path &archive_path,
    const ClassicAlembicFrameSample &sample,
    const ClassicAlembicLimits &limits) {
    if (!std::isfinite(sample.frame) ||
        !std::isfinite(sample.lookup_offset) ||
        !std::isfinite(sample.frames_per_second) ||
        !(sample.frames_per_second > 0.0)) {
        fail("motion frame, lookup offset, and FPS must be finite with positive FPS");
    }
    const double time_seconds =
        (sample.frame + sample.lookup_offset) /
        sample.frames_per_second;
    if (!std::isfinite(time_seconds)) {
        fail("motion lookup time is not finite");
    }
    return build_xgen_classic_alembic_asset_input_impl(
        description, archive_path, limits,
        {false, time_seconds, sample.interpolation});
}

bool xgen_classic_alembic_deformation_is_static(
    const ClassicDescription &description,
    const std::filesystem::path &archive_path,
    const ClassicAlembicLimits &limits) {
    if (description.patches.empty()) {
        fail("description has no patches");
    }
    if (limits.max_objects == 0u) {
        fail("object limit must be nonzero");
    }
    AbcFactory::IFactory factory;
    const Abc::IArchive archive = factory.getArchive(archive_path.string());
    if (!archive.valid()) {
        fail("cannot open archive: " + archive_path.string());
    }
    bool result = true;
    for (const ClassicPatch &patch : description.patches) {
        std::size_t visited{};
        std::size_t matches{};
        inspect_static_deformation(
            archive.getTop(), patch.name, false, false, visited,
            limits.max_objects, matches, result);
        if (matches == 0u) {
            fail("patch object not found: " + patch.name);
        }
        if (matches != 1u) {
            fail("patch object resolves to multiple meshes: " + patch.name);
        }
    }
    return result;
}

} // namespace nanoxgen
