#pragma once

#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

namespace nanoxgen {

inline constexpr std::size_t kBlobAlignment = 64u;

template<typename T, std::size_t Alignment>
class AlignedAllocator {
public:
    static_assert(Alignment >= alignof(T));
    static_assert((Alignment & (Alignment - 1u)) == 0u);

    using value_type = T;
    using is_always_equal = std::true_type;

    AlignedAllocator() noexcept = default;

    template<typename U>
    AlignedAllocator(const AlignedAllocator<U, Alignment> &) noexcept {}

    [[nodiscard]] T *allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length{};
        }
        if (count == 0u) { return nullptr; }
        return static_cast<T *>(
            ::operator new(count * sizeof(T), std::align_val_t{Alignment}));
    }

    void deallocate(T *pointer, std::size_t) noexcept {
        ::operator delete(pointer, std::align_val_t{Alignment});
    }

    template<typename U>
    struct rebind {
        using other = AlignedAllocator<U, Alignment>;
    };
};

template<typename T, typename U, std::size_t Alignment>
bool operator==(
    const AlignedAllocator<T, Alignment> &,
    const AlignedAllocator<U, Alignment> &) noexcept {
    return true;
}

template<typename T, typename U, std::size_t Alignment>
bool operator!=(
    const AlignedAllocator<T, Alignment> &,
    const AlignedAllocator<U, Alignment> &) noexcept {
    return false;
}

using AlignedByteVector =
    std::vector<std::byte, AlignedAllocator<std::byte, kBlobAlignment>>;

} // namespace nanoxgen
