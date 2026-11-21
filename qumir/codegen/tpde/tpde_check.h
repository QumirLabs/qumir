#pragma once

#include "tpde_adaptor.h"

#include <optional>
#include <string>

namespace NQumir {
namespace NCodeGen {
namespace NTpde {

// How many general and floating-point registers the platform returns a value in.
struct TReturnLimits {
    u32 Gp;
    u32 Fp;
};

// Describes the first construct of the module that the TPDE compilers do not
// implement; nullopt when all of it is supported.
std::optional<std::string> FindUnsupported(TModuleInfo& info, TReturnLimits returnLimits);

} // namespace NTpde
} // namespace NCodeGen
} // namespace NQumir
