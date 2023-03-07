set(LLVM_LINK_COMPONENTS
 support
 FrontendOpenMP
 )

add_clang_library(clangTidyMISRAModule
c2012/6p2Check.cpp
 MISRATidyModule.cpp
 c2012/6p2Check.cpp
 LINK_LIBS
 clangTidy
 clangTidyBugproneModule
 clangTidyGoogleModule
 clangTidyMiscModule
 clangTidyPerformanceModule
 clangTidyReadabilityModule
 clangTidyUtils

 DEPENDS
 omp_gen
 )

clang_target_link_libraries(clangTidyMISRAModule
 PRIVATE
 clangAST
 clangASTMatchers
 clangBasic
 clangLex
 )