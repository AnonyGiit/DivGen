#include "DeadInstrumenter.hpp"

#include "clang/Tooling/Transformer/SourceCode.h"
#include <clang/AST/Decl.h>
#include <clang/AST/Stmt.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/ASTMatchers/ASTMatchersMacros.h>
#include <clang/Basic/TokenKinds.h>
#include <clang/Lex/Lexer.h>
#include <clang/Sema/Ownership.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Transformer/MatchConsumer.h>
#include <clang/Tooling/Transformer/RewriteRule.h>
#include <limits>
#include <llvm/ADT/Any.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Error.h>
#include <memory>
#include <sstream>
#include <string>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace clang::transformer;
using namespace clang::transformer::detail;

namespace dead {

namespace {
enum class EditMetadataKind { MarkerDecl, MarkerCall };
std::string GetFilenameFromRange(const CharSourceRange &R,
                                 const SourceManager &SM) {
    const std::pair<FileID, unsigned> DecomposedLocation =
        SM.getDecomposedLoc(SM.getSpellingLoc(R.getBegin()));
    const FileEntry *Entry = SM.getFileEntryForID(DecomposedLocation.first);
    return std::string(Entry ? Entry->getName() : "");
}
} // namespace

void detail::RuleActionEditCollector::run(
    const clang::ast_matchers::MatchFinder::MatchResult &Result) {
    if (Result.Context->getDiagnostics().hasErrorOccurred()) {
        llvm::errs() << "An error has occured.\n";
        return;
    }
    Expected<SmallVector<transformer::Edit, 1>> Edits =
        findSelectedCase(Result, Rule).Edits(Result);
    if (!Edits) {
        llvm::errs() << "Rewrite failed: " << llvm::toString(Edits.takeError())
                     << "\n";
        return;
    }
    auto SM = Result.SourceManager;
    for (const auto &T : *Edits) {
        assert(T.Kind == transformer::EditKind::Range);
        bool isMarker = llvm::any_isa<EditMetadataKind>(T.Metadata)
                            ? (llvm::any_cast<EditMetadataKind>(T.Metadata) ==
                               EditMetadataKind::MarkerCall

                               )
                            : false;
        if (isMarker) {
            auto N =
                FileToNumberMarkerDecls[GetFilenameFromRange(T.Range, *SM)]++;
            Replacements.emplace_back(*SM, T.Range,
                                      T.Replacement + "marker_" +
                                          std::to_string(N) + "();");
        } else
            Replacements.emplace_back(*SM, T.Range, T.Replacement);
    }
}

void detail::RuleActionEditCollector::registerMatchers(
    clang::ast_matchers::MatchFinder &Finder) {
    for (auto &Matcher : buildMatchers(Rule))
        Finder.addDynamicMatcher(Matcher, this);
}

namespace {

AST_MATCHER(VarDecl, isExtern) {
    (void)Finder;
    (void)Builder;
    return Node.hasExternalStorage();
}

AST_MATCHER(Stmt, notInMacro) {
    (void)Finder;
    (void)Builder;
    return !Node.getBeginLoc().isMacroID() && !Node.getEndLoc().isMacroID();
}

Expected<DynTypedNode> getNode(const ast_matchers::BoundNodes &Nodes,
                               StringRef ID) {
    auto &NodesMap = Nodes.getMap();
    auto It = NodesMap.find(ID);
    if (It == NodesMap.end())
        return llvm::make_error<llvm::StringError>(llvm::errc::invalid_argument,
                                                   ID + "not bound");
    return It->second;
}

ASTEdit addMarker(ASTEdit Edit) {
    return withMetadata(
        Edit,
        [](const clang::ast_matchers::MatchFinder::MatchResult &)
            -> EditMetadataKind { return EditMetadataKind::MarkerCall; });
}

template <typename T>
SourceLocation handleReturnStmts(const T &Node, SourceLocation End,
                                 const SourceManager &SM) {
    if (const auto &Ret = Node.template get<ReturnStmt>()) {
        if (Ret->getRetValue())
            return End;
        // ReturnStmts without an Expr have broken range...
        return End.getLocWithOffset(7);
    }
    // The end loc might be pointing to a nested return...
    bool Invalid;
    const char *Char = SM.getCharacterData(End, &Invalid);
    if (Invalid)
        return End;
    auto Return = "return";
    for (int i = 0; i < 5; ++i)
        if (Char[i] != Return[i])
            return End;
    return End.getLocWithOffset(7);
}

template <typename T>
CharSourceRange getExtendedRangeWithCommentsAndSemi(const T &Node,
                                                    ASTContext &Context) {
    auto &SM = Context.getSourceManager();
    auto Range = CharSourceRange::getTokenRange(Node.getSourceRange());
    Range = SM.getExpansionRange(Range);
    Range.setEnd(handleReturnStmts(Node, Range.getEnd(), SM));
    Range = maybeExtendRange(Range, tok::TokenKind::comment, Context);
    return maybeExtendRange(Range, tok::TokenKind::semi, Context);
}

RangeSelector statementWithMacrosExpanded(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        auto Range = SM.getExpansionRange(
            getExtendedRangeWithCommentsAndSemi(*Node, *Result.Context));
        return Range;
    };
}

auto inMainAndNotMacro = allOf(notInMacro(), isExpansionInMainFile());

AST_POLYMORPHIC_MATCHER(BeginNotInMacroAndInMain,
                        AST_POLYMORPHIC_SUPPORTED_TYPES(IfStmt, SwitchStmt,
                                                        ForStmt, WhileStmt,
                                                        DoStmt,
                                                        CXXForRangeStmt)) {
    (void)Builder;
    auto EndLoc = Node.getBeginLoc();
    const auto &SM = Finder->getASTContext().getSourceManager();
    return !EndLoc.isMacroID() && SM.isInMainFile(SM.getExpansionLoc(EndLoc));
}

auto instrumentStmtAfterReturnRule() {
    auto matcher =
        mapAnyOf(ifStmt, switchStmt, forStmt, whileStmt, doStmt,
                 cxxForRangeStmt)
            .with(BeginNotInMacroAndInMain(), hasDescendant(returnStmt()))
            .bind("stmt_with_return_descendant");

    auto action = addMarker(insertAfter(
        statementWithMacrosExpanded("stmt_with_return_descendant"), cat("")));
    return makeRule(matcher, action);
}

auto InstrumentCStmt(std::string id) {
    return addMarker(insertBefore(statements(id), cat("")));
}

EditGenerator InstrumentNonCStmt(std::string id) {
    return flattenVector(
        {edit(addMarker(
             insertBefore(statementWithMacrosExpanded(id), cat("{")))),
         edit(insertAfter(statementWithMacrosExpanded(id), cat("\n}")))});
}

auto instrumentFunction() {
    auto matcher =
        functionDecl(unless(isImplicit()),
                     hasBody(compoundStmt(inMainAndNotMacro).bind("body")));
    auto action = InstrumentCStmt("body");
    return makeRule(matcher, action);
}

AST_MATCHER(IfStmt, RParenNotInMacroAndInMain) {
    (void)Builder;
    auto RParenLoc = Node.getRParenLoc();
    const auto &SM = Finder->getASTContext().getSourceManager();
    return !RParenLoc.isMacroID() &&
           SM.isInMainFile(SM.getExpansionLoc(RParenLoc));
}

AST_MATCHER(IfStmt, ElseNotInMacroAndInMain) {
    (void)Builder;
    auto ElseLoc = Node.getElseLoc();
    const auto &SM = Finder->getASTContext().getSourceManager();
    return !ElseLoc.isMacroID() && SM.isInMainFile(SM.getExpansionLoc(ElseLoc));
}

auto instrumentIfStmt() {
    auto matcher = ifStmt(
        RParenNotInMacroAndInMain(),
        optionally(hasElse(anyOf(
            compoundStmt(inMainAndNotMacro).bind("celse"),
            stmt(hasParent(ifStmt(ElseNotInMacroAndInMain()))).bind("else")))),
        hasThen(anyOf(compoundStmt(inMainAndNotMacro).bind("cthen"),
                      stmt().bind("then"))));
    auto actions =
        flattenVector({ifBound("cthen", InstrumentCStmt("cthen")),
                       ifBound("celse", InstrumentCStmt("celse")),
                       ifBound("then", InstrumentNonCStmt("then"), noEdits()),
                       ifBound("else", InstrumentNonCStmt("else"), noEdits())

        });
    return makeRule(matcher, actions);
}

RangeSelector doStmtWhile(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        return SM.getExpansionRange(
            SourceRange(Node->get<DoStmt>()->getWhileLoc()));
    };
}

AST_MATCHER(DoStmt, DoAndWhileNotMacroAndInMain) {
    (void)Builder;
    const auto &SM = Finder->getASTContext().getSourceManager();
    const auto &DoLoc = Node.getDoLoc();
    const auto &WhileLoc = Node.getWhileLoc();
    return !Node.getDoLoc().isMacroID() && !Node.getWhileLoc().isMacroID() &&
           SM.isInMainFile(SM.getExpansionLoc(DoLoc)) &&
           SM.isInMainFile(SM.getExpansionLoc(WhileLoc));
}

auto instrumentLoop() {
    auto compoundMatcher =
        mapAnyOf(forStmt, whileStmt, doStmt, cxxForRangeStmt)
            .with(inMainAndNotMacro,
                  hasBody(compoundStmt(inMainAndNotMacro).bind("body")));
    auto nonCompoundDoWhileMatcher =
        doStmt(DoAndWhileNotMacroAndInMain(), hasBody(stmt().bind("body")))
            .bind("dostmt");
    auto doWhileAction = {
        addMarker(insertBefore(statementWithMacrosExpanded("body"), cat("{"))),
        insertBefore(doStmtWhile("dostmt"), cat("\n}"))};
    auto nonCompoundLoopMatcher =
        mapAnyOf(forStmt, whileStmt, cxxForRangeStmt)
            .with(inMainAndNotMacro,
                  hasBody(stmt(inMainAndNotMacro).bind("body")));
    return applyFirst(
        {makeRule(compoundMatcher, InstrumentCStmt("body")),
         makeRule(nonCompoundDoWhileMatcher, doWhileAction),
         makeRule(nonCompoundLoopMatcher, InstrumentNonCStmt("body"))});
}

RangeSelector SwitchCaseColonLoc(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        return SM.getExpansionRange(Node->get<SwitchCase>()->getColonLoc());
    };
}

AST_MATCHER(SwitchCase, colonNotInMacroAndInMain) {
    (void)Builder;
    auto ColonLoc = Node.getColonLoc();
    const auto &SM = Finder->getASTContext().getSourceManager();
    return !ColonLoc.isMacroID() &&
           SM.isInMainFile(SM.getExpansionLoc(ColonLoc));
}

auto instrumentSwitchCase() {
    auto matcher = switchCase(colonNotInMacroAndInMain()).bind("stmt");
    auto action = addMarker(insertAfter(SwitchCaseColonLoc("stmt"), cat("")));
    return makeRule(matcher, action);
}

} // namespace

Instrumenter::Instrumenter(
    std::map<std::string, clang::tooling::Replacements> &FileToReplacements)
    : FileToReplacements{FileToReplacements},
      Rules{{instrumentStmtAfterReturnRule(), Replacements,
             FileToNumberMarkerDecls},
            {instrumentIfStmt(), Replacements, FileToNumberMarkerDecls},
            {instrumentFunction(), Replacements, FileToNumberMarkerDecls},
            {instrumentLoop(), Replacements, FileToNumberMarkerDecls},
            {instrumentSwitchCase(), Replacements, FileToNumberMarkerDecls}} {}

void Instrumenter::applyReplacements() {
    for (const auto &[File, NumberMarkerDecls] : FileToNumberMarkerDecls) {
        std::stringstream ss;
        auto gen = [i = 0]() mutable {
	    int index = i;
            return "void  __attribute__ ((noinline)) marker_" + std::to_string(i++) + "(void) { printf(\" b" + std::to_string(index) + "b \");} \n";
        };
        std::generate_n(std::ostream_iterator<std::string>(ss),
                        NumberMarkerDecls, gen);
        auto Decls = ss.str();
        auto R = Replacement(File, 0, 0, Decls);
        if (auto Err = FileToReplacements[File].add(R))
            llvm_unreachable(llvm::toString(std::move(Err)).c_str());
    }

    for (auto Rit = Replacements.rbegin(); Rit != Replacements.rend(); ++Rit) {
        auto &R = *Rit;
        auto &Replacements = FileToReplacements[std::string(R.getFilePath())];
        auto Err = Replacements.add(R);
        if (Err) {
            auto NewOffset = Replacements.getShiftedCodePosition(R.getOffset());
            auto NewLength = Replacements.getShiftedCodePosition(
                                 R.getOffset() + R.getLength()) -
                             NewOffset;
            if (NewLength == R.getLength()) {
                R = Replacement(R.getFilePath(), NewOffset, NewLength,
                                R.getReplacementText());
                Replacements = Replacements.merge(tooling::Replacements(R));
            } else {
                llvm_unreachable(llvm::toString(std::move(Err)).c_str());
            }
        }
    }
}

void Instrumenter::registerMatchers(clang::ast_matchers::MatchFinder &Finder) {
    for (auto &Rule : Rules)
        Rule.registerMatchers(Finder);
}

namespace {
auto globalizeRule() {
    return makeRule(decl(anyOf(varDecl(hasGlobalStorage(), unless(isExtern()),
                                       unless(isStaticStorageClass())),
                               functionDecl(isDefinition(), unless(isMain()),
                                            unless(isStaticStorageClass()))),
                         isExpansionInMainFile())
                        .bind("global"),
                    insertBefore(node("global"), cat(" static ")));
}
} // namespace

void detail::RuleActionCallback::run(
    const clang::ast_matchers::MatchFinder::MatchResult &Result) {
    if (Result.Context->getDiagnostics().hasErrorOccurred()) {
        llvm::errs() << "An error has occured.\n";
        return;
    }
    Expected<SmallVector<transformer::Edit, 1>> Edits =
        findSelectedCase(Result, Rule).Edits(Result);
    if (!Edits) {
        llvm::errs() << "Rewrite failed: " << llvm::toString(Edits.takeError())
                     << "\n";
        return;
    }
    auto SM = Result.SourceManager;
    for (const auto &T : *Edits) {
        assert(T.Kind == transformer::EditKind::Range);
        auto R = tooling::Replacement(*SM, T.Range, T.Replacement);
        auto &Replacements = FileToReplacements[std::string(R.getFilePath())];
        auto Err = Replacements.add(R);
        if (Err) {
            auto NewOffset = Replacements.getShiftedCodePosition(R.getOffset());
            auto NewLength = Replacements.getShiftedCodePosition(
                                 R.getOffset() + R.getLength()) -
                             NewOffset;
            if (NewLength == R.getLength()) {
                R = Replacement(R.getFilePath(), NewOffset, NewLength,
                                R.getReplacementText());
                Replacements = Replacements.merge(tooling::Replacements(R));
            } else {
                llvm_unreachable(llvm::toString(std::move(Err)).c_str());
            }
        }
    }
}

void detail::RuleActionCallback::registerMatchers(
    clang::ast_matchers::MatchFinder &Finder) {
    for (auto &Matcher : buildMatchers(Rule))
        Finder.addDynamicMatcher(Matcher, this);
}

GlobalStaticMaker::GlobalStaticMaker(
    std::map<std::string, clang::tooling::Replacements> &FileToReplacements)
    : FileToReplacements{FileToReplacements}, Rule{globalizeRule(),
                                                   FileToReplacements} {}

void GlobalStaticMaker::registerMatchers(
    clang::ast_matchers::MatchFinder &Finder) {
    Rule.registerMatchers(Finder);
}

} // namespace dead
