#include "allocate_registers.h"

#include <qumir/align.h>

#include <algorithm>
#include <limits>
#include <tuple>
#include <vector>

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
    const std::vector<TLiveSet>& liveOut,
    TRegisterAllocation allocation)
{
    // Tmp ids are dense, so per-tmp state is indexed by id.
    int tmpCount = 0;
    auto see = [&](const TOperand& operand) {
        if (operand.Type == TOperand::EType::Tmp) {
            tmpCount = std::max(tmpCount, operand.Tmp.Idx + 1);
        }
    };
    for (const auto& block : function.Blocks) {
        for (const auto& phi : block.Phis) {
            tmpCount = std::max(tmpCount, phi.Dest.Idx + 1);
            for (const auto& operand : phi.Operands) {
                see(operand);
            }
        }
        for (const auto& instr : block.Instrs) {
            tmpCount = std::max(tmpCount, instr.Dest.Idx + 1);
            for (size_t i = 0; i < instr.OperandCount; ++i) {
                see(instr.Operands[i]);
            }
        }
    }
    std::vector<char> touched(tmpCount);
    std::vector<std::vector<int>> conflicts(tmpCount), preferences(tmpCount);
    auto touch = [&](int tmp) {
        if (tmp >= 0) {
            touched[tmp] = true;
        }
    };
    auto edge = [&](int a, int b) {
        if (a >= 0 && b >= 0 && a != b) {
            conflicts[a].push_back(b);
            conflicts[b].push_back(a);
        }
    };
    // Live tmps as a sparse set: the members and the position of each one.
    std::vector<int> live;
    std::vector<int> position(tmpCount, -1);
    auto insert = [&](int tmp) {
        if (position[tmp] < 0) {
            position[tmp] = live.size();
            live.push_back(tmp);
        }
    };
    auto erase = [&](int tmp) {
        if (tmp >= 0 && position[tmp] >= 0) {
            position[live.back()] = position[tmp];
            live[position[tmp]] = live.back();
            live.pop_back();
            position[tmp] = -1;
        }
    };
    for (size_t b = 0; b < function.Blocks.size(); ++b) {
        const auto& block = function.Blocks[b];
        for (int tmp : live) {
            position[tmp] = -1;
        }
        live.clear();
        for (int tmp : liveOut[b]) {
            insert(tmp);
        }
        for (auto it = block.Instrs.rbegin(); it != block.Instrs.rend(); ++it) {
            const auto& instr = *it;
            touch(instr.Dest.Idx);
            for (int tmp : live) {
                edge(instr.Dest.Idx, tmp);
            }
            erase(instr.Dest.Idx);
            for (size_t i = 0; i < instr.OperandCount; ++i) {
                const auto& operand = instr.Operands[i];
                if (operand.Type == TOperand::EType::Tmp && operand.Tmp.Idx >= 0) {
                    touch(operand.Tmp.Idx);
                    // Vector handlers can write a lane before reading later inputs.
                    edge(instr.Dest.Idx, operand.Tmp.Idx);
                    insert(operand.Tmp.Idx);
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
                if (source.Type == TOperand::EType::Tmp && source.Tmp.Idx >= 0) {
                    touch(source.Tmp.Idx);
                    preferences[phi.Dest.Idx].push_back(source.Tmp.Idx);
                    preferences[source.Tmp.Idx].push_back(phi.Dest.Idx);
                }
            }
        }
    }

    std::vector<int> order;
    std::vector<TRegisterSlot> shapes(tmpCount);
    for (int tmp = 0; tmp < tmpCount; ++tmp) {
        if (!touched[tmp]) {
            continue;
        }
        for (auto* list : {&conflicts[tmp], &preferences[tmp]}) {
            std::sort(list->begin(), list->end());
            list->erase(std::unique(list->begin(), list->end()), list->end());
        }
        shapes[tmp] = Shape(function.GetTmpType(tmp), types);
        order.push_back(tmp);
    }
    auto priority = [&](int tmp) {
        return std::tuple{-shapes[tmp].Size, -shapes[tmp].Alignment, -static_cast<int>(conflicts[tmp].size()), tmp};
    };
    std::sort(order.begin(), order.end(), [&](int a, int b) { return priority(a) < priority(b); });

    std::vector<TRegisterSlot> colors;
    std::vector<int> colorOf(tmpCount, -1);
    // The last tmp that has a neighbor in the color.
    std::vector<int> takenBy;
    for (int tmp : order) {
        if (allocation.Slots.contains(tmp)) {
            continue;
        }
        const auto& shape = shapes[tmp];
        // Struct return metadata binds an offset to one specific frame object.
        if (types.GetKind(function.GetTmpType(tmp)) == EKind::Struct) {
            allocation.Reserve(tmp, shape.Size, shape.Alignment);
            continue;
        }
        for (int neighbor : conflicts[tmp]) {
            if (colorOf[neighbor] >= 0) {
                takenBy[colorOf[neighbor]] = tmp;
            }
        }
        auto fits = [&](int color) {
            return colors[color].Size == shape.Size && colors[color].Alignment >= shape.Alignment
                && takenBy[color] != tmp;
        };
        int chosen = -1;
        for (int preferred : preferences[tmp]) {
            if (colorOf[preferred] >= 0 && fits(colorOf[preferred])) {
                chosen = colorOf[preferred];
                break;
            }
        }
        for (int color = 0; chosen < 0 && color < (int)colors.size(); ++color) {
            if (fits(color)) {
                chosen = color;
            }
        }
        if (chosen < 0) {
            chosen = colors.size();
            colors.push_back(allocation.Reserve(tmp, shape.Size, shape.Alignment));
            takenBy.push_back(-1);
        } else {
            allocation.Slots[tmp] = colors[chosen];
        }
        colorOf[tmp] = chosen;
    }
    return allocation;
}

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
