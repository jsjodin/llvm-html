//===- HTMLWriter.cpp - Printing LLVM as HTML file--- ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This library implements `print` family of functions in classes like
// Module, Function, Value, etc. In-memory representation of those classes is
// converted to IR strings.
//
// Note that these routines must be extremely tolerant of various errors in the
// LLVM code, because it can be used for debugging transformations.
//
//===----------------------------------------------------------------------===//

using namespace llvm;

class HTMLWriter {
private:
  const Module *M;
public:
  HTMLWriter(const Module *M) : M(M) {}

  void print(raw_ostream &ROS, raw_ostream &CSSROS,
             std::string CSSFileName,
             AssemblyAnnotationWriter *AAW,
             bool ShouldPreserveUseListOrder = false,
             bool IsForDebug = false) const;
};
