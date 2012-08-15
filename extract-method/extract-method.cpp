#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Rewrite/Rewriter.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"
#include "MethodExtractor.h"
#include <iostream>
#include <string>
using namespace clang;
using namespace clang::tooling;
using namespace llvm;

// Runs our AST visitor on top-level declarations.
class ExtractMethodASTConsumer : public ASTConsumer {
public:
  ExtractMethodASTConsumer(Rewriter &R,
                           SourceManager &SM,
                           unsigned FirstLine,
                           unsigned LastLine,
                           std::string NewFunctionName)
    : TheRewriter(R)
    , SM(SM)
    , DoneExtracting(false)
    , FirstLine(FirstLine)
    , LastLine(LastLine)
    , NewFunctionName(std::move(NewFunctionName))
  {}

  virtual ~ExtractMethodASTConsumer() {
    if (!DoneExtracting) {
      std::cerr << "Did not find any function that contains the given "
                << "range of line numbers. No code was extracted.\n";
    }
  }

  virtual bool HandleTopLevelDecl(DeclGroupRef DR) {
    for (auto DB = DR.begin(), DE=DR.end(); DB != DE; ++DB) {
      // We need a function decl that contains the specified range of lines.
      if (!isa<FunctionDecl>(*DB)) continue;
      FunctionDecl *FD = dyn_cast<FunctionDecl>(*DB);
      if (!DoesDeclContainLineRange(FD)) continue;

      MethodExtractor MethodEx(*FD,
                               SM,
                               TheRewriter,
                               FirstLine,
                               LastLine,
                               NewFunctionName);
      MethodEx.Run();
      DoneExtracting = true;
      return true;
    }

    return true;
  }

private:
  Rewriter& TheRewriter;
  SourceManager& SM;
  bool DoneExtracting;

  // First and last lines of the code to extract.
  const unsigned FirstLine, LastLine;
  // Name of the new function to create.
  const std::string NewFunctionName;

  // Returns whether D contains the range of lines of code to extract.
  bool DoesDeclContainLineRange(Decl* D) {
    const SourceRange Range = D->getSourceRange();
    if (FirstLine < SM.getSpellingLineNumber(Range.getBegin())) return false;
    if (LastLine > SM.getSpellingLineNumber(Range.getEnd())) return false;
    return true;
  }
};

cl::opt<std::string> BuildPath(
  "p",
  cl::value_desc("build-path"),
  cl::desc("Build path for the compilation database"),
  cl::Optional);
cl::list<std::string> SourcePaths(
  cl::Positional,
  cl::desc("<source0> [... <sourceN>]"),
  cl::OneOrMore);
cl::opt<unsigned> FirstLine(
  "first",
  cl::desc("The first line of the code to extract"),
  cl::Required);
cl::opt<unsigned> LastLine(
  "last",
  cl::desc("The last line of the code to extract"),
  cl::Required);
cl::opt<std::string> FunctionName(
  "name",
  cl::desc("Name of the new function to create"),
  cl::Required);

// Frontend action to extract a method
class FixUnusedParamAction : public ASTFrontendAction {
public:
  virtual clang::ASTConsumer *CreateASTConsumer(
    clang::CompilerInstance &Compiler, llvm::StringRef InFile) {
    TheRewriter.setSourceMgr(Compiler.getSourceManager(),
                             Compiler.getLangOpts());
    return new ExtractMethodASTConsumer(TheRewriter,
                                        Compiler.getSourceManager(),
                                        FirstLine,
                                        LastLine,
                                        FunctionName);
  }

  // Upon destruction, write all changes to disk.
  virtual ~FixUnusedParamAction() {
    TheRewriter.overwriteChangedFiles();
  }

private:
  Rewriter TheRewriter;
};

void LoadCompilationDatabaseIfNotFound(
    OwningPtr<CompilationDatabase>& Compilations) {

  if (Compilations) return;

  std::string ErrorMessage;
  if (!BuildPath.empty()) {
    Compilations.reset(
        CompilationDatabase::autoDetectFromDirectory(BuildPath,
                                                     ErrorMessage));
  } else {
    Compilations.reset(CompilationDatabase::autoDetectFromSource(
        SourcePaths[0], ErrorMessage));
  }
  if (!Compilations) {
    llvm::report_fatal_error(ErrorMessage);
  }
}

int main(int argc, char **argv) {
  // Try to create a fixed compile command database.
  OwningPtr<CompilationDatabase> Compilations(
      FixedCompilationDatabase::loadFromCommandLine(
        argc, const_cast<const char**>(argv)));
  
  // Next, use normal llvm command line parsing to get the tool specific
  // parameters.
  cl::ParseCommandLineOptions(argc, argv);

  LoadCompilationDatabaseIfNotFound(Compilations);

  ClangTool Tool(*Compilations, SourcePaths);

  return Tool.run(newFrontendActionFactory<FixUnusedParamAction>());
}

