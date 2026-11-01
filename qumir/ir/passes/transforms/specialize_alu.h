#pragma once

#include <qumir/ir/vminstr.h>

#include <span>

namespace NQumir {
namespace NIR {
namespace NPasses {

void SpecializeAlu(std::span<TVMInstr> code);

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
