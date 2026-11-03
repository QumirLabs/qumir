#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace NQumir {
namespace NIR {

// Registers of every active call, as a stack of windows in one reservation.
// A call opens a window right above its caller's; the caller's registers are
// neither copied nor cleared, and the reservation never moves.
class TRegisterFile {
public:
    static constexpr size_t ReservedBytes = 128 * 1024 * 1024;
    static constexpr size_t MaxAlignment = 64;

    // Starts a call chain: the entry function's window at offset zero.
    void Reset(size_t size, size_t alignment) {
        Open(0, size, alignment);
    }

    // End of the window that Push would open, for the caller's stack limit check.
    size_t WindowEnd(size_t size, size_t alignment) const {
        return AlignUp(Base_ + Size_, alignment) + size;
    }

    // Opens a zeroed window above the active one and returns its base.
    size_t Push(size_t size, size_t alignment) {
        const size_t base = AlignUp(Base_ + Size_, alignment);
        Open(base, size, alignment);
        return base;
    }

    // Reactivates a window opened earlier; its registers are as the caller left them.
    void Pop(size_t base, size_t size) {
        assert(base <= Base_);
        Base_ = base;
        Size_ = size;
        Active_ = Data_.get() + base;
    }

    template<typename T>
    T& Get(int32_t byteOffset) {
        CheckAccess<T>(byteOffset);
        return *reinterpret_cast<T*>(Active_ + byteOffset);
    }

    template<typename T>
    const T& Get(int32_t byteOffset) const {
        CheckAccess<T>(byteOffset);
        return *reinterpret_cast<const T*>(Active_ + byteOffset);
    }

    uint8_t* Data() {
        return Active_;
    }

    const uint8_t* Data() const {
        return Active_;
    }

    size_t Size() const {
        return Size_;
    }

    // Bytes of all active windows.
    size_t StackSize() const {
        return Base_ + Size_;
    }

private:
    static size_t AlignUp(size_t value, size_t alignment) {
        alignment = std::max(alignment, alignof(std::max_align_t));
        assert((alignment & (alignment - 1)) == 0 && alignment <= MaxAlignment);
        return (value + alignment - 1) & ~(alignment - 1);
    }

    void Open(size_t base, size_t size, size_t alignment) {
        assert(AlignUp(base, alignment) == base);
        if (base > ReservedBytes || size > ReservedBytes - base) {
            throw std::runtime_error("Stack overflow in interpreter");
        }
        if (!Data_) {
            // Pages are committed as windows first touch them.
            Data_.reset(static_cast<uint8_t*>(::operator new(ReservedBytes, std::align_val_t(MaxAlignment))));
        }
        Base_ = base;
        Size_ = size;
        Active_ = Data_.get() + base;
        if (size != 0) {
            std::memset(Active_, 0, size);
        }
    }

    struct TDeleter {
        void operator()(uint8_t* data) const {
            ::operator delete(data, std::align_val_t(MaxAlignment));
        }
    };

    template<typename T>
    void CheckAccess(int32_t byteOffset) const {
        static_assert(std::is_trivially_copyable_v<T>);
        assert(byteOffset >= 0 && static_cast<size_t>(byteOffset) + sizeof(T) <= Size_);
        assert(reinterpret_cast<uintptr_t>(Active_ + byteOffset) % alignof(T) == 0);
    }

    std::unique_ptr<uint8_t, TDeleter> Data_;
    uint8_t* Active_ = nullptr;
    size_t Base_ = 0;
    size_t Size_ = 0;
};

} // namespace NIR
} // namespace NQumir
