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

TRegisterAllocation AllocateRegisters(
    const TFunction& function,
    const TTypeTable& types,
    const std::vector<TLiveSet>& liveOut);

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
