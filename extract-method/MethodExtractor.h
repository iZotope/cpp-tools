class MethodExtractor {
public:
  MethodExtractor(clang::FunctionDecl &FnDecl,
                  clang::SourceManager &SourceMgr,
                  clang::Rewriter &TheRewriter,
                  unsigned FirstLine,
                  unsigned LastLine,
                  std::string NewFunctionName)
    : FnDecl(FnDecl)
    , SourceMgr(SourceMgr)
    , TheRewriter(TheRewriter)
    , FirstLine(FirstLine)
    , LastLine(LastLine)
    , NewFunctionName(std::move(NewFunctionName))
  {}

  void Run();
private:
  clang::FunctionDecl &FnDecl;
  clang::SourceManager &SourceMgr;
  clang::Rewriter &TheRewriter;
  const unsigned FirstLine, LastLine;
  const std::string NewFunctionName;
};

