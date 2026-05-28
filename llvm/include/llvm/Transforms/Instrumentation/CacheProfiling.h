//===- CacheProfiling.h - inline L1 cache simulation ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CacheProfilingPass instruments scalar loads and stores with an inlined
// direct-mapped L1 cache simulator. Hit/miss counters live in TLS and are
// dumped to a cache profile file at process exit by the compiler-rt
// runtime (compiler-rt/lib/profile/InstrProfilingCache.c).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_CACHEPROFILING_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_CACHEPROFILING_H

#include "llvm/IR/PassManager.h"
#include "llvm/Support/Compiler.h"

namespace llvm {
class Module;

struct CacheProfilingPass : public PassInfoMixin<CacheProfilingPass> {
  LLVM_ABI PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_INSTRUMENTATION_CACHEPROFILING_H
