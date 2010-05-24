#include "Python.h"

#include "JIT/opcodes/loop.h"
#include "JIT/llvm_fbuilder.h"

#include "llvm/BasicBlock.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"

using llvm::BasicBlock;
using llvm::ConstantInt;
using llvm::Function;
using llvm::Type;
using llvm::Value;

// Use like "this->GET_GLOBAL_VARIABLE(Type, variable)".
#define GET_GLOBAL_VARIABLE(TYPE, VARIABLE) \
    GetGlobalVariable<TYPE>(&VARIABLE, #VARIABLE)

namespace py {

OpcodeLoop::OpcodeLoop(LlvmFunctionBuilder *fbuilder) :
    fbuilder_(fbuilder), state_(fbuilder->state())
{
}

void
OpcodeLoop::GET_ITER()
{
    Value *obj = fbuilder_->Pop();
    Function *pyobject_getiter =
        state_->GetGlobalFunction<PyObject*(PyObject*)>(
        "PyObject_GetIter");
    Value *iter = state_->CreateCall(pyobject_getiter, obj);
    state_->DecRef(obj);
    fbuilder_->PropagateExceptionOnNull(iter);
    fbuilder_->Push(iter);
}

void
OpcodeLoop::FOR_ITER(llvm::BasicBlock *target,
                     llvm::BasicBlock *fallthrough)
{
    Value *iter = fbuilder_->Pop();
    Value *iter_tp = fbuilder_->builder_.CreateBitCast(
        fbuilder_->builder_.CreateLoad(
            ObjectTy::ob_type(fbuilder_->builder_, iter)),
        PyTypeBuilder<PyTypeObject *>::get(fbuilder_->context_),
        "iter_type");
    Value *iternext = fbuilder_->builder_.CreateLoad(
        TypeTy::tp_iternext(fbuilder_->builder_, iter_tp),
        "iternext");
    Value *next = state_->CreateCall(iternext, iter, "next");
    BasicBlock *got_next = state_->CreateBasicBlock("got_next");
    BasicBlock *next_null = state_->CreateBasicBlock("next_null");
    fbuilder_->builder_.CreateCondBr(state_->IsNull(next),
                                     next_null, got_next);

    fbuilder_->builder_.SetInsertPoint(next_null);
    Value *err_occurred = state_->CreateCall(
        state_->GetGlobalFunction<PyObject*()>("PyErr_Occurred"));
    BasicBlock *iter_ended = state_->CreateBasicBlock("iter_ended");
    BasicBlock *exception = state_->CreateBasicBlock("exception");
    fbuilder_->builder_.CreateCondBr(state_->IsNull(err_occurred),
                                     iter_ended, exception);

    fbuilder_->builder_.SetInsertPoint(exception);
    Value *exc_stopiteration = fbuilder_->builder_.CreateLoad(
        state_->GET_GLOBAL_VARIABLE(PyObject*, PyExc_StopIteration));
    Value *was_stopiteration = state_->CreateCall(
        state_->GetGlobalFunction<int(PyObject *)>("PyErr_ExceptionMatches"),
        exc_stopiteration);
    BasicBlock *clear_err = state_->CreateBasicBlock("clear_err");
    BasicBlock *propagate = state_->CreateBasicBlock("propagate");
    fbuilder_->builder_.CreateCondBr(state_->IsNonZero(was_stopiteration),
                                     clear_err, propagate);

    fbuilder_->builder_.SetInsertPoint(propagate);
    state_->DecRef(iter);
    fbuilder_->PropagateException();

    fbuilder_->builder_.SetInsertPoint(clear_err);
    state_->CreateCall(state_->GetGlobalFunction<void()>("PyErr_Clear"));
    fbuilder_->builder_.CreateBr(iter_ended);

    fbuilder_->builder_.SetInsertPoint(iter_ended);
    state_->DecRef(iter);
    fbuilder_->builder_.CreateBr(target);

    fbuilder_->builder_.SetInsertPoint(got_next);
    fbuilder_->Push(iter);
    fbuilder_->Push(next);
}

void
OpcodeLoop::CONTINUE_LOOP(llvm::BasicBlock *target,
                          int target_opindex,
                          llvm::BasicBlock *fallthrough)
{
    // Accept code after a continue statement, even though it's never executed.
    // Otherwise, CPython's willingness to insert code after block
    // terminators causes problems.
    BasicBlock *dead_code = state_->CreateBasicBlock("dead_code");
    fbuilder_->builder_.CreateStore(
        ConstantInt::get(Type::getInt8Ty(fbuilder_->context_),
                         UNWIND_CONTINUE),
        fbuilder_->unwind_reason_addr_);
    Value *unwind_target = fbuilder_->AddUnwindTarget(target, target_opindex);
    // Yes, store the unwind target in the return value slot. This is to
    // keep the translation from eval.cc as close as possible; deviation will
    // only introduce bugs. The UNWIND_CONTINUE cases in the unwind block
    // (see FillUnwindBlock()) will pick this up and deal with it.
    const Type *long_type = PyTypeBuilder<long>::get(fbuilder_->context_);
    Value *pytarget = state_->CreateCall(
            state_->GetGlobalFunction<PyObject *(long)>("PyInt_FromLong"),
            fbuilder_->builder_.CreateZExt(unwind_target, long_type));
    fbuilder_->builder_.CreateStore(pytarget, fbuilder_->retval_addr_);
    fbuilder_->builder_.CreateBr(fbuilder_->unwind_block_);

    fbuilder_->builder_.SetInsertPoint(dead_code);
}

void
OpcodeLoop::BREAK_LOOP()
{
    // Accept code after a break statement, even though it's never executed.
    // Otherwise, CPython's willingness to insert code after block
    // terminators causes problems.
    BasicBlock *dead_code = state_->CreateBasicBlock("dead_code");
    fbuilder_->builder_.CreateStore(
        ConstantInt::get(Type::getInt8Ty(fbuilder_->context_),
                         UNWIND_BREAK),
        fbuilder_->unwind_reason_addr_);
    fbuilder_->builder_.CreateBr(fbuilder_->unwind_block_);

    fbuilder_->builder_.SetInsertPoint(dead_code);
}

}
