#pragma once

#include <qumir/ir/builder.h>

namespace NQumir {
namespace NIR {
namespace NPasses {

// Replaces calls to functions marked inline with copies of their bodies, callees
// first. Recursive calls, coroutines and externals stay calls. Runs before
// PromoteLocalsToSSA, which turns the parameter and result locals into values.
void InlineCalls(TModule& module);

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
