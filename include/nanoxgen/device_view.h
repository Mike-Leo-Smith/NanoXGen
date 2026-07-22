#pragma once

#include "nanoxgen/types.h"

namespace nanoxgen {

class DeviceAssetView {
public:
    NXG_HOST_DEVICE explicit DeviceAssetView(const void *data = nullptr) noexcept
        : _base(static_cast<const std::byte *>(data)) {}

    NXG_HOST_DEVICE const AssetHeader &header() const noexcept {
        return *reinterpret_cast<const AssetHeader *>(_base);
    }

    NXG_HOST_DEVICE const Vec3 *positions() const noexcept { return at<Vec3>(header().positions_offset); }
    NXG_HOST_DEVICE const Vec3 *normals() const noexcept { return at<Vec3>(header().normals_offset); }
    NXG_HOST_DEVICE const Vec2 *texcoords() const noexcept { return at<Vec2>(header().texcoords_offset); }
    NXG_HOST_DEVICE const UInt3 *triangles() const noexcept { return at<UInt3>(header().triangles_offset); }
    NXG_HOST_DEVICE const AliasEntry *alias_table() const noexcept { return at<AliasEntry>(header().alias_table_offset); }
    NXG_HOST_DEVICE const GuideRecord *guides() const noexcept { return at<GuideRecord>(header().guides_offset); }
    NXG_HOST_DEVICE const Vec3 *guide_cvs() const noexcept { return at<Vec3>(header().guide_cvs_offset); }
    NXG_HOST_DEVICE const std::uint32_t *triangle_guides() const noexcept {
        return at<std::uint32_t>(header().triangle_guides_offset);
    }
    NXG_HOST_DEVICE const Vec3 *noise_gradients() const noexcept {
        return at<Vec3>(header().noise_gradients_offset);
    }

    NXG_HOST_DEVICE const void *data() const noexcept { return _base; }
    NXG_HOST_DEVICE explicit operator bool() const noexcept { return _base != nullptr; }

private:
    template<typename T>
    NXG_HOST_DEVICE const T *at(std::uint64_t offset) const noexcept {
        return offset == 0u ? nullptr : reinterpret_cast<const T *>(_base + offset);
    }

    const std::byte *_base{};
};

} // namespace nanoxgen
