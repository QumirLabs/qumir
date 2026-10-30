#pragma once

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

struct TPrintOptions {
    ETypePrintMode TypeMode = ETypePrintMode::Required;
    bool Pretty = true;
    int IndentStep = 2;
    size_t LineWidth = 120;
    std::set<std::string> ShortNamedTypes;
    TPrintExprFactory NodePrinters;
    bool HideResolvedOrigins = true;
};

class TPrinter {
    friend struct NImpl::TPrintExpr;
public:
    TPrinter(std::ostream& out, TPrintOptions options);

    // main entry points
    void PrintExpr(TExprPtr expr);
    void PrintType(TTypePtr type);

    // helpers
    bool FitsOneLine(TExprPtr expr, bool allowTypeWrap, int level);
    void PrintExpr(TExprPtr expr, bool allowTypeWrap, int level);
    void PrintType(TTypePtr type, int level);
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
    void PrintCompact(TExprPtr expr, bool allowTypeWrap, int level);
    bool ShouldWrapType(const TExprPtr& expr) const;
    bool IsNonDefaultNumberLiteral(const TExprPtr& expr) const;
    bool HasPrintableTypeAttrs(TTypePtr type) const;
    void PrintTypeAttrs(TTypePtr type);
    void PrintScalarType(std::string_view name, TTypePtr type);
    void PrintFunctionType(const std::shared_ptr<TFunctionType>& type, int level);
    void PrintStructType(const std::shared_ptr<TStructType>& type, int level);
    void PrintGenericParams(const std::vector<TGenericParam>& params, int level);
    std::vector<std::string> PushGenericTypeParamShortNames(const std::vector<TGenericParam>& params);
    void PopShortNamedTypes(const std::vector<std::string>& names);
    std::shared_ptr<TStructType> GetStructType(TTypePtr type);

    void PrintArrayAssign(TArrayAssignExpr& node, int level);
    void PrintIndexVector(const std::vector<TExprPtr>& indices, int level);
    void PrintExprList(std::string_view head, const std::vector<TExprPtr>& items, int level);
    void PrintIfLike(std::string_view head, TExprPtr cond, TExprPtr thenBranch, TExprPtr elseBranch, int level);
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

void PrintAst(std::ostream& out, TExprPtr expr, const TPrintOptions& options = {});
std::string PrintAst(TExprPtr expr, const TPrintOptions& options = {});

void PrintType(std::ostream& out, TTypePtr type, const TPrintOptions& options = {});
std::string PrintType(TTypePtr type, const TPrintOptions& options = {});

} // namespace NCore

std::ostream& operator<<(std::ostream& out, const TExprPtr& expr);

} // namespace NAst
} // namespace NQumir
