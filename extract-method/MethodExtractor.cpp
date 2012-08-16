#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Rewrite/Rewriter.h"
#include "MethodExtractor.h"
#include <cctype>
#include <iterator>
#include <map>
#include <sstream>
#include <set>
#include <utility>
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

  assert(SL.isValid());
  return SL;
}

static bool IsNotSpace(char c) { return !std::isspace(c); }
static bool IsLineEnding(char c) { return c == '\n' || c == '\r'; }
static bool IsNotLineEnding(char c) { return !IsLineEnding(c); }

// Takes two line numbers and a file ID, and returns the corresponding
// source range.
static SourceRange GetSourceRangeForLines(SourceManager &SM,
                                          FileID FID,
                                          unsigned FirstLine,
                                          unsigned LastLine) {
  assert(FirstLine <= LastLine);

  SourceLocation StartLoc = SM.translateLineCol(FID, FirstLine, /*Col*/1);
  assert(StartLoc.isValid());

  // Advance the end location until the end of the line is hit.
  SourceLocation EndLoc = SM.translateLineCol(FID, LastLine, /*Col*/1);
  EndLoc = AdvanceSourceLocationUntil(EndLoc, SM, IsLineEnding);
  EndLoc = EndLoc.getLocWithOffset(-1);
  assert(EndLoc.isValid());

  assert(SM.isBeforeInTranslationUnit(StartLoc, EndLoc));

  return SourceRange(StartLoc, EndLoc);
}

// Returns a string containing all the source code from the given source range.
static StringRef GetSourceRangeAsString(const SourceManager &SM,
                                        const SourceRange &SR) {
  bool Invalid = false;
  const char *Begin = SM.getCharacterData(SR.getBegin(), &Invalid);
  assert(!Invalid);
  const char *End = SM.getCharacterData(SR.getEnd(), &Invalid);
  assert(!Invalid);

  assert(End >= Begin);
  return StringRef(Begin, End-Begin+1);
}

// Replaces the given source range with the code, so that the indentation
// level is preserved.
static void ReplaceSourceRangeWithCode(const SourceRange &Range,
                                       const string& NewCode,
                                       const SourceManager &SourceMgr,
                                       Rewriter &TheRewriter) {
  // The range should skip all leading whitespace, and extend all the
  // way until the end of the line.
  SourceRange SkipLeadingWhitespace(
      AdvanceSourceLocationUntil(Range.getBegin(),
                                 SourceMgr,
                                 IsNotSpace),
      AdvanceSourceLocationUntil(Range.getEnd(),
                                 SourceMgr,
                                 IsLineEnding));
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
      if (!IsExprInRange(DRE)) return true;

      // We only thread through declarator decls. They have names and
      // types, which are both needed.
      Decl *D = DRE->getDecl();
      if (DeclaratorDecl* DD = GetCanonicalDeclaratorDecl(D)) {
        AddFoundDecl(DD, DRE);
      }

      return true;
    }

    bool VisitMemberExpr(MemberExpr *ME) {
      if (!IsExprInRange(ME)) return true;

      Decl *D = ME->getMemberDecl();
      if (DeclaratorDecl* DD = GetCanonicalDeclaratorDecl(D)) {
        AddFoundDecl(DD, ME);
      }

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

    set<DeclaratorDecl*, order_decl_by_location> FoundDecls;
    map<DeclaratorDecl*, Expr*> DeclExprMap;

    // Only adds D if it's not in our set already.
    void AddFoundDecl(DeclaratorDecl *D, Expr *E) {
      FoundDecls.insert(D);
      DeclExprMap.insert(make_pair(D, E));
    }


    bool IsExprInRange(Expr *E) const {
      SourceLocation Loc = E->getLocStart();
      if (SourceMgr.getFileID(Loc) != FID) return false;
      if (SourceMgr.isBeforeInTranslationUnit(Loc, Range.getBegin())) {
        return false;
      }
      if (SourceMgr.isBeforeInTranslationUnit(Range.getEnd(), Loc)) {
        return false;
      }
      return true;
    }

    // If the canonical decl is a declarator decl, returns that.
    // Otherwise returns NULL.
    DeclaratorDecl* GetCanonicalDeclaratorDecl(Decl *D) {
      Decl *CanDecl = D->getCanonicalDecl();
      return dyn_cast<DeclaratorDecl>(CanDecl);
    }
  public:
    // Set of all the decls that were found, ordered by declaration order.
    typedef set<DeclaratorDecl*, order_decl_by_location> decl_set;
    typedef decl_set::const_iterator decl_set_iterator;
    decl_set_iterator found_decls_begin() const { return FoundDecls.begin(); }
    decl_set_iterator found_decls_end() const { return FoundDecls.end(); }

    // Map from all found decls to the first usage of it as an expression.
    typedef map<DeclaratorDecl*, Expr*> decl_expr_map;
    typedef decl_expr_map::const_iterator decl_expr_map_iterator;
    decl_expr_map_iterator decl_to_expr_begin() const {
      return DeclExprMap.begin();
    }
    decl_expr_map_iterator decl_to_expr_end() const {
      return DeclExprMap.end();
    }
  };
}

// Makes the type of the given declaration into a reference type if it isn't
// already one, then returns the resulting type as a string.
static string PrintAsReferenceType(DeclaratorDecl &DD,
                                   const SourceManager &SourceMgr) {
  // Start out with the type.
  QualType Ty = DD.getType();
  string BaseStr = Ty.getAsString();

  // If it's not a reference type, make it one.
  const Type *RawTy = Ty.getTypePtrOrNull();
  assert(RawTy);
  if (!RawTy->isReferenceType()) {
    BaseStr += "&";
  }

  return BaseStr;
}

// Takes a range of decls that should turn into a function declaration
// formal parameter list, and builds that list.
template <class DeclIterator>
static string BuildFunctionDeclParameterList(DeclIterator BeginDecl,
                                             DeclIterator EndDecl,
                                             const SourceManager &SourceMgr) {
  DeclIterator LastDecl = prev(EndDecl);
  stringstream params;

  for (; BeginDecl != EndDecl; ++BeginDecl) {
    params << PrintAsReferenceType(**BeginDecl, SourceMgr) 
           << " " << (*BeginDecl)->getNameAsString();
    if (BeginDecl != LastDecl) params << ", ";
  }
  return params.str();
}

// Takes a range of decls that should get passed as function arguments,
// and builds the comma-separated list of arguments.
template <class DeclExprIterator>
static string BuildFunctionCallArgumentList(DeclExprIterator BeginDecl,
                                            DeclExprIterator EndDecl,
                                            const SourceManager &SourceMgr) {
  DeclExprIterator LastDecl = prev(EndDecl);
  stringstream args;
  for (; BeginDecl != EndDecl; ++BeginDecl) {
    Expr *UseExpr = BeginDecl->second;
    SourceRange UseRange = UseExpr->getSourceRange();

    args << GetSourceRangeAsString(SourceMgr, UseRange).str();
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

  // Build the new function call, but don't use it yet.
  std::stringstream callstr;
  callstr << NewFunctionName << "("
          << BuildFunctionCallArgumentList(Finder.decl_to_expr_begin(),
                                           Finder.decl_to_expr_end(),
                                           SourceMgr)
          << ");";

  // Create the new function with the extracted code as its body.
  // Again, don't use it yet.
  SourceRange SkipLeadingNewline(
      AdvanceSourceLocationUntil(Range.getBegin(),
                                 SourceMgr,
                                 IsNotLineEnding),
      Range.getEnd());
  const string NewFunctionParamList = 
      BuildFunctionDeclParameterList(Finder.found_decls_begin(),
                                     Finder.found_decls_end(),
                                     SourceMgr);
  const string NewFunctionBody =
      GetSourceRangeAsString(SourceMgr, SkipLeadingNewline).str();                                     
  // Finally, perform all the replacements. It's important to do them at
  // the end so that we don't try to build code by reading existing source
  // code that has been moved around as a result of rewriting.
  ReplaceSourceRangeWithCode(Range, callstr.str(), SourceMgr, TheRewriter);
  InsertNewFunctionWithBody(FnDecl,
                            NewFunctionName,
                            NewFunctionParamList,
                            NewFunctionBody,
                            TheRewriter);
}

