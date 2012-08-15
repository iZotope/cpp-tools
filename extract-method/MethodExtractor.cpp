#include "clang/AST/Decl.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Rewrite/Rewriter.h"
#include "MethodExtractor.h"
#include <cctype>
#include <sstream>
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
                                      const string& NewFunctionBody,
                                      Rewriter &TheRewriter) {
  stringstream sstr;
  sstr << "static void " << NewFunctionName << "() {\n";
  sstr << NewFunctionBody;
  sstr << "\n";
  sstr << "}\n";
  sstr << "\n";

  TheRewriter.InsertTextBefore(BeforeDecl.getSourceRange().getBegin(),
                               sstr.str());
}

void MethodExtractor::Run() {
  FileID FID = SourceMgr.getFileID(FnDecl.getSourceRange().getBegin());
  SourceRange Range = GetSourceRangeForLines(SourceMgr,
                                             FID,
                                             FirstLine,
                                             LastLine);

  std::stringstream callstr;
  callstr << NewFunctionName << "();\n";
  ReplaceSourceRangeWithCode(Range, callstr.str(), SourceMgr, TheRewriter);

  SourceRange SkipLeadingNewline(
      AdvanceSourceLocationUntil(Range.getBegin(),
                                 SourceMgr,
                                 IsNotLineEnding),
      Range.getEnd());
  InsertNewFunctionWithBody(
      FnDecl,
      NewFunctionName,
      GetSourceRangeAsString(SourceMgr, SkipLeadingNewline).str(),
      TheRewriter);
}

