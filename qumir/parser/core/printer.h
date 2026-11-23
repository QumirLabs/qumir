#pragma once

#include <optional>

#include <functional>
#include <iosfwd>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

namespace NQumir {
namespace NAst {
namespace NCore {

namespace NImpl {
struct TPrintExpr;
} // namespace NImpl

class TPrinter;

enum class ETypePrintMode {
    Required,
    All,
};

struct TPrintFrame {
    bool AllowTypeWrap;
    int Level;
};

using TNodePrinter = std::function<void(TExpr&, TPrinter&, TPrintFrame)>;
using TPrintExprFactory = std::unordered_map<std::string_view, TNodePrinter>;

// Prints a type unknown to the core printer, found by TypeName(). To stay parseable,
// emit "<named Name [args]>" with TPrinter::PrintNamedType.
using TTypePrinter = std::function<void(const TType&, TPrinter&, int level)>;
using TPrintTypeFactory = std::unordered_map<std::string_view, TTypePrinter>;

struct TPrintOptions {
    ETypePrintMode TypeMode = ETypePrintMode::Required;
    bool Pretty = true;
    int IndentStep = 2;
    size_t LineWidth = 120;
    std::set<std::string> ShortNamedTypes;
    TPrintExprFactory NodePrinters;
    TPrintTypeFactory TypePrinters;
    bool HideResolvedOrigins = true;
};

class TPrinter {
    friend struct NImpl::TPrintExpr;
public:
    TPrinter(std::ostream& out, TPrintOptions options);

    // main entry points
    void PrintExpr(const TExprPtr& expr);
    void PrintType(const TTypePtr& type);

    // helpers
    bool FitsOneLine(const TExprPtr& expr, bool allowTypeWrap, int level);
    void PrintExpr(const TExprPtr& expr, bool allowTypeWrap, int level);
    void PrintType(const TTypePtr& type, int level);
    // Prints a named type from its parts, so a type printer needs no type object.
    void PrintNamedType(
        const std::string& name,
        const std::vector<TGenericArg>& typeArgs,
        TTypePtr underlying,
        int level);
    void Separator(int level);
    void Space();
    void PrintIdentifier(const std::string& value);
    void PrintString(const std::string& value, char quote);
    void PrintNumber(TNumberExpr& node);
    std::ostream& GetOut() { return *Out; }

private:
    std::ostream* Out;
    TPrintOptions Options;

    bool ForceMultiline(const TExprPtr& expr) const;
    void PrintCompact(const TExprPtr& expr, bool allowTypeWrap, int level);
    bool ShouldWrapType(const TExprPtr& expr) const;
    bool IsNonDefaultNumberLiteral(const TExprPtr& expr) const;
    void PrintFunctionType(const TFunctionType& type, int level);
    void PrintStructType(const TStructType& type, int level);
    void PrintGenericParams(const std::vector<TGenericParam>& params, int level);
    std::vector<std::string> PushGenericTypeParamShortNames(const std::vector<TGenericParam>& params);
    void PopShortNamedTypes(const std::vector<std::string>& names);
    const TStructType* GetStructType(const TTypePtr& type);

    void PrintArrayAssign(TArrayAssignExpr& node, int level);
    void PrintIndexVector(std::span<const TExprPtr> indices, int level);
    void PrintExprList(std::string_view head, const std::vector<TExprPtr>& items, int level);
    void PrintIfLike(std::string_view head, const TExprPtr& cond, const TExprPtr& thenBranch, const TExprPtr& elseBranch, int level);
    void PrintWhile(TWhileStmtExpr& node, int level);
    void PrintRepeat(TRepeatStmtExpr& node, int level);
    void PrintFor(TForStmtExpr& node, int level);
    void PrintTimes(TTimesStmtExpr& node, int level);
    void PrintVar(TVarStmt& node, int level);
    void PrintFun(TFunDecl& node, int level);
    void PrintCall(TCallExpr& node, int level);
    void PrintOutput(TOutputExpr& node, int level);
    void PrintMultiIndex(TMultiIndexExpr& node, int level);
    void PrintSlice(TSliceExpr& node, int level);
    void PrintStructConstruct(TStructConstructExpr& node, int level);
};

void PrintAst(std::ostream& out, const TExprPtr& expr, const TPrintOptions& options = {});
std::string PrintAst(const TExprPtr& expr, const TPrintOptions& options = {});

void PrintType(std::ostream& out, const TTypePtr& type, const TPrintOptions& options = {});
std::string PrintType(const TTypePtr& type, const TPrintOptions& options = {});

} // namespace NCore

std::ostream& operator<<(std::ostream& out, const TExprPtr& expr);

} // namespace NAst
} // namespace NQumir
