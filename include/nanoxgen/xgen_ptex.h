#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace nanoxgen {

enum class XgenPtexMeshType : std::uint8_t { Triangle, Quad };
enum class XgenPtexDataType : std::uint8_t { UInt8, UInt16, Half, Float };
enum class XgenPtexFilter : std::uint8_t {
    Point,
    Bilinear,
    Box,
    Gaussian,
    Bicubic,
    BSpline,
    CatmullRom,
    Mitchell
};

struct XgenPtexInfo {
    XgenPtexMeshType mesh_type{};
    XgenPtexDataType data_type{};
    std::uint32_t channel_count{};
    std::uint32_t face_count{};
    std::int32_t alpha_channel{-1};
    bool has_mipmaps{};
};

struct XgenPtexFaceInfo {
    std::uint32_t u_resolution{};
    std::uint32_t v_resolution{};
    bool constant{};
};

struct XgenPtexSampleOptions {
    XgenPtexFilter filter{XgenPtexFilter::Bilinear};
    float du{0.0f};
    float dv{0.0f};
    float width{1.0f};
    float blur{0.0f};
    float sharpness{0.0f};
    bool interpolate_mipmaps{};
    bool disable_edge_blending{};
};

// Float-only facade over the optional system Ptex library. Ptex decoding is
// deliberately outside the default NanoXGen core and outside GPU hot paths.
class XgenPtexMap {
public:
    explicit XgenPtexMap(const std::filesystem::path &path);
    ~XgenPtexMap();
    XgenPtexMap(XgenPtexMap &&) noexcept;
    XgenPtexMap &operator=(XgenPtexMap &&) noexcept;
    XgenPtexMap(const XgenPtexMap &) = delete;
    XgenPtexMap &operator=(const XgenPtexMap &) = delete;

    [[nodiscard]] const XgenPtexInfo &info() const noexcept;
    [[nodiscard]] XgenPtexFaceInfo face_info(std::uint32_t face) const;
    [[nodiscard]] float sample(
        std::uint32_t face, float u, float v, std::uint32_t channel = 0u,
        const XgenPtexSampleOptions &options = {}) const;
    [[nodiscard]] std::vector<float> read_face_channel(
        std::uint32_t face, std::uint32_t channel = 0u) const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace nanoxgen
