//===-- llvm-html.cpp - LLVM to HTML printer---------- --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  llvm-html [options] x.bc - Read LLVM bitcode from the x.bc file, write asm
//                            to the x.html file.
//  Options:
//      --help   - Output information about command line switches
//
//===----------------------------------------------------------------------===//

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ModuleSummaryIndex.h"
#include "llvm/IR/ModuleSlotTracker.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/WithColor.h"
#include <system_error>
#include <regex>
#include "HTMLWriter.h"

using namespace llvm;

static cl::OptionCategory HtmlCategory("HTML Printer Options");

static cl::list<std::string> InputFilenames(cl::Positional,
                                            cl::desc("[input bitcode]..."),
                                            cl::cat(HtmlCategory));

static cl::opt<std::string> OutputFilename("o",
                                           cl::desc("Override output filename"),
                                           cl::value_desc("filename"),
                                           cl::cat(HtmlCategory));

static cl::opt<bool> Force("f", cl::desc("Enable binary output on terminals"),
                           cl::cat(HtmlCategory));

static cl::opt<bool> DontPrint("disable-output",
                               cl::desc("Don't output the .html file"),
                               cl::Hidden, cl::cat(HtmlCategory));

static cl::opt<bool>
    SetImporting("set-importing",
                 cl::desc("Set lazy loading to pretend to import a module"),
                 cl::Hidden, cl::cat(HtmlCategory));

static cl::opt<bool>
    ShowAnnotations("show-annotations",
                    cl::desc("Add informational comments to the .html file"),
                    cl::cat(HtmlCategory));

static cl::opt<bool> PreserveAssemblyUseListOrder(
    "preserve-ll-uselistorder",
    cl::desc("Preserve use-list order when writing LLVM assembly."),
    cl::init(false), cl::Hidden, cl::cat(HtmlCategory));

static cl::opt<bool>
    MaterializeMetadata("materialize-metadata",
                        cl::desc("Load module without materializing metadata, "
                                 "then materialize only the metadata"),
                        cl::cat(HtmlCategory));

static cl::opt<bool> PrintThinLTOIndexOnly(
    "print-thinlto-index-only",
    cl::desc("Only read thinlto index and print the index as LLVM assembly."),
    cl::init(false), cl::Hidden, cl::cat(HtmlCategory));

namespace {

static void printDebugLoc(const DebugLoc &DL, formatted_raw_ostream &OS) {
  OS << DL.getLine() << ":" << DL.getCol();
  if (DILocation *IDL = DL.getInlinedAt()) {
    OS << "@";
    printDebugLoc(IDL, OS);
  }
}

class CommentWriter : public AssemblyAnnotationWriter {
public:
  void emitFunctionAnnot(const Function *F,
                         formatted_raw_ostream &OS) override {
    OS << "; [#uses=" << F->getNumUses() << ']';  // Output # uses
    OS << '\n';
  }
  void printInfoComment(const Value &V, formatted_raw_ostream &OS) override {
    bool Padded = false;
    if (!V.getType()->isVoidTy()) {
      OS.PadToColumn(50);
      Padded = true;
      // Output # uses and type
      OS << "; [#uses=" << V.getNumUses() << " type=" << *V.getType() << "]";
    }
    if (const Instruction *I = dyn_cast<Instruction>(&V)) {
      if (const DebugLoc &DL = I->getDebugLoc()) {
        if (!Padded) {
          OS.PadToColumn(50);
          Padded = true;
          OS << ";";
        }
        OS << " [debug line = ";
        printDebugLoc(DL,OS);
        OS << "]";
      }
      if (const DbgDeclareInst *DDI = dyn_cast<DbgDeclareInst>(I)) {
        if (!Padded) {
          OS.PadToColumn(50);
          OS << ";";
        }
        OS << " [debug variable = " << DDI->getVariable()->getName() << "]";
      }
      else if (const DbgValueInst *DVI = dyn_cast<DbgValueInst>(I)) {
        if (!Padded) {
          OS.PadToColumn(50);
          OS << ";";
        }
        OS << " [debug variable = " << DVI->getVariable()->getName() << "]";
      }
    }
  }
};

struct LLVMHtmlDiagnosticHandler : public DiagnosticHandler {
  char *Prefix;
  LLVMHtmlDiagnosticHandler(char *PrefixPtr) : Prefix(PrefixPtr) {}
  bool handleDiagnostics(const DiagnosticInfo &DI) override {
    raw_ostream &OS = errs();
    OS << Prefix << ": ";
    switch (DI.getSeverity()) {
      case DS_Error: WithColor::error(OS); break;
      case DS_Warning: WithColor::warning(OS); break;
      case DS_Remark: OS << "remark: "; break;
      case DS_Note: WithColor::note(OS); break;
    }

    DiagnosticPrinterRawOStream DP(OS);
    DI.print(DP);
    OS << '\n';

    if (DI.getSeverity() == DS_Error)
      exit(1);
    return true;
  }
};
} // end anon namespace

static void inlineCSS(raw_ostream &OS, std::string &Out, std::string &CSS) {
  std::string ReplacementString;
  ReplacementString += "<style> \n";
  ReplacementString += CSS;
  ReplacementString += "</style> \n";
  OS << std::regex_replace(Out, std::regex("<link.*>"), ReplacementString);
}

static ExitOnError ExitOnErr;

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  ExitOnErr.setBanner(std::string(argv[0]) + ": error: ");

  cl::HideUnrelatedOptions({&HtmlCategory, &getColorCategory()});
  cl::ParseCommandLineOptions(argc, argv, "llvm .bc -> .html emitter\n");

  LLVMContext Context;
  Context.setDiagnosticHandler(
      std::make_unique<LLVMHtmlDiagnosticHandler>(argv[0]));

  if (InputFilenames.size() < 1) {
    InputFilenames.push_back("-");
  } else if (InputFilenames.size() > 1 && !OutputFilename.empty()) {
    errs()
        << "error: output file name cannot be set for multiple input files\n";
    return 1;
  }

  for (std::string InputFilename : InputFilenames) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> BufferOrErr =
        MemoryBuffer::getFileOrSTDIN(InputFilename);
    if (std::error_code EC = BufferOrErr.getError()) {
      WithColor::error() << InputFilename << ": " << EC.message() << '\n';
      return 1;
    }
    std::unique_ptr<MemoryBuffer> MB = std::move(BufferOrErr.get());

    BitcodeFileContents IF = ExitOnErr(llvm::getBitcodeFileContents(*MB));

    const size_t N = IF.Mods.size();

    if (OutputFilename == "-" && N > 1)
      errs() << "only single module bitcode files can be written to stdout\n";

    for (size_t I = 0; I < N; ++I) {
      BitcodeModule MB = IF.Mods[I];

      std::unique_ptr<Module> M;

      if (!PrintThinLTOIndexOnly) {
        M = ExitOnErr(
            MB.getLazyModule(Context, MaterializeMetadata, SetImporting));
        if (MaterializeMetadata)
          ExitOnErr(M->materializeMetadata());
        else
          ExitOnErr(M->materializeAll());
      }

      BitcodeLTOInfo LTOInfo = ExitOnErr(MB.getLTOInfo());
      std::unique_ptr<ModuleSummaryIndex> Index;
      if (LTOInfo.HasSummary)
        Index = ExitOnErr(MB.getSummary());

      std::string FinalFilename(OutputFilename);
      // Just use stdout.  We won't actually print anything on it.
      if (DontPrint)
        FinalFilename = "-";

      if (FinalFilename.empty()) { // Unspecified output, infer it.
        if (InputFilename == "-") {
          FinalFilename = "-";
        } else {
          StringRef IFN = InputFilename;
          FinalFilename = (IFN.endswith(".bc") ? IFN.drop_back(3) : IFN).str();
          if (N > 1)
            FinalFilename += std::string(".") + std::to_string(I);
          FinalFilename += ".html";
        }
      } else {
        if (N > 1)
          FinalFilename += std::string(".") + std::to_string(I);
      }

      std::error_code EC;
      std::unique_ptr<ToolOutputFile> Out(
          new ToolOutputFile(FinalFilename, EC, sys::fs::OF_TextWithCRLF));
      if (EC) {
        errs() << EC.message() << '\n';
        return 1;
      }

      std::unique_ptr<AssemblyAnnotationWriter> Annotator;
      if (ShowAnnotations)
        Annotator.reset(new CommentWriter());

      std::string OutString;
      std::string CSSOutString;
      raw_string_ostream OutOS(OutString);
      raw_string_ostream CSSOutOS(CSSOutString);
      if (!DontPrint) {
        if (M) {
          HTMLWriter HTMLW(M.get());
          HTMLW.print(OutOS, CSSOutOS, "" /* unused filename */,
                      Annotator.get(), PreserveAssemblyUseListOrder);
        }
        if (Index)
          Index->print(Out->os());
      }

      inlineCSS(Out->os(), OutOS.str(), CSSOutOS.str());

      // Declare success.
      Out->keep();
    }
  }

  return 0;
}
