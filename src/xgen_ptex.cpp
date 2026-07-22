#include "nanoxgen/xgen_ptex.h"

#include <Ptexture.h>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace nanoxgen {
namespace {

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error("Classic PTEX: " + message);
}

Ptex::PtexFilter::FilterType ptex_filter(XgenPtexFilter filter) {
    switch (filter) {
    case XgenPtexFilter::Point: return Ptex::PtexFilter::f_point;
    case XgenPtexFilter::Bilinear: return Ptex::PtexFilter::f_bilinear;
    case XgenPtexFilter::Box: return Ptex::PtexFilter::f_box;
    case XgenPtexFilter::Gaussian: return Ptex::PtexFilter::f_gaussian;
    case XgenPtexFilter::Bicubic: return Ptex::PtexFilter::f_bicubic;
    case XgenPtexFilter::BSpline: return Ptex::PtexFilter::f_bspline;
    case XgenPtexFilter::CatmullRom: return Ptex::PtexFilter::f_catmullrom;
    case XgenPtexFilter::Mitchell: return Ptex::PtexFilter::f_mitchell;
    }
    fail("invalid filter");
}

XgenPtexDataType data_type(Ptex::DataType type) {
    switch (type) {
    case Ptex::dt_uint8: return XgenPtexDataType::UInt8;
    case Ptex::dt_uint16: return XgenPtexDataType::UInt16;
    case Ptex::dt_half: return XgenPtexDataType::Half;
    case Ptex::dt_float: return XgenPtexDataType::Float;
    }
    fail("invalid data type");
}

} // namespace

struct XgenPtexMap::Impl {
    explicit Impl(const std::filesystem::path &path) {
        Ptex::String error;
        texture = Ptex::PtexTexture::open(path.c_str(), error, false);
        if (!texture) {
            fail("cannot open '" + path.string() + "': " + error.c_str());
        }
        const Ptex::PtexTexture::Info source = texture->getInfo();
        if (source.numChannels <= 0 || source.numFaces <= 0) {
            texture->release();
            texture = nullptr;
            fail("invalid channel or face count");
        }
        info.mesh_type = source.meshType == Ptex::mt_triangle
                             ? XgenPtexMeshType::Triangle
                             : XgenPtexMeshType::Quad;
        info.data_type = data_type(source.dataType);
        info.channel_count = static_cast<std::uint32_t>(source.numChannels);
        info.face_count = static_cast<std::uint32_t>(source.numFaces);
        info.alpha_channel = source.alphaChannel;
        info.has_mipmaps = texture->hasMipMaps();
    }

    ~Impl() {
        if (texture) { texture->release(); }
    }

    Ptex::PtexTexture *texture{};
    XgenPtexInfo info{};
};

XgenPtexMap::XgenPtexMap(const std::filesystem::path &path)
    : _impl{std::make_unique<Impl>(path)} {}

XgenPtexMap::~XgenPtexMap() = default;
XgenPtexMap::XgenPtexMap(XgenPtexMap &&) noexcept = default;
XgenPtexMap &XgenPtexMap::operator=(XgenPtexMap &&) noexcept = default;

const XgenPtexInfo &XgenPtexMap::info() const noexcept { return _impl->info; }

XgenPtexFaceInfo XgenPtexMap::face_info(std::uint32_t face) const {
    if (face >= _impl->info.face_count) { fail("face index is out of range"); }
    const Ptex::FaceInfo &source =
        _impl->texture->getFaceInfo(static_cast<int>(face));
    if (source.res.ulog2 < 0 || source.res.vlog2 < 0 ||
        source.res.ulog2 >= 31 || source.res.vlog2 >= 31) {
        fail("face resolution is invalid");
    }
    return {static_cast<std::uint32_t>(source.res.u()),
            static_cast<std::uint32_t>(source.res.v()),
            source.isConstant()};
}

float XgenPtexMap::sample(std::uint32_t face, float u, float v,
                          std::uint32_t channel,
                          const XgenPtexSampleOptions &options) const {
    if (face >= _impl->info.face_count) { fail("face index is out of range"); }
    if (channel >= _impl->info.channel_count) {
        fail("channel index is out of range");
    }
    if (!std::isfinite(u) || !std::isfinite(v) || u < 0.0f || u > 1.0f ||
        v < 0.0f || v > 1.0f) {
        fail("sample coordinates must be finite and in [0, 1]");
    }
    if (!std::isfinite(options.du) || !std::isfinite(options.dv) ||
        !std::isfinite(options.width) || !std::isfinite(options.blur) ||
        !std::isfinite(options.sharpness) || options.du < 0.0f ||
        options.dv < 0.0f || options.width < 0.0f || options.blur < 0.0f ||
        options.sharpness < 0.0f || options.sharpness > 1.0f) {
        fail("filter options are invalid");
    }
    const Ptex::PtexFilter::Options ptex_options{
        ptex_filter(options.filter), options.interpolate_mipmaps,
        options.sharpness, options.disable_edge_blending};
    float result{};
    Ptex::PtexFilter::eval(
        _impl->texture, ptex_options, &result, static_cast<int>(channel), 1,
        static_cast<int>(face), u, v, options.du, 0.0f, 0.0f, options.dv,
        options.width, options.blur);
    if (!std::isfinite(result)) { fail("filter produced a non-finite value"); }
    return result;
}

std::vector<float> XgenPtexMap::read_face_channel(
    std::uint32_t face, std::uint32_t channel) const {
    const XgenPtexFaceInfo face_metadata = face_info(face);
    if (channel >= _impl->info.channel_count) {
        fail("channel index is out of range");
    }
    const std::size_t texel_count =
        static_cast<std::size_t>(face_metadata.u_resolution) *
        face_metadata.v_resolution;
    if (texel_count > std::numeric_limits<std::size_t>::max() /
                          _impl->info.channel_count) {
        fail("face texel count overflows address space");
    }
    std::vector<float> result(texel_count);
    std::vector<float> pixel(_impl->info.channel_count);
    for (std::uint32_t v = 0u; v < face_metadata.v_resolution; ++v) {
        for (std::uint32_t u = 0u; u < face_metadata.u_resolution; ++u) {
            _impl->texture->getPixel(
                static_cast<int>(face), static_cast<int>(u),
                static_cast<int>(v), pixel.data(), 0,
                static_cast<int>(_impl->info.channel_count));
            const float value = pixel[channel];
            if (!std::isfinite(value)) { fail("face contains a non-finite texel"); }
            result[static_cast<std::size_t>(v) * face_metadata.u_resolution + u] =
                value;
        }
    }
    return result;
}

} // namespace nanoxgen
