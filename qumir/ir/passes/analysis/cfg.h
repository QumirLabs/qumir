#pragma once

#include <qumir/ir/builder.h>

namespace NQumir {
namespace NIR {
namespace NPasses {

void BuildCfg(TFunction& function);
void BuildCfg(TModule& module);

// reverse post order
std::vector<TLabel> ComputeRPO(const TFunction& function);

} // namespace NPasses
} // namespace NIR
} // namespace NQumir