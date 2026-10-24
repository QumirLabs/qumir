#pragma once

#include <string>
#include <memory>
#include <vector>
#include <sstream>
#include <iostream>
#include <functional>
#include <optional>

#include <qumir/location.h>

#include "type.h"
#include "operator.h"

namespace NQumir {
namespace NAst {

struct TExpr;
using TExprPtr = std::shared_ptr<TExpr>;

// Copies one node while sharing its children, or recursively copies a parsed
// source tree with fresh scope bookkeeping. Used when a cached source module is
// composed into more than one independent compilation.
TExprPtr ShallowCloneNode(const TExprPtr& node);
TExprPtr DeepCloneExpr(const TExprPtr& node);

// Lifetime AST node forward declarations.
struct TRetainExpr;
struct TOwnLiteralExpr;
struct TMoveExpr;
struct TBorrowExpr;
struct TDestroyExpr;
struct TReplaceExpr;
struct TCleanupExitExpr;
struct TGlobalCleanupExpr;
// End lifetime AST node forward declarations.

template<typename T>
struct TMaybeNode {
    TMaybeNode(TExprPtr node);

    std::shared_ptr<T> Cast() {
        return std::static_pointer_cast<T>(Node);
    }

    TExprPtr Node;
    operator bool() const { return Node != nullptr; }
};

struct IVisitor;

struct TExpr {
    TLocation Location;
    TTypePtr Type = nullptr;
    std::optional<std::string> Origin;

    TExpr() = default;
    TExpr(TLocation loc)
        : Location(std::move(loc))
    { }
    TExpr(TLocation loc, TTypePtr type)
        : Location(std::move(loc))
        , Type(std::move(type))
    { }
    virtual ~TExpr() = default;
    virtual std::vector<TExprPtr> Children() const {
        return {};
    }
    virtual std::vector<TExprPtr*> MutableChildren() {
        return {};
    }
    virtual const std::string_view NodeName() const = 0;
    virtual const std::string ToString() const {
        return std::string(NodeName());
    }

    virtual void Accept(class IVisitor& visitor) = 0;
};

template<typename T>
inline TMaybeNode<T>::TMaybeNode(TExprPtr node)
    : Node(node && std::string_view(T::NodeId) == node->NodeName()
        ? std::move(node)
        : nullptr)
{ }

struct TIdentExpr : TExpr {
    static constexpr const char* NodeId = "Ident";

    std::string Name;
    explicit TIdentExpr(TLocation loc, std::string n)
        : TExpr(std::move(loc))
        , Name(std::move(n))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        return "$" + Name;
    }

    void Accept(IVisitor& visitor) override;
};

struct TAssignExpr : TExpr {
    static constexpr const char* NodeId = "Assign";

    std::string Name;
    TExprPtr Value;
    TAssignExpr(TLocation loc, std::string n, TExprPtr v)
        : TExpr(std::move(loc))
        , Name(std::move(n))
        , Value(std::move(v))
    { }

    std::vector<TExprPtr> Children() const override {
        return { Value };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        return { &Value };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        return std::string("$") + Name + " =";
    }

    void Accept(IVisitor& visitor) override;
};

struct TArrayAssignExpr : TExpr {
    static constexpr const char* NodeId = "ArrayAssign";

    std::string Name;
    std::vector<TExprPtr> Indices;
    TExprPtr Value;
    TArrayAssignExpr(TLocation loc, std::string n, std::vector<TExprPtr> idxs, TExprPtr v)
        : TExpr(std::move(loc))
        , Name(std::move(n))
        , Indices(std::move(idxs))
        , Value(std::move(v))
    { }

    std::vector<TExprPtr> Children() const override {
        std::vector<TExprPtr> children = Indices;
        children.push_back(Value);
        return children;
    }

    std::vector<TExprPtr*> MutableChildren() override {
        std::vector<TExprPtr*> children;
        for (auto& idx : Indices) {
            children.push_back(&idx);
        }
        children.push_back(&Value);
        return children;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        return std::string("$") + Name + "[...] =";
    }

    void Accept(IVisitor& visitor) override;
};

struct TNumberExpr : TExpr {
    static constexpr const char* NodeId = "Number";

    union {
        int64_t IntValue;
        double FloatValue;
    };

    explicit TNumberExpr(TLocation loc, bool v)
        : TExpr(std::move(loc)), IntValue(v)
    {
        Type = std::make_shared<TBoolType>();
    }
    explicit TNumberExpr(TLocation loc, int64_t v)
        : TExpr(std::move(loc)), IntValue(v)
    {
        Type = std::make_shared<TIntegerType>();
    }
    explicit TNumberExpr(TLocation loc, double v)
        : TExpr(std::move(loc)), FloatValue(v)
    {
        Type = std::make_shared<TFloatType>();
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const bool IsFloat() const {
        return TMaybeType<TFloatType>(Type);
    }

    const std::string ToString() const override {
        return IsFloat() ? std::to_string(FloatValue) : std::to_string(IntValue);
    }

    void Accept(IVisitor& visitor) override;
};

struct TVectorExpr : TExpr {
    static constexpr const char* NodeId = "Vector";

    std::vector<TExprPtr> Elements;
    explicit TVectorExpr(TLocation loc, std::vector<TExprPtr> elems)
        : TExpr(std::move(loc)), Elements(std::move(elems))
    { }

    std::vector<TExprPtr> Children() const override {
        return Elements;
    }

    std::vector<TExprPtr*> MutableChildren() override {
        std::vector<TExprPtr*> result;
        result.reserve(Elements.size());
        for (auto& elem : Elements) {
            result.push_back(&elem);
        }
        return result;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        return "vec";
    }

    void Accept(IVisitor& visitor) override;
};

// c-style const char* string literal
struct TStringLiteralExpr : TExpr {
    static constexpr const char* NodeId = "StringLiteral";

    std::string Value;
    explicit TStringLiteralExpr(TLocation loc, std::string v)
        : TExpr(std::move(loc)), Value(std::move(v))
    {
        Type = std::make_shared<TStringType>();
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        // For simplicity, output escaped string:
        std::string escaped; escaped.reserve(Value.size());
        // Escape special characters
        for (size_t i = 0; i < Value.size(); ++i) {
            if (Value[i] == '\"') {
                escaped.insert(i, "\\");
            } else if (Value[i] == '\\') {
                escaped.insert(i, "\\");
            } else if (Value[i] == '\n') {
                escaped.insert(i, "\\n");
            } else if (Value[i] == '\t') {
                escaped.insert(i, "\\t");
            } else {
                escaped.push_back(Value[i]);
            }
        }
        return "\"" + escaped + "\"";
    }

    void Accept(IVisitor& visitor) override;
};

struct TUnaryExpr : TExpr {
    static constexpr const char* NodeId = "Unary";

    TOperator Operator;
    TExprPtr Operand;
    TUnaryExpr(TLocation loc, TOperator o, TExprPtr e)
        : TExpr(std::move(loc))
        , Operator(o)
        , Operand(std::move(e))
    { }

    std::vector<TExprPtr> Children() const override {
        return { Operand };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        return { &Operand };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        return Operator.ToString();
    }

    void Accept(IVisitor& visitor) override;
};

struct TBinaryExpr : TExpr {
    static constexpr const char* NodeId = "Binary";

    TOperator Operator;
    TExprPtr Left;
    TExprPtr Right;
    TBinaryExpr(TLocation loc, TOperator o, TExprPtr l, TExprPtr r)
        : TExpr(std::move(loc))
        , Operator(o)
        , Left(std::move(l))
        , Right(std::move(r))
    { }

    std::vector<TExprPtr> Children() const override {
        return { Left, Right };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        return { &Left, &Right };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        return Operator.ToString();
    }

    void Accept(IVisitor& visitor) override;
};

struct TBlockExpr : TExpr {
    static constexpr const char* NodeId = "Block";

    std::vector<TExprPtr> Stmts;
    int32_t Scope = -1; // filled in by name resolver, 0 - root scope, -1 - unscoped

    explicit TBlockExpr(TLocation loc, std::vector<TExprPtr> s)
        : TExpr(std::move(loc))
        , Stmts(std::move(s))
    { }

    std::vector<TExprPtr> Children() const override {
        std::vector<TExprPtr> result;
        result.reserve(Stmts.size());
        for (const auto& stmt : Stmts) {
            result.push_back(stmt);
        }
        return result;
    }

    std::vector<TExprPtr*> MutableChildren() override {
        std::vector<TExprPtr*> result;
        result.reserve(Stmts.size());
        for (auto& stmt : Stmts) {
            result.push_back(&stmt);
        }
        return result;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TIfExpr : TExpr {
    static constexpr const char* NodeId = "IfExpr";

    TExprPtr Cond;
    TExprPtr Then;
    TExprPtr Else;
    TIfExpr(TLocation loc, TExprPtr c, TExprPtr t, TExprPtr e)
        : TExpr(std::move(loc))
        , Cond(std::move(c))
        , Then(std::move(t))
        , Else(std::move(e))
    { }

    std::vector<TExprPtr> Children() const override {
        if (Else) return { Cond, Then, Else };
        return { Cond, Then };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        if (Else) return { &Cond, &Then, &Else };
        return { &Cond, &Then };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TWhileStmtExpr : TExpr {
    static constexpr const char* NodeId = "WhileStmt";

    TExprPtr Cond;
    TExprPtr Body;

    TWhileStmtExpr(TLocation loc, TExprPtr cond, TExprPtr body)
        : TExpr(std::move(loc))
        , Cond(std::move(cond))
        , Body(std::move(body))
    { }

    std::vector<TExprPtr> Children() const override {
        return { Cond, Body };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        return { &Cond, &Body };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TRepeatStmtExpr : TExpr {
    static constexpr const char* NodeId = "RepeatStmt";

    TExprPtr Body;
    TExprPtr Cond;

    TRepeatStmtExpr(TLocation loc, TExprPtr body, TExprPtr cond)
        : TExpr(std::move(loc))
        , Body(std::move(body))
        , Cond(std::move(cond))
    { }

    std::vector<TExprPtr> Children() const override {
        return { Body, Cond };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        return { &Body, &Cond };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TForStmtExpr : TExpr {
    static constexpr const char* NodeId = "ForStmt";

    std::string VarName;
    TExprPtr From;
    TExprPtr To;
    TExprPtr Step;
    TExprPtr Body;

    TForStmtExpr(TLocation loc, std::string varName, TExprPtr from, TExprPtr to, TExprPtr step, TExprPtr body)
        : TExpr(std::move(loc))
        , VarName(std::move(varName))
        , From(std::move(from))
        , To(std::move(to))
        , Step(std::move(step))
        , Body(std::move(body))
    { }

    std::vector<TExprPtr> Children() const override {
        return { From, To, Step, Body };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        return { &From, &To, &Step, &Body };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TTimesStmtExpr : TExpr {
    static constexpr const char* NodeId = "TimesStmt";

    TExprPtr Count;
    TExprPtr Body;

    TTimesStmtExpr(TLocation loc, TExprPtr count, TExprPtr body)
        : TExpr(std::move(loc))
        , Count(std::move(count))
        , Body(std::move(body))
    { }

    std::vector<TExprPtr> Children() const override {
        return { Count, Body };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        return { &Count, &Body };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TBreakStmt : TExpr {
    static constexpr const char* NodeId = "Break";

    explicit TBreakStmt(TLocation loc)
        : TExpr(std::move(loc))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TContinueStmt : TExpr {
    static constexpr const char* NodeId = "Continue";

    explicit TContinueStmt(TLocation loc)
        : TExpr(std::move(loc))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TReturnExpr : TExpr {
    static constexpr const char* NodeId = "Return";

    TExprPtr Value; // optional; null for `(return)`

    TReturnExpr(TLocation loc, TExprPtr value)
        : TExpr(std::move(loc))
        , Value(std::move(value))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    std::vector<TExprPtr> Children() const override {
        if (Value) { return { Value }; }
        return {};
    }

    std::vector<TExprPtr*> MutableChildren() override {
        if (Value) { return { &Value }; }
        return {};
    }

    void Accept(IVisitor& visitor) override;
};

struct TVarStmt : TExpr {
    static constexpr const char* NodeId = "Var";

    std::string Name;
    std::vector<std::pair<TExprPtr, TExprPtr>> Bounds; // for array types
    TExprPtr Init; // for (var name = expr) form; type inferred from Init

    TVarStmt(TLocation loc, std::string name, NAst::TTypePtr type, std::vector<std::pair<TExprPtr, TExprPtr>> bounds = {})
        : TExpr(std::move(loc), std::move(type))
        , Name(std::move(name))
        , Bounds(std::move(bounds))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    std::vector<TExprPtr> Children() const override {
        std::vector<TExprPtr> result;
        if (Init) { result.push_back(Init); }
        for (const auto& b : Bounds) {
            result.push_back(b.first);
            result.push_back(b.second);
        }
        return result;
    }

    std::vector<TExprPtr*> MutableChildren() override {
        std::vector<TExprPtr*> result;
        if (Init) { result.push_back(&Init); }
        for (auto& b : Bounds) {
            result.push_back(&b.first);
            result.push_back(&b.second);
        }
        return result;
    }

    const std::string ToString() const override {
        return std::string(NodeName()) + " $" + Name;
    }

    void Accept(IVisitor& visitor) override;
};

struct TVarsBlockExpr : TExpr {
    // like TBlockExpr but without scope, used during parsing
    static constexpr const char* NodeId = "VarsBlock";

    std::vector<TExprPtr> Vars; // decl or assignments

    explicit TVarsBlockExpr(TLocation loc, std::vector<TExprPtr> vars)
        : TExpr(std::move(loc))
        , Vars(std::move(vars))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

using TParam = std::shared_ptr<TVarStmt>;

struct TGenericParam {
    enum class EKind {
        Type,
        Value,
    };

    std::string Name;
    EKind Kind;
    TTypePtr ValueType = nullptr; // optional, for value params only
};

struct TFunDecl : TExpr {
    static constexpr const char* NodeId = "FunDecl";

    std::string Name;
    std::string OriginalName; // retained for debug info when Name becomes a synthetic symbol
    std::string MangledName;
    std::vector<TGenericParam> GenericParams;
    std::vector<TParam> Params;
    std::shared_ptr<TBlockExpr> Body;
    std::shared_ptr<TExpr> LastAssert = nullptr; // last assert in function body, executed before return
    using TPacked = uint64_t(*)(const uint64_t* args, size_t argCount);
    TPacked Packed = nullptr; // packed thunk for built-in functions
    using TInlineFactory = std::function<TExprPtr(std::vector<TExprPtr>)>;
    std::optional<TInlineFactory> InlineFactory; // if set, call is replaced by the returned AST
    bool RequireArgsMaterialization = false; // if true, arguments must be materialized before calling, used for strings
    bool NeedsLocator = false; // external function: calls are preceded by builtin::record_locator(line, byte, column)
    NAst::TTypePtr RetType; // ret type different from TExpr::Type which is the function value type
    int32_t Scope = -1; // Function internal scope, filled in by name resolver, -1 - unscoped
    // (operator "X") attribute: this function implements operator/cast "X"
    // (e.g. "+", "neg", "cast", "print"); registered into the resolver's
    // type-keyed op/cast dispatch tables instead of plain name lookup.
    std::optional<std::string> OperatorName;
    // (literal "X") attribute: this function is the constructor for the numeric
    // literal suffix "X" (e.g. `2i` lowers to a call of this function).
    std::optional<std::string> LiteralSuffix;
    // `used` attribute: keep this function even when no explicit reference makes
    bool Used = false;
    // `cacheable` attribute: content-stable and reusable across compilations,
    // so its compiled object may be persisted in the JIT object cache.
    bool Cacheable = false;
    // `inline` attribute: request LLVM alwaysinline on a function definition.
    bool Inline = false;
    // `noinline` attribute: keep every call to this function out of line, so
    // it is neither inlined nor imported into another compilation unit.
    bool NoInline = false;
    // Set once the resolver finalized the emit symbol (overload/cacheable mangling);
    // guards against re-mangling when the mangled name is re-declared.
    bool Mangled = false;
    TFunDecl(TLocation loc, std::string name, std::vector<TGenericParam> genericParams, std::vector<TParam> args, std::shared_ptr<TBlockExpr> body, NAst::TTypePtr type)
        : TExpr(std::move(loc))
        , Name(std::move(name))
        , GenericParams(std::move(genericParams))
        , Params(std::move(args))
        , Body(std::move(body))
        , RetType(std::move(type))
    { }

    // A mangled/link symbol marks an external function, except a cacheable one
    // which carries a MangledName (its emit symbol) yet still has a body.
    // Bodyless but NOT external: inline/intrinsic builtins (InlineFactory/Packed,
    // no MangledName) - expanded at the call site, so nothing to lower or link.
    bool IsExternal() const {
        return !MangledName.empty() && !(Cacheable && Body);
    }

    std::vector<TExprPtr> Children() const override {
        if (LastAssert == nullptr) {
            return { Body };
        } else {
            return { Body, LastAssert };
        }
    }

    std::vector<TExprPtr*> MutableChildren() override {
        if (LastAssert == nullptr) {
            return { reinterpret_cast<TExprPtr*>(&Body) };
        } else {
            return { reinterpret_cast<TExprPtr*>(&Body), reinterpret_cast<TExprPtr*>(&LastAssert) };
        }
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        auto s = std::string(NodeName()) + " $" + Name;
        s += " (";
        for (size_t i = 0; i < Params.size(); ++i) {
            s += "$" + Params[i]->Name;
            if (!Type && Params[i]->Type) {
                std::ostringstream str; str << *Params[i]->Type;
                s += ": " + str.str();
            }
            if (i < Params.size() - 1) {
                s += ", ";
            }
        }
        s += ")";
        if (!Type && RetType) {
            std::ostringstream str; str << *RetType;
            s += " -> " + str.str();
        }
        return s;
    }

    void Accept(IVisitor& visitor) override;
};

struct TCallExpr : TExpr {
    static constexpr const char* NodeId = "Call";

    TExprPtr Callee; // should evaluate to a function
    std::vector<TExprPtr> Args;
    TCallExpr(TLocation loc, TExprPtr c, std::vector<TExprPtr> a)
        : TExpr(std::move(loc))
        , Callee(std::move(c))
        , Args(std::move(a))
    { }

    std::vector<TExprPtr> Children() const override {
        std::vector<TExprPtr> result;
        result.reserve(1 + Args.size());
        result.push_back(Callee);
        for (const auto& arg : Args) {
            result.push_back(arg);
        }
        return result;
    }

    std::vector<TExprPtr*> MutableChildren() override {
        std::vector<TExprPtr*> result;
        result.reserve(1 + Args.size());
        result.push_back(&Callee);
        for (auto& arg : Args) {
            result.push_back(&arg);
        }
        return result;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TAwaitExpr : TExpr {
    static constexpr const char* NodeId = "Await";

    TExprPtr Operand;
    explicit TAwaitExpr(TLocation loc, TExprPtr operand)
        : TExpr(std::move(loc))
        , Operand(std::move(operand))
    { }

    std::vector<TExprPtr> Children() const override {
        return { Operand };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        return { &Operand };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

// Sugared AST node for input(...) expression, which is transformed into calls to input_xxx functions
struct TInputExpr : TExpr {
    static constexpr const char* NodeId = "Input";

    std::vector<TExprPtr> Args;
    TInputExpr(TLocation loc, std::vector<TExprPtr> a)
        : TExpr(std::move(loc))
        , Args(std::move(a))
    {
        Type = std::make_shared<TVoidType>();
    }

    std::vector<TExprPtr> Children() const override {
        std::vector<TExprPtr> result;
        result.reserve(Args.size());
        for (const auto& arg : Args) {
            result.push_back(arg);
        }
        return result;
    }

    std::vector<TExprPtr*> MutableChildren() override {
        std::vector<TExprPtr*> result;
        result.reserve(Args.size());
        for (auto& arg : Args) {
            result.push_back(&arg);
        }
        return result;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

// Sugared AST node for output(...) expression, which is transformed into calls to output_xxx functions
struct TOutputArg {
    TExprPtr Expr;
    TExprPtr Width; // optional width expression
    TExprPtr Precision; // optional precision expression
};

struct TOutputExpr : TExpr {
    static constexpr const char* NodeId = "Output";

    std::vector<TOutputArg> Args;
    TOutputExpr(TLocation loc, std::vector<TOutputArg> a)
        : TExpr(std::move(loc))
        , Args(std::move(a))
    {
        Type = std::make_shared<TVoidType>();
    }

    std::vector<TExprPtr> Children() const override {
        std::vector<TExprPtr> result;
        result.reserve(Args.size());
        for (const auto& arg : Args) {
            result.push_back(arg.Expr);
            if (arg.Width) {
                result.push_back(arg.Width);
            }
            if (arg.Precision) {
                result.push_back(arg.Precision);
            }
        }
        return result;
    }

    std::vector<TExprPtr*> MutableChildren() override {
        std::vector<TExprPtr*> result;
        result.reserve(Args.size());
        for (auto& arg : Args) {
            result.push_back(&arg.Expr);
            if (arg.Width) {
                result.push_back(&arg.Width);
            }
            if (arg.Precision) {
                result.push_back(&arg.Precision);
            }
        }
        return result;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TCastExpr : public TExpr {
    static constexpr const char* NodeId = "Cast";

    TExprPtr  Operand;

    TCastExpr(TLocation loc, TExprPtr operand, TTypePtr target)
        : TExpr(std::move(loc))
        , Operand(std::move(operand))
    {
        Type = target;
    }

    std::vector<TExprPtr> Children() const override {
        return { Operand };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        std::vector<TExprPtr*> result;
        result.push_back(&Operand);
        return result;
    }

    const std::string ToString() const override {
        return std::string("Cast<") + (Type ? std::string(Type->TypeName()) : std::string("?")) + ">";
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TBitcastExpr : public TExpr {
    static constexpr const char* NodeId = "Bitcast";

    TExprPtr Operand;

    TBitcastExpr(TLocation loc, TExprPtr operand, TTypePtr target)
        : TExpr(std::move(loc))
        , Operand(std::move(operand))
    {
        Type = std::move(target);
    }

    std::vector<TExprPtr> Children() const override {
        return { Operand };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        return { &Operand };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

inline TExprPtr MakeCast(TExprPtr e, TTypePtr to) {
    return std::make_shared<TCastExpr>(e->Location, std::move(e), std::move(to));
}

struct TIndexExpr : public TExpr {
    static constexpr const char* NodeId = "Index";

    TExprPtr Collection;
    TExprPtr Index;

    TIndexExpr(TLocation loc, TExprPtr collection, TExprPtr index)
        : TExpr(std::move(loc))
        , Collection(std::move(collection))
        , Index(std::move(index))
    { }

    std::vector<TExprPtr> Children() const override {
        return { Collection, Index };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        return { &Collection, &Index };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TMultiIndexExpr : public TExpr {
    static constexpr const char* NodeId = "MultiIndex";

    TExprPtr Collection;
    std::vector<TExprPtr> Indices;

    TMultiIndexExpr(TLocation loc, TExprPtr collection, std::vector<TExprPtr> indices)
        : TExpr(std::move(loc))
        , Collection(std::move(collection))
        , Indices(std::move(indices))
    { }

    std::vector<TExprPtr> Children() const override {
        std::vector<TExprPtr> result;
        result.reserve(1 + Indices.size());
        result.push_back(Collection);
        for (const auto& index : Indices) {
            result.push_back(index);
        }
        return result;
    }

    std::vector<TExprPtr*> MutableChildren() override {
        std::vector<TExprPtr*> result;
        result.reserve(1 + Indices.size());
        result.push_back(&Collection);
        for (auto& index : Indices) {
            result.push_back(&index);
        }
        return result;
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

struct TSliceExpr : public TExpr {
    static constexpr const char* NodeId = "Slice";

    TExprPtr Collection;
    TExprPtr Start;
    TExprPtr End;

    TSliceExpr(TLocation loc, TExprPtr collection, TExprPtr start, TExprPtr end)
        : TExpr(std::move(loc))
        , Collection(std::move(collection))
        , Start(std::move(start))
        , End(std::move(end))
    { }

    std::vector<TExprPtr> Children() const override {
        return { Collection, Start, End };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        return { &Collection, &Start, &End };
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(IVisitor& visitor) override;
};

class TUseExpr : public TExpr {
public:
    static constexpr const char* NodeId = "Use";
    std::string ModuleName;
    bool Resolved = false;

    explicit TUseExpr(TLocation loc, std::string moduleName)
        : TExpr(std::move(loc))
        , ModuleName(std::move(moduleName))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    const std::string ToString() const override {
        return "use \"" + ModuleName + "\"";
    }

    void Accept(IVisitor& visitor) override;
};

class TAssertStmt : public TExpr {
public:
    static constexpr const char* NodeId = "Assert";
    TExprPtr Expr;

    explicit TAssertStmt(TLocation loc, TExprPtr expr)
        : TExpr(std::move(loc))
        , Expr(std::move(expr))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    std::vector<TExprPtr> Children() const override {
        return { Expr };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        return { &Expr };
    }

    void Accept(IVisitor& visitor) override;
};

class TTypeDeclStmt : public TExpr {
public:
    static constexpr const char* NodeId = "TypeDecl";

    std::vector<TGenericParam> GenericParams;

    explicit TTypeDeclStmt(
        TLocation loc,
        TTypePtr type,
        std::vector<TGenericParam> genericParams = {})
        : TExpr(std::move(loc), std::move(type))
        , GenericParams(std::move(genericParams))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    std::vector<TExprPtr> Children() const override { return {}; }
    std::vector<TExprPtr*> MutableChildren() override { return {}; }

    void Accept(IVisitor& visitor) override;
};

class TFieldAccessExpr : public TExpr {
public:
    static constexpr const char* NodeId = "FieldAccess";
    TExprPtr Object;
    std::string FieldName;
    int FieldIndex = -1;

    TFieldAccessExpr(TLocation loc, TExprPtr object, std::string fieldName)
        : TExpr(std::move(loc))
        , Object(std::move(object))
        , FieldName(std::move(fieldName))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    std::vector<TExprPtr> Children() const override {
        return { Object };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        return { &Object };
    }

    void Accept(IVisitor& visitor) override;
};

// Constructs a struct value from per-field expressions. Type must be set to the struct type.
// Used by inline factories to build struct results without requiring a TVarStmt.
class TStructConstructExpr : public TExpr {
public:
    static constexpr const char* NodeId = "StructConstruct";
    std::vector<TExprPtr> Fields;
    std::vector<std::string> FieldNames;

    TStructConstructExpr(TLocation loc, TTypePtr structType, std::vector<TExprPtr> fields, std::vector<std::string> fieldNames = {})
        : TExpr(std::move(loc))
        , Fields(std::move(fields))
        , FieldNames(std::move(fieldNames))
    {
        Type = std::move(structType);
    }

    const std::string_view NodeName() const override { return NodeId; }

    std::vector<TExprPtr> Children() const override { return Fields; }

    std::vector<TExprPtr*> MutableChildren() override {
        std::vector<TExprPtr*> r;
        for (auto& f : Fields) r.push_back(&f);
        return r;
    }

    void Accept(IVisitor& visitor) override;
};

class TFieldAssignExpr : public TExpr {
public:
    static constexpr const char* NodeId = "FieldAssign";
    TExprPtr Object;
    std::string FieldName;
    int FieldIndex = -1;
    TExprPtr Value;

    TFieldAssignExpr(TLocation loc, TExprPtr object, std::string fieldName, TExprPtr value)
        : TExpr(std::move(loc))
        , Object(std::move(object))
        , FieldName(std::move(fieldName))
        , Value(std::move(value))
    { }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    std::vector<TExprPtr> Children() const override {
        return { Object, Value };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        return { &Object, &Value };
    }

    void Accept(IVisitor& visitor) override;
};

// Lifetime AST nodes.
struct TRetainExpr : TExpr {
    static constexpr const char* NodeId = "Retain";

    TExprPtr Value;

    TRetainExpr(TLocation loc, TExprPtr value)
        : TExpr(std::move(loc), value ? value->Type : nullptr)
        , Value(std::move(value))
    { }

    const std::string_view NodeName() const override { return NodeId; }
    std::vector<TExprPtr> Children() const override { return { Value }; }
    std::vector<TExprPtr*> MutableChildren() override { return { &Value }; }
    void Accept(IVisitor& visitor) override;
};

struct TOwnLiteralExpr : TExpr {
    static constexpr const char* NodeId = "OwnLiteral";

    TExprPtr Value;

    TOwnLiteralExpr(TLocation loc, TExprPtr value)
        : TExpr(std::move(loc), value ? value->Type : nullptr)
        , Value(std::move(value))
    { }

    const std::string_view NodeName() const override { return NodeId; }
    std::vector<TExprPtr> Children() const override { return { Value }; }
    std::vector<TExprPtr*> MutableChildren() override { return { &Value }; }
    void Accept(IVisitor& visitor) override;
};

struct TMoveExpr : TExpr {
    static constexpr const char* NodeId = "Move";

    TExprPtr Value;

    TMoveExpr(TLocation loc, TExprPtr value)
        : TExpr(std::move(loc), value ? value->Type : nullptr)
        , Value(std::move(value))
    { }

    const std::string_view NodeName() const override { return NodeId; }
    std::vector<TExprPtr> Children() const override { return { Value }; }
    std::vector<TExprPtr*> MutableChildren() override { return { &Value }; }
    void Accept(IVisitor& visitor) override;
};

struct TBorrowExpr : TExpr {
    static constexpr const char* NodeId = "Borrow";

    TExprPtr Value;

    TBorrowExpr(TLocation loc, TExprPtr value)
        : TExpr(std::move(loc), value ? value->Type : nullptr)
        , Value(std::move(value))
    { }

    const std::string_view NodeName() const override { return NodeId; }
    std::vector<TExprPtr> Children() const override { return { Value }; }
    std::vector<TExprPtr*> MutableChildren() override { return { &Value }; }
    void Accept(IVisitor& visitor) override;
};

struct TDestroyExpr : TExpr {
    static constexpr const char* NodeId = "Destroy";

    TExprPtr Value;
    TExprPtr Aux;

    TDestroyExpr(TLocation loc, TExprPtr value, TExprPtr aux = nullptr)
        : TExpr(std::move(loc), std::make_shared<TVoidType>())
        , Value(std::move(value))
        , Aux(std::move(aux))
    { }

    const std::string_view NodeName() const override { return NodeId; }

    std::vector<TExprPtr> Children() const override {
        if (Aux) {
            return { Value, Aux };
        }
        return { Value };
    }

    std::vector<TExprPtr*> MutableChildren() override {
        if (Aux) {
            return { &Value, &Aux };
        }
        return { &Value };
    }

    void Accept(IVisitor& visitor) override;
};

struct TReplaceExpr : TExpr {
    static constexpr const char* NodeId = "Replace";

    TExprPtr Target;
    TExprPtr Value;

    TReplaceExpr(TLocation loc, TExprPtr target, TExprPtr value)
        : TExpr(std::move(loc), std::make_shared<TVoidType>())
        , Target(std::move(target))
        , Value(std::move(value))
    { }

    const std::string_view NodeName() const override { return NodeId; }
    std::vector<TExprPtr> Children() const override { return { Target, Value }; }
    std::vector<TExprPtr*> MutableChildren() override { return { &Target, &Value }; }
    void Accept(IVisitor& visitor) override;
};

enum class ECleanupExitKind {
    Break,
    Continue,
    Return,
};

struct TCleanupExitExpr : TExpr {
    static constexpr const char* NodeId = "CleanupExit";

    ECleanupExitKind Kind;
    TExprPtr Value;
    std::vector<TExprPtr> Cleanups;

    TCleanupExitExpr(
        TLocation loc,
        ECleanupExitKind kind,
        TExprPtr value,
        std::vector<TExprPtr> cleanups)
        : TExpr(std::move(loc), std::make_shared<TVoidType>())
        , Kind(kind)
        , Value(std::move(value))
        , Cleanups(std::move(cleanups))
    { }

    const std::string_view NodeName() const override { return NodeId; }

    std::vector<TExprPtr> Children() const override {
        std::vector<TExprPtr> result;
        result.reserve(Cleanups.size() + (Value ? 1 : 0));
        if (Value) {
            result.push_back(Value);
        }
        result.insert(result.end(), Cleanups.begin(), Cleanups.end());
        return result;
    }

    std::vector<TExprPtr*> MutableChildren() override {
        std::vector<TExprPtr*> result;
        result.reserve(Cleanups.size() + (Value ? 1 : 0));
        if (Value) {
            result.push_back(&Value);
        }
        for (auto& cleanup : Cleanups) {
            result.push_back(&cleanup);
        }
        return result;
    }

    void Accept(IVisitor& visitor) override;
};

struct TGlobalCleanupExpr : TExpr {
    static constexpr const char* NodeId = "GlobalCleanup";

    std::vector<TExprPtr> Cleanups;

    TGlobalCleanupExpr(TLocation loc, std::vector<TExprPtr> cleanups)
        : TExpr(std::move(loc), std::make_shared<TVoidType>())
        , Cleanups(std::move(cleanups))
    { }

    const std::string_view NodeName() const override { return NodeId; }
    std::vector<TExprPtr> Children() const override { return Cleanups; }

    std::vector<TExprPtr*> MutableChildren() override {
        std::vector<TExprPtr*> result;
        result.reserve(Cleanups.size());
        for (auto& cleanup : Cleanups) {
            result.push_back(&cleanup);
        }
        return result;
    }

    void Accept(IVisitor& visitor) override;
};
// End lifetime AST nodes.

struct IVisitor {
    virtual ~IVisitor() = default;
    virtual void Visit(TIdentExpr& node) = 0;
    virtual void Visit(TAssignExpr& node) = 0;
    virtual void Visit(TArrayAssignExpr& node) = 0;
    virtual void Visit(TNumberExpr& node) = 0;
    virtual void Visit(TStringLiteralExpr& node) = 0;
    virtual void Visit(TUnaryExpr& node) = 0;
    virtual void Visit(TBinaryExpr& node) = 0;
    virtual void Visit(TBlockExpr& node) = 0;
    virtual void Visit(TIfExpr& node) = 0;
    virtual void Visit(TWhileStmtExpr& node) = 0;
    virtual void Visit(TRepeatStmtExpr& node) = 0;
    virtual void Visit(TForStmtExpr& node) = 0;
    virtual void Visit(TTimesStmtExpr& node) = 0;
    virtual void Visit(TBreakStmt& node) = 0;
    virtual void Visit(TContinueStmt& node) = 0;
    virtual void Visit(TReturnExpr& node) = 0;
    virtual void Visit(TVarStmt& node) = 0;
    virtual void Visit(TVarsBlockExpr& node) = 0;
    virtual void Visit(TFunDecl& node) = 0;
    virtual void Visit(TCallExpr& node) = 0;
    virtual void Visit(TAwaitExpr& node) = 0;
    virtual void Visit(TInputExpr& node) = 0;
    virtual void Visit(TOutputExpr& node) = 0;
    virtual void Visit(TCastExpr& node) = 0;
    virtual void Visit(TBitcastExpr& node) = 0;
    virtual void Visit(TIndexExpr& node) = 0;
    virtual void Visit(TMultiIndexExpr& node) = 0;
    virtual void Visit(TSliceExpr& node) = 0;
    virtual void Visit(TUseExpr& node) = 0;
    virtual void Visit(TAssertStmt& node) = 0;
    virtual void Visit(TTypeDeclStmt& node) = 0;
    virtual void Visit(TFieldAccessExpr& node) = 0;
    virtual void Visit(TStructConstructExpr& node) = 0;
    virtual void Visit(TFieldAssignExpr& node) = 0;
    virtual void Visit(TRetainExpr& node) = 0;
    virtual void Visit(TOwnLiteralExpr& node) = 0;
    virtual void Visit(TMoveExpr& node) = 0;
    virtual void Visit(TBorrowExpr& node) = 0;
    virtual void Visit(TDestroyExpr& node) = 0;
    virtual void Visit(TReplaceExpr& node) = 0;
    virtual void Visit(TCleanupExitExpr& node) = 0;
    virtual void Visit(TGlobalCleanupExpr& node) = 0;
    virtual void Visit(TVectorExpr& node) = 0;

    virtual void VisitOtherwise(TExpr& node) = 0;
     // Add more
};

template<typename TransformFunctor, typename FilterFunctor>
bool TransformAst(TExprPtr& result, TExprPtr node, TransformFunctor f, FilterFunctor filter) {
    if (!node) return false;
    if (!filter(node)) return false;
    bool changed = false;
    for (auto* child : node->MutableChildren()) {
        if (*child) {
            changed |= TransformAst(*child, *child, f, filter);
        }
    }
    result = f(node);
    return changed || result != node;
}

template<typename TransformFunctor, typename FilterFunctor>
bool PreorderTransformAst(TExprPtr& result, TExprPtr node, TransformFunctor f, FilterFunctor filter) {
    if (!node) return false;
    if (!filter(node)) return false;
    bool changed = false;
    result = f(node);
    changed |= result != node;
    if (!changed) {
        // no change, recurse into children of the original node
        for (auto* child : result->MutableChildren()) {
            if (*child) {
                changed |= PreorderTransformAst(*child, *child, f, filter);
            }
        }
    }
    return changed;
}

std::ostream& operator<<(std::ostream& os, const TExprPtr& expr);

} // namespace NAst
} // namespace NQumir
