#pragma once

// Lightweight forward declarations to avoid heavy LLVM includes in dependents.
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include <qumir/ir/builder.h>

namespace llvm {
class AllocaInst;
class LLVMContext;
class Module;
class Function;
class Type;
class Value;
class BasicBlock;
class IRBuilderBase;
class TargetMachine;
class GlobalVariable;
} // namespace llvm

namespace NQumir::NIR {
struct TModule;
struct TFunction;
struct TInstr;
struct TBlock;
} // namespace NQumir::NIR

namespace NQumir::NCodeGen {

class TLLVMDebugInfo;

// A generic instance: stable, specialization-addressed name. Implementation
// changes are invalidated via the fingerprint's KernelLibVersion, not by hashing.
inline bool IsCacheableSymbol(std::string_view name) {
    return name.starts_with("__generic_");
}

// Names of cacheable function *definitions* in the module (generic instances or
// `cacheable`-flagged), excluding coroutines. Pure discovery: no LLVM emission,
// no state mutation.
std::vector<std::string> CollectCacheableSymbols(const NIR::TModule& module);

// Reachable cacheable bodies suitable for importing into the definitions.
// Explicit inline requests and a bounded set of small helpers are included.
std::unordered_set<std::string> CollectInlineDefinitions(
    const NIR::TModule& module,
    const std::unordered_set<std::string>& cacheable,
    const std::unordered_set<std::string>& definitions);

struct TLLVMCodeGenOptions {
    std::string ModuleName {"oz_module"};
    bool Optimize {false};
    bool NativeCode {false};
    // Optional target triple override (e.g., "wasm32-unknown-unknown").
    // If empty, defaults are used.
    std::string TargetTriple;
    // Module partitioning for the object cache (mutually exclusive):
    //   RestrictToDefinitions - only these get bodies (dependency-only object).
    //   EmitAsExternal        - these become external decls (kernel object).
    // Others are external declarations / definitions respectively. Both null =
    // define everything (default).
    const std::unordered_set<std::string>* RestrictToDefinitions {nullptr};
    const std::unordered_set<std::string>* EmitAsExternal {nullptr};
    // Optimizer-visible bodies owned by another object. They are never exports.
    const std::unordered_set<std::string>* InlineDefinitions {nullptr};
    // Binary LLVM bitcode modules linked into the generated module before the
    // LLVM optimization pipeline. The pointed-to vector must outlive Emit().
    const std::vector<std::string>* LlvmBitcode {nullptr};
};

struct ILLVMModuleArtifacts {
    virtual ~ILLVMModuleArtifacts() = default;
    // Expose defined (non-declaration) function names for tests/introspection without LLVM headers
    virtual const std::vector<std::string>& GetDefinedFunctionNames() const = 0;
    virtual void PrintModule(std::ostream& os) const = 0;
    // Currently used only by tests that build in-memory bitcode fixtures.
    virtual void GenerateBitcode(std::ostream& os) const = 0;
    virtual void Generate(std::ostream& os, bool generateAsm, bool generateObj) const = 0;
};

class TLLVMCodeGen {
public:
    explicit TLLVMCodeGen(const TLLVMCodeGenOptions& opts = {});
    ~TLLVMCodeGen();

    std::unique_ptr<ILLVMModuleArtifacts> Emit(NIR::TModule& module, int optLevel = 0);

    void PrintFunction(int symId, std::ostream& os) const;

private:
    llvm::Function* LowerFunction(
        const NIR::TFunction& fun,
        NIR::TModule& module,
        TLLVMDebugInfo* debugInfo);
    llvm::Function* LowerCoroutineFunction(const NIR::TFunction& fun, NIR::TModule& module);
    void LowerBlock(
        const NIR::TBlock& blk,
        NIR::TModule& module,
        llvm::Function* lf,
        std::vector<llvm::BasicBlock*>& orderedBBs,
        TLLVMDebugInfo* debugInfo);
    llvm::Value* LowerInstr(const NIR::TInstr& instr, NIR::TModule& module);
    llvm::Value* LowerVectorInstr(const NIR::TInstr& instr, NIR::TModule& module, int outputTypeId);
    llvm::Value* LowerVectorIndex(llvm::Value* vector, llvm::Value* index, llvm::Type* resultType);
    llvm::Value* EmitPhi(const NIR::TPhi& instr, NIR::TModule& module);
    void AddIncomingPhiEdges(const NIR::TPhi& instr, NIR::TModule& module);
    llvm::Value* GetOp(const NIR::TOperand& op, NIR::TModule& module);
    void CreateTargetMachine();
    void Optimize(int optLevel);
    void RunCoroutinePasses();

    llvm::GlobalVariable* EnsureSlotGlobal(int64_t sidx, NIR::TModule& module);

private:
    TLLVMCodeGenOptions Opts;
    std::unique_ptr<llvm::LLVMContext> Ctx;
    std::unique_ptr<llvm::Module> LModule;
    std::unique_ptr<llvm::TargetMachine> TM;
    std::unique_ptr<llvm::IRBuilderBase> BuilderBase; // concrete created in cpp

    struct TFunState {
        const NIR::TFunction* Fun {nullptr};
        llvm::Function* LFun {nullptr};
        std::vector<llvm::Value*> TmpValues; // size = Fun->NextTmpIdx
        std::unordered_map<int64_t, llvm::BasicBlock*> LabelToBB; // direct mapping for jmp/cmp targets
        std::unordered_map<int64_t, llvm::BasicBlock*> LabelExitBB; // actual predecessor block for PHI incoming edges
        std::vector<llvm::Value*> PendingArgs; // collected via 'arg' ops before 'call'
        std::vector<llvm::AllocaInst*> Allocas;
    };
    std::unique_ptr<TFunState> CurFun;
    // Module-global slot storage (i64 globals), indexed by module-wide slot index
    std::vector<llvm::Value*> ModuleSlots;
    std::vector<llvm::Value*> StringLiterals;
    // Map from SymId to lowered LLVM function for call lowering
    std::unordered_map<int, llvm::Function*> SymIdToLFun;
    std::unordered_map<int, int> SymIdToUniqueFunId; // for updating code of updated functions
};

} // namespace NQumir::NCodeGen
