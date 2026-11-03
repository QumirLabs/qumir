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

// The active window by value: the interpreter loop keeps it in machine registers.
// It stays valid until the next call or return.
class TRegisterView {
public:
    TRegisterView(uint8_t* data, size_t size)
        : Data_(data)
        , Size_(size)
    {}

    // Shared with TRegisterFile, which checks the same window.
    template<typename T>
    static void CheckAccess(const uint8_t* data, size_t size, int32_t byteOffset) {
        static_assert(std::is_trivially_copyable_v<T>);
        assert(byteOffset >= 0 && static_cast<size_t>(byteOffset) + sizeof(T) <= size);
        assert(reinterpret_cast<uintptr_t>(data + byteOffset) % alignof(T) == 0);
    }

    template<typename T>
    T& Get(int32_t byteOffset) const {
        CheckAccess<T>(Data_, Size_, byteOffset);
        return *reinterpret_cast<T*>(Data_ + byteOffset);
    }

    uint8_t* Data() const {
        return Data_;
    }

    size_t Size() const {
        return Size_;
    }

private:
    uint8_t* Data_;
    size_t Size_;
};

// Registers of every active call, as a stack of windows in one reservation.
// A call opens a window right above its caller's; the caller's registers are
// neither copied nor cleared, and the reservation never moves.
class TRegisterFile {
public:
    static constexpr size_t ReservedBytes = 128 * 1024 * 1024;
    static constexpr size_t MaxAlignment = 64;

    // Starts a call chain: the entry function's window at offset zero.
    void Reset(size_t size, size_t alignment) {
        Open(0, size, alignment, 0);
    }

    // End of the window that Push would open, for the caller's stack limit check.
    size_t WindowEnd(size_t offset, size_t size) const {
        return Base_ + offset + size;
    }

    // Opens a window at `offset` inside the active one, which is where the caller
    // wrote the callee's first `keep` bytes (parameters); the rest is zeroed.
    // Returns the window's base.
    size_t Push(size_t offset, size_t size, size_t alignment, size_t keep) {
        const size_t base = Base_ + offset;
        Open(base, size, alignment, keep);
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
        TRegisterView::CheckAccess<T>(Active_, Size_, byteOffset);
        return *reinterpret_cast<T*>(Active_ + byteOffset);
    }

    template<typename T>
    const T& Get(int32_t byteOffset) const {
        TRegisterView::CheckAccess<T>(Active_, Size_, byteOffset);
        return *reinterpret_cast<const T*>(Active_ + byteOffset);
    }

    TRegisterView View() const {
        return TRegisterView(Active_, Size_);
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

    void Open(size_t base, size_t size, size_t alignment, size_t keep) {
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
        if (size > keep) {
            std::memset(Active_ + keep, 0, size - keep);
        }
    }

    struct TDeleter {
        void operator()(uint8_t* data) const {
            ::operator delete(data, std::align_val_t(MaxAlignment));
        }
    };

    std::unique_ptr<uint8_t, TDeleter> Data_;
    uint8_t* Active_ = nullptr;
    size_t Base_ = 0;
    size_t Size_ = 0;
};

} // namespace NIR
} // namespace NQumir
