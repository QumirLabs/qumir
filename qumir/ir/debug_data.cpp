#include "debug_data.h"

#include <format>
#include <sstream>

namespace NQumir {
namespace NIR {
namespace {

std::string Quote(std::string_view value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += c;
        } else if (c < 32) {
            result += std::format("\\u{:04x}", c);
        } else {
            result += c;
        }
    }
    return result + '"';
}

void Location(std::ostream& out, const TLocation& loc) {
    out << "\"line\":" << loc.Line << ",\"column\":" << loc.Column << ",\"byte\":" << loc.Byte;
}

void Type(std::ostream& out, NAst::TTypePtr type) {
    type = NAst::UnwrapNamedType(type);
    out << "{\"kind\":" << Quote(type ? type->TypeName() : "unknown");
    if (auto integer = NAst::TMaybeType<NAst::TIntegerType>(type)) {
        out << ",\"bits\":" << integer.Cast()->BitWidth()
            << ",\"signed\":" << (integer.Cast()->IsSigned() ? "true" : "false");
    } else if (auto ref = NAst::TMaybeType<NAst::TReferenceType>(type)) {
        out << ",\"target\":";
        Type(out, ref.Cast()->ReferencedType);
    }
    out << '}';
}

} // namespace

std::string SerializeRuntimeData(const TModule& module) {
    std::ostringstream out;
    out << "{\"version\":1,\"mode\":" << Quote(module.AsyncMode);
    const TFunction* entry = nullptr;
    for (const auto& function : module.Functions) {
        if (!function.Name.starts_with("__") && !function.Name.starts_with("$$")) {
            entry = &function;
            break;
        }
    }
    if (entry) {
        const int resultType = entry->IsCoroutine
            ? entry->CoroutineResultTypeId
            : entry->ReturnTypeId;
        std::ostringstream type;
        module.Types.Print(type, resultType);
        out << ",\"entryPoint\":" << Quote(entry->Name)
            << ",\"entryIsCoroutine\":" << (entry->IsCoroutine ? "true" : "false")
            << ",\"entryReturnType\":" << Quote(type.str())
            << ",\"entryReturnsString\":" << (entry->ReturnTypeIsString ? "true" : "false");
    }
    // Coroutine lifecycle functions must be driven by the host, rather than
    // registered as synchronous WebAssembly constructors/destructors.
    for (const auto& [key, index] : {
        std::pair{"constructorFunction", module.ModuleConstructorFunctionId},
        std::pair{"destructorFunction", module.ModuleDestructorFunctionId}})
    {
        if (index >= 0 && module.Functions[index].IsCoroutine) {
            out << ',' << Quote(key) << ':' << Quote(module.Functions[index].Name);
        }
    }
    out << '}';
    return out.str();
}

std::string SerializeDebugData(const TModule& module) {
    std::ostringstream out;
    out << "{\"version\":1,\"pointerSize\":" << module.Types.PointerSizeInBytes()
        << ",\"file\":" << Quote(module.SourceFilePath) << ",\"functions\":[";
    for (size_t i = 0; i < module.Functions.size(); ++i) {
        if (i) {
            out << ',';
        }
        const auto& f = module.Functions[i];
        out << "{\"name\":" << Quote(f.DebugInfo ? f.DebugInfo->Name : f.Name)
            << ",\"mangledName\":" << Quote(f.Name) << ",\"parents\":[";
        for (size_t j = 0; j < f.ScopeParents.size(); ++j) {
            if (j) {
                out << ',';
            }
            out << f.ScopeParents[j];
        }
        out << "],\"args\":[";
        for (size_t j = 0; j < f.ArgLocals.size(); ++j) {
            if (j) {
                out << ',';
            }
            out << f.ArgLocals[j].Idx;
        }
        out << "],";
        Location(out, f.DebugInfo ? f.DebugInfo->Location : TLocation{});
        out << ",\"locals\":[";
        for (size_t j = 0; j < f.LocalDebugInfo.size(); ++j) {
            if (j) {
                out << ',';
            }
            const auto& local = f.LocalDebugInfo[j];
            out << "{\"name\":" << Quote(local.Name) << ",\"scope\":" << local.ScopeId << ',';
            Location(out, local.Location);
            out << ",\"type\":";
            Type(out, local.AstType);
            out << '}';
        }
        out << "]}";
    }
    out << "],\"points\":[";
    for (size_t i = 0; i < module.DebugPoints.size(); ++i) {
        if (i) {
            out << ',';
        }
        const auto& point = module.DebugPoints[i];
        out << "{\"kind\":" << Quote(point.Kind) << ",\"function\":" << point.FunctionIdx
            << ",\"scope\":" << point.Info.ScopeId << ",\"local\":" << point.LocalId
            << ",\"op\":" << Quote(point.Op) << ',';
        Location(out, point.Info.Location);
        out << '}';
    }
    out << "],\"bindings\":[";
    for (size_t i = 0; i < module.DebugBindings.size(); ++i) {
        if (i) {
            out << ',';
        }
        const auto& binding = module.DebugBindings[i];
        out << "{\"function\":" << binding.FunctionIdx << ",\"local\":" << binding.LocalId
            << ",\"name\":" << Quote(binding.Name) << ',';
        Location(out, binding.Location);
        out << '}';
    }
    out << "]}";
    return out.str();
}

} // namespace NIR
} // namespace NQumir
