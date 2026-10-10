#include "transform.h"
#include "qumir/error.h"
#include "qumir/parser/ast.h"
#include "qumir/parser/core/printer.h"

#include <qumir/semantics/type_annotation/type_annotation.h>
#include <qumir/semantics/vector_intrinsics.h>
#include <qumir/semantics/definite_assignment/definite_assignment.h>
#include <qumir/semantics/lifetime/pass.h>
#include <qumir/semantics/lifetime/validator.h>
#include <qumir/semantics/return_normalization/pass.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NQumir {
namespace NTransform {

namespace {

bool IsFileType(const NAst::TTypePtr& type) {
    auto named = NAst::TMaybeType<NAst::TNamedType>(type);
    return named && named.Cast()->Name == "file";
}

std::optional<int> ScalarSizeInBytes(const NAst::TTypePtr& inputType) {
    auto type = NAst::UnwrapNamedType(NAst::UnwrapReferenceType(inputType));
    if (auto integer = NAst::TMaybeType<NAst::TIntegerType>(type)) {
        return integer.Cast()->BitWidth() / 8;
    }
    if (NAst::TMaybeType<NAst::TFloatType>(type)) {
        return 8;
    }
    if (NAst::TMaybeType<NAst::TSymbolType>(type)) {
        return 4;
    }
    if (NAst::TMaybeType<NAst::TPointerType>(type)) {
        return 8;
    }
    if (NAst::TMaybeType<NAst::TStringType>(type)) {
        return 8;
    }
    return std::nullopt;
}

} // namespace

std::expected<bool, TError> PreNameResolutionTransform(NAst::TExprPtr& expr)
{
    bool changed = TransformAst(expr, expr,
        [](const NAst::TExprPtr& node) -> NAst::TExprPtr {
            if (auto maybeAssert = NAst::TMaybeNode<NAst::TAssertStmt>(node)) {
                // rewrite assert statement into a call: __ensure(condition, "condition_text")
                auto assertStmt = maybeAssert.Cast();
                std::ostringstream oss;
                if (assertStmt->Expr) {
                    oss << assertStmt->Expr;
                } else {
                    oss << "<empty>";
                }
                oss << " @ " << assertStmt->Location.ToString();
                std::vector<NAst::TExprPtr> args;
                args.push_back(assertStmt->Expr);
                args.push_back(std::make_shared<NAst::TStringLiteralExpr>(assertStmt->Location, oss.str()));
                return std::make_shared<NAst::TCallExpr>(
                    assertStmt->Location,
                    std::make_shared<NAst::TIdentExpr>(assertStmt->Location, "__ensure"),
                    std::move(args));
            }
            return node;
        },
        [](const NAst::TExprPtr& node) {
            return true;
        });
    return changed;
}

std::expected<bool, TError> ImportPendingCoreUses(NAst::TExprPtr& expr, NSemantics::TNameResolver& context)
{
    std::list<TError> errors;
    bool changed = false;
    TransformAst(expr, expr,
        [&](const NAst::TExprPtr& node) -> NAst::TExprPtr {
            if (auto maybeUse = NAst::TMaybeNode<NAst::TUseExpr>(node)) {
                if (maybeUse.Cast()->Resolved) {
                    return node;
                }
                auto result = context.ImportModule(maybeUse.Cast()->ModuleName);
                if (!result) {
                    errors.push_back(TError(node->Location, result.error()));
                } else if (result.value()) {
                    changed = true;
                }
            }
            return node;
        },
        [](const NAst::TExprPtr& node) {
            return true;
        });
    if (!errors.empty()) {
        return std::unexpected(TError(expr->Location, errors));
    }
    return changed;
}

std::expected<bool, TError> PostTypeAnnotationTransform(NAst::TExprPtr& expr, NSemantics::TNameResolver& context)
{
    std::list<TError> errors;
    bool changed = TransformAst(expr, expr,
        [&](const NAst::TExprPtr& node) -> NAst::TExprPtr {
            if (auto maybeBitcast = NAst::TMaybeNode<NAst::TBitcastExpr>(node)) {
                auto bitcast = maybeBitcast.Cast();
                auto sourceSize = ScalarSizeInBytes(bitcast->Operand->Type);
                auto targetSize = ScalarSizeInBytes(bitcast->Type);
                if (!sourceSize || !targetSize) {
                    errors.push_back(TError(
                        bitcast->Location,
                        "bitcast supports integer, float, symbol, pointer, and string types only"));
                    return node;
                }
                if (*sourceSize != *targetSize) {
                    errors.push_back(TError(bitcast->Location, "bitcast types must have the same size"));
                    return node;
                }

                if (auto maybeNumber = NAst::TMaybeNode<NAst::TNumberExpr>(bitcast->Operand)) {
                    auto number = maybeNumber.Cast();
                    bool sourceIsFloat = NAst::TMaybeType<NAst::TFloatType>(
                        NAst::UnwrapNamedType(number->Type));
                    bool targetIsFloat = NAst::TMaybeType<NAst::TFloatType>(
                        NAst::UnwrapNamedType(bitcast->Type));
                    if (sourceIsFloat && !targetIsFloat) {
                        number->IntValue = std::bit_cast<int64_t>(number->FloatValue);
                    } else if (!sourceIsFloat && targetIsFloat) {
                        number->FloatValue = std::bit_cast<double>(number->IntValue);
                    }
                    number->Type = bitcast->Type;
                    return number;
                }
                return node;
            }

            // Inline substitution: replace TCallExpr to functions with InlineFactory with their AST.
            // Args must be annotated (Type set) so the factory receives typed expressions.
            // The loop then re-runs type annotation on the replacement.
            if (auto maybeCall = NAst::TMaybeNode<NAst::TCallExpr>(node)) {
                auto call = maybeCall.Cast();
                if (call->Type) { // only inline already-annotated calls
                    if (auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(call->Callee)) {
                        auto symId = context.Lookup(maybeIdent.Cast()->Name, NSemantics::TScopeId{0});
                        if (symId) {
                            auto symNode = context.GetSymbolNode(NSemantics::TSymbolId{symId->Id});
                            if (auto maybeFunDecl = NAst::TMaybeNode<NAst::TFunDecl>(symNode)) {
                                auto funDecl = maybeFunDecl.Cast();
                                if (funDecl->InlineFactory) {
                                    bool argsReady = std::all_of(call->Args.begin(), call->Args.end(),
                                        [](const NAst::TExprPtr& a){ return a && a->Type; });
                                    if (argsReady) {
                                        return (*funDecl->InlineFactory)(call->Args);
                                    }
                                }
                            }
                        }
                    }
                }
                return node;
            }

            // Transform string element assignment: s[i] = 'c' => s = str_replace_sym(s, 'c', i)
            if (auto maybeArrayAssign = NAst::TMaybeNode<NAst::TArrayAssignExpr>(node)) {
                auto arrayAssign = maybeArrayAssign.Cast();
                auto unrefType = UnwrapReferenceType(arrayAssign->Value->Type);
                auto maybeSymbolType = NAst::TMaybeType<NAst::TSymbolType>(unrefType);
                if (!maybeSymbolType) {
                    return node;
                }
                if (arrayAssign->Indices.size() != 1) {
                    errors.push_back(TError(arrayAssign->Location, "string element assignment must have exactly one index"));
                    return node;
                }
                auto call = std::make_shared<NAst::TCallExpr>(arrayAssign->Location,
                    std::make_shared<NAst::TIdentExpr>(arrayAssign->Location, "str_replace_sym"),
                    std::vector<NAst::TExprPtr>{
                        std::make_shared<NAst::TIdentExpr>(arrayAssign->Location, arrayAssign->Name),
                        arrayAssign->Value,
                        arrayAssign->Indices[0]});

                auto assign = std::make_shared<NAst::TAssignExpr>(arrayAssign->Location,
                    arrayAssign->Name,
                    call);
                return assign;
            }
            if (auto maybeBinary = NAst::TMaybeNode<NAst::TBinaryExpr>(node)) {
                auto binary = maybeBinary.Cast();

                auto leftType = UnwrapReferenceType(binary->Left->Type);
                auto rightType = UnwrapReferenceType(binary->Right->Type);
                bool leftStr = NAst::TMaybeType<NAst::TStringType>(leftType);
                bool rightStr = NAst::TMaybeType<NAst::TStringType>(rightType);

                if (leftStr || rightStr) {
                    // string + string (string + symbol handled with implicit cast)
                    if (binary->Operator == NAst::TOperator("+")) {
                        auto call = std::make_shared<NAst::TCallExpr>(binary->Location,
                            std::make_shared<NAst::TIdentExpr>(binary->Location, "str_concat"),
                            std::vector<NAst::TExprPtr>{binary->Left, binary->Right});
                        return call;
                    }
                    if ((binary->Operator == "==" || binary->Operator == "!=" || binary->Operator == "<=" || binary->Operator == ">=" || binary->Operator == "<" || binary->Operator == ">")) {
                        auto call = std::make_shared<NAst::TCallExpr>(binary->Location,
                            std::make_shared<NAst::TIdentExpr>(binary->Location, "str_compare"),
                            std::vector<NAst::TExprPtr>{binary->Left, binary->Right});
                        std::vector<NAst::TExprPtr> args;
                        args.push_back(call);
                        args.push_back(std::make_shared<NAst::TNumberExpr>(binary->Location, (int64_t)0));
                        return std::make_shared<NAst::TBinaryExpr>(binary->Location, binary->Operator, args[0], args[1]);
                    }
                }

            } else if (auto maybeOutput = NAst::TMaybeNode<NAst::TOutputExpr>(node)) {
                auto output = maybeOutput.Cast();
                // transform output(a, b, c) into a series of calls to output_xxx functions
                std::vector<NAst::TExprPtr> stmts;
                if (output->Args.size() == 0) {
                    errors.push_back(TError(output->Location, "output requires at least one argument"));
                    return node;
                }
                int i = 0;
                const auto& arg0 = output->Args[0];
                bool hasFileArg = false;
                if (IsFileType(arg0.Expr->Type)) {
                    // first argument is a file, set output file
                    auto call = std::make_shared<NAst::TCallExpr>(
                        output->Location,
                        std::make_shared<NAst::TIdentExpr>(output->Location, "output_set_file"),
                        std::vector<NAst::TExprPtr>{ arg0.Expr });
                    stmts.push_back(call);
                    hasFileArg = true;
                    i = 1;
                }

                for (; i < (int)output->Args.size(); ++i) {
                    const auto& arg = output->Args[i];
                    NAst::TExprPtr call;
                    auto type = arg.Expr->Type;
                    if (auto refType = NAst::TMaybeType<NAst::TReferenceType>(type)) {
                        type = refType.Cast()->ReferencedType;
                    }
                    auto width = arg.Width;
                    auto prec = arg.Precision;
                    if (width) {
                        if (!NAst::TMaybeType<NAst::TIntegerType>(width->Type)) {
                            errors.push_back(TError(width->Location, "width argument must be of integer type"));
                            return node;
                        }
                    }
                    if (prec) {
                        if (!NAst::TMaybeType<NAst::TIntegerType>(prec->Type)) {
                            errors.push_back(TError(prec->Location, "precision argument must be of integer type"));
                            return node;
                        }
                    }
                    if (NAst::TMaybeType<NAst::TFloatType>(type)) {
                        std::vector<NAst::TExprPtr> args;
                        args.push_back(arg.Expr);
                        if (width) {
                            args.push_back(width);
                        } else {
                            args.push_back(std::make_shared<NAst::TNumberExpr>(output->Location, (int64_t)0));
                        }
                        if (prec) {
                            args.push_back(prec);
                        } else {
                            args.push_back(std::make_shared<NAst::TNumberExpr>(output->Location, (int64_t)-1));
                        }
                        call = std::make_shared<NAst::TCallExpr>(
                            output->Location,
                            std::make_shared<NAst::TIdentExpr>(output->Location, "output_double"),
                            std::move(args));
                    } else if (NAst::TMaybeType<NAst::TIntegerType>(type)) {
                        std::vector<NAst::TExprPtr> args;
                        args.push_back(arg.Expr);
                        if (width) {
                            args.push_back(width);
                        } else {
                            args.push_back(std::make_shared<NAst::TNumberExpr>(output->Location, (int64_t)0));
                        }
                        if (prec) {
                            errors.push_back(TError(prec->Location, "precision argument is not applicable for integer output"));
                            return node;
                        }
                        call = std::make_shared<NAst::TCallExpr>(
                            output->Location,
                            std::make_shared<NAst::TIdentExpr>(output->Location, "output_int64"),
                            std::move(args));
                    } else if (NAst::TMaybeType<NAst::TBoolType>(type)) {
                        if (width || prec) {
                            errors.push_back(TError(output->Location, "width and precision arguments are not applicable for boolean output"));
                            return node;
                        }
                        std::vector<NAst::TExprPtr> args;
                        args.push_back(arg.Expr);
                        call = std::make_shared<NAst::TCallExpr>(
                            output->Location,
                            std::make_shared<NAst::TIdentExpr>(output->Location, "output_bool"),
                            std::move(args));
                    } else if (NAst::TMaybeType<NAst::TStringType>(type)) {
                        if (width || prec) {
                            errors.push_back(TError(output->Location, "width and precision arguments are not applicable for string output"));
                            return node;
                        }
                        std::vector<NAst::TExprPtr> args;
                        args.push_back(arg.Expr);
                        call = std::make_shared<NAst::TCallExpr>(
                            output->Location,
                            std::make_shared<NAst::TIdentExpr>(output->Location, "output_string"),
                            std::move(args));
                    } else if (NAst::TMaybeType<NAst::TSymbolType>(type)) {
                        if (width || prec) {
                            errors.push_back(TError(output->Location, "width and precision arguments are not applicable for symbol output"));
                            return node;
                        }
                        std::vector<NAst::TExprPtr> args;
                        args.push_back(arg.Expr);
                        call = std::make_shared<NAst::TCallExpr>(
                            output->Location,
                            std::make_shared<NAst::TIdentExpr>(output->Location, "output_symbol"),
                            std::move(args));
                    } else if (NAst::TMaybeType<NAst::TNamedType>(type) || NAst::TMaybeType<NAst::TVectorType>(type)) {
                        if (width || prec) {
                            errors.push_back(TError(output->Location, "width and precision arguments are not applicable for custom output"));
                            return node;
                        }
                        auto op = context.GetUnaryOp("print", type);
                        if (!op) {
                            errors.push_back(TError(arg.Expr->Location, "тип '" + NAst::TypeDiagnosticName(type) + "' не поддерживает вывод"));
                            return node;
                        }
                        call = std::make_shared<NAst::TCallExpr>(
                            output->Location,
                            std::make_shared<NAst::TIdentExpr>(output->Location, op->SynthName),
                            std::vector<NAst::TExprPtr>{ arg.Expr });
                    } else {
                        // need annotation pass to know types
                        return node;
                    }
                    stmts.push_back(call);
                }

                if (hasFileArg) {
                    // reset file after output
                    auto resetFileCall = std::make_shared<NAst::TCallExpr>(
                        output->Location,
                        std::make_shared<NAst::TIdentExpr>(output->Location, "output_reset_file"),
                        std::vector<NAst::TExprPtr>{});
                    stmts.push_back(resetFileCall);
                }

                return std::make_shared<NAst::TBlockExpr>(output->Location, std::move(stmts));
            } else if (auto maybeInput = NAst::TMaybeNode<NAst::TInputExpr>(node)) {
                auto input = maybeInput.Cast();
                if (input->Args.size() == 0) {
                    errors.push_back(TError(input->Location, "input requires at least one argument"));
                    return node;
                }
                // transform input(type) into a call to input_xxx function
                std::vector<NAst::TExprPtr> stmts;
                int i = 0;
                const auto& arg0 = input->Args[0];
                bool hasFileArg = false;
                if (IsFileType(arg0->Type)) {
                    auto call = std::make_shared<NAst::TCallExpr>(
                        input->Location,
                        std::make_shared<NAst::TIdentExpr>(input->Location, "input_set_file"),
                        std::vector<NAst::TExprPtr>{arg0});
                    stmts.push_back(call);
                    hasFileArg = true;
                    i++;
                }

                for (; i < (int)input->Args.size(); ++i) {
                    const auto& arg = input->Args[i];
                    NAst::TExprPtr call;
                    auto type = arg->Type;
                    if (NAst::TMaybeType<NAst::TFloatType>(type)) {
                        call = std::make_shared<NAst::TCallExpr>(
                            input->Location,
                            std::make_shared<NAst::TIdentExpr>(input->Location, "input_double"),
                            std::vector<NAst::TExprPtr>{});
                    } else if (NAst::TMaybeType<NAst::TIntegerType>(type)) {
                        call = std::make_shared<NAst::TCallExpr>(
                            input->Location,
                            std::make_shared<NAst::TIdentExpr>(input->Location, "input_int64"),
                            std::vector<NAst::TExprPtr>{});
                    } else if (NAst::TMaybeType<NAst::TStringType>(type)) {
                        call = std::make_shared<NAst::TCallExpr>(
                            input->Location,
                            std::make_shared<NAst::TIdentExpr>(input->Location, "str_input"),
                            std::vector<NAst::TExprPtr>{});
                    } else {
                        errors.push_back(TError(arg->Location, "input argument must be float or int64, got: " + type->ToString()));
                    }

                    if (auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(arg)) {
                        auto ident = maybeIdent.Cast();
                        // assign to ident
                        call = std::make_shared<NAst::TAssignExpr>(
                            input->Location,
                            ident->Name,
                            call);
                    } else if (auto maybeIndex = NAst::TMaybeNode<NAst::TIndexExpr>(arg)) {
                        auto index = maybeIndex.Cast();
                        auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(index->Collection);
                        if (!maybeIdent) {
                            errors.push_back(TError(arg->Location, "input argument index expression must index an identifier"));
                            continue;
                        }
                        auto ident = maybeIdent.Cast();
                        // assign to index
                        call = std::make_shared<NAst::TArrayAssignExpr>(
                            input->Location,
                            ident->Name,
                            std::vector<NAst::TExprPtr>{index->Index},
                            call);
                    } else if (auto maybeMultiIndex = NAst::TMaybeNode<NAst::TMultiIndexExpr>(arg)) {
                        auto multiIndex = maybeMultiIndex.Cast();
                        auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(multiIndex->Collection);
                        if (!maybeIdent) {
                            errors.push_back(TError(arg->Location, "input argument multi-index expression must index an identifier"));
                            continue;
                        }
                        auto ident = maybeIdent.Cast();
                        // assign to multi-index
                        call = std::make_shared<NAst::TArrayAssignExpr>(
                            input->Location,
                            ident->Name,
                            multiIndex->Indices,
                            call);
                    } else {
                        errors.push_back(TError(arg->Location, "input argument must be an identifier or an index expression"));
                    }

                    stmts.push_back(call);
                }
                if (hasFileArg) {
                    // reset file after input
                    auto resetFileCall = std::make_shared<NAst::TCallExpr>(
                        input->Location,
                        std::make_shared<NAst::TIdentExpr>(input->Location, "input_reset_file"),
                        std::vector<NAst::TExprPtr>{});
                    stmts.push_back(resetFileCall);
                }

                return std::make_shared<NAst::TBlockExpr>(input->Location, std::move(stmts));
            } else if (auto maybeIndex = NAst::TMaybeNode<NAst::TIndexExpr>(node)) {
                auto index = maybeIndex.Cast();
                auto collectionType = UnwrapReferenceType(index->Collection->Type);
                if (NAst::TMaybeType<NAst::TStringType>(collectionType)) {
                    // rewrite to str_symbol_at(collection, index)
                    auto funcNameIdent = std::make_shared<NAst::TIdentExpr>(index->Location, "str_symbol_at");
                    auto symbolAt = std::make_shared<NAst::TCallExpr>(index->Location, funcNameIdent, std::vector<NAst::TExprPtr>{
                        index->Collection,
                        index->Index,
                    });
                    symbolAt->Type = index->Type;
                    return symbolAt;
                }
            } else if (auto maybeSlice = NAst::TMaybeNode<NAst::TSliceExpr>(node)) {
                auto slice = maybeSlice.Cast();
                auto collectionType = UnwrapReferenceType(slice->Collection->Type);
                if (NAst::TMaybeType<NAst::TStringType>(collectionType)) {
                    // rewrite to str_slice(collection, start, end)
                    auto funcNameIdent = std::make_shared<NAst::TIdentExpr>(slice->Location, "str_slice");
                    auto sliceCall = std::make_shared<NAst::TCallExpr>(slice->Location, funcNameIdent, std::vector<NAst::TExprPtr>{
                        slice->Collection,
                        slice->Start,
                        slice->End
                    });
                    sliceCall->Type = slice->Type;
                    return sliceCall;
                }
            } else if (auto maybeCast = NAst::TMaybeNode<NAst::TCastExpr>(node)) {
                auto cast = maybeCast.Cast();
                if (NAst::TMaybeType<NAst::TStringType>(cast->Type) && NAst::TMaybeType<NAst::TSymbolType>(cast->Operand->Type)) {
                    // symbol -> string cast
                    auto funcNameIdent = std::make_shared<NAst::TIdentExpr>(cast->Location, "str_from_unicode");
                    auto castCall = std::make_shared<NAst::TCallExpr>(cast->Location, funcNameIdent, std::vector<NAst::TExprPtr>{
                        cast->Operand
                    });
                    castCall->Type = cast->Type;
                    return castCall;
                }
            } else if (auto maybeBlock = NAst::TMaybeNode<NAst::TBlockExpr>(node)) {
                auto block = maybeBlock.Cast();
                bool blockIsExpr = block->Type && !NAst::TMaybeType<NAst::TVoidType>(block->Type);
                for (size_t i = 0; i < block->Stmts.size(); ++i) {
                    auto& stmt = block->Stmts[i];
                    bool isLast = (i + 1 == block->Stmts.size());
                    if (blockIsExpr && isLast) {
                        continue; // last stmt is the block's return value
                    }
                    if (auto maybeFunDecl = NAst::TMaybeNode<NAst::TFunDecl>(stmt)) {
                        continue;
                    }
                    if (auto maybeVarDecl = NAst::TMaybeNode<NAst::TVarStmt>(stmt)) {
                        continue;
                    }
                    if (auto maybeTypeDecl = NAst::TMaybeNode<NAst::TTypeDeclStmt>(stmt)) {
                        continue;
                    }
                    if (stmt->Type) {
                        auto maybeVoidType = NAst::TMaybeType<NAst::TVoidType>(stmt->Type);
                        auto futureResultType = NAst::FutureResultType(stmt->Type);
                        bool isFutureVoid = futureResultType && NAst::TMaybeType<NAst::TVoidType>(futureResultType);
                        if (!maybeVoidType && !isFutureVoid) {
                            errors.push_back(TError(stmt->Location, "выражение, возвращающее результат, должно быть присвоено переменной или использовано"));
                        }
                    }
                }
            }
            return node;
        },
        [](const NAst::TExprPtr& node) {
            auto function = NAst::TMaybeNode<NAst::TFunDecl>(node);
            return !function || function.Cast()->GenericParams.empty();
        });

    if (!errors.empty()) {
        return std::unexpected(TError(expr->Location, errors));
    }
    return changed;
}

// replace 'ident' with 'ident()' if ident is a function with no parameters
std::expected<bool, TError> PostNameResolutionTransform(NAst::TExprPtr& expr, NSemantics::TNameResolver& context) {
    std::list<TError> errors;
    int scopeId = -1;
    // Need PreorderTransformAst for scope tracking: update scopeId when entering a block
    // TODO: implement scrope tracking in TransformAst and use it here

    std::unordered_set<const NAst::TExpr*> callCallees;
    auto collectCallCallees = [&](auto&& self, const NAst::TExprPtr& node) -> void {
        if (!node) {
            return;
        }
        if (auto maybeCall = NAst::TMaybeNode<NAst::TCallExpr>(node)) {
            auto call = maybeCall.Cast();
            if (call->Callee) {
                callCallees.insert(call->Callee.get());
            }
        }
        for (const auto& child : node->Children()) {
            self(self, child);
        }
    };
    collectCallCallees(collectCallCallees, expr);

    bool changed = PreorderTransformAst(expr, expr,
        [&](const NAst::TExprPtr& node) -> NAst::TExprPtr {
            if (auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(node)) {
                if (scopeId == -1) {
                    return node;
                }

                auto ident = maybeIdent.Cast();
                auto symbolId = context.Lookup(ident->Name, NSemantics::TScopeId{scopeId});
                if (!symbolId) {
                    if (callCallees.contains(node.get()) && !NSemantics::VectorIntrinsic(ident->Name).empty()) {
                        return node;
                    }
                    if (!context.LookupOverloads(ident->Name, NSemantics::TScopeId{scopeId}).empty()) {
                        return node;
                    }
                    auto suggestion = context.Suggest(ident->Name, NSemantics::TScopeId{scopeId}, /*includeFunctions=*/ true);
                    auto suggestionMsg = suggestion ? suggestion->ToString() : "";
                    errors.push_back(TError(ident->Location, "Идентификатор '" + ident->Name + "' не определён." + suggestionMsg));
                    return node;
                }
                auto sym = context.GetSymbolNode(NSemantics::TSymbolId{symbolId->Id});
                if (!sym) {
                    errors.push_back(TError(ident->Location, "Некорректный идентификатор: '" + ident->Name + "'."));
                    return node;
                }
                if (auto maybeFun = NAst::TMaybeNode<NAst::TFunDecl>(sym)) {
                    auto fun = maybeFun.Cast();
                    if (fun->Params.empty() && !callCallees.contains(node.get())) {
                        // function call without brackets
                        auto call = std::make_shared<NAst::TCallExpr>(ident->Location, ident, std::vector<NAst::TExprPtr>{});
                        call->Type = fun->RetType;
                        return call;
                    }
                }
                return node;
            } else if (auto maybeBlock = NAst::TMaybeNode<NAst::TBlockExpr>(node)) {
                auto block = maybeBlock.Cast();
                scopeId = block->Scope;
            }
            return node;
        },
        [](const NAst::TExprPtr& node) {
            auto function = NAst::TMaybeNode<NAst::TFunDecl>(node);
            return !function || function.Cast()->GenericParams.empty();
        });
    if (!errors.empty()) {
        return std::unexpected(TError(expr->Location, errors));
    }
    return changed;
}

namespace {

void RefreshFunctionType(const std::shared_ptr<NAst::TFunDecl>& funDecl)
{
    std::vector<NAst::TTypePtr> params;
    params.reserve(funDecl->Params.size());
    for (const auto& param : funDecl->Params) {
        params.push_back(param->Type);
    }
    funDecl->Type = std::make_shared<NAst::TFunctionType>(std::move(params), funDecl->RetType);
}

struct TCoroutineAnalysis {
    NSemantics::TNameResolver& Context;
    bool Changed = false;
    std::vector<std::shared_ptr<NAst::TFunDecl>> Functions;
    std::unordered_set<NAst::TFunDecl*> UserFunctions;
    std::unordered_map<NAst::TFunDecl*, std::shared_ptr<NAst::TFunDecl>> FunctionPtrs;
    std::unordered_set<NAst::TFunDecl*> DirectCoroutineFunctions;
    std::unordered_set<NAst::TFunDecl*> CoroutineFunctions;
    std::unordered_map<NAst::TFunDecl*, std::unordered_set<NAst::TFunDecl*>> Calls;

    void CollectUserFunctions(const NAst::TExprPtr& expr)
    {
        if (!expr) {
            return;
        }
        if (auto maybeFun = NAst::TMaybeNode<NAst::TFunDecl>(expr)) {
            auto fun = maybeFun.Cast();
            if (fun->Body) {
                Functions.push_back(fun);
                UserFunctions.insert(fun.get());
                FunctionPtrs[fun.get()] = fun;
            }
            return;
        }
        for (const auto& child : expr->Children()) {
            CollectUserFunctions(child);
        }
    }

    bool EnsureFutureReturn(const std::shared_ptr<NAst::TFunDecl>& funDecl)
    {
        if (!funDecl || NAst::IsFutureType(funDecl->RetType)) {
            return false;
        }
        funDecl->RetType = NAst::WrapFutureType(funDecl->RetType);
        RefreshFunctionType(funDecl);
        return true;
    }

    std::shared_ptr<NAst::TFunDecl> LookupFunction(const std::shared_ptr<NAst::TIdentExpr>& ident, NSemantics::TScopeId scopeId)
    {
        auto symbolId = Context.Lookup(ident->Name, scopeId);
        if (!symbolId) {
            return nullptr;
        }
        auto symbolNode = Context.GetSymbolNode(NSemantics::TSymbolId{symbolId->Id});
        return NAst::TMaybeNode<NAst::TFunDecl>(symbolNode).Cast();
    }

    void AnalyzeFunction(const std::shared_ptr<NAst::TFunDecl>& funDecl)
    {
        if (!funDecl->Body) {
            return;
        }
        AnalyzeExpr(funDecl->Body, funDecl.get(), NSemantics::TScopeId{funDecl->Body->Scope});
    }

    void AnalyzeExpr(const NAst::TExprPtr& expr, NAst::TFunDecl* currentFunction, NSemantics::TScopeId scopeId)
    {
        if (!expr || !currentFunction) {
            return;
        }
        if (auto maybeFun = NAst::TMaybeNode<NAst::TFunDecl>(expr)) {
            if (maybeFun.Cast().get() != currentFunction) {
                return;
            }
        }
        if (auto maybeBlock = NAst::TMaybeNode<NAst::TBlockExpr>(expr)) {
            auto block = maybeBlock.Cast();
            if (block->Scope >= 0) {
                scopeId = NSemantics::TScopeId{block->Scope};
            }
        } else if (auto maybeCall = NAst::TMaybeNode<NAst::TCallExpr>(expr)) {
            auto call = maybeCall.Cast();
            if (auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(call->Callee)) {
                if (auto callee = LookupFunction(maybeIdent.Cast(), scopeId)) {
                    if (NAst::IsFutureType(callee->RetType)) {
                        DirectCoroutineFunctions.insert(currentFunction);
                    }
                    if (callee->Body) {
                        Calls[currentFunction].insert(callee.get());
                    }
                }
            }
        }
        for (const auto& child : expr->Children()) {
            AnalyzeExpr(child, currentFunction, scopeId);
        }
    }

    void Propagate()
    {
        CoroutineFunctions = DirectCoroutineFunctions;
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& [caller, callees] : Calls) {
                if (CoroutineFunctions.contains(caller)) {
                    continue;
                }
                for (auto* callee : callees) {
                    if (CoroutineFunctions.contains(callee)) {
                        CoroutineFunctions.insert(caller);
                        changed = true;
                        break;
                    }
                }
            }
        }
    }

    void ApplyFunctionTypes()
    {
        for (auto* rawFun : CoroutineFunctions) {
            if (!UserFunctions.contains(rawFun)) {
                continue;
            }
            auto it = FunctionPtrs.find(rawFun);
            if (it == FunctionPtrs.end()) {
                continue;
            }
            Changed = EnsureFutureReturn(it->second) || Changed;
        }
    }

    bool IsSuspendCall(const std::shared_ptr<NAst::TCallExpr>& call, NSemantics::TScopeId scopeId)
    {
        if (auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(call->Callee)) {
            if (auto callee = LookupFunction(maybeIdent.Cast(), scopeId)) {
                return NAst::IsFutureType(callee->RetType);
            }
        }
        return false;
    }

    void MarkAwaitCalls(NAst::TExprPtr& expr, NSemantics::TScopeId scopeId)
    {
        if (!expr) {
            return;
        }
        if (NAst::TMaybeNode<NAst::TAwaitExpr>(expr)) {
            return;
        }
        if (auto maybeFun = NAst::TMaybeNode<NAst::TFunDecl>(expr)) {
            auto fun = maybeFun.Cast();
            if (!fun->Body) {
                return;
            }
            NAst::TExprPtr body = fun->Body;
            MarkAwaitCalls(body, NSemantics::TScopeId{fun->Body->Scope});
            fun->Body = NAst::TMaybeNode<NAst::TBlockExpr>(body).Cast();
            return;
        }
        if (auto maybeBlock = NAst::TMaybeNode<NAst::TBlockExpr>(expr)) {
            auto block = maybeBlock.Cast();
            if (block->Scope >= 0) {
                scopeId = NSemantics::TScopeId{block->Scope};
            }
        } else if (auto maybeCall = NAst::TMaybeNode<NAst::TCallExpr>(expr)) {
            auto call = maybeCall.Cast();
            for (auto* child : call->MutableChildren()) {
                MarkAwaitCalls(*child, scopeId);
            }
            if (IsSuspendCall(call, scopeId)) {
                expr = std::make_shared<NAst::TAwaitExpr>(call->Location, expr);
                Changed = true;
            }
            return;
        }
        for (auto* child : expr->MutableChildren()) {
            MarkAwaitCalls(*child, scopeId);
        }
    }
};

} // namespace

std::expected<bool, TError> CoroutineAnnotationTransform(NAst::TExprPtr& expr, NSemantics::TNameResolver& context)
{
    TCoroutineAnalysis analysis{context};
    analysis.CollectUserFunctions(expr);
    for (const auto& function : analysis.Functions) {
        analysis.AnalyzeFunction(function);
    }
    analysis.Propagate();
    analysis.ApplyFunctionTypes();
    analysis.MarkAwaitCalls(expr, NSemantics::TScopeId{0});
    return analysis.Changed;
}

namespace {

std::expected<bool, TError> RunSourceNameResolution(
    NAst::TExprPtr& expr,
    NSemantics::TNameResolver& context,
    const TPipelineExtensions& extensions)
{
    bool changed = false;
    auto preResult = PreNameResolutionTransform(expr);
    if (!preResult) {
        return std::unexpected(preResult.error());
    }
    changed = preResult.value();

    for (const auto& pass : extensions.BeforeNameResolution) {
        auto result = pass(expr, context);
        if (!result) {
            return std::unexpected(result.error());
        }
        changed = result.value() || changed;
    }

    auto importResult = ImportPendingCoreUses(expr, context);
    if (!importResult) {
        return std::unexpected(importResult.error());
    }
    changed = importResult.value() || changed;

    if (auto error = context.Resolve(expr)) {
        return std::unexpected(*error);
    }

    auto postResult = PostNameResolutionTransform(expr, context);
    if (!postResult) {
        return std::unexpected(postResult.error());
    }
    changed = postResult.value() || changed;

    for (const auto& pass : extensions.AfterNameResolution) {
        auto result = pass(expr, context);
        if (!result) {
            return std::unexpected(result.error());
        }
        changed = result.value() || changed;
    }
    return changed;
}

} // namespace

std::expected<std::monostate, TError> RunSourceTransformFixpoint(
    NAst::TExprPtr& expr,
    NSemantics::TNameResolver& context,
    TPipelineOptions options)
{
    static constexpr int MaxIterations = 10;

    auto initialNameResolution = RunSourceNameResolution(
        expr,
        context,
        options.Extensions);
    if (!initialNameResolution) {
        return std::unexpected(initialNameResolution.error());
    }

    NTypeAnnotation::TTypeAnnotator annotator(context);
    for (int iteration = 0; iteration < MaxIterations; ++iteration) {
        auto annotationResult = annotator.Annotate(expr);
        if (!annotationResult) {
            return std::unexpected(annotationResult.error());
        }
        expr = std::move(annotationResult.value());

        auto postResult = PostTypeAnnotationTransform(expr, context);
        if (!postResult) {
            return std::unexpected(postResult.error());
        }
        bool changed = postResult.value();

        for (const auto& pass : options.Extensions.AfterTypeAnnotation) {
            auto result = pass(expr, context);
            if (!result) {
                return std::unexpected(result.error());
            }
            changed = result.value() || changed;
        }

        auto nameResolution = RunSourceNameResolution(
            expr,
            context,
            options.Extensions);
        if (!nameResolution) {
            return std::unexpected(nameResolution.error());
        }
        changed = nameResolution.value() || changed;
        if (!changed) {
            return std::monostate{};
        }
    }

    return std::unexpected(TError(expr->Location, "too many iterations in transform pipeline"));
}

std::expected<std::monostate, TError> FinalNameResolution(
    NAst::TExprPtr& expr,
    NSemantics::TNameResolver& context)
{
    if (auto error = context.Resolve(expr)) {
        return std::unexpected(*error);
    }
    return std::monostate{};
}

std::expected<std::monostate, TError> FinalTypeAnnotation(
    NAst::TExprPtr& expr,
    NSemantics::TNameResolver& context)
{
    NTypeAnnotation::TTypeAnnotator annotator(context);
    auto result = annotator.Annotate(expr);
    if (!result) {
        return std::unexpected(result.error());
    }
    expr = std::move(result.value());
    return std::monostate{};
}

std::expected<std::monostate, TError> RunFinalSemanticPipeline(
    NAst::TExprPtr& expr,
    NSemantics::TNameResolver& context)
{
    auto returnResult = NSemantics::ReturnNormalizationPass(expr, context);
    if (!returnResult) {
        return std::unexpected(returnResult.error());
    }

    NSemantics::TSyntheticNameGenerator syntheticNames(context, expr);
    auto lifetimeResult = NSemantics::LifetimePass(expr, context, syntheticNames);
    if (!lifetimeResult) {
        return std::unexpected(lifetimeResult.error());
    }

    if (auto result = FinalNameResolution(expr, context); !result) {
        return result;
    }
    if (auto result = FinalTypeAnnotation(expr, context); !result) {
        return result;
    }

    NSemantics::TLifetimeValidator validator(context);
    if (auto result = validator.Validate(expr); !result) {
        return std::unexpected(result.error());
    }
    return std::monostate{};
}

std::expected<std::monostate, TError> Pipeline(
    NAst::TExprPtr& expr,
    NSemantics::TNameResolver& context,
    TPipelineOptions options)
{
    if (auto result = RunSourceTransformFixpoint(expr, context, options); !result) {
        return result;
    }

    if (options.RunDefiniteAssignment) {
        NSemantics::TDefiniteAssignmentChecker definiteAssignmentChecker(context);
        if (auto result = definiteAssignmentChecker.Check(expr); !result) {
            return std::unexpected(result.error());
        }
    }

    return RunFinalSemanticPipeline(expr, context);
}

} // namespace NTransform
} // namespace NQumir
