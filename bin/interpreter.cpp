#include <istream>
#include <qumir/runner/runner_ir.h>
#include <qumir/runner/runner_llvm.h>
#include <qumir/codegen/llvm/llvm_backend.h>
#include <qumir/codegen/llvm/llvm_initializer.h>
#ifdef QUMIR_ENABLE_TPDE
#include <qumir/codegen/tpde/tpde_backend.h>
#endif

#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <unordered_map>

#include <readline/readline.h>
#include <readline/history.h>

using namespace NQumir;

namespace {

volatile std::sig_atomic_t StopOnNextInstruction = 0;

void PrintResultIR(const std::optional<std::string>& v) {
    if (v.has_value()) {
        std::cout << "\n";
        std::cout << "Res:\n";
        std::cout << *v << std::endl;
    }
}

class TValuePrinter {
public:
    explicit TValuePrinter(const NIR::TTypeTable& types)
        : Types(types)
    {}

    void Print(std::ostream& out, const char* data, int typeId, NAst::TTypePtr astType) const {
        const auto kind = Types.GetKind(typeId);
        while (auto named = NAst::TMaybeType<NAst::TNamedType>(astType)) {
            astType = named.Cast()->UnderlyingType;
        }
        if (kind == NIR::EKind::Struct) {
            const auto& fields = Types.GetStructFields(typeId);
            auto astStruct = NAst::TMaybeType<NAst::TStructType>(astType).Cast();
            out << "{";
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i != 0) {
                    out << ", ";
                }
                NAst::TTypePtr fieldAstType;
                if (astStruct && i < astStruct->Fields.size()) {
                    out << astStruct->Fields[i].first;
                    fieldAstType = astStruct->Fields[i].second;
                } else {
                    out << "#" << i;
                }
                out << ": ";
                Print(out, data + Types.FieldOffset(typeId, i), fields[i], fieldAstType);
            }
            out << "}";
            return;
        }
        if (kind == NIR::EKind::Undef) {
            out << "<undef>";
            return;
        }
        if (kind == NIR::EKind::I128 || kind == NIR::EKind::U128) {
            __uint128_t bits;
            std::memcpy(&bits, data, sizeof(bits));
            if (kind == NIR::EKind::I128 && static_cast<__int128_t>(bits) < 0) {
                out << "-";
                bits = ~bits + 1;
            }
            PrintUnsigned128(out, bits);
            return;
        }
        if (kind == NIR::EKind::F32) {
            float value;
            std::memcpy(&value, data, sizeof(value));
            out << value;
            return;
        }
        uint64_t bits = 0;
        int valueSize = Types.SizeInBytes(typeId);
        if (valueSize > 0) {
            std::memcpy(&bits, data, std::min(static_cast<size_t>(valueSize), sizeof(bits)));
        }
        if (NAst::TMaybeType<NAst::TStringType>(astType)) {
            if (bits == 0) {
                out << "null";
            } else {
                out << std::quoted(reinterpret_cast<const char*>(bits));
            }
        } else if (NAst::TMaybeType<NAst::TSymbolType>(astType)) {
            out << "U+" << std::hex << static_cast<uint32_t>(bits) << std::dec;
        } else {
            Types.Format(out, bits, typeId);
        }
    }

private:
    static void PrintUnsigned128(std::ostream& out, __uint128_t value) {
        char digits[40];
        size_t pos = sizeof(digits);
        do {
            digits[--pos] = '0' + value % 10;
            value /= 10;
        } while (value != 0);
        out.write(digits + pos, sizeof(digits) - pos);
    }

    const NIR::TTypeTable& Types;
};

} // namespace

struct TInteractiveDebugger : public NIR::IDebugger {
    void SetRuntime(const NIR::TRuntime& runtime) override {
        Runtime = &runtime;
    }

    void SetModule(const NIR::TModule& module) override {
        Module = &module;
    }

    void OnFunctionCompilationFinished(const NIR::TFunction& function) override {
        std::cout << "Function compiled: " << function.Name << std::endl;
        FunctionMap[function.Name] = &function;
        FunctionNameMap.emplace(function.DebugInfo ? function.DebugInfo->Name : function.Name, &function);
        for (size_t i = 0; i < function.Exec->InstrDebugInfo.size(); ++i) {
            const auto& debugInfo = function.Exec->InstrDebugInfo[i];
            if (debugInfo) {
                LineMap[debugInfo.Location.Line] = function.Exec->VMCode.data() + i;
            }
        }
    }

    void OnModuleCompilationFinished() override {
        std::signal(SIGINT, [](int) { StopOnNextInstruction = 1; });
        std::cout << "Module compilation finished." << std::endl;
        UserInput(nullptr);
    }

    void OnInstruction(const NIR::TFrame& frame) override {
        const NIR::TVMInstr& instr = *frame.PC;
        const bool interrupted = StopOnNextInstruction != 0;
        if (interrupted || Breakpoints.find(frame.PC) != Breakpoints.end()) {
            StopOnNextInstruction = 0;
            std::cout << (interrupted ? "Interrupted at instruction: " : "Breakpoint hit at instruction: ")
                << instr << " in function: " << frame.Name << std::endl;
            UserInput(&frame);
        }
    }

private:
    void AddHistory(const std::string& input) {
        // TODO
    }

    std::string InputString() {
        char* line = readline("(qumir) ");
        if (!line) {
            std::exit(0);
            return "";
        }
        std::string input(line); free(line);
        if (!input.empty()) {
            AddHistory(input);
        }
        return input;
    }

    void UserInput(const NIR::TFrame* frame) {
        while (true) {
            auto input = InputString();
            if (input.empty()) {
                continue;
            } else if (input == "c" || input == "continue") {
                StopOnNextInstruction = 0;
                break;
            } else if (input == "bt") {
                if (frame) {
                    PrintCallStack(*frame);
                } else {
                    std::cout << "No frame available." << std::endl;
                }
            } else if (input == "locals") {
                if (frame) {
                    PrintNamedLocals(*frame);
                } else {
                    std::cout << "No frame available." << std::endl;
                }
            } else if (input == "list" || input == "l") {
                ListSource(frame);
            } else if (input.starts_with("b ")) {
                std::string arg = input.substr(2);
                if (arg.find(':') != std::string::npos) {
                    auto parts = arg.find(':');
                    std::string fileName = arg.substr(0, parts);
                    int lineNumber = std::stoi(arg.substr(parts + 1));
                    auto it = LineMap.find(lineNumber);
                    if (it != LineMap.end()) {
                        Breakpoints.insert(it->second);
                        std::cout << "Breakpoint set at " << fileName << ":" << lineNumber << std::endl;
                    } else {
                        std::cout << "No instruction found for " << fileName << ":" << lineNumber << std::endl;
                    }
                } else {
                    auto [first, last] = FunctionNameMap.equal_range(arg);
                    if (first == last) {
                        std::cout << "No function found for: " << arg << std::endl;
                        continue;
                    }
                    for (auto it = first; it != last; ++it) {
                        const auto& code = it->second->Exec->VMCode;
                        if (!code.empty()) {
                            Breakpoints.insert(code.data());
                        }
                    }
                    std::cout << "Breakpoint set at function: " << arg << std::endl;
                }
            }
        }
    }

    std::optional<NIR::TInstrDebugInfo> GetInstrDebugInfo(const NIR::TFrame& frame, bool current) const {
        size_t index = frame.PC - frame.Exec->VMCode.data();
        if (!current && index > 0) {
            --index;
        }
        if (index < frame.Exec->InstrDebugInfo.size() && frame.Exec->InstrDebugInfo[index]) {
            return frame.Exec->InstrDebugInfo[index];
        }
        return std::nullopt;
    }

    void ListSource(const NIR::TFrame* frame) const {
        if (!Module || Module->SourceFilePath.empty()) {
            std::cout << "No source file available.\n";
            return;
        }
        std::ifstream source(Module->SourceFilePath);
        if (!source) {
            std::cout << "Failed to open source file: " << Module->SourceFilePath << "\n";
            return;
        }
        int currentLine = 0;
        if (frame) {
            if (auto debugInfo = GetInstrDebugInfo(*frame, true)) {
                currentLine = debugInfo->Location.Line;
            }
        }
        const int firstLine = std::max(1, currentLine - 5);
        const int lastLine = firstLine + 10;
        std::cout << Module->SourceFilePath << ":\n";
        std::string line;
        for (int lineNumber = 1; lineNumber <= lastLine && std::getline(source, line); ++lineNumber) {
            if (lineNumber >= firstLine) {
                std::cout << (lineNumber == currentLine ? "=> " : "   ")
                    << std::setw(4) << lineNumber << "  " << line << "\n";
            }
        }
    }

    void PrintNamedLocals(const NIR::TFrame& frame) {
        if (!Runtime || !Module) {
            return;
        }
        auto funcIt = FunctionMap.find(std::string(frame.Name));
        if (funcIt == FunctionMap.end()) {
            return;
        }
        auto func = funcIt->second;

        std::set<int> reachableScopes;
        TLocation location;
        size_t index = frame.PC - frame.Exec->VMCode.data();
        if (index < frame.Exec->InstrDebugInfo.size() && frame.Exec->InstrDebugInfo[index]) {
            auto& debugInfo = frame.Exec->InstrDebugInfo[index];
            location = debugInfo.Location;
            std::cout << "Source location: " << debugInfo.Location.ToString() << std::endl;
            if (debugInfo.ScopeId != -1) {
                std::cout << "Scope ID: " << debugInfo.ScopeId << std::endl;
            }

            int scopeId = debugInfo.ScopeId;
            while (scopeId != -1 && scopeId < func->ScopeParents.size()) {
                reachableScopes.insert(scopeId);
                scopeId = func->ScopeParents[scopeId];
            }
        }

        TValuePrinter printer(Module->Types);
        std::cout << "Locals:\n";
        for (size_t i = 0; i < func->LocalDebugInfo.size()
            && i < func->LocalTypes.size()
            && i < frame.Exec->LocalByteOffsets.size(); ++i)
        {
            const auto& debugInfo = func->LocalDebugInfo[i];
            if (debugInfo.Name.empty()) {
                continue;
            }
            if (reachableScopes.find(debugInfo.ScopeId) == reachableScopes.end()) {
                continue;
            }
            if (debugInfo.Location > location) {
                continue;
            }
            int typeId = func->LocalTypes[i];
            std::cout << "  " << debugInfo.Name << " (";
            if (debugInfo.AstType) {
                std::cout << NAst::TypeDiagnosticName(debugInfo.AstType);
            } else {
                Module->Types.Print(std::cout, typeId);
            }
            std::cout << ") = ";
            if (typeId < 0) {
                std::cout << "<unknown>\n";
                continue;
            }
            int offset = frame.Exec->LocalByteOffsets[i];
            size_t size = static_cast<size_t>(Module->Types.SizeInBytes(typeId));
            if (offset < 0 || frame.StackBase > Runtime->Stack.size()
                || static_cast<size_t>(offset) > Runtime->Stack.size() - frame.StackBase
                || size > Runtime->Stack.size() - frame.StackBase - offset)
            {
                std::cout << "<unavailable>\n";
                continue;
            }
            const char* value = size == 0
                ? nullptr
                : Runtime->Stack.data() + frame.StackBase + offset;
            printer.Print(std::cout, value, typeId, debugInfo.AstType);
            std::cout << "\n";
        }
    }

    void PrintCallStack(const NIR::TFrame& frame) {
        if (!Runtime || !Module) {
            return;
        }
        TValuePrinter printer(Module->Types);
        std::cout << "Call stack:\n";
        for (size_t depth = 0; depth < Runtime->CallStack.size(); ++depth) {
            const auto& f = Runtime->CallStack[depth];
            std::cout << std::string((depth + 1) * 2, ' ') << f.Name << "(";
            auto funcIt = FunctionMap.find(std::string(f.Name));
            if (funcIt != FunctionMap.end()) {
                const auto& func = *funcIt->second;
                for (size_t i = 0; i < func.ArgLocals.size(); ++i) {
                    if (i != 0) {
                        std::cout << ", ";
                    }
                    int localIdx = func.ArgLocals[i].Idx;
                    const NIR::TLocalVarDebugInfo* info = localIdx >= 0
                        && static_cast<size_t>(localIdx) < func.LocalDebugInfo.size()
                        ? &func.LocalDebugInfo[localIdx] : nullptr;
                    std::cout << (info && !info->Name.empty() ? info->Name : "#" + std::to_string(i)) << "=";
                    if (localIdx < 0 || static_cast<size_t>(localIdx) >= func.LocalTypes.size()
                        || static_cast<size_t>(localIdx) >= f.Exec->LocalByteOffsets.size())
                    {
                        std::cout << "<unavailable>";
                        continue;
                    }
                    int typeId = func.LocalTypes[localIdx];
                    int offset = f.Exec->LocalByteOffsets[localIdx];
                    if (typeId < 0 || offset < 0) {
                        std::cout << "<unavailable>";
                        continue;
                    }
                    size_t size = static_cast<size_t>(Module->Types.SizeInBytes(typeId));
                    if (f.StackBase > Runtime->Stack.size()
                        || static_cast<size_t>(offset) > Runtime->Stack.size() - f.StackBase
                        || size > Runtime->Stack.size() - f.StackBase - offset)
                    {
                        std::cout << "<unavailable>";
                        continue;
                    }
                    const char* value = size == 0
                        ? nullptr : Runtime->Stack.data() + f.StackBase + offset;
                    printer.Print(std::cout, value, typeId, info ? info->AstType : nullptr);
                }
            }
            std::cout << ")";
            if (auto debugInfo = GetInstrDebugInfo(f, &f == &frame)) {
                std::cout << " at " << debugInfo->Location.ToString();
            }
            std::cout << "\n";
        }
    }

    const NIR::TRuntime* Runtime{nullptr};
    const NIR::TModule* Module{nullptr};

    std::unordered_map<std::string, const NIR::TFunction*> FunctionMap;
    std::unordered_multimap<std::string, const NIR::TFunction*> FunctionNameMap;
    std::unordered_set<const void*> Breakpoints;
    std::unordered_map<int, const void*> LineMap;
};

int main(int argc, char ** argv) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;

    enum class RunnerType { IR, LLVM };
    RunnerType runnerType = RunnerType::IR; // default
    bool printEvalTimeUs = false;
    bool printAst = false;
    bool printTransformedAst = false;
    bool printIr = false;
    bool printLlvm = false;
    bool printAsm = false;
    bool printByteCode = false;
    bool coreInput = false;
    bool useTpde = false;
    int optLevel = 0;
    std::string inputFile; // stdin by default if empty
    std::vector<std::string> modulePaths;
    std::vector<std::string> moduleFiles;
    std::unique_ptr<TInteractiveDebugger> debugger;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--jit")) {
            runnerType = RunnerType::LLVM;
        } else if (!std::strcmp(argv[i], "--tpde")) {
            runnerType = RunnerType::LLVM;
            useTpde = true;
        } else if (!std::strcmp(argv[i], "--time-us")) {
            printEvalTimeUs = true;
        } else if (!std::strcmp(argv[i], "--print-ast")) {
            printAst = true;
        } else if (!std::strcmp(argv[i], "--print-transformed-ast")) {
            printTransformedAst = true;
        } else if (!std::strcmp(argv[i], "--print-ir")) {
            printIr = true;
        } else if (!std::strcmp(argv[i], "--print-llvm")) {
            printLlvm = true;
        } else if (!std::strcmp(argv[i], "--print-asm")) {
            printAsm = true;
        } else if (!std::strcmp(argv[i], "--print-bytecode")) {
            printByteCode = true;
        } else if (!std::strcmp(argv[i], "--core")) {
            coreInput = true;
        } else if (!std::strcmp(argv[i], "--module-path")) {
            if (i + 1 < argc) {
                modulePaths.push_back(argv[++i]);
            } else {
                std::cerr << "--module-path requires a directory argument\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--module")) {
            if (i + 1 < argc) {
                moduleFiles.push_back(argv[++i]);
            } else {
                std::cerr << "--module requires a file argument\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--input-file") || !std::strcmp(argv[i], "-i")) {
            if (i + 1 < argc) {
                inputFile = argv[++i];
            } else {
                std::cerr << "--input-file requires a filename argument\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "-O")) {
            if (i + 1 < argc) {
                optLevel = std::atoi(argv[++i]);
                if (optLevel < 0 || optLevel > 3) {
                    std::cerr << "Optimization level must be between 0 and 3\n";
                    return 1;
                }
            } else {
                std::cerr << "-O requires an argument\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "-O0")) {
            optLevel = 0;
        } else if (!std::strcmp(argv[i], "-O1")) {
            optLevel = 1;
        } else if (!std::strcmp(argv[i], "-O2")) {
            optLevel = 2;
        } else if (!std::strcmp(argv[i], "-O3")) {
            optLevel = 3;
        } else if (!std::strcmp(argv[i], "--debug")) {
            debugger = std::make_unique<TInteractiveDebugger>();
        } else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            std::cout << "qumiri [options]\n"
                         "Options:\n"
                         "  --jit                Enable llvm jit\n"
                         "  --tpde               JIT with the TPDE backend (AArch64 Linux, QUMIR_ENABLE_TPDE)\n"
                         "  --time-us            Print evaluation time in microseconds\n"
                         "  --print-ast          Print AST after parsing\n"
                         "  --print-transformed-ast Print AST after semantic transforms\n"
                         "  --print-ir           Print IR after lowering\n"
                         "  --print-llvm         Print LLVM IR after codegen\n"
                         "  --print-asm          Print target assembly before JIT\n"
                         "  --core               Parse input as core language\n"
                         "  --module-path <dir>  Add a search directory for .oz modules (repeatable)\n"
                         "  --module <file.oz>   Register an explicit .oz module (repeatable)\n"
                         "  --input-file|-i <file>  Input file (default: stdin)\n"
                         "  --debug              Enable interactive debugging\n"
                         "  -O <level>           Optimization level (0-3), default 0\n"
                         "  -O0                  Optimization level 0 (no optimizations)\n"
                         "  -O1                  Optimization level 1 (some optimizations)\n"
                         "  -O2                  Optimization level 2 (more optimizations)\n"
                         "  -O3                  Optimization level 3 (aggressive optimizations)\n"
                         "  --help, -h           Show this help message\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            return 1;
        }
    }

    if (optLevel > 0) {
        // TODO: unsupported
        debugger.reset();
    }

    std::unique_ptr<NCodeGen::ICodegenBackend> backend;
    if (useTpde) {
#ifdef QUMIR_ENABLE_TPDE
        backend = std::make_unique<NCodeGen::TTpdeBackend>();
#else
        std::cerr << "--tpde: qumiri is built without QUMIR_ENABLE_TPDE\n";
        return 1;
#endif
    } else {
        backend = std::make_unique<NCodeGen::TLLVMBackend>(NCodeGen::TLLVMBackendOptions{
            .OptLevel = optLevel,
            .PrintLlvm = printLlvm,
            .PrintAsm = printAsm,
        });
    }

    std::istream* in = &std::cin;
    std::ifstream infile;
    if (!inputFile.empty() && inputFile != "-") {
        infile.open(inputFile);
        if (!infile.is_open()) {
            std::cerr << "Failed to open input file: " << inputFile << "\n";
            return 1;
        }
        in = &infile;
    }
    std::istringstream source(std::string((std::istreambuf_iterator<char>(*in)), std::istreambuf_iterator<char>()));
    SignErrorsIfMarked(source.str(), "Qumir " QUMIR_VERSION_STRING ", https://github.com/resetius/qumir");
    in = &source;

    // The directory of the main source file is searched before explicit paths.
    if (!inputFile.empty() && inputFile != "-") {
        auto dir = std::filesystem::path(inputFile).parent_path();
        modulePaths.insert(modulePaths.begin(), dir.empty() ? "." : dir.string());
    }

    // qumiri is a host: it provides the System runtime as the core prelude.
    std::vector<std::string> corePrelude;
    if (coreInput) {
        corePrelude = {"System"};
    }

    TIRRunner irRunner(
        std::cout,
        std::cin,
        TIRRunnerOptions {
            .PrintAst = printAst,
            .PrintTransformedAst = printTransformedAst,
            .PrintIr = printIr,
            .PrintByteCode = printByteCode,
            .CoreInput = coreInput,
            .OptLevel = optLevel,
            .Prelude = corePrelude,
            .ModuleSearchPaths = modulePaths,
            .ModuleFiles = moduleFiles,
            .Debugger = debugger.get(),
            .SourceFilePath = inputFile == "-"
                ? ""
                : inputFile,
        }
    );

    TLLVMRunner llvmRunner(TLLVMRunnerOptions {
        .PrintAst = printAst,
        .PrintTransformedAst = printTransformedAst,
        .PrintIr = printIr,
        .PrintLlvm = printLlvm,
        .PrintAsm = printAsm,
        .CoreInput = coreInput,
        .OptLevel = optLevel,
        .Prelude = corePrelude,
        .ModuleSearchPaths = modulePaths,
        .ModuleFiles = moduleFiles,
        .SourceFilePath = inputFile == "-"
            ? ""
            : inputFile,
        .EmitDebugInfo = debugger != nullptr,
    }, std::move(backend));

    long long lastEvalUs = 0;
    std::expected<std::optional<std::string>, TError> result;
    if (runnerType == RunnerType::LLVM) {
        auto t0 = std::chrono::steady_clock::now();
        result = llvmRunner.Run(*in);
        auto t1 = std::chrono::steady_clock::now();
        lastEvalUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    } else {
        auto t0 = std::chrono::steady_clock::now();
        result = irRunner.Run(*in);
        auto t1 = std::chrono::steady_clock::now();
        lastEvalUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    }

    std::optional<std::string> value;
    if (!result) {
        auto errStr = result.error().ToString();
        std::cerr << errStr;
        return 1;
    }

    value = std::move(result.value());

    PrintResultIR(value);
    if (printEvalTimeUs) {
        std::cout << lastEvalUs << " us" << std::endl;
    }

    return 0;
}
