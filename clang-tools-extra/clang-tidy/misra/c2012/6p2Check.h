//===--- 6.2.h - clang-tidy-misra -------------------------------*- C++ -*-===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef CLANG_TIDY_MISRA_C_2012_RULE_6_2_H
#define CLANG_TIDY_MISRA_C_2012_RULE_6_2_H

#include "../ClangTidyCheck.h"

namespace clang {
namespace tidy {
namespace misra {
namespace c2012 {

class Rule6p2 : public ClangTidyCheck {
public:
  Rule6p2(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}  
  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace c2012
} // namespace misra
} // namespace tidy
} // namespace clang

#endif // CLANG_TIDY_MISRA_C_2012_RULE_6_2_H
