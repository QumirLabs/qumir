#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

namespace NQumir {
namespace NIR {

class TRegisterFile {
public:
    void Reset(size_t size, size_t alignment) {
        InitializeFrame(0, size, alignment);
        StackSize_ = size;
    }

    void PushFrame(size_t size, size_t alignment) {
        InitializeFrame(FrameIndex_ + 1, size, alignment);
        StackSize_ += size;
    }

    void PopFrame() {
        assert(FrameIndex_ != 0);
        StackSize_ -= Size_;
        --FrameIndex_;
        Data_ = Frames_[FrameIndex_].Data.get();
        Size_ = Frames_[FrameIndex_].Size;
    }

    template<typename T>
    T& Get(int32_t byteOffset) {
        CheckAccess<T>(byteOffset);
        return *reinterpret_cast<T*>(Data_ + byteOffset);
    }

    template<typename T>
    const T& Get(int32_t byteOffset) const {
        CheckAccess<T>(byteOffset);
        return *reinterpret_cast<const T*>(Data_ + byteOffset);
    }

    uint8_t* Data() { return Data_; }
    const uint8_t* Data() const { return Data_; }
    size_t Size() const { return Size_; }
    size_t StackSize() const { return StackSize_; }

private:
    struct TDeleter {
        size_t Alignment = alignof(std::max_align_t);

        void operator()(uint8_t* data) const {
            ::operator delete(data, std::align_val_t(Alignment));
        }
    };

    struct TStorage {
        std::unique_ptr<uint8_t, TDeleter> Data{nullptr, TDeleter{}};
        size_t Size = 0;
        size_t Capacity = 0;
        size_t Alignment = alignof(std::max_align_t);
    };

    void InitializeFrame(size_t index, size_t size, size_t alignment) {
        alignment = std::max(alignment, alignof(std::max_align_t));
        assert((alignment & (alignment - 1)) == 0);
        if (index == Frames_.size()) {
            Frames_.emplace_back();
        }
        auto& storage = Frames_[index];
        if (size > storage.Capacity || alignment > storage.Alignment) {
            auto* data = static_cast<uint8_t*>(::operator new(size, std::align_val_t(alignment)));
            storage.Data = {data, TDeleter{alignment}};
            storage.Capacity = size;
            storage.Alignment = alignment;
        }
        storage.Size = size;
        if (size != 0) {
            std::memset(storage.Data.get(), 0, size);
        }
        FrameIndex_ = index;
        Data_ = storage.Data.get();
        Size_ = size;
    }

    template<typename T>
    void CheckAccess(int32_t byteOffset) const {
        static_assert(std::is_trivially_copyable_v<T>);
        assert(byteOffset >= 0 && static_cast<size_t>(byteOffset) + sizeof(T) <= Size_);
        assert(reinterpret_cast<uintptr_t>(Data_ + byteOffset) % alignof(T) == 0);
    }

    // Buffers are retained by call depth; growing the descriptor vector cannot
    // move a suspended caller's registers or require copying them on return.
    std::vector<TStorage> Frames_;
    uint8_t* Data_ = nullptr;
    size_t Size_ = 0;
    size_t FrameIndex_ = 0;
    size_t StackSize_ = 0;
};

} // namespace NIR
} // namespace NQumir
