//===--- 6.2.cpp - clang-tidy-misra ---------------------------------------===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "6p2Check.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"
#include <cassert>
#include <elf.h>

namespace clang {
namespace tidy {
namespace misra {
namespace c2012 {

void Rule6p2::registerMatchers(ast_matchers::MatchFinder *Finder) {
  using namespace ast_matchers;
  Finder->addMatcher(fieldDecl(hasType(isInteger())).bind("FieldDecl"), this);
}

void Rule6p2::check(const ast_matchers::MatchFinder::MatchResult &Result) {
  const auto *FD = Result.Nodes.getNodeAs<FieldDecl>("FieldDecl");

  // Rule applies only to bit fields
  if (!FD->isBitField()) {
    return;
  }

  // Rule applies only to single bit fields
  if (FD->getBitWidthValue(*Result.Context) != 1) {
    return;
  }

  // Rule does not apply to unnamed bit fields
  if (FD->isUnnamedBitfield()) {
    return;
  }

  // At this stage, a signed integer type qualifies as error
  if (FD->getType().getTypePtrOrNull()) {
    if (FD->getType().getTypePtr()->isSignedIntegerType()) {
      diag(FD->getInnerLocStart(),
           "Single-bit Named bit fields shall not be of a signed type");
    }
  } else {
    llvm::errs() << "null check fail\n";
  }
}

} // namespace c2012
} // namespace misra
} // namespace tidy
} // namespace clang
