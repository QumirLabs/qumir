#pragma once

#include <qumir/ir/builder.h>

#include <vector>

namespace NQumir {
namespace NIR {
namespace NPasses {

// Sorted tmp ids.
using TLiveSet = std::vector<int>;

// Requires a built CFG. Phi operands are uses on their incoming edges.
std::vector<TLiveSet> ComputeLiveOut(const TFunction& function);

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
