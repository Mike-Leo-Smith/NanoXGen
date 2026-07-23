#pragma once

#include <luisa/dsl/sugar.h>

#include <array>

namespace nanoxgen::luisa_backend::packed_io {

static_assert(sizeof(std::array<float, 3u>) == 3u * sizeof(float));
static_assert(alignof(std::array<float, 3u>) == alignof(float));

[[nodiscard]] inline luisa::compute::Float3 read_packed_float3(
    const luisa::compute::ByteBufferVar &buffer,
    luisa::compute::Expr<luisa::uint> byte_offset) noexcept {
    const auto packed = buffer.read<std::array<float, 3u>>(byte_offset);
    return luisa::compute::make_float3(
        packed[0u], packed[1u], packed[2u]);
}

} // namespace nanoxgen::luisa_backend::packed_io
