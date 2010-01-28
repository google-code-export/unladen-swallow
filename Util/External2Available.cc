// This utility converts external GlobalValues to available_externally.  We use
// this to expose the Python standard library to the inliner without letting the
// JIT actually codegen bits of it.
//
// Usage: External2Available [inputfilename] [-o outputfilename]
//
// 'inputfilename' defaults to stdin and can be either LLVM assembly or bitcode.
// 'outputfilename' will be bitcode and defaults to stdout.  It's safe to have
// inputfilename==outputfilename.
//
// $ cat demo.ll
// define i32 @square(i32 %x) {
// entry:
//   %res = mul i32 %x, %x
//   ret i32 %res
// }
// $ External2Available demo.ll -o demo_avail.bc
// $ llvm-dis < demo_avail.bc
// ; ModuleID = '<stdin>'
//
// define available_externally i32 @square(i32 %x) {
// entry:
//   %res = mul i32 %x, %x                           ; <i32> [#uses=1]
//   ret i32 %res
// }
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "external2availableexternally"
#include "llvm/GlobalValue.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/IRReader.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
using namespace llvm;

STATISTIC(NumConverted, "Number of globals marked available");

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
    cl::init("-"), cl::value_desc("filename"));
static cl::opt<std::string>
OutputFilename("o", cl::desc("Override output filename"),
               cl::value_desc("filename"), cl::init("-"));

static void ConvertLinkage(GlobalValue &GV) {
    if (GV.hasExternalLinkage() && !GV.isDeclaration()) {
        GV.setLinkage(GlobalValue::AvailableExternallyLinkage);
        DEBUG(errs() << "Marked " << GV.getName() << " available_externally\n");
        ++NumConverted;
    }
}

int main(int argc, char *argv[]) {
    llvm_shutdown_obj X;  // Call llvm_shutdown() on exit.
    cl::ParseCommandLineOptions(argc, argv);

    LLVMContext context;

    SMDiagnostic diagnostic;
    const OwningPtr<Module> M(
        ParseIRFile(InputFilename.c_str(), diagnostic, context));
    if (M == NULL) {
        diagnostic.Print(argv[0], errs());
        return 1;
    }

    std::for_each(M->begin(), M->end(), ConvertLinkage);
    std::for_each(M->global_begin(), M->global_end(), ConvertLinkage);

    std::string errors;
    raw_fd_ostream out(OutputFilename.c_str(), errors,
                       raw_fd_ostream::F_Binary);
    WriteBitcodeToFile(M.get(), out);
}
