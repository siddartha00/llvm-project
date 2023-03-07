//===--- MISRATidyModule.cpp - clang-tidy ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../ClangTidy.h"
#include "../ClangTidyModule.h"
#include "../ClangTidyModuleRegistry.h"
#include "c2012/6p2Check.h"

namespace clang {
namespace tidy {
namespace misra {

class MISRAModule : public ClangTidyModule {
public:
 void addCheckFactories(ClangTidyCheckFactories &CheckFactories) override {
   CheckFactories.registerCheck<c2012::Rule6p2>("misra-c2012-6.2");
 }

 ClangTidyOptions getModuleOptions() override {
   ClangTidyOptions Options;
   return Options;
 }
};

} // namespace misra

// Register the MiscTidyModule using this statically initialized variable.
static ClangTidyModuleRegistry::Add<misra::MISRAModule>
   X("misra-module",
     "Adds lint checks corresponding to MISRA safe coding guidelines.");

// This anchor is used to force the linker to link in the generated object file
// and thus register the CERTModule.
volatile int MISRAModuleAnchorSource = 0;

} // namespace tidy
} // namespace clang
