#include "array.h"

#include <new>
#include <iostream>
#include <cstring>

#include "string.h"

namespace NQumir::NRuntime {

void* array_create(size_t sizeInBytes) {
    auto ptr = operator new(sizeInBytes, std::align_val_t(8));
    memset(ptr, 0, sizeInBytes);
    return ptr;
}

void array_destroy(void* ptr) {
    operator delete(ptr, std::align_val_t(8));
}

void array_str_destroy(void* ptr, size_t elements) {
    if (!ptr) return;
    char** strArray = static_cast<char**>(ptr);
    for (size_t i = 0; i < elements; ++i) {
        str_release(strArray[i]);
    }
    array_destroy(ptr);
}

} // namespace NQumir::NRuntime
