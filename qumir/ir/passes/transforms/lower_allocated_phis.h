#pragma once

#include <qumir/ir/passes/transforms/allocate_registers.h>

namespace NQumir {
namespace NIR {
namespace NPasses {

void LowerAllocatedPhis(TFunction& function, TRegisterAllocation& allocation);

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
