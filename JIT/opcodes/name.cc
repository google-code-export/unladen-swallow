#include "Python.h"

#include "JIT/opcodes/name.h"
#include "JIT/llvm_fbuilder.h"

#include "llvm/BasicBlock.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"

using llvm::BasicBlock;
using llvm::ConstantInt;
using llvm::Function;
using llvm::Type;
using llvm::Value;

namespace py {

OpcodeName::OpcodeName(LlvmFunctionBuilder *fbuilder) :
    fbuilder_(fbuilder)
{
}

void
OpcodeName::LOAD_NAME(int index)
{
    Value *result = fbuilder_->CreateCall(
        fbuilder_->GetGlobalFunction<PyObject *(PyFrameObject*, int)>(
            "_PyEval_LoadName"),
        fbuilder_->frame_,
        ConstantInt::get(PyTypeBuilder<int>::get(fbuilder_->context_), index));
    fbuilder_->PropagateExceptionOnNull(result);
    fbuilder_->Push(result);
}

void
OpcodeName::STORE_NAME(int index)
{
    Value *to_store = fbuilder_->Pop();
    Value *err = fbuilder_->CreateCall(
        fbuilder_->GetGlobalFunction<int(PyFrameObject*, int, PyObject*)>(
            "_PyEval_StoreName"),
        fbuilder_->frame_,
        ConstantInt::get(PyTypeBuilder<int>::get(fbuilder_->context_), index),
        to_store);
    fbuilder_->PropagateExceptionOnNonZero(err);
}

void
OpcodeName::DELETE_NAME(int index)
{
    Value *err = fbuilder_->CreateCall(
        fbuilder_->GetGlobalFunction<int(PyFrameObject*, int)>(
            "_PyEval_DeleteName"),
        fbuilder_->frame_,
        ConstantInt::get(PyTypeBuilder<int>::get(fbuilder_->context_), index));
    fbuilder_->PropagateExceptionOnNonZero(err);
}

}
