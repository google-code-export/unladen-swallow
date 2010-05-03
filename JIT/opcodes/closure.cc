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
    fbuilder_(fbuilder)
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
    fbuilder_->IncRef(cell);
    fbuilder_->Push(cell);
}

void
OpcodeClosure::MAKE_CLOSURE(int num_defaults)
{
    Value *code_object = fbuilder_->Pop();
    Function *pyfunction_new = fbuilder_->GetGlobalFunction<
        PyObject *(PyObject *, PyObject *)>("PyFunction_New");
    Value *func_object = fbuilder_->CreateCall(
        pyfunction_new, code_object, fbuilder_->globals_,
        "MAKE_CLOSURE_result");
    fbuilder_->DecRef(code_object);
    fbuilder_->PropagateExceptionOnNull(func_object);
    Value *closure = fbuilder_->Pop();
    Function *pyfunction_setclosure = fbuilder_->GetGlobalFunction<
        int(PyObject *, PyObject *)>("PyFunction_SetClosure");
    Value *setclosure_result = fbuilder_->CreateCall(
        pyfunction_setclosure, func_object, closure,
        "MAKE_CLOSURE_setclosure_result");
    fbuilder_->DecRef(closure);
    fbuilder_->PropagateExceptionOnNonZero(setclosure_result);
    if (num_defaults > 0) {
        // Effectively inline BuildSequenceLiteral and
        // PropagateExceptionOnNull so we can DecRef func_object on error.
        BasicBlock *failure =
            fbuilder_->CreateBasicBlock("MAKE_CLOSURE_failure");
        BasicBlock *success =
            fbuilder_->CreateBasicBlock("MAKE_CLOSURE_success");

        Value *tupsize = ConstantInt::get(
            PyTypeBuilder<Py_ssize_t>::get(fbuilder_->context_), num_defaults);
        Function *pytuple_new =
            fbuilder_->GetGlobalFunction<PyObject *(Py_ssize_t)>("PyTuple_New");
        Value *defaults = fbuilder_->CreateCall(pytuple_new, tupsize,
                                                    "MAKE_CLOSURE_defaults");
        fbuilder_->builder_.CreateCondBr(fbuilder_->IsNull(defaults),
                                    failure, success);

        fbuilder_->builder_.SetInsertPoint(failure);
        fbuilder_->DecRef(func_object);
        fbuilder_->PropagateException();

        fbuilder_->builder_.SetInsertPoint(success);
        // XXX(twouters): do this with a memcpy?
        while (--num_defaults >= 0) {
            Value *itemslot = fbuilder_->GetTupleItemSlot(defaults,
                                                          num_defaults);
            fbuilder_->builder_.CreateStore(fbuilder_->Pop(), itemslot);
        }
        // End of inlining.
        Function *pyfunction_setdefaults = fbuilder_->GetGlobalFunction<
            int(PyObject *, PyObject *)>("PyFunction_SetDefaults");
        Value *setdefaults_result = fbuilder_->CreateCall(
            pyfunction_setdefaults, func_object, defaults,
            "MAKE_CLOSURE_setdefaults_result");
        fbuilder_->DecRef(defaults);
        fbuilder_->PropagateExceptionOnNonZero(setdefaults_result);
    }
    fbuilder_->Push(func_object);
}

void
OpcodeClosure::LOAD_DEREF(int index)
{
    BasicBlock *failed_load =
        fbuilder_->CreateBasicBlock("LOAD_DEREF_failed_load");
    BasicBlock *unbound_local =
        fbuilder_->CreateBasicBlock("LOAD_DEREF_unbound_local");
    BasicBlock *error =
        fbuilder_->CreateBasicBlock("LOAD_DEREF_error");
    BasicBlock *success =
        fbuilder_->CreateBasicBlock("LOAD_DEREF_success");

    Value *cell = fbuilder_->builder_.CreateLoad(
        fbuilder_->builder_.CreateGEP(
            fbuilder_->freevars_,
            ConstantInt::get(Type::getInt32Ty(fbuilder_->context_),
                             index)));
    Function *pycell_get = fbuilder_->GetGlobalFunction<
        PyObject *(PyObject *)>("PyCell_Get");
    Value *value = fbuilder_->CreateCall(
        pycell_get, cell, "LOAD_DEREF_cell_contents");
    fbuilder_->builder_.CreateCondBr(fbuilder_->IsNull(value),
                                     failed_load, success);

    fbuilder_->builder_.SetInsertPoint(failed_load);
    Function *pyerr_occurred =
        fbuilder_->GetGlobalFunction<PyObject *()>("PyErr_Occurred");
    Value *was_err =
        fbuilder_->CreateCall(pyerr_occurred, "LOAD_DEREF_err_occurred");
    fbuilder_->builder_.CreateCondBr(fbuilder_->IsNull(was_err),
                                     unbound_local, error);

    fbuilder_->builder_.SetInsertPoint(unbound_local);
    Function *do_raise =
        fbuilder_->GetGlobalFunction<void(PyFrameObject*, int)>(
            "_PyEval_RaiseForUnboundFreeVar");
    fbuilder_->CreateCall(
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
    Function *pycell_set = fbuilder_->GetGlobalFunction<
        int(PyObject *, PyObject *)>("PyCell_Set");
    Value *result = fbuilder_->CreateCall(
        pycell_set, cell, value, "STORE_DEREF_result");
    fbuilder_->DecRef(value);
    // eval.cc doesn't actually check the return value of this, I guess
    // we are a little more likely to do things wrong.
    fbuilder_->PropagateExceptionOnNonZero(result);
}

}
