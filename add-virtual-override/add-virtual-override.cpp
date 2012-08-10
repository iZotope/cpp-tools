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

// Traverses the AST, adding explicit "virtual" and "override" where they
// are implicit.
class AddOverrideASTVisitor :
  public RecursiveASTVisitor<AddOverrideASTVisitor> {
public:
  AddOverrideASTVisitor(Rewriter &R, std::string OverrideString)
    : TheRewriter(R)
    , OverrideStringPreSpace(" " + OverrideString)
    , OverrideStringPostSpace(std::move(OverrideString) + " ")
    {}

  bool VisitCXXMethodDecl(CXXMethodDecl *MD) {
    if (ShouldAddVirtual(MD)) {
      MarkVirtual(MD);
    }

    if (ShouldAddOverride(MD)) {
      MarkOverride(MD);
    }

    return true;
  }

private:
  Rewriter &TheRewriter;
  const std::string OverrideStringPreSpace,
                    OverrideStringPostSpace;

  // Decides whether a method needs "virtual" added to it.
  bool ShouldAddVirtual(CXXMethodDecl *MD) {
    // We only care about declarations.
    if (MD->getCanonicalDecl() != MD) return false;
    // Only virtual functions should be marked virtual.
    if (!MD->isVirtual()) return false;
    // If it's already marked virtual, there's no problem.
    if (MD->isVirtualAsWritten()) return false;

    return true;
  }

  // Adds "virtual" to a method's declaration that lacks it.
  void MarkVirtual(CXXMethodDecl *MD) {
    SourceLocation Loc = isa<CXXDestructorDecl>(MD)
      ? MD->getInnerLocStart()
      : MD->getTypeSpecStartLoc();
    
    TheRewriter.InsertTextBefore(Loc, "virtual ");
  }

  // Decides whether a method needs "override" added to it.
  bool ShouldAddOverride(CXXMethodDecl *MD) {
    // We only care about declarations.
    if (MD->getCanonicalDecl() != MD) return false;
    // If it doesn't override anything, it shouldn't be marked.
    if (!MD->size_overridden_methods()) return false;
    // If it's marked override, it's good.
    if (MD->getAttr<OverrideAttr>()) return false;
    // Destructors aren't need to be marked override.
    if (isa<CXXDestructorDecl>(MD)) return false;
    // Pure virtual functions don't need to be marked.
    if (MD->isPure()) return false;

    return true;
  }

  // Adds "override" to a method's declaration that lacks it.
  void MarkOverride(CXXMethodDecl *MD) {
    if (MD->hasBody()) {
      TheRewriter.InsertTextAfter(MD->getBody()->getLocStart(),
                                  OverrideStringPostSpace);
    } else {
      TheRewriter.InsertTextAfterToken(MD->getLocEnd(),
                                       OverrideStringPreSpace);
    }
  }
};

// Runs our AST visitor on top-level declarations.
class AddOverrideASTConsumer : public ASTConsumer {
public:
  AddOverrideASTConsumer(Rewriter &R, std::string OverrideString)
    : Visitor(R, std::move(OverrideString))
  {}

  virtual bool HandleTopLevelDecl(DeclGroupRef DR) {
    for (auto DB = DR.begin(), DE = DR.end(); DB != DE; ++DB) {
      // Traverse the declaration using our AST visitor.
      Visitor.TraverseDecl(*DB);
    }
    return true;
  }

private:
  AddOverrideASTVisitor Visitor;
};

cl::opt<std::string> BuildPath(
  "p",
  cl::desc("<build-path>"),
  cl::Optional);
cl::list<std::string> SourcePaths(
  cl::Positional,
  cl::desc("<source0> [... <sourceN>]"),
  cl::OneOrMore);
cl::opt<std::string> OverrideString(
  "override",
  cl::desc("Alternate override specifier, i.e. a macro."),
  cl::init("override"));

// Frontend action to fix unused arguments and overwrite the changed files.
class FixUnusedParamAction : public ASTFrontendAction {
public:
  virtual clang::ASTConsumer *CreateASTConsumer(
    clang::CompilerInstance &Compiler, llvm::StringRef InFile) {
    TheRewriter.setSourceMgr(Compiler.getSourceManager(),
                             Compiler.getLangOpts());
    return new AddOverrideASTConsumer(TheRewriter, OverrideString);
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

