#include "Python.h"

#include "JIT/opcodes/locals.h"
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

OpcodeLocals::OpcodeLocals(LlvmFunctionBuilder *fbuilder) :
    fbuilder_(fbuilder)
{
}

void
OpcodeLocals::LOAD_CONST(int index)
{
    PyObject *co_consts = fbuilder_->code_object_->co_consts;
    Value *const_ = fbuilder_->builder_.CreateBitCast(
        fbuilder_->GetGlobalVariableFor(PyTuple_GET_ITEM(co_consts, index)),
        PyTypeBuilder<PyObject*>::get(fbuilder_->context_));
    fbuilder_->IncRef(const_);
    fbuilder_->Push(const_);
}

// TODO(collinwinter): we'd like to implement this by simply marking the load
// as "cannot be NULL" and let LLVM's constant propgation optimizers remove the
// conditional branch for us. That is currently not supported, so we do this
// manually.
void
OpcodeLocals::LOAD_FAST(int index)
{
    // Simple check: if DELETE_FAST is never used, function parameters cannot
    // be NULL.
    if (!fbuilder_->uses_delete_fast && index < fbuilder_->GetParamCount())
        this->LOAD_FAST_fast(index);
    else
        this->LOAD_FAST_safe(index);
}

void
OpcodeLocals::LOAD_FAST_fast(int index)
{
    Value *local = fbuilder_->builder_.CreateLoad(
        fbuilder_->locals_[index], "FAST_loaded");
#ifndef NDEBUG
    Value *frame_local_slot = fbuilder_->builder_.CreateGEP(
        fbuilder_->fastlocals_,
        ConstantInt::get(Type::getInt32Ty(fbuilder_->context_),
                         index));
    Value *frame_local = fbuilder_->builder_.CreateLoad(frame_local_slot);
    Value *sane_locals = fbuilder_->builder_.CreateICmpEQ(frame_local, local);
    fbuilder_->Assert(sane_locals, "alloca locals do not match frame locals!");
#endif  /* NDEBUG */
    fbuilder_->IncRef(local);
    fbuilder_->Push(local);
}

void
OpcodeLocals::LOAD_FAST_safe(int index)
{
    BasicBlock *unbound_local =
        fbuilder_->CreateBasicBlock("LOAD_FAST_unbound");
    BasicBlock *success =
        fbuilder_->CreateBasicBlock("LOAD_FAST_success");

    Value *local = fbuilder_->builder_.CreateLoad(
        fbuilder_->locals_[index], "FAST_loaded");
#ifndef NDEBUG
    Value *frame_local_slot = fbuilder_->builder_.CreateGEP(
        fbuilder_->fastlocals_,
        ConstantInt::get(Type::getInt32Ty(fbuilder_->context_),
                         index));
    Value *frame_local = fbuilder_->builder_.CreateLoad(frame_local_slot);
    Value *sane_locals = fbuilder_->builder_.CreateICmpEQ(frame_local, local);
    fbuilder_->Assert(sane_locals, "alloca locals do not match frame locals!");
#endif  /* NDEBUG */
    fbuilder_->builder_.CreateCondBr(fbuilder_->IsNull(local),
                                     unbound_local, success);

    fbuilder_->builder_.SetInsertPoint(unbound_local);
    Function *do_raise =
        fbuilder_->GetGlobalFunction<void(PyFrameObject*, int)>(
            "_PyEval_RaiseForUnboundLocal");
    fbuilder_->CreateCall(do_raise, fbuilder_->frame_,
                          fbuilder_->GetSigned<int>(index));
    fbuilder_->PropagateException();

    fbuilder_->builder_.SetInsertPoint(success);
    fbuilder_->IncRef(local);
    fbuilder_->Push(local);
}

void
OpcodeLocals::STORE_FAST(int index)
{
    this->SetLocal(index, fbuilder_->Pop());
}

void
OpcodeLocals::SetLocal(int locals_index, llvm::Value *new_value)
{
    // We write changes twice: once to our LLVM-visible locals, and again to the
    // PyFrameObject. This makes vars(), locals() and dir() happy.
    Value *frame_local_slot = fbuilder_->builder_.CreateGEP(
        fbuilder_->fastlocals_, 
        ConstantInt::get(Type::getInt32Ty(fbuilder_->context_),
                         locals_index));
    fbuilder_->llvm_data_->tbaa_locals.MarkInstruction(frame_local_slot);
    fbuilder_->builder_.CreateStore(new_value, frame_local_slot);

    Value *llvm_local_slot = fbuilder_->locals_[locals_index];
    Value *orig_value =
        fbuilder_->builder_.CreateLoad(llvm_local_slot,
                                       "llvm_local_overwritten");
    fbuilder_->builder_.CreateStore(new_value, llvm_local_slot);
    fbuilder_->XDecRef(orig_value);
}

void
OpcodeLocals::DELETE_FAST(int index)
{
    BasicBlock *failure =
        fbuilder_->CreateBasicBlock("DELETE_FAST_failure");
    BasicBlock *success =
        fbuilder_->CreateBasicBlock("DELETE_FAST_success");
    Value *local_slot = fbuilder_->locals_[index];
    Value *orig_value = fbuilder_->builder_.CreateLoad(
        local_slot, "DELETE_FAST_old_reference");
    fbuilder_->builder_.CreateCondBr(fbuilder_->IsNull(orig_value),
                                     failure, success);

    fbuilder_->builder_.SetInsertPoint(failure);
    Function *do_raise = fbuilder_->GetGlobalFunction<
        void(PyFrameObject *, int)>("_PyEval_RaiseForUnboundLocal");
    fbuilder_->CreateCall(
        do_raise, fbuilder_->frame_,
        ConstantInt::getSigned(
            PyTypeBuilder<int>::get(fbuilder_->context_), index));
    fbuilder_->PropagateException();

    /* We clear both the LLVM-visible locals and the PyFrameObject's locals to
       make vars(), dir() and locals() happy. */
    fbuilder_->builder_.SetInsertPoint(success);
    Value *frame_local_slot = fbuilder_->builder_.CreateGEP(
        fbuilder_->fastlocals_,
        ConstantInt::get(Type::getInt32Ty(fbuilder_->context_),
                         index));
    fbuilder_->builder_.CreateStore(fbuilder_->GetNull<PyObject*>(),
                                    frame_local_slot);
    fbuilder_->builder_.CreateStore(fbuilder_->GetNull<PyObject*>(),
                                    local_slot);
    fbuilder_->DecRef(orig_value);
}

}
