#include "Python.h"

#include "JIT/opcodes/unaryops.h"
#include "JIT/llvm_fbuilder.h"

#include "llvm/BasicBlock.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"

using llvm::BasicBlock;
using llvm::Function;
using llvm::Value;

namespace py {

OpcodeUnaryops::OpcodeUnaryops(LlvmFunctionBuilder *fbuilder) :
    fbuilder_(fbuilder)
{
}

// Implementation of almost all unary operations
void
OpcodeUnaryops::GenericUnaryOp(const char *apifunc)
{
    Value *value = fbuilder_->Pop();
    Function *op = fbuilder_->GetGlobalFunction<PyObject*(PyObject*)>(apifunc);
    Value *result = fbuilder_->CreateCall(op, value, "unaryop_result");
    fbuilder_->DecRef(value);
    fbuilder_->PropagateExceptionOnNull(result);
    fbuilder_->Push(result);
}

#define UNARYOP_METH(NAME, APIFUNC)			\
void							\
OpcodeUnaryops::NAME()				\
{							\
    this->GenericUnaryOp(#APIFUNC);			\
}

UNARYOP_METH(UNARY_CONVERT, PyObject_Repr)
UNARYOP_METH(UNARY_INVERT, PyNumber_Invert)
UNARYOP_METH(UNARY_POSITIVE, PyNumber_Positive)
UNARYOP_METH(UNARY_NEGATIVE, PyNumber_Negative)

#undef UNARYOP_METH

void
OpcodeUnaryops::UNARY_NOT()
{
    Value *value = fbuilder_->Pop();
    Value *retval = fbuilder_->builder_.CreateSelect(
        fbuilder_->IsPythonTrue(value),
        fbuilder_->GetGlobalVariableFor((PyObject*)&_Py_ZeroStruct),
        fbuilder_->GetGlobalVariableFor((PyObject*)&_Py_TrueStruct),
        "UNARY_NOT_result");
    fbuilder_->IncRef(retval);
    fbuilder_->Push(retval);
}

}
