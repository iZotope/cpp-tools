#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Rewrite/Rewriter.h"
#include "MethodExtractor.h"
#include <cctype>
#include <iterator>
#include <sstream>
#include <set>
using namespace clang;
using namespace std;

// Advances a source location until some predicate is true on the first
// character that it corresponds to.
template <class Pred>
static SourceLocation AdvanceSourceLocationUntil(SourceLocation SL,
                                                 const SourceManager &SM,
                                                 Pred pred) {
  for (bool Invalid = false;;) {
    const char* CharData = SM.getCharacterData(SL, &Invalid);
    if (Invalid) break;
    if (pred(*CharData)) break;
    SL = SL.getLocWithOffset(1);
  }

  return SL;
}

static bool IsNotSpace(char c) { return !std::isspace(c); }
static bool IsLineEnding(char c) { return c == '\n'; }
static bool IsNotLineEnding(char c) { return !IsLineEnding(c); }

// Takes two line numbers and a file ID, and returns the corresponding
// source range.
static SourceRange GetSourceRangeForLines(SourceManager &SM,
                                          FileID FID,
                                          unsigned FirstLine,
                                          unsigned LastLine) {
  // Make these zero-based.
  if (FirstLine > 0) --FirstLine;
  if (LastLine > 0) --LastLine;

  // Advance the start location past any whitespace.
  SourceLocation StartLoc = SM.translateLineCol(FID, FirstLine, /*Col*/0);

  // Advance the end location until the end of the line is hit.
  SourceLocation EndLoc = SM.translateLineCol(FID, LastLine, /*Col*/0);

  return SourceRange(StartLoc, EndLoc);
}

// Returns a string containing all the source code from the given source range.
static StringRef GetSourceRangeAsString(const SourceManager &SM,
                                        const SourceRange &SR) {
  const char *Begin = SM.getCharacterData(SR.getBegin());
  const char *End = SM.getCharacterData(SR.getEnd());
  return StringRef(Begin, End-Begin);
}

// Replaces the given source range with the code, so that the indentation
// level is preserved.
static void ReplaceSourceRangeWithCode(const SourceRange &Range,
                                       const string& NewCode,
                                       const SourceManager &SourceMgr,
                                       Rewriter &TheRewriter) {
  SourceRange SkipLeadingWhitespace(
      AdvanceSourceLocationUntil(Range.getBegin(),
                                 SourceMgr,
                                 IsNotSpace),
      AdvanceSourceLocationUntil(Range.getEnd(),
                                 SourceMgr,
                                 IsNotLineEnding));
  TheRewriter.ReplaceText(SkipLeadingWhitespace, NewCode);
}

// Inserts a new function before the given decl, with the given function body.
// Corrects the indentation as well.
static void InsertNewFunctionWithBody(Decl &BeforeDecl,
                                      const string& NewFunctionName,
                                      const string& NewFunctionParams,
                                      const string& NewFunctionBody,
                                      Rewriter &TheRewriter) {
  stringstream sstr;
  sstr << "static void " << NewFunctionName << "("
       << NewFunctionParams << ") {\n";
  sstr << NewFunctionBody;
  sstr << "\n";
  sstr << "}\n";
  sstr << "\n";

  TheRewriter.InsertTextBefore(BeforeDecl.getSourceRange().getBegin(),
                               sstr.str());
}

namespace {
  // Searches for all DeclRefs in a given source range.
  class DeclRefFinder : public RecursiveASTVisitor<DeclRefFinder> {
  public:
    DeclRefFinder(const SourceRange &Range,
                  const SourceManager &SourceMgr)
      : Range(Range)
      , SourceMgr(SourceMgr)
      , FID(SourceMgr.getFileID(Range.getBegin()))
      , FoundDecls(order_decl_by_location(&SourceMgr))
    {}

    bool VisitDeclRefExpr(DeclRefExpr *DRE) {
      if (!IsDeclRefInRange(DRE)) return true;

      // We only thread through value decls. They have names and
      // types, which are both needed.
      auto VD = dyn_cast<ValueDecl>(DRE->getDecl()->getCanonicalDecl());
      if (!VD) return true;
      AddFoundDecl(VD);

      return true;
    }
  private:
    const SourceRange Range;
    const SourceManager &SourceMgr;
    const FileID FID;

    struct order_decl_by_location {
      const SourceManager *SourceMgr;
      order_decl_by_location(const SourceManager *SourceMgr)
        : SourceMgr(SourceMgr)
      {}

      bool operator()( const Decl* lhs, const Decl* rhs ) {
        return SourceMgr->isBeforeInTranslationUnit(lhs->getLocation(),
                                                    rhs->getLocation());
      }
    };

    set<ValueDecl*, order_decl_by_location> FoundDecls;

    // Only adds D if it's not in our set already.
    void AddFoundDecl(ValueDecl* D) {
      FoundDecls.insert(D);
    }

    bool IsDeclRefInRange(DeclRefExpr *DRE) const {
      SourceLocation Loc = DRE->getLocation();
      if (SourceMgr.getFileID(Loc) != FID) return false;
      if (SourceMgr.isBeforeInTranslationUnit(Loc, Range.getBegin())) {
        return false;
      }
      if (SourceMgr.isBeforeInTranslationUnit(Range.getEnd(), Loc)) {
        return false;
      }
      return true;
    }
  public:
    typedef set<ValueDecl*, order_decl_by_location> decl_set;
    typedef decl_set::const_iterator decl_set_iterator;
    decl_set_iterator found_decls_begin() const { return FoundDecls.begin(); }
    decl_set_iterator found_decls_end() const { return FoundDecls.end(); }
  };
}

// Takes a range of decls that should turn into a function declaration
// formal parameter list, and builds that list.
template <class DeclIterator>
static string BuildFunctionDeclParameterList(DeclIterator BeginDecl,
                                             DeclIterator EndDecl) {
  DeclIterator LastDecl = prev(EndDecl);
  stringstream params;

  for (; BeginDecl != EndDecl; ++BeginDecl) {
    QualType Ty = (*BeginDecl)->getType();
    params << Ty.getAsString() << "& " << (*BeginDecl)->getNameAsString();
    if (BeginDecl != LastDecl) params << ", ";
  }
  return params.str();
}

// Takes a range of decls that should get passed as function arguments,
// and builds the comma-separated list of arguments.
template <class DeclIterator>
static string BuildFunctionCallArgumentList(DeclIterator BeginDecl,
                                            DeclIterator EndDecl) {
  DeclIterator LastDecl = prev(EndDecl);
  stringstream args;
  for (; BeginDecl != EndDecl; ++BeginDecl) {
    args << (*BeginDecl)->getNameAsString();
    if (BeginDecl != LastDecl) args << ", ";
  }
  return args.str();
}

void MethodExtractor::Run() {
  FileID FID = SourceMgr.getFileID(FnDecl.getSourceRange().getBegin());
  SourceRange Range = GetSourceRangeForLines(SourceMgr,
                                             FID,
                                             FirstLine,
                                             LastLine);

  // Find all references to declarations inside this source range.
  // We'll need to thread those through to the new function.
  DeclRefFinder Finder(Range, SourceMgr);
  Finder.TraverseDecl(&FnDecl);

  // Replace the code to extract with a call to the new function.
  std::stringstream callstr;
  callstr << NewFunctionName << "("
          << BuildFunctionCallArgumentList(Finder.found_decls_begin(),
                                           Finder.found_decls_end())
          << ");\n";
  ReplaceSourceRangeWithCode(Range, callstr.str(), SourceMgr, TheRewriter);

  // Create the new function with the extracted code as its body.
  SourceRange SkipLeadingNewline(
      AdvanceSourceLocationUntil(Range.getBegin(),
                                 SourceMgr,
                                 IsNotLineEnding),
      Range.getEnd());
  InsertNewFunctionWithBody(
      FnDecl,
      NewFunctionName,
      BuildFunctionDeclParameterList(Finder.found_decls_begin(),
                                     Finder.found_decls_end()),
      GetSourceRangeAsString(SourceMgr, SkipLeadingNewline).str(),
      TheRewriter);
}

