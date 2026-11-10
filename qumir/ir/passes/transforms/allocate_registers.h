#pragma once

#include <qumir/ir/passes/analysis/liveness.h>

#include <unordered_map>

namespace NQumir {
namespace NIR {
namespace NPasses {

struct TRegisterSlot {
    int Offset;
    int Size;
    int Alignment;
};

struct TRegisterAllocation {
    std::unordered_map<int, TRegisterSlot> Slots;
    int Size = 0;
    int Alignment = 8;

    TRegisterSlot Reserve(int tmp, int size, int alignment);
};

// Slots already in `allocation`, such as parameter registers, stay where they are
// and are not shared; the other values get slots past `allocation.Size`.
TRegisterAllocation AllocateRegisters(
    const TFunction& function,
    const TTypeTable& types,
    const std::vector<TLiveSet>& liveOut,
    TRegisterAllocation allocation);

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
