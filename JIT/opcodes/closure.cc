#include "Python.h"

#include "JIT/opcodes/closure.h"
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

OpcodeClosure::OpcodeClosure(LlvmFunctionBuilder *fbuilder) :
    fbuilder_(fbuilder), state_(fbuilder->state())
{
}

void
OpcodeClosure::LOAD_CLOSURE(int freevars_index)
{
    Value *cell = fbuilder_->builder_.CreateLoad(
        fbuilder_->builder_.CreateGEP(
            fbuilder_->freevars_,
            ConstantInt::get(Type::getInt32Ty(fbuilder_->context_),
                             freevars_index)));
    state_->IncRef(cell);
    fbuilder_->Push(cell);
}

void
OpcodeClosure::MAKE_CLOSURE(int num_defaults)
{
    Value *code_object = fbuilder_->Pop();
    Function *pyfunction_new = state_->GetGlobalFunction<
        PyObject *(PyObject *, PyObject *)>("PyFunction_New");
    Value *func_object = state_->CreateCall(
        pyfunction_new, code_object, fbuilder_->globals_,
        "MAKE_CLOSURE_result");
    state_->DecRef(code_object);
    fbuilder_->PropagateExceptionOnNull(func_object);
    Value *closure = fbuilder_->Pop();
    Function *pyfunction_setclosure = state_->GetGlobalFunction<
        int(PyObject *, PyObject *)>("PyFunction_SetClosure");
    Value *setclosure_result = state_->CreateCall(
        pyfunction_setclosure, func_object, closure,
        "MAKE_CLOSURE_setclosure_result");
    state_->DecRef(closure);
    fbuilder_->PropagateExceptionOnNonZero(setclosure_result);
    if (num_defaults > 0) {
        // Effectively inline BuildSequenceLiteral and
        // PropagateExceptionOnNull so we can DecRef func_object on error.
        BasicBlock *failure =
            state_->CreateBasicBlock("MAKE_CLOSURE_failure");
        BasicBlock *success =
            state_->CreateBasicBlock("MAKE_CLOSURE_success");

        Value *tupsize = ConstantInt::get(
            PyTypeBuilder<Py_ssize_t>::get(fbuilder_->context_), num_defaults);
        Function *pytuple_new =
            state_->GetGlobalFunction<PyObject *(Py_ssize_t)>("PyTuple_New");
        Value *defaults = state_->CreateCall(pytuple_new, tupsize,
                                             "MAKE_CLOSURE_defaults");
        fbuilder_->builder_.CreateCondBr(state_->IsNull(defaults),
                                         failure, success);

        fbuilder_->builder_.SetInsertPoint(failure);
        state_->DecRef(func_object);
        fbuilder_->PropagateException();

        fbuilder_->builder_.SetInsertPoint(success);
        // XXX(twouters): do this with a memcpy?
        while (--num_defaults >= 0) {
            Value *itemslot = state_->GetTupleItemSlot(defaults,
                                                       num_defaults);
            fbuilder_->builder_.CreateStore(fbuilder_->Pop(), itemslot);
        }
        // End of inlining.
        Function *pyfunction_setdefaults = state_->GetGlobalFunction<
            int(PyObject *, PyObject *)>("PyFunction_SetDefaults");
        Value *setdefaults_result = state_->CreateCall(
            pyfunction_setdefaults, func_object, defaults,
            "MAKE_CLOSURE_setdefaults_result");
        state_->DecRef(defaults);
        fbuilder_->PropagateExceptionOnNonZero(setdefaults_result);
    }
    fbuilder_->Push(func_object);
}

void
OpcodeClosure::LOAD_DEREF(int index)
{
    BasicBlock *failed_load =
        state_->CreateBasicBlock("LOAD_DEREF_failed_load");
    BasicBlock *unbound_local =
        state_->CreateBasicBlock("LOAD_DEREF_unbound_local");
    BasicBlock *error =
        state_->CreateBasicBlock("LOAD_DEREF_error");
    BasicBlock *success =
        state_->CreateBasicBlock("LOAD_DEREF_success");

    Value *cell = fbuilder_->builder_.CreateLoad(
        fbuilder_->builder_.CreateGEP(
            fbuilder_->freevars_,
            ConstantInt::get(Type::getInt32Ty(fbuilder_->context_),
                             index)));
    Function *pycell_get = state_->GetGlobalFunction<
        PyObject *(PyObject *)>("PyCell_Get");
    Value *value = state_->CreateCall(
        pycell_get, cell, "LOAD_DEREF_cell_contents");
    fbuilder_->builder_.CreateCondBr(state_->IsNull(value),
                                     failed_load, success);

    fbuilder_->builder_.SetInsertPoint(failed_load);
    Function *pyerr_occurred =
        state_->GetGlobalFunction<PyObject *()>("PyErr_Occurred");
    Value *was_err =
        state_->CreateCall(pyerr_occurred, "LOAD_DEREF_err_occurred");
    fbuilder_->builder_.CreateCondBr(state_->IsNull(was_err),
                                     unbound_local, error);

    fbuilder_->builder_.SetInsertPoint(unbound_local);
    Function *do_raise =
        state_->GetGlobalFunction<void(PyFrameObject*, int)>(
            "_PyEval_RaiseForUnboundFreeVar");
    state_->CreateCall(
        do_raise, fbuilder_->frame_,
        ConstantInt::get(PyTypeBuilder<int>::get(fbuilder_->context_), index));

    fbuilder_->FallThroughTo(error);
    fbuilder_->PropagateException();

    fbuilder_->builder_.SetInsertPoint(success);
    fbuilder_->Push(value);
}

void
OpcodeClosure::STORE_DEREF(int index)
{
    Value *value = fbuilder_->Pop();
    Value *cell = fbuilder_->builder_.CreateLoad(
        fbuilder_->builder_.CreateGEP(
            fbuilder_->freevars_,
            ConstantInt::get(Type::getInt32Ty(fbuilder_->context_),
                             index)));
    Function *pycell_set = state_->GetGlobalFunction<
        int(PyObject *, PyObject *)>("PyCell_Set");
    Value *result = state_->CreateCall(
        pycell_set, cell, value, "STORE_DEREF_result");
    state_->DecRef(value);
    // eval.cc doesn't actually check the return value of this, I guess
    // we are a little more likely to do things wrong.
    fbuilder_->PropagateExceptionOnNonZero(result);
}

}
