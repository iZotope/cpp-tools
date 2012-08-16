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
    map<Expr*, DeclaratorDecl*> UsesToDeclMap;

    // When we encounter a use of a decl, this updates our data structures.
    void AddFoundDecl(DeclaratorDecl *D, Expr *E) {
      auto lb = FoundDecls.lower_bound(D);

      // Only update the two containers that are keyed on the decl if
      // this is a completely new decl. This preserves the invariant that
      // we store the first use of a decl.
      if (lb == FoundDecls.end() || FoundDecls.key_comp()(D, *lb)) {
        FoundDecls.insert(lb, D);
        DeclExprMap.insert(make_pair(D, E));
      }

      // Always update the map of uses, whether this is the first time we've
      // seen this decl, or not.
      UsesToDeclMap.insert(make_pair(E, D));
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
    const decl_set& found_decls() const { return FoundDecls; }

    // Map from all found decls to the first usage of it as an expression.
    typedef map<DeclaratorDecl*, Expr*> decl_expr_map;
    typedef decl_expr_map::const_iterator decl_expr_map_iterator;
    decl_expr_map_iterator decl_to_expr_begin() const {
      return DeclExprMap.begin();
    }
    decl_expr_map_iterator decl_to_expr_end() const {
      return DeclExprMap.end();
    }
    const decl_expr_map decl_to_expr() const { return DeclExprMap; }

    // Map from all uses of found decls back to the decl. This is useful
    // for rewriting all uses in the code.
    typedef map<Expr*, DeclaratorDecl*> uses_to_decl_map;
    typedef uses_to_decl_map::const_iterator uses_to_decl_map_iterator;
    uses_to_decl_map_iterator uses_to_decl_begin() const {
      return UsesToDeclMap.begin();
    }
    uses_to_decl_map_iterator uses_to_decl_end() const {
      return UsesToDeclMap.end();
    }
    const uses_to_decl_map& uses_to_decl() const { return UsesToDeclMap; }

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
template <class DeclNameIterator>
static string BuildFunctionDeclParameterList(DeclNameIterator BeginDecl,
                                             DeclNameIterator EndDecl,
                                             const SourceManager &SourceMgr) {
  DeclNameIterator LastDecl = prev(EndDecl);
  stringstream params;

  for (; BeginDecl != EndDecl; ++BeginDecl) {

    params << PrintAsReferenceType(*BeginDecl->first, SourceMgr) 
           << " " << BeginDecl->second;
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

// Maps a range of decls to a list of unique strings that could be function
// arguments. If the decls all have unique names, we'll just use the
// identifiers. However, member expressions like 'this->n' get turned into
// just 'n'. Then if there's redundancy, i.e. if there was also a non-member
// variable called 'n', the names have to get uniqued.
template <class DeclExprIterator>
static map<DeclaratorDecl*, std::string>
MapDeclsToParamNames(DeclExprIterator BeginDecl,
                     DeclExprIterator EndDecl) {
  typedef DeclaratorDecl* DeclPtr;
  map<DeclPtr, std::string> DeclNames;
  map<std::string, DeclPtr> NamesToDecl;

  // First go through and add all the non-member decls. The member decls
  // need to be added last, so that if there are conflicting names, the
  // member ones get renamed;
  for (auto CurDecl = BeginDecl; CurDecl != EndDecl; ++CurDecl) {
    if ((*CurDecl)->isCXXClassMember()) continue;

    // First try to use the decl's name.
    std::string Name = (*CurDecl)->getNameAsString();

    // If a decl by this name already exists, keep appending
    // underscores until it doesn't.
    auto lb = NamesToDecl.lower_bound(Name);
    while (lb != NamesToDecl.end()
           && !(NamesToDecl.key_comp()(Name, lb->first))) {
      Name += "_";
      lb = NamesToDecl.lower_bound(Name);
    }

    // Now the name is unique.
    assert(NamesToDecl.find(Name) == NamesToDecl.end());
    NamesToDecl.insert(lb, make_pair(Name, *CurDecl));
    DeclNames.insert(make_pair(*CurDecl, Name));
  }

  // Now add the member decls.
  for (auto CurDecl = BeginDecl; CurDecl != EndDecl; ++CurDecl) {
    if (!(*CurDecl)->isCXXClassMember()) continue;

    // First try to use the decl's name.
    std::string Name = (*CurDecl)->getNameAsString();
    
    // If a decl by this name already exists, first try adding "this_"
    // as a prefix, since we added non-member variables first. If that
    // doesn't work, roll back to the original name.
    auto lb = NamesToDecl.lower_bound(Name);
    if (lb != NamesToDecl.end()
        && !(NamesToDecl.key_comp()(Name, lb->first))) {
      std::string NewName = "this_" + Name;
      auto new_lb = NamesToDecl.lower_bound(NewName);
      if (new_lb == NamesToDecl.end()
          || NamesToDecl.key_comp()(NewName, lb->first)) {
        Name = NewName;
        lb = new_lb;
      }
    }

    // If a decl by this name already exists, keep appending
    // underscores until it doesn't.
    while (lb != NamesToDecl.end()
           && !(NamesToDecl.key_comp()(Name, lb->first))) {
      Name += "_";
      lb = NamesToDecl.lower_bound(Name);
    }

    // Now the name is unique.
    assert(NamesToDecl.find(Name) == NamesToDecl.end());
    NamesToDecl.insert(lb, make_pair(Name, *CurDecl));
    DeclNames.insert(make_pair(*CurDecl, Name));
  }

  return DeclNames;
}

// Rewrites all expressions using the given decls with their new names.
void RewriteDeclUses(const map<Expr*, DeclaratorDecl*>& UsesMap,
                     const map<DeclaratorDecl*, std::string>& NamesMap,
                     Rewriter &R) {

  for (auto CurUse = UsesMap.begin(), EndUse = UsesMap.end();
       CurUse != EndUse; ++CurUse) {

    auto NewNameEntry = NamesMap.find(CurUse->second);
    assert(NewNameEntry != NamesMap.end());
    R.ReplaceText(CurUse->first->getSourceRange(), NewNameEntry->second);
  }
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

  // Each decl that we thread through needs to have a unique parameter name.
  auto DeclNames = MapDeclsToParamNames(Finder.found_decls_begin(),
                                        Finder.found_decls_end());

  // Create the new function with the extracted code as its body.
  // Again, don't use it yet.
  SourceRange SkipLeadingNewline(
      AdvanceSourceLocationUntil(Range.getBegin(),
                                 SourceMgr,
                                 IsNotLineEnding),
      Range.getEnd());
  const string NewFunctionParamList = 
      BuildFunctionDeclParameterList(DeclNames.begin(),
                                     DeclNames.end(),
                                     SourceMgr);

  // Rewrite all uses of the decls that we're threading through, as
  // necessary. That rewritten code will get used for the newly created
  // function's body.
  RewriteDeclUses(Finder.uses_to_decl(),
                  DeclNames,
                  TheRewriter);
  const string NewFunctionBody =
      TheRewriter.getRewrittenText(SkipLeadingNewline);

  // Finally, perform all the replacements.
  ReplaceSourceRangeWithCode(Range, callstr.str(), SourceMgr, TheRewriter);
  InsertNewFunctionWithBody(FnDecl,
                            NewFunctionName,
                            NewFunctionParamList,
                            NewFunctionBody,
                            TheRewriter);
}

