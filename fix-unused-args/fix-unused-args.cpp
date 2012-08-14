#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Rewrite/Rewriter.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"
#include <string>
using namespace clang;
using namespace clang::tooling;
using namespace llvm;

// Traverses the AST, finding named function arguments that are unused,
// and making them unnamed by commenting out the name.
class FixUnusedArgsASTVisitor :
  public RecursiveASTVisitor<FixUnusedArgsASTVisitor> {
public:
  FixUnusedArgsASTVisitor(Rewriter &R,
                          std::string UnusedPrefix,
                          std::string UnusedSuffix)
    : TheRewriter(R)
    , UnusedPrefix(std::move(UnusedPrefix))
    , UnusedSuffix(std::move(UnusedSuffix))
    {}

  bool VisitFunctionDecl(FunctionDecl *f) {
    // Only visit function definitions (with bodies), not declarations.
    // We don't want to modify the declaration at all, just the definition.
    if (!f->getBody()) return true;

    for (auto PI=f->param_begin(), PE=f->param_end(); PI != PE; ++PI) {
      const ParmVarDecl *Param = *PI;

      if (Param->isUsed()) continue;
      if (Param->getName().empty()) continue;
      makeParamDeclUnnamed(Param);
    }

    return true;
  }

private:
  Rewriter &TheRewriter;
  const std::string UnusedPrefix, UnusedSuffix;

  // Makes a param decl unnamed by commenting the name out.
  void makeParamDeclUnnamed(const ParmVarDecl *Param) {
    SourceLocation NameLoc = Param->getLocation();
    TheRewriter.InsertTextBefore(NameLoc, UnusedPrefix);
    TheRewriter.InsertTextAfterToken(NameLoc, UnusedSuffix);
  }
};

// Runs our AST visitor on top-level declarations.
class FixUnusedArgsASTConsumer : public ASTConsumer {
public:
  FixUnusedArgsASTConsumer(Rewriter &R,
                           std::string UnusedPrefix,
                           std::string UnusedSuffix)
    : Visitor(R, std::move(UnusedPrefix), std::move(UnusedSuffix))
  {}

  virtual bool HandleTopLevelDecl(DeclGroupRef DR) {
    for (auto DB = DR.begin(), DE = DR.end(); DB != DE; ++DB) {
      // Traverse the declaration using our AST visitor.
      Visitor.TraverseDecl(*DB);
    }
    return true;
  }

private:
  FixUnusedArgsASTVisitor Visitor;
};

cl::opt<std::string> BuildPath(
  "p",
  cl::value_desc("build-path"),
  cl::desc("Build path for the compilation database"),
  cl::Optional);
cl::opt<std::string> UnusedPrefix(
  "unused-prefix",
  cl::desc("Prefix for removing unused parameters"),
  cl::init("/*"));
cl::opt<std::string> UnusedSuffix(
  "unused-suffix",
  cl::desc("Suffix for removing unused parameters"),
  cl::init("*/"));
cl::list<std::string> SourcePaths(
  cl::Positional,
  cl::desc("<source0> [... <sourceN>]"),
  cl::OneOrMore);

// Frontend action to fix unused arguments and overwrite the changed files.
class FixUnusedParamAction : public ASTFrontendAction {
public:
  virtual clang::ASTConsumer *CreateASTConsumer(
    clang::CompilerInstance &Compiler, llvm::StringRef InFile) {
    TheRewriter.setSourceMgr(Compiler.getSourceManager(),
                             Compiler.getLangOpts());
    return new FixUnusedArgsASTConsumer(TheRewriter,
                                        UnusedPrefix,
                                        UnusedSuffix);
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

