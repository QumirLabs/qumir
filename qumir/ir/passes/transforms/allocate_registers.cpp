#include "allocate_registers.h"

#include <qumir/align.h>

#include <algorithm>
#include <limits>
#include <tuple>

namespace NQumir {
namespace NIR {
namespace NPasses {

using namespace NLiterals;

namespace {

TRegisterSlot Shape(int typeId, const TTypeTable& types) {
    const auto kind = types.GetKind(typeId);
    return {
        .Offset = 0,
        .Size = kind == EKind::Struct ? 8 : std::max(8, types.SizeInBytes(typeId)),
        .Alignment = kind == EKind::I128 || kind == EKind::U128 ? 16 : 8,
    };
}

} // namespace

TRegisterSlot TRegisterAllocation::Reserve(int tmp, int size, int alignment) {
    TRegisterSlot slot{0, size, alignment};
    if (Size > std::numeric_limits<int32_t>::max() - slot.Alignment - slot.Size) {
        throw std::runtime_error("VM register file is too large");
    }
    slot.Offset = AlignUp(Size, slot.Alignment);
    Size = slot.Offset + slot.Size;
    Alignment = std::max(Alignment, slot.Alignment);
    Slots[tmp] = slot;
    return slot;
}

TRegisterAllocation AllocateRegisters(
    const TFunction& function,
    const TTypeTable& types,
    const std::vector<TLiveSet>& liveOut)
{
    std::map<int, TLiveSet> conflicts, preferences;
    auto touch = [&](int tmp) {
        if (tmp >= 0) {
            conflicts.try_emplace(tmp);
        }
    };
    auto edge = [&](int a, int b) {
        if (a >= 0 && b >= 0 && a != b) {
            conflicts[a].insert(b);
            conflicts[b].insert(a);
        }
    };
    for (size_t b = 0; b < function.Blocks.size(); ++b) {
        const auto& block = function.Blocks[b];
        auto live = liveOut[b];
        for (auto it = block.Instrs.rbegin(); it != block.Instrs.rend(); ++it) {
            const auto& instr = *it;
            touch(instr.Dest.Idx);
            for (int tmp : live) {
                edge(instr.Dest.Idx, tmp);
            }
            live.erase(instr.Dest.Idx);
            for (size_t i = 0; i < instr.OperandCount; ++i) {
                const auto& operand = instr.Operands[i];
                if (operand.Type == TOperand::EType::Tmp && operand.Tmp.Idx >= 0) {
                    touch(operand.Tmp.Idx);
                    // Vector handlers can write a lane before reading later inputs.
                    edge(instr.Dest.Idx, operand.Tmp.Idx);
                    live.insert(operand.Tmp.Idx);
                }
            }
        }
        for (const auto& phi : block.Phis) {
            if (phi.Op != "phi"_op) {
                continue;
            }
            touch(phi.Dest.Idx);
            for (int tmp : live) {
                edge(phi.Dest.Idx, tmp);
            }
            for (size_t i = 0; i < phi.Operands.size(); i += 2) {
                const auto& source = phi.Operands[i];
                if (source.Type == TOperand::EType::Tmp) {
                    touch(source.Tmp.Idx);
                    preferences[phi.Dest.Idx].insert(source.Tmp.Idx);
                    preferences[source.Tmp.Idx].insert(phi.Dest.Idx);
                }
            }
        }
    }

    std::vector<int> order;
    for (const auto& [tmp, neighbors] : conflicts) {
        order.push_back(tmp);
    }
    auto priority = [&](int tmp) {
        const auto shape = Shape(function.GetTmpType(tmp), types);
        return std::tuple{-shape.Size, -shape.Alignment, -static_cast<int>(conflicts.at(tmp).size()), tmp};
    };
    std::sort(order.begin(), order.end(), [&](int a, int b) { return priority(a) < priority(b); });

    std::vector<TRegisterSlot> colors;
    TRegisterAllocation allocation;
    for (int tmp : order) {
        const int typeId = function.GetTmpType(tmp);
        const auto shape = Shape(typeId, types);
        // Struct return metadata binds an offset to one specific frame object.
        if (types.GetKind(typeId) == EKind::Struct) {
            allocation.Reserve(tmp, shape.Size, shape.Alignment);
            continue;
        }
        auto fits = [&](const TRegisterSlot& color) {
            return color.Size == shape.Size && color.Alignment >= shape.Alignment
                && std::none_of(conflicts.at(tmp).begin(), conflicts.at(tmp).end(), [&](int neighbor) {
                    const auto it = allocation.Slots.find(neighbor);
                    return it != allocation.Slots.end() && it->second.Offset == color.Offset;
                });
        };
        auto chosen = colors.end();
        for (int preferred : preferences[tmp]) {
            if (auto it = allocation.Slots.find(preferred); it != allocation.Slots.end()) {
                chosen = std::find_if(colors.begin(), colors.end(), [&](const TRegisterSlot& color) {
                    return color.Offset == it->second.Offset && fits(color);
                });
                if (chosen != colors.end()) {
                    break;
                }
            }
        }
        if (chosen == colors.end()) {
            chosen = std::find_if(colors.begin(), colors.end(), fits);
        }
        if (chosen == colors.end()) {
            colors.push_back(allocation.Reserve(tmp, shape.Size, shape.Alignment));
        } else {
            allocation.Slots[tmp] = *chosen;
        }
    }
    return allocation;
}

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
