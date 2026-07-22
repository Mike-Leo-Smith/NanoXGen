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
    std::vector<std::int32_t> face_counts;
    std::vector<std::int32_t> face_indices;
    Imath::M44d transform{};
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

struct SubdSample {
    Vec3 position{};
    Vec3 du{};
    Vec3 dv{};
};

struct MeshSearch {
    std::string target;
    std::size_t visited{};
    std::size_t max_objects{};
    std::vector<std::pair<Abc::IObject, Imath::M44d>> matches;
};

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error("Classic Alembic import: " + message);
}

bool finite(const Imath::V3d &value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
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
            AbcGeom::XformSample sample;
            xform.getSchema().get(
                sample, Abc::ISampleSelector{AbcCore::index_t{0}});
            transform = sample.getMatrix() * parent_transform;
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
    result.face_counts.assign(counts->get(), counts->get() + counts->size());
    result.face_indices.assign(indices->get(), indices->get() + indices->size());
    result.transform = transform;
    return result;
}

LoadedMesh load_patch_mesh(const Abc::IArchive &archive, std::string_view name,
                           const ClassicAlembicLimits &limits) {
    MeshSearch search{std::string{name}, 0u, limits.max_objects, {}};
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
        AbcGeom::IPolyMeshSchema::Sample sample;
        mesh.getSchema().get(
            sample, Abc::ISampleSelector{AbcCore::index_t{0}});
        return copy_mesh_sample(sample, transform, limits);
    }
    AbcGeom::ISubD mesh{object, Abc::kWrapExisting};
    if (mesh.getSchema().getNumSamples() == 0u) {
        fail("subdivision mesh has no samples: " + object.getFullName());
    }
    AbcGeom::ISubDSchema::Sample sample;
    mesh.getSchema().get(
        sample, Abc::ISampleSelector{AbcCore::index_t{0}});
    return copy_mesh_sample(sample, transform, limits);
}

Vec3 position(const LoadedMesh &mesh, std::uint32_t index) {
    if (index >= mesh.positions.size()) { fail("face vertex index is out of range"); }
    const Imath::V3f &source = mesh.positions[index];
    Imath::V3d transformed;
    mesh.transform.multVecMatrix(
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

Vec3 direction(const LoadedMesh &mesh, const ClassicFloat3 &source) {
    Imath::V3d transformed;
    mesh.transform.multDirMatrix(
        Imath::V3d{source.x, source.y, source.z}, transformed);
    if (!finite(transformed)) { fail("guide contains a non-finite direction"); }
    const Vec3 result{static_cast<float>(transformed.x),
                      static_cast<float>(transformed.y),
                      static_cast<float>(transformed.z)};
    if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
        !std::isfinite(result.z)) {
        fail("guide direction cannot be represented as float");
    }
    return result;
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
            Sdc::Options::VTX_BOUNDARY_EDGE_AND_CORNER);
        typename Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options
            refiner_options{Sdc::SCHEME_CATMARK, scheme_options};
        refiner_options.validateFullTopology = true;
        _base_refiner.reset(
            Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
                descriptor, refiner_options));
        if (!_base_refiner) { fail("OpenSubdiv rejected the patch topology"); }
        _ptex_indices = std::make_unique<Far::PtexIndices>(*_base_refiner);

        _base_positions.resize(mesh.positions.size());
        for (std::uint32_t index = 0u; index < mesh.positions.size(); ++index) {
            const Vec3 value = position(mesh, index);
            _base_positions[index] = {value.x, value.y, value.z};
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
        Far::PatchTableFactory::Options patch_options{5u};
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
        if (refined_vertices != 0) {
            Far::PrimvarRefiner primvar_refiner{*local_refiner};
            const SubdPosition *source = _base_positions.data();
            SubdPosition *destination = _local_positions.data();
            for (int level = 1; level < local_refiner->GetNumLevels(); ++level) {
                primvar_refiner.Interpolate(level, source, destination);
                source = destination;
                destination += local_refiner->GetLevel(level).GetNumVertices();
            }
        }
        if (local_points != 0) {
            const Far::StencilTable *stencils =
                _patch_table->GetLocalPointStencilTable();
            if (!stencils) { fail("OpenSubdiv local point stencils are missing"); }
            stencils->UpdateValues(
                _base_positions.data(), base_vertices,
                _local_positions.data(),
                _local_positions.data() + refined_vertices);
        }
    }

    [[nodiscard]] SubdSample evaluate(std::uint32_t face, float u, float v) const {
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
            if (vertex < static_cast<Far::Index>(_base_positions.size())) {
                source = &_base_positions[vertex];
            } else {
                const std::size_t local = static_cast<std::size_t>(vertex) -
                                          _base_positions.size();
                if (local >= _local_positions.size()) {
                    fail("OpenSubdiv patch control point is out of range");
                }
                source = &_local_positions[local];
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

private:
    std::unique_ptr<Far::TopologyRefiner> _base_refiner;
    std::unique_ptr<Far::PtexIndices> _ptex_indices;
    std::unique_ptr<Far::PatchTable> _patch_table;
    std::unique_ptr<Far::PatchMap> _patch_map;
    std::vector<SubdPosition> _base_positions;
    std::vector<SubdPosition> _local_positions;
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

ClassicAlembicAssetInput build_xgen_classic_alembic_asset_input(
    const ClassicDescription &description,
    const std::filesystem::path &archive_path,
    const ClassicAlembicLimits &limits) {
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
    float guide_cage_root_squared_distance_sum = 0.0f;
    std::size_t subdivision_guide_count = 0u;
    for (const ClassicPatch &patch : description.patches) {
        const LoadedMesh mesh = load_patch_mesh(archive, patch.name, limits);
        result.source_vertex_count += mesh.positions.size();
        result.source_face_count += mesh.face_counts.size();
        const std::vector<std::size_t> offsets = face_offsets(mesh);
        const bool subdivide = patch.type == "Subd";
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
        auto append_position = [&](Vec3 value) -> std::uint32_t {
            if (result.asset.positions.size() >= limits.max_vertices ||
                result.asset.positions.size() >=
                    std::numeric_limits<std::uint32_t>::max()) {
                fail("combined vertex count exceeds the limit");
            }
            const std::uint32_t index =
                static_cast<std::uint32_t>(result.asset.positions.size());
            result.asset.positions.push_back(value);
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
            for (std::uint32_t index = 0u; index < mesh.positions.size(); ++index) {
                result.asset.positions.push_back(position(mesh, index));
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
                        append_position(subd->evaluate(face_id, u, v).position);
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
            if (!std::isfinite(u) || !std::isfinite(v) ||
                u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
                fail("guide patch coordinates must be finite and in [0, 1]");
            }
            Vec3 root{};
            Vec3 du{};
            Vec3 dv{};
            Vec3 corners[4u]{};
            for (std::size_t corner = 0u; corner < 4u; ++corner) {
                const std::int32_t source_index =
                    mesh.face_indices[face.offset + corner];
                if (source_index < 0) {
                    fail("guide face has a negative vertex index");
                }
                corners[corner] = position(
                    mesh, static_cast<std::uint32_t>(source_index));
            }
            const Vec3 cage_root =
                corners[0] * ((1.0f - u) * (1.0f - v)) +
                corners[1] * (u * (1.0f - v)) +
                corners[2] * (u * v) +
                corners[3] * ((1.0f - u) * v);
            if (subdivide) {
                const SubdSample sample = subd->evaluate(guide.face_id, u, v);
                root = sample.position;
                du = sample.du;
                dv = sample.dv;
                const float distance_squared = length_squared(root - cage_root);
                guide_cage_root_squared_distance_sum += distance_squared;
                result.guide_cage_root_max_distance = std::max(
                    result.guide_cage_root_max_distance,
                    std::sqrt(distance_squared));
                ++subdivision_guide_count;
            } else {
                root = cage_root;
                du = (corners[1] - corners[0]) * (1.0f - v) +
                     (corners[2] - corners[3]) * v;
                dv = (corners[3] - corners[0]) * (1.0f - u) +
                     (corners[2] - corners[1]) * u;
            }
            GuideInput output{};
            output.root_normal = normalize(cross(du, dv));
            output.root_uv = {u, v};
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
                output.cvs.push_back(
                    root + direction(mesh, patch.guide_cvs[guide.cv_offset + index]));
            }
            result.asset.guides.emplace_back(std::move(output));
        }
    }
    if (subdivision_guide_count != 0u) {
        result.guide_cage_root_rms_distance = std::sqrt(
            guide_cage_root_squared_distance_sum /
            static_cast<float>(subdivision_guide_count));
    }
    return result;
}

} // namespace nanoxgen
