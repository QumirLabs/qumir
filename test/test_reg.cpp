#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>
#include <algorithm>

#include <qumir/parser/lexer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/parser.h>
#include <qumir/semantics/name_resolution/name_resolver.h>
#include <qumir/semantics/kumir/pipeline.h>
#include <qumir/semantics/type_annotation/type_annotation.h>
#include <qumir/semantics/transform/transform.h>
#include <qumir/modules/system/system.h>
#include <qumir/modules/builtins/builtins.h>
#include <qumir/modules/turtle/turtle.h>
#include <qumir/modules/robot/robot.h>
#include <qumir/modules/drawer/drawer.h>
#include <qumir/modules/painter/painter.h>
#include <qumir/modules/colors/colors.h>
#include <qumir/modules/keyboard/keyboard.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/ir/builder.h>

#include <qumir/runtime/runtime.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>
#include <qumir/runner/runner_ir.h>
#include <qumir/frontend/compose.h>
#include <qumir/frontend/source_module_loader.h>

using namespace NQumir;
namespace fs = std::filesystem;

// `extern` scenario symbols, resolved by the VM via dlsym (ENABLE_EXPORTS).
extern "C" {

struct TQumirTwoChar {
    char a;
    char b;
};

int64_t qumir_two_char_sum(TQumirTwoChar p) {
    return p.a + p.b;
}

TQumirTwoChar qumir_two_char_swap(TQumirTwoChar p) {
    return TQumirTwoChar{p.b, p.a};
}

// One struct per ABI class to exercise every coercion path at the LLVM->C
// boundary, as an argument (reduce to scalar) and as a return value (rebuild).
struct TQumirSse {
    double a;
};

struct TQumirSseSse {
    double a;
    double b;
};

struct TQumirIntInt {
    int64_t a;
    int64_t b;
};

struct TQumirIntSse {
    int64_t a;
    double b;
};

struct TQumirSseInt {
    double a;
    int64_t b;
};

struct TQumirWide {
    int64_t a;
    int64_t b;
    int64_t c;
};

double qumir_sse_scale(TQumirSse p) {
    return p.a * 10;
}

TQumirSse qumir_sse_make(double a) {
    return TQumirSse{a};
}

double qumir_ss_combine(TQumirSseSse p) {
    return p.a + p.b * 100;
}

TQumirSseSse qumir_ss_swap(TQumirSseSse p) {
    return TQumirSseSse{p.b, p.a};
}

int64_t qumir_ii_combine(TQumirIntInt p) {
    return p.a + p.b * 100;
}

TQumirIntInt qumir_ii_swap(TQumirIntInt p) {
    return TQumirIntInt{p.b, p.a};
}

double qumir_is_combine(TQumirIntSse p) {
    return p.a + p.b * 100;
}

TQumirIntSse qumir_is_make(int64_t a, double b) {
    return TQumirIntSse{a, b};
}

double qumir_si_combine(TQumirSseInt p) {
    return p.a + p.b * 100;
}

TQumirSseInt qumir_si_make(double a, int64_t b) {
    return TQumirSseInt{a, b};
}

int64_t qumir_wide_sum(TQumirWide p) {
    return p.a + p.b * 100 + p.c * 10000;
}

TQumirWide qumir_wide_make(int64_t a, int64_t b, int64_t c) {
    return TQumirWide{a, b, c};
}

// Appends v as one decimal digit, so the output shows each value in its place.
static int64_t QumirDigits(std::initializer_list<double> values) {
    int64_t acc = 0;
    for (double v : values) {
        acc = acc * 10 + int64_t(v);
    }
    return acc;
}

// The leading scalars take every argument register (8 GP and 8 FP on AArch64,
// 6 and 8 on x86-64), so the structs and scalars after them go to the stack.
int64_t qumir_stack_mix(
    int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6, int64_t a7,
    double d0, double d1, double d2, double d3, double d4, double d5, double d6, double d7,
    TQumirIntInt ii, TQumirSseSse ss, TQumirIntSse is, TQumirWide wide, int64_t t, double u)
{
    int64_t lead = a0 + a1 * 2 + a2 * 3 + a3 * 4 + a4 * 5 + a5 * 6 + a6 * 7 + a7 * 8;
    lead += int64_t(d0 + d1 * 2 + d2 * 3 + d3 * 4 + d4 * 5 + d5 * 6 + d6 * 7 + d7 * 8);
    int64_t tail = QumirDigits({
        double(ii.a), double(ii.b), ss.a, ss.b, double(is.a), is.b,
        double(wide.a), double(wide.b), double(wide.c), double(t), u,
    });
    return tail * 1000 + lead;
}

// The structs take the argument registers and push the scalar to the stack.
int64_t qumir_regs_ii(TQumirIntInt p, TQumirIntInt q, TQumirIntInt r, TQumirIntInt s, int64_t t) {
    return QumirDigits({
        double(p.a), double(p.b), double(q.a), double(q.b),
        double(r.a), double(r.b), double(s.a), double(s.b), double(t),
    });
}

// A result in memory with every argument register taken.
TQumirWide qumir_stack_wide(
    int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6, int64_t a7,
    TQumirWide p)
{
    return TQumirWide{p.a + a0, p.b + a7, p.c * 10 + a3};
}

} // extern "C"

namespace {

fs::path RootDir = "regtest";
fs::path CasesDir = RootDir / "cases";
fs::path GoldensDir = RootDir / "goldens";

bool updateGoldens = false;
bool printOutput = false;

struct TModuleSet {
    NRegistry::SystemModule system;
    NRegistry::BuiltinsModule builtins;
    NRegistry::TurtleModule turtle;
    NRegistry::RobotModule robot;
    NRegistry::DrawerModule drawer;
    NRegistry::PainterModule painter;
    NRegistry::ColorsModule colors;
    NRegistry::KeyboardModule keyboard;
};

void RegisterRuntimeModules(NSemantics::TNameResolver& resolver, TModuleSet& mods) {
    resolver.RegisterModule(&mods.system);
    resolver.RegisterModule(&mods.builtins);
    resolver.RegisterModule(&mods.turtle);
    resolver.RegisterModule(&mods.robot);
    resolver.RegisterModule(&mods.drawer);
    resolver.RegisterModule(&mods.painter);
    resolver.RegisterModule(&mods.colors);
    resolver.RegisterModule(&mods.keyboard);
    resolver.ImportModule(mods.system.Name());
    // Always available, independent of prelude (qumir/modules/builtins).
    resolver.ImportModule(mods.builtins.Name());
}

std::string ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

void WriteAll(const fs::path& p, const std::string& s) {
    std::cerr << "Updating golden file: " << p << "\n";
    std::cerr << "Written " << s.size() << " bytes\n";
    std::cerr << "Data:\n" << s << "\n";
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary); out << s;
}

struct ProgCase { fs::path base; }; // base without extension

std::vector<ProgCase> Collect(const fs::path& root, std::string_view extension = ".kum") {
    std::vector<ProgCase> v;
    if (!fs::exists(root)) {
        return v;
    }

    auto casePath = [&](const fs::path& p) {
        auto rel = fs::relative(p, root);
        return rel.replace_extension();
    };

    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (e.is_regular_file() && e.path().extension() == extension) {
            auto path = e.path();
            // Library modules imported via `use` are compiled as part of a main
            // program, not as standalone cases.
            std::ifstream in(path);
            std::string firstLine;
            std::getline(in, firstLine);
            if (firstLine.find("module-fixture") != std::string::npos) {
                continue;
            }
            v.push_back({ casePath(path) });
        }
    }
    std::sort(v.begin(), v.end(),
        [](auto& a, auto& b){ return a.base.string() < b.base.string(); });
    return v;
}

std::string NameFromPath(const fs::path& p) {
    std::string s = p.string();
    for (auto& c : s) if (c == '/' || c == '\\' || c == '.') c = '_';
    return s;
}

// TODO: move to utils
std::string BuildAst(NAst::TTokenStream& ts, const fs::path& moduleDir = {}) {
    NAst::TParser p;
    TModuleSet mods;
    NSemantics::TNameResolver nr;
    RegisterRuntimeModules(nr, mods);

    NFrontend::TSourceModuleLoader loader;
    if (!moduleDir.empty()) {
        loader.AddSearchPath(moduleDir.string());
    }
    auto parsed = p.parse(ts, &nr, &loader);
    if (!parsed) {
        return parsed.error().ToString() + "\n";
    }
    auto mainPragmas = ts.GetContext()->GetPragmas();
    nr.ApplyPragmas(mainPragmas);

    auto expr = std::move(parsed.value());
    auto composed = NFrontend::LoadAndCompose(loader, expr, mainPragmas);
    if (!composed) {
        return composed.error().ToString() + "\n";
    }
    expr = std::move(composed->Ast);
    nr.ApplyPragmas(composed->Pragmas);

    auto error = NTransform::Pipeline(
        expr,
        nr,
        {.Extensions = NSemantics::NKumir::PipelineExtensions()});
    if (!error) {
        return error.error().ToString() + "\n";
    }

    std::ostringstream out;
    out << expr;
    return out.str();
}

std::string BuildCoreSource(NAst::TTokenStream& ts, const fs::path& moduleDir = {}) {
    NAst::TParser p;
    TModuleSet mods;
    NSemantics::TNameResolver nr;
    RegisterRuntimeModules(nr, mods);

    NFrontend::TSourceModuleLoader loader;
    if (!moduleDir.empty()) {
        loader.AddSearchPath(moduleDir.string());
    }
    auto parsed = p.parse(ts, &nr, &loader);
    if (!parsed) {
        return parsed.error().ToString() + "\n";
    }
    auto mainPragmas = ts.GetContext()->GetPragmas();
    nr.ApplyPragmas(mainPragmas);

    auto expr = std::move(parsed.value());
    auto composed = NFrontend::LoadAndCompose(loader, expr, mainPragmas);
    if (!composed) {
        return composed.error().ToString() + "\n";
    }
    expr = std::move(composed->Ast);
    nr.ApplyPragmas(composed->Pragmas);

    auto error = NTransform::Pipeline(
        expr,
        nr,
        {.Extensions = NSemantics::NKumir::PipelineExtensions()});
    if (!error) {
        return error.error().ToString() + "\n";
    }

    return NAst::NCore::PrintAst(expr, {
        .TypeMode = NAst::NCore::ETypePrintMode::All,
        .HideResolvedOrigins = false,
    });
}

std::string BuildIR(std::istream& input, bool coreInput = false, const fs::path& moduleDir = {}) {
    TModuleSet mods;
    NSemantics::TNameResolver resolver;
    RegisterRuntimeModules(resolver, mods);

    std::expected<NAst::TExprPtr, TError> parsed;
    if (coreInput) {
        NAst::NCore::TTokenStream ts(input);
        NAst::NCore::TParser p;
        parsed = p.Parse(ts);
        if (parsed) {
            std::vector<std::string> modulePaths;
            if (!moduleDir.empty()) {
                modulePaths.push_back(moduleDir.string());
            }
            auto composed = NFrontend::LoadAndCompose(*parsed, p.Pragmas, modulePaths);
            if (!composed) {
                return composed.error().ToString() + "\n";
            }
            parsed = composed->Ast;
            resolver.ApplyPragmas(composed->Pragmas);
        }
    } else {
        NAst::TTokenStream ts(input);
        NAst::TParser p;
        NFrontend::TSourceModuleLoader loader;
        if (!moduleDir.empty()) {
            loader.AddSearchPath(moduleDir.string());
        }
        parsed = p.parse(ts, &resolver, &loader);
        if (parsed) {
            auto mainPragmas = ts.GetContext()->GetPragmas();
            resolver.ApplyPragmas(mainPragmas);
            auto composed = NFrontend::LoadAndCompose(loader, *parsed, mainPragmas);
            if (!composed) {
                return composed.error().ToString() + "\n";
            }
            parsed = composed->Ast;
            resolver.ApplyPragmas(composed->Pragmas);
        }
    }
    if (!parsed) {
        return parsed.error().ToString() + "\n";
    }

    auto expr = parsed.value();
    if (coreInput) {
        auto scope = resolver.GetOrCreateRootScope();
        // scope->AllowsRedeclare = true;
        scope->RootLevel = false;
        if (auto resolveError = resolver.Resolve(expr)) {
            return resolveError->ToString() + "\n";
        }
    }
    NTransform::TPipelineExtensions extensions;
    if (coreInput) {
        extensions.AfterTypeAnnotation.push_back(
            NTransform::CoroutineAnnotationTransform);
    } else {
        extensions = NSemantics::NKumir::PipelineExtensions();
    }
    auto error = NTransform::Pipeline(
        expr,
        resolver,
        {.Extensions = std::move(extensions)});
    if (!error) {
        return error.error().ToString() + "\n";
    }

    NIR::TModule module;
    NIR::TBuilder builder(module);
    NIR::TAstLowerer lowerer(module, builder, resolver);
    auto lowerRes = lowerer.LowerTop(expr);
    if (!lowerRes) {
        return lowerRes.error().ToString();
    }

    std::ostringstream out;
    module.Print(out);
    return out.str();
}

} // namespace

class RegAst : public ::testing::TestWithParam<ProgCase> {};
class RegExec : public ::testing::TestWithParam<ProgCase> {};
class RegCoreLang : public ::testing::TestWithParam<ProgCase> {};

std::string ResultString(const std::expected<std::optional<std::string>, TError>& res) {
    if (!res) {
        return res.error().ToString();
    }
    std::ostringstream out;
    if (res.value().has_value()) {
        out << *(res.value());
    }
    return out.str();
}

void CheckExecGoldens(
    const fs::path& src,
    const fs::path& golden,
    const fs::path& goldenStdOut,
    const std::string& got,
    const std::string& stdoutText,
    std::string_view label)
{
    if (printOutput) {
        std::cout << "=== Output " << label << " for " << src << " ===\n";
        std::cout << "Result: " << got << "\n";
        std::cout << "StdOut: " << stdoutText << "\n";
        std::cout << "=== End of output ===\n";
    }

    if (updateGoldens) {
        WriteAll(golden, got);
        if (!stdoutText.empty()) {
            WriteAll(goldenStdOut, stdoutText);
        }
    }
    if (!fs::exists(golden)) {
        std::cerr << "Missing golden " << label << " file: " << golden << "\n";
        FAIL();
    }
    if (!fs::exists(goldenStdOut) && !stdoutText.empty()) {
        std::cerr << "Missing golden " << label << " stdout file: " << goldenStdOut << "\n";
        FAIL();
    }
    const auto exp = ReadAll(golden);
    EXPECT_EQ(got, exp);
    if (fs::exists(goldenStdOut)) {
        const auto expOut = ReadAll(goldenStdOut);
        EXPECT_EQ(stdoutText, expOut);
    }
}

enum class EExecBackend {
    IR,
    LLVM,
};

bool IsExecDisabled(const std::string& code) {
    auto header = code.substr(0, code.find('\n'));
    return header.find("disable_exec") != std::string::npos;
}

std::pair<std::string, std::string> RunExec(
    const std::string& code,
    const fs::path& stdin,
    bool coreInput,
    EExecBackend backend,
    int optLevel,
    const fs::path& moduleDir = {})
{
    std::istringstream input(code);

    std::vector<std::string> modulePaths;
    if (!moduleDir.empty()) {
        modulePaths.push_back(moduleDir.string());
    }

    std::ostringstream out;
    NRuntime::SetOutputStream(&out);
    std::istream* inStream = nullptr;
    std::ifstream fin;
    if (fs::exists(stdin)) {
        fin.open(stdin, std::ios::binary);
        inStream = &fin;
    }
    NRuntime::SetInputStream(inStream);

    // The test harness is a host: it provides the System runtime as the core
    // prelude. Kumir fixtures import their prelude through the frontend.
    std::vector<std::string> corePrelude;
    if (coreInput) {
        corePrelude = {"System"};
    }

    std::expected<std::optional<std::string>, TError> res;
    if (backend == EExecBackend::IR) {
        TIRRunner runner(std::cout, std::cin, {
            .CoreInput = coreInput,
            .ResolveCoreInput = coreInput,
            .OptLevel = optLevel,
            .Prelude = corePrelude,
            .ModuleSearchPaths = modulePaths,
        });
        res = runner.Run(input);
    } else {
        TLLVMRunner runner({
            .CoreInput = coreInput,
            .ResolveCoreInput = coreInput,
            .OptLevel = optLevel,
            .Prelude = corePrelude,
            .ModuleSearchPaths = modulePaths,
        });
        res = runner.Run(input);
    }
    return {ResultString(res), out.str()};
}

void CheckExecCase(
    const fs::path& src,
    const fs::path& base,
    bool coreInput,
    EExecBackend backend,
    int optLevel,
    std::string_view label)
{
    const fs::path stdin = fs::path(src).replace_extension(".stdin");
    const fs::path golden = fs::path(GoldensDir / base).replace_extension(".result");
    const fs::path goldenStdOut = fs::path(GoldensDir / base).replace_extension(".result.stdout");

    const auto code = ReadAll(src);
    if (IsExecDisabled(code)) {
        GTEST_SKIP() << "Execution disabled for this test case";
    }
    if (backend == EExecBackend::LLVM
        && code.substr(0, code.find('\n')).find("disable_llvm") != std::string::npos)
    {
        GTEST_SKIP() << "LLVM execution disabled for this test case";
    }

    auto [got, stdoutText] = RunExec(code, stdin, coreInput, backend, optLevel, src.parent_path());
    CheckExecGoldens(src, golden, goldenStdOut, got, stdoutText, label);
}

TEST_P(RegAst, Ast) {
    const fs::path src = fs::path(CasesDir / GetParam().base).replace_extension(".kum");
    const fs::path golden = fs::path(GoldensDir / GetParam().base).replace_extension(".ast");

    const auto code = ReadAll(src);
    std::istringstream input(code);

    NAst::TTokenStream ts(input);
    std::string got = BuildAst(ts, src.parent_path());

    if (printOutput) {
        std::cout << "=== Output AST for " << src << " ===\n";
        std::cout << got << "\n";
        std::cout << "=== End of output ===\n";
    }

    if (updateGoldens) {
        WriteAll(golden, got);
    }
    if (!fs::exists(golden)) {
        // fail if golden missing
        std::cerr << "Missing golden AST file: " << golden << "\n";
        FAIL();
    }
    const auto exp = ReadAll(golden);
    EXPECT_EQ(got, exp);
}

TEST_P(RegAst, IR) {
    const fs::path src = fs::path(CasesDir / GetParam().base).replace_extension(".kum");
    const fs::path golden = fs::path(GoldensDir / GetParam().base).replace_extension(".ir");

    const auto code = ReadAll(src);
    std::istringstream input(code);

    std::string got = BuildIR(input, false, src.parent_path());

    if (printOutput) {
        std::cout << "=== Output IR for " << src << " ===\n";
        std::cout << got << "\n";
        std::cout << "=== End of output ===\n";
    }

    if (updateGoldens) {
        WriteAll(golden, got);
    }
    if (!fs::exists(golden)) {
        // fail if golden missing
        std::cerr << "Missing golden IR file: " << golden << "\n";
        FAIL();
    }
    const auto exp = ReadAll(golden);
    EXPECT_EQ(got, exp);
}

TEST_P(RegExec, ExecIR) {
    const fs::path src = fs::path(CasesDir / GetParam().base).replace_extension(".kum");
    CheckExecCase(src, GetParam().base, false, EExecBackend::IR, 0, "IR RUN");
}

TEST_P(RegExec, ExecIROPT) {
    const fs::path src = fs::path(CasesDir / GetParam().base).replace_extension(".kum");
    CheckExecCase(src, GetParam().base, false, EExecBackend::IR, 1, "IR OPT RUN");
}

TEST_P(RegExec, ExecLLVM) {
    const fs::path src = fs::path(CasesDir / GetParam().base).replace_extension(".kum");
    CheckExecCase(src, GetParam().base, false, EExecBackend::LLVM, 0, "LLVM RUN");
}

TEST_P(RegExec, ExecLLVMOPT) {
    const fs::path src = fs::path(CasesDir / GetParam().base).replace_extension(".kum");
    CheckExecCase(src, GetParam().base, false, EExecBackend::LLVM, 3, "LLVM OPT RUN");
}

TEST_P(RegExec, CoreExec) {
    const fs::path src = fs::path(CasesDir / GetParam().base).replace_extension(".kum");
    const fs::path stdin = fs::path(CasesDir / GetParam().base).replace_extension(".stdin");
    const fs::path golden = fs::path(GoldensDir / GetParam().base).replace_extension(".result");
    const fs::path goldenStdOut = fs::path(GoldensDir / GetParam().base).replace_extension(".result.stdout");

    const auto code = ReadAll(src);
    auto header = code.substr(0, code.find('\n'));
    if (header.find("disable_exec") != std::string::npos) {
        GTEST_SKIP() << "Execution disabled for this test case";
    }

    std::istringstream kumirInput(code);
    NAst::TTokenStream ts(kumirInput);
    auto coreSource = BuildCoreSource(ts, src.parent_path());
    if (coreSource.starts_with("Error: ")) {
        CheckExecGoldens(src, golden, goldenStdOut, coreSource, "", "CORE RUN");
        return;
    }

    std::istringstream coreInput(coreSource);
    std::ostringstream out;
    NRuntime::SetOutputStream(&out);
    std::istream* inStream = nullptr;
    std::ifstream fin;
    if (fs::exists(stdin)) {
        fin.open(stdin, std::ios::binary);
        inStream = &fin;
    }
    NRuntime::SetInputStream(inStream);

    TIRRunner runner(std::cout, std::cin, {
        .CoreInput = true,
        .ResolveCoreInput = false,
        .Prelude = {"System"},
    });
    auto res = runner.Run(coreInput);
    auto got = ResultString(res);

    CheckExecGoldens(src, golden, goldenStdOut, got, out.str(), "CORE RUN");
}

TEST_P(RegCoreLang, IR) {
    const fs::path relBase = fs::path("corelang") / GetParam().base;
    const fs::path src = fs::path(CasesDir / relBase).replace_extension(".oz");
    const fs::path golden = fs::path(GoldensDir / relBase).replace_extension(".ir");

    const auto code = ReadAll(src);
    std::istringstream input(code);

    std::string got = BuildIR(input, true, src.parent_path());

    if (printOutput) {
        std::cout << "=== Output CORE IR for " << src << " ===\n";
        std::cout << got << "\n";
        std::cout << "=== End of output ===\n";
    }

    if (updateGoldens) {
        WriteAll(golden, got);
    }
    if (!fs::exists(golden)) {
        std::cerr << "Missing golden CORE IR file: " << golden << "\n";
        FAIL();
    }
    const auto exp = ReadAll(golden);
    EXPECT_EQ(got, exp);
}

TEST_P(RegCoreLang, ExecIR) {
    const fs::path relBase = fs::path("corelang") / GetParam().base;
    const fs::path src = fs::path(CasesDir / relBase).replace_extension(".oz");
    CheckExecCase(src, relBase, true, EExecBackend::IR, 0, "CORE IR RUN");
}

TEST_P(RegCoreLang, ExecIROPT) {
    const fs::path relBase = fs::path("corelang") / GetParam().base;
    const fs::path src = fs::path(CasesDir / relBase).replace_extension(".oz");
    CheckExecCase(src, relBase, true, EExecBackend::IR, 1, "CORE IR OPT RUN");
}

TEST_P(RegCoreLang, ExecLLVM) {
    const fs::path relBase = fs::path("corelang") / GetParam().base;
    const fs::path src = fs::path(CasesDir / relBase).replace_extension(".oz");
    CheckExecCase(src, relBase, true, EExecBackend::LLVM, 0, "CORE LLVM RUN");
}

TEST_P(RegCoreLang, ExecLLVMOPT) {
    const fs::path relBase = fs::path("corelang") / GetParam().base;
    const fs::path src = fs::path(CasesDir / relBase).replace_extension(".oz");
    CheckExecCase(src, relBase, true, EExecBackend::LLVM, 3, "CORE LLVM OPT RUN");
}

INSTANTIATE_TEST_SUITE_P(
    RegProgAst,
    RegAst,
    ::testing::ValuesIn(Collect(CasesDir)),
    [](const ::testing::TestParamInfo<ProgCase>& i){ return "AST_" + NameFromPath(i.param.base); });

INSTANTIATE_TEST_SUITE_P(
    RegProgExec,
    RegExec,
    ::testing::ValuesIn(Collect(CasesDir)),
    [](const ::testing::TestParamInfo<ProgCase>& i){ return "EXEC_" + NameFromPath(i.param.base); });

INSTANTIATE_TEST_SUITE_P(
    RegCoreLangProg,
    RegCoreLang,
    ::testing::ValuesIn(Collect(CasesDir / "corelang", ".oz")),
    [](const ::testing::TestParamInfo<ProgCase>& i){ return "CORE_" + NameFromPath(i.param.base); });

// `output` resolves only when the host imports System; an empty prelude leaves
// core-lang with nothing imported.
TEST(CorePrelude, PureCoreImportsNothing) {
    const std::string program =
        "(block (fun <main> () (block (output (: 42 i64) \"\\n\"))))";
    std::ostringstream sink;
    NRuntime::SetOutputStream(&sink);

    {
        std::istringstream in(program);
        TIRRunner runner(std::cout, std::cin, {
            .CoreInput = true,
            .ResolveCoreInput = true,
        });
        EXPECT_FALSE(runner.Run(in).has_value());
    }
    {
        std::istringstream in(program);
        TIRRunner runner(std::cout, std::cin, {
            .CoreInput = true,
            .ResolveCoreInput = true,
            .Prelude = {"System"},
        });
        EXPECT_TRUE(runner.Run(in).has_value());
    }
}

int main(int argc, char** argv) {
    if (argc > 1) {
        RootDir = argv[1];
        CasesDir = RootDir / "cases";
        GoldensDir = RootDir / "goldens";
    }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--canonize")) {
            updateGoldens = true;
        } else if (!strcmp(argv[i], "--print")) {
            printOutput = true;
        }
    }
    ::testing::InitGoogleTest(&argc, argv);
    NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
