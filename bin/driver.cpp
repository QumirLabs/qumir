#include <cstring>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <cstdlib>

#include <qumir/parser/parser.h>
#include <qumir/semantics/kumir/pipeline.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/ir/builder.h>
#include <qumir/ir/passes/transforms/pipeline.h>
#include <qumir/semantics/name_resolution/name_resolver.h>
#include <qumir/semantics/transform/transform.h>
#include <qumir/frontend/compose.h>
#include <qumir/frontend/source_module_loader.h>
#include <qumir/codegen/llvm/llvm_codegen.h>
#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/modules/builtins/builtins.h>
#include <qumir/modules/system/system.h>
#include <qumir/modules/turtle/turtle.h>
#include <qumir/modules/robot/robot.h>
#include <qumir/modules/drawer/drawer.h>
#include <qumir/modules/painter/painter.h>
#include <qumir/modules/colors/colors.h>
#include <qumir/modules/keyboard/keyboard.h>
#include <sstream>
#include <string>
#include <filesystem>

#include <sys/wait.h>
#include <thread>

#ifdef _WIN32
static constexpr std::string A_OUT = "a.exe";
#else
static constexpr std::string A_OUT = "a.out";
#endif

using namespace NQumir;

namespace {

struct TModuleConfig {
    std::vector<std::string> Paths;
    std::vector<std::string> Files;
    bool IncludeDebugRuntime = false;
};

std::expected<NAst::TExprPtr, TError> ParseInput(
    std::istream& in, NSemantics::TNameResolver& r, bool coreInput,
    const TModuleConfig& modules = {})
{
    NFrontend::TSourceModuleLoader loader;
    for (const auto& dir : modules.Paths) {
        loader.AddSearchPath(dir);
    }
    for (const auto& file : modules.Files) {
        if (auto reg = loader.RegisterSourceModule(file); !reg) {
            return std::unexpected(reg.error());
        }
    }

    std::expected<NAst::TExprPtr, TError> parsed;
    std::vector<NAst::TPragma> mainPragmas;
    if (coreInput) {
        NAst::NCore::TTokenStream ts(in);
        NAst::NCore::TParser p;
        parsed = p.Parse(ts);
        if (parsed) {
            mainPragmas = std::move(p.Pragmas);
        }
    } else {
        NAst::TTokenStream ts(in);
        NAst::TParser p;
        parsed = p.parse(ts, &r, &loader);
        if (parsed) {
            mainPragmas = ts.GetContext()->GetPragmas();
        }
    }
    if (!parsed) {
        return parsed;
    }

    if (modules.IncludeDebugRuntime) {
        auto runtime = loader.Load("debug");
        if (!runtime) {
            return std::unexpected(runtime.error());
        }
        for (auto& stmt : NAst::TMaybeNode<NAst::TBlockExpr>((*runtime)->Ast).Cast()->Stmts) {
            if (auto function = NAst::TMaybeNode<NAst::TFunDecl>(stmt)) {
                function.Cast()->Used = true;
            }
        }
    }

    auto composed = NFrontend::LoadAndCompose(loader, *parsed, mainPragmas);
    if (!composed) {
        return std::unexpected(composed.error());
    }
    r.ApplyPragmas(composed->Pragmas);
    return composed->Ast;
}

NTransform::TPipelineOptions PipelineOptions(bool coreInput) {
    NTransform::TPipelineExtensions extensions;
    if (coreInput) {
        extensions.AfterTypeAnnotation.push_back(
            NTransform::CoroutineAnnotationTransform);
    } else {
        extensions = NSemantics::NKumir::PipelineExtensions();
    }
    return NTransform::TPipelineOptions{.Extensions = std::move(extensions)};
}

// Registers the standard module set into the resolver and keeps the instances
// alive in the returned vector. qumirc is a host: it provides the System
// runtime as the prelude. For the Kumir frontend it also registers the legacy
// module aliases (e.g. "Файлы" -> "System").
std::vector<std::shared_ptr<NRegistry::IModule>> SetupModules(
    NSemantics::TNameResolver& r, bool coreInput) {
    std::vector<std::shared_ptr<NRegistry::IModule>> modules = {
        std::make_shared<NRegistry::BuiltinsModule>(),
        std::make_shared<NRegistry::SystemModule>(),
        std::make_shared<NRegistry::TurtleModule>(),
        std::make_shared<NRegistry::RobotModule>(),
        std::make_shared<NRegistry::DrawerModule>(),
        std::make_shared<NRegistry::PainterModule>(),
        std::make_shared<NRegistry::ColorsModule>(),
        std::make_shared<NRegistry::KeyboardModule>(),
    };
    for (const auto& mod : modules) {
        r.RegisterModule(mod.get());
    }
    // Like the runners: Builtins is always imported, independent of the frontend.
    (void)r.ImportModule(NRegistry::BuiltinsModule::ModuleName);
    (void)r.ImportModule("System");
    if (!coreInput) {
        for (const auto& [alias, canonical] : NSemantics::NKumir::ModuleAliases()) {
            r.RegisterModuleAlias(alias, canonical);
        }
    }
    return modules;
}

void ConfigureWasmRuntime(NIR::TModule& module) {
    module.AsyncMode = "coroutine";
}

std::shared_ptr<std::istream> OpenInputFile(const std::string& filename) {
    std::ifstream file;
    if (filename != "-") {
        file.open(filename);
        if (!file.is_open()) {
            return nullptr;
        }
    }
    std::istream& in = filename == "-" ? std::cin : file;
    std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    SignErrorsIfMarked(source, "Qumir " QUMIR_VERSION_STRING ", https://github.com/resetius/qumir");
    return std::make_shared<std::istringstream>(std::move(source));
}

std::shared_ptr<std::ostream> OpenOutputFile(const std::string& filename, bool randomAccess = false) {
    if (filename == "-") {
        if (!randomAccess) {
            return std::make_shared<std::ostream>(std::cout.rdbuf());
        } else {
            std::shared_ptr<std::ostream> stream;
            stream.reset(new std::ostringstream(), [](std::ostream* os) {
                auto sstr = static_cast<std::ostringstream*>(os);
                std::cout << sstr->str();
                delete sstr;
            });
            return stream;
        }
    } else {
        auto fileStream = std::make_shared<std::ofstream>(filename, std::ios::binary);
        if (!fileStream->is_open()) {
            return nullptr;
        }
        return fileStream;
    }
}

int GenerateAst(const std::string& inputFile, const std::string& outputFile, bool transformed, bool coreInput, bool verbose, const TModuleConfig& moduleConfig) {
    if (verbose) {
        std::cerr << "Generating " << (transformed ? "transformed " : "") << "AST from " << inputFile << " to " << outputFile << "\n";
    }

    auto in = OpenInputFile(inputFile);
    if (!in) {
        std::cerr << "Failed to open input file: " << inputFile << "\n";
        return 1;
    }

    NSemantics::TNameResolver r;
    auto modules = SetupModules(r, coreInput);

    auto expected = ParseInput(*in, r, coreInput, moduleConfig);
    if (!expected.has_value()) {
        std::cerr << expected.error().ToString() << std::endl;
        return 1;
    }
    auto ast = std::move(expected.value());
    if (transformed) {
        auto error = NTransform::Pipeline(ast, r, PipelineOptions(coreInput));
        if (!error) {
            std::cerr << error.error().ToString() << "\n";
            return 1;
        }
    }

    auto out = OpenOutputFile(outputFile);
    if (!out) {
        std::cerr << "Failed to open output file: " << outputFile << "\n";
        return 1;
    }

    *out << ast;
    return 0;
}

int GenerateIr(const std::string& inputFile, const std::string& outputFile, int optLevel, bool coreInput, bool verbose, const TModuleConfig& moduleConfig, NIR::TDebugOptions debugOptions, int targetBits) {
    if (verbose) {
        std::cerr << "Generating IR from " << inputFile << " to " << outputFile << "\n";
    }

    auto in = OpenInputFile(inputFile);
    if (!in) {
        std::cerr << "Failed to open input file: " << inputFile << "\n";
        return 1;
    }

    NSemantics::TNameResolver r;
    auto modules = SetupModules(r, coreInput);

    auto expected = ParseInput(*in, r, coreInput, moduleConfig);
    if (!expected.has_value()) {
        std::cerr << expected.error().ToString() << std::endl;
        return 1;
    }
    auto ast = std::move(expected.value());

    auto error = NTransform::Pipeline(ast, r, PipelineOptions(coreInput));
    if (!error) {
        std::cerr << error.error().ToString() << "\n";
        return 1;
    }

    NIR::TModule module{
        .SourceFilePath = inputFile == "-"
            ? ""
            : inputFile,
    };
    if (targetBits != 0) {
        module.Types.SetPointerSize(targetBits / 8);
        ConfigureWasmRuntime(module);
    }
    NIR::TBuilder builder(module);

    NIR::TAstLowerer lowerer(module, builder, r, debugOptions);
    auto lowerResult = lowerer.LowerTop(ast);
    if (!lowerResult.has_value()) {
        std::cerr << lowerResult.error().ToString() << "\n";
        return 1;
    }
    if (optLevel > 0) {
        NIR::NPasses::Pipeline(module);
    }

    auto out = OpenOutputFile(outputFile);
    if (!out) {
        std::cerr << "Failed to open output file: " << outputFile << "\n";
        return 1;
    }

    module.Print(*out);
    return 0;
}

int GenerateLlvm(const std::string& inputFile, const std::string& outputFile, int optLevel, bool coreInput, bool verbose, const TModuleConfig& moduleConfig, NIR::TDebugOptions debugOptions, int targetBits) {
    if (verbose) {
        std::cerr << "Generating LLVM IR from " << inputFile << " to " << outputFile << "\n";
    }

    auto in = OpenInputFile(inputFile);
    if (!in) {
        std::cerr << "Failed to open input file: " << inputFile << "\n";
        return 1;
    }

    NSemantics::TNameResolver r;
    auto modules = SetupModules(r, coreInput);

    auto expected = ParseInput(*in, r, coreInput, moduleConfig);
    if (!expected.has_value()) {
        std::cerr << expected.error().ToString() << std::endl;
        return 1;
    }
    auto ast = std::move(expected.value());

    auto error = NTransform::Pipeline(ast, r, PipelineOptions(coreInput));
    if (!error) {
        std::cerr << error.error().ToString() << "\n";
        return 1;
    }

    NIR::TModule module{
        .SourceFilePath = inputFile == "-"
            ? ""
            : inputFile,
    };
    if (targetBits != 0) {
        module.Types.SetPointerSize(targetBits / 8);
        ConfigureWasmRuntime(module);
    }
    NIR::TBuilder builder(module);

    NIR::TAstLowerer lowerer(module, builder, r, debugOptions);
    auto lowerResult = lowerer.LowerTop(ast);
    if (!lowerResult.has_value()) {
        std::cerr << lowerResult.error().ToString() << "\n";
        return 1;
    }

    NCodeGen::TLLVMCodeGenOptions cgOptions;
    if (targetBits != 0) {
        cgOptions.TargetTriple = targetBits == 32
            ? "wasm32-unknown-unknown"
            : "wasm64-unknown-unknown";
    }
    NCodeGen::TLLVMCodeGen cg(cgOptions);
    auto artifacts = cg.Emit(module, optLevel);
    if (!artifacts) {
        std::cerr << "Codegen error " << "\n";
        return 1;
    }

    auto out = OpenOutputFile(outputFile);
    if (!out) {
        std::cerr << "Failed to open output file: " << outputFile << "\n";
        return 1;
    }

    artifacts->PrintModule(*out);
    return 0;
}

#if 0
struct TSpawner {
    TSpawner(std::string command, std::vector<const char*> args)
        : Command(std::move(command))
        , Args(std::move(args))
    {
    }

    void Run() {
        std::cerr << "Spawning process: " << Command;
        for (const auto& arg : Args) {
            std::cerr << " " << arg;
        }
        std::cerr << "\n";

        if (pipe(Pipefd) == -1) {
            throw std::runtime_error("pipe() failed");
        }

        Pid = fork();
        if (Pid == -1) {
            throw std::runtime_error("fork() failed");
        }

        if (Pid == 0) {
            // Child process
            close(Pipefd[0]); // Close read end
            dup2(Pipefd[1], STDOUT_FILENO); // Redirect stdout to pipe
            close(Pipefd[1]); // Close original write end

            std::vector<char*> execArgs;
            execArgs.push_back(const_cast<char*>(Command.c_str()));
            for (const auto& arg : Args) {
                execArgs.push_back(const_cast<char*>(arg));
            }
            execArgs.push_back(nullptr);

            execvp(Command.c_str(), execArgs.data());
            // If execvp returns, an error occurred
            std::cerr << "execvp() failed\n";
            exit(1);
        } else {
            // Parent process
            close(Pipefd[1]); // Close write end
        }
    }

    void Wait() {
        int status;
        waitpid(Pid, &status, 0);
        if (WIFEXITED(status)) {
            int exitStatus = WEXITSTATUS(status);
            if (exitStatus != 0) {
                throw std::runtime_error("Child process exited with status " + std::to_string(exitStatus));
            }
        } else {
            throw std::runtime_error("Child process did not terminate normally");
        }
    }

    int GetReadFd() const {
        return Pipefd[0];
    }

    int GetWriteFd() const {
        return Pipefd[1];
    }

    std::string Command;
    std::vector<const char*> Args;
    int Pipefd[2];
    int Pid;
};

void GenerateObjFromAsm(const std::string& asmCode, std::ostream& objOut) {
    TSpawner spawner("/usr/bin/as", {"as", "-o", "/dev/stdout", "-", nullptr});

    spawner.Run();

    auto reader = std::jthread([&]() {
        char buffer[4096];
        ssize_t bytesRead;
        while ((bytesRead = read(spawner.GetReadFd(), buffer, sizeof(buffer))) > 0) {
            objOut.write(buffer, bytesRead);
        }
        close(spawner.GetReadFd());
    });

    auto writer = std::jthread([&]() {
        ssize_t totalWritten = 0;
        ssize_t toWrite = asmCode.size();
        const char* data = asmCode.data();
        while (totalWritten < toWrite) {
            ssize_t written = write(spawner.GetWriteFd(), data + totalWritten, toWrite - totalWritten);
            if (written == -1) {
                if (errno == EINTR) continue; // Retry on interrupt
                std::cerr << "write() failed\n";
                break;
            }
            totalWritten += written;
        }
        close(spawner.GetWriteFd());
    });

    reader.join();
    writer.join();

    spawner.Wait();
}
#endif

int Generate(const std::string& inputFile, const std::string& outputFile, bool compileOnly, bool generateAsm, int optLevel, int wasmBits, bool coreInput, bool verbose, const TModuleConfig& moduleConfig, NIR::TDebugOptions debugOptions) {
    if (verbose) {
        std::cerr << "Compiling " << inputFile << " to " << outputFile << "\n";
    }

    auto in = OpenInputFile(inputFile);
    if (!in) {
        std::cerr << "Failed to open input file: " << inputFile << "\n";
        return 1;
    }

    NSemantics::TNameResolver r;
    auto modules = SetupModules(r, coreInput);

    auto expected = ParseInput(*in, r, coreInput, moduleConfig);
    if (!expected.has_value()) {
        std::cerr << expected.error().ToString() << std::endl;
        return 1;
    }
    auto ast = std::move(expected.value());

    auto error = NTransform::Pipeline(ast, r, PipelineOptions(coreInput));
    if (!error) {
        std::cerr << error.error().ToString() << "\n";
        return 1;
    }

    NIR::TModule module{
        .SourceFilePath = inputFile == "-"
            ? ""
            : inputFile,
    };
    if (wasmBits != 0) {
        module.Types.SetPointerSize(wasmBits / 8);
        ConfigureWasmRuntime(module);
    }
    NIR::TBuilder builder(module);

    NIR::TAstLowerer lowerer(module, builder, r, debugOptions);
    auto lowerResult = lowerer.LowerTop(ast);
    if (!lowerResult.has_value()) {
        std::cerr << lowerResult.error().ToString() << "\n";
        return 1;
    }
    const bool hasCoroutines = std::any_of(module.Functions.begin(), module.Functions.end(),
        [](const NIR::TFunction& function) { return function.IsCoroutine; });
    // coro-split requires at least O1; bump automatically when coroutines are present
    const int effectiveOptLevel = (hasCoroutines && optLevel == 0) ? 1 : optLevel;

    if (effectiveOptLevel > 0 && !debugOptions.EmitDebugPoints) {
        NIR::NPasses::Pipeline(module);
    }

    NCodeGen::TLLVMCodeGenOptions cgOpts;
    if (wasmBits == 32) {
        cgOpts.TargetTriple = "wasm32-unknown-unknown";
    } else if (wasmBits == 64) {
        cgOpts.TargetTriple = "wasm64-unknown-unknown";
    }
    NCodeGen::TLLVMCodeGen cg(cgOpts);
    auto artifacts = cg.Emit(module, effectiveOptLevel);
    if (!artifacts) {
        std::cerr << "Codegen error " << "\n";
        return 1;
    }

    // Special path: when targeting wasm and linking, produce a final .wasm via wasm-ld
    if (wasmBits != 0 && !generateAsm && !compileOnly) {
        // TODO: move to ->Generate
        const std::string objTmp = outputFile + ".tmp.o";
        {
            std::ofstream objFile(objTmp, std::ios::binary);
            if (!objFile) {
                std::cerr << "Failed to open temp object file: " << objTmp << "\n";
                return 1;
            }
            artifacts->Generate(objFile, /*asm*/false, /*obj*/true);
        }
        std::string cmd = std::string("wasm-ld --no-entry --export-all --allow-undefined ")
            + (wasmBits == 64 ? "-mwasm64 " : "") + "-o " + outputFile + " " + objTmp;
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::cerr << "wasm-ld failed with code " << rc << "\n";
            return 1;
        }
        std::remove(objTmp.c_str());
        return 0;
    }

    auto outFile = OpenOutputFile(outputFile, /*randomAccess*/ !generateAsm);
    if (!outFile) {
        std::cerr << "Failed to open output file: " << outputFile << "\n";
        return 1;
    }
    artifacts->Generate(*outFile, generateAsm, compileOnly && !generateAsm);
    return 0;
}

std::string OutputFilename(const std::string& inputFile, const std::string& newExt) {
    auto dotPos = inputFile.rfind('.');
    if (dotPos != std::string::npos) {
        return inputFile.substr(0, dotPos) + newExt;
    } else {
        return inputFile + newExt;
    }
}

} // namespace {

// Compiler driver (stub)
int main(int argc, char** argv) {
    NCodeGen::TLLVMInitializer llvmInit;

    bool compileOnly = false;
    std::string outputFile;
    std::string inputFile;
    bool generateAst = false;
    bool generateTransformedAst = false;
    bool generateIr = false;
    bool generateLlvm = false;
    bool generateAsm = false;
    int optLevel = 0;
    int wasmBits = 0; // 0 = native, 32, 64
    bool coreInput = false;
    bool verbose = false;
    NIR::TDebugOptions debugOptions;
    TModuleConfig moduleConfig;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-c")) {
            compileOnly = true;
        } else if (!std::strcmp(argv[i], "-g")) {
            debugOptions.EmitDebugInfo = true;
        } else if (!std::strcmp(argv[i], "--debug-points")) {
            debugOptions.EmitDebugPoints = true;
        } else if (!std::strcmp(argv[i], "-o")) {
            if (i + 1 < argc) {
                outputFile = argv[++i];
            } else {
                std::cerr << "-o requires an argument\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            std::cout << "qumirc [options] <input file>\n"
                         "Options:\n"
                         "  -c            Compile only, do not link\n"
                         "  -g            Collect debug info (O0 only)\n"
                         "  --debug-points Emit browser debug points (O0 only)\n"
                         "  -o <file>     Write output to <file> (default: " << (compileOnly ? "N/A" : A_OUT) << ")\n"
                         "  --ast         Generate parsed AST only (no IR, no codegen)\n"
                         "  --transformed-ast Generate transformed AST only (no IR, no codegen)\n"
                         "  --ir          Generate IR only (no codegen)\n"
                         "  --llvm        Generate LLVM IR only (no object code)\n"
                         "  --wasm        Target WebAssembly, 32-bit (alias for --wasm32)\n"
                         "  --wasm32      Target wasm32-unknown-unknown\n"
                         "  --wasm64      Target wasm64-unknown-unknown\n"
                         "  --core        Parse input as core language\n"
                         "  --module-path <dir>  Add a search directory for .oz modules (repeatable)\n"
                         "  --module <file.oz>   Register an explicit .oz module (repeatable)\n"
                         "  -S            Generate assembly only (no linking), implies -c\n"
                         "  -O <level>    Optimization level (0-3), default 0\n"
                         "  -O0           Optimization level 0 (no optimizations)\n"
                         "  -O1           Optimization level 1\n"
                         "  -O2           Optimization level 2\n"
                         "  -O3           Optimization level 3\n"
                         "  --verbose     Enable verbose output\n"
                         "  --version, -v Show version information\n"
                         "  --help, -h    Show this help message\n";
            return 0;
        } else if (!std::strcmp(argv[i], "--version") || !std::strcmp(argv[i], "-v")) {
            std::cout << QUMIR_VERSION_STRING << "\n";
            return 0;
        } else if (!std::strcmp(argv[i], "--ast")) {
            generateAst = true;
        } else if (!std::strcmp(argv[i], "--transformed-ast")) {
            generateTransformedAst = true;
        } else if (!std::strcmp(argv[i], "--ir")) {
            generateIr = true;
        } else if (!std::strcmp(argv[i], "--llvm")) {
            generateLlvm = true;
        } else if (!std::strcmp(argv[i], "--wasm") || !std::strcmp(argv[i], "--wasm32")) {
            wasmBits = 32;
        } else if (!std::strcmp(argv[i], "--wasm64")) {
            wasmBits = 64;
        } else if (!std::strcmp(argv[i], "-S")) {
            generateAsm = true;
            compileOnly = true;
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
        } else if (!std::strcmp(argv[i], "--core")) {
            coreInput = true;
        } else if (!std::strcmp(argv[i], "--module-path")) {
            if (i + 1 < argc) {
                moduleConfig.Paths.push_back(argv[++i]);
            } else {
                std::cerr << "--module-path requires a directory argument\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--module")) {
            if (i + 1 < argc) {
                moduleConfig.Files.push_back(argv[++i]);
            } else {
                std::cerr << "--module requires a file argument\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--verbose")) {
            verbose = true;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            return 1;
        } else {
            inputFile = argv[i];
        }
    }
    if (inputFile.empty()) {
        std::cerr << "No input file\n";
        return 1;
    }

    debugOptions.EmitDebugInfo = debugOptions.EmitDebugInfo && optLevel == 0;
    if (debugOptions.EmitDebugPoints && (optLevel != 0 || (!generateIr && !generateLlvm && wasmBits != 32))) {
        std::cerr << "--debug-points requires O0 and wasm32, --ir or --llvm\n";
        return 1;
    }

    // The directory of the main source file is searched before explicit paths.
    {
        auto dir = std::filesystem::path(inputFile).parent_path();
        moduleConfig.Paths.insert(moduleConfig.Paths.begin(), dir.empty() ? "." : dir.string());
    }

    moduleConfig.IncludeDebugRuntime = debugOptions.EmitDebugPoints;
    if (generateAst || generateTransformedAst) {
        if (outputFile.empty()) {
            outputFile = OutputFilename(inputFile, ".ast");
        }
        return GenerateAst(inputFile, outputFile, generateTransformedAst, coreInput, verbose, moduleConfig);
    }

    if (generateIr) {
        if (outputFile.empty()) {
            outputFile = OutputFilename(inputFile, ".ir");
        }
        return GenerateIr(inputFile, outputFile, optLevel, coreInput, verbose, moduleConfig, debugOptions, wasmBits);
    }

    if (generateLlvm) {
        if (outputFile.empty()) {
            outputFile = OutputFilename(inputFile, ".ll");
        }
        return GenerateLlvm(inputFile, outputFile, optLevel, coreInput, verbose, moduleConfig, debugOptions, wasmBits);
    }

    if (!compileOnly && outputFile.empty()) {
        outputFile = wasmBits != 0 ? OutputFilename(inputFile, ".wasm") : A_OUT;
    }

    std::string finalOutput = outputFile;
    if (finalOutput.empty()) {
        finalOutput = compileOnly
            ? (generateAsm
                ? OutputFilename(inputFile, ".s")
                : OutputFilename(inputFile, ".o"))
            : outputFile;
    }

    return Generate(inputFile, finalOutput, compileOnly, generateAsm, optLevel, wasmBits, coreInput, verbose, moduleConfig, debugOptions);
}
