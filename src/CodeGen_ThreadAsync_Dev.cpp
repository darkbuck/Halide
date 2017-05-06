#include "CodeGen_ThreadAsync_Dev.h"
#include "CodeGen_Internal.h"
#include "IROperator.h"
#include "IRPrinter.h"
#include "Debug.h"
#include "Target.h"
#include "LLVM_Headers.h"
#include "LLVM_Runtime_Linker.h"

namespace Halide {
namespace Internal {

using std::vector;
using std::string;

using namespace llvm;

CodeGen_ThreadAsync_Dev::CodeGen_ThreadAsync_Dev(Target host) {
}

void CodeGen_ThreadAsync_Dev::add_kernel(Stmt stmt,
                                 const std::string &name,
                                 const std::vector<DeviceArgument> &args) {
    user_error << "Cannot actually schedule anything on experimental thread async device. Use define_extern instead.\n";
}

}}
