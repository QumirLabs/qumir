#pragma once

#include <cstddef>

namespace NQumir::NRuntime {

extern "C" {

void* array_create(size_t sizeInBytes);
void array_destroy(void* ptr);
void array_str_destroy(void* ptr, size_t elements);

} // extern "C"

} // namespace NQumir::NRuntime
