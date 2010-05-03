#include "Python.h"
#include "opcode.h"

#include "JIT/opcodes/block.h"
#include "JIT/llvm_fbuilder.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BasicBlock.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"

using llvm::BasicBlock;
using llvm::ConstantInt;
using llvm::Function;
using llvm::Type;
using llvm::Value;
using llvm::array_endof;

// Use like "this->GET_GLOBAL_VARIABLE(Type, variable)".
#define GET_GLOBAL_VARIABLE(TYPE, VARIABLE) \
    GetGlobalVariable<TYPE>(&VARIABLE, #VARIABLE)

namespace py {

OpcodeBlock::OpcodeBlock(LlvmFunctionBuilder *fbuilder) :
    fbuilder_(fbuilder)
{
}

void
OpcodeBlock::SETUP_LOOP(llvm::BasicBlock *target,
                        int target_opindex,
                        llvm::BasicBlock *fallthrough)
{
    this->CallBlockSetup(::SETUP_LOOP, target, target_opindex);
}

void
OpcodeBlock::POP_BLOCK()
{
    Value *block_info = fbuilder_->CreateCall(
        fbuilder_->GetGlobalFunction<PyTryBlock *(PyTryBlock *, char *)>(
            "_PyLlvm_Frame_BlockPop"),
        fbuilder_->blockstack_addr_,
        fbuilder_->num_blocks_addr_);
    Value *pop_to_level = fbuilder_->builder_.CreateLoad(
        PyTypeBuilder<PyTryBlock>::b_level(fbuilder_->builder_, block_info));
    Value *pop_to_addr =
        fbuilder_->builder_.CreateGEP(fbuilder_->stack_bottom_, pop_to_level);
    fbuilder_->PopAndDecrefTo(pop_to_addr);
}

void
OpcodeBlock::SETUP_EXCEPT(llvm::BasicBlock *target,
                          int target_opindex,
                          llvm::BasicBlock *fallthrough)
{
    this->CallBlockSetup(::SETUP_EXCEPT, target, target_opindex);
}

void
OpcodeBlock::SETUP_FINALLY(llvm::BasicBlock *target,
                           int target_opindex,
                           llvm::BasicBlock *fallthrough)
{
    this->CallBlockSetup(::SETUP_FINALLY, target, target_opindex);
}

void
OpcodeBlock::CallBlockSetup(int block_type, llvm::BasicBlock *handler,
                            int handler_opindex)
{
    Value *stack_level = fbuilder_->GetStackLevel();
    Value *unwind_target_index =
        fbuilder_->AddUnwindTarget(handler, handler_opindex);
    Function *blocksetup =
        fbuilder_->GetGlobalFunction<void(PyTryBlock *, char *, int, int, int)>(
            "_PyLlvm_Frame_BlockSetup");
    Value *args[] = {
        fbuilder_->blockstack_addr_, fbuilder_->num_blocks_addr_,
        ConstantInt::get(PyTypeBuilder<int>::get(fbuilder_->context_),
                         block_type),
        unwind_target_index,
        stack_level
    };
    fbuilder_->CreateCall(blocksetup, args, array_endof(args));
}

void
OpcodeBlock::END_FINALLY()
{
    Value *finally_discriminator = fbuilder_->Pop();
    // END_FINALLY is fairly complicated. It decides what to do based
    // on the top value in the stack.  If that value is an int, it's
    // interpreted as one of the unwind reasons.  If it's an exception
    // type, the next two stack values are the rest of the exception,
    // and it's re-raised.  Otherwise, it's supposed to be None,
    // indicating that the finally was entered through normal control
    // flow.

    BasicBlock *unwind_code =
        fbuilder_->CreateBasicBlock("unwind_code");
    BasicBlock *test_exception =
        fbuilder_->CreateBasicBlock("test_exception");
    BasicBlock *reraise_exception =
        fbuilder_->CreateBasicBlock("reraise_exception");
    BasicBlock *check_none = fbuilder_->CreateBasicBlock("check_none");
    BasicBlock *not_none = fbuilder_->CreateBasicBlock("not_none");
    BasicBlock *finally_fallthrough =
        fbuilder_->CreateBasicBlock("finally_fallthrough");

    fbuilder_->builder_.CreateCondBr(
        fbuilder_->IsInstanceOfFlagClass(finally_discriminator,
                                    Py_TPFLAGS_INT_SUBCLASS),
        unwind_code, test_exception);

    fbuilder_->builder_.SetInsertPoint(unwind_code);
    // The top of the stack was an int, interpreted as an unwind code.
    // If we're resuming a return or continue, the return value or
    // loop target (respectively) is now on top of the stack and needs
    // to be popped off.
    Value *unwind_reason = fbuilder_->builder_.CreateTrunc(
        fbuilder_->CreateCall(
            fbuilder_->GetGlobalFunction<long(PyObject *)>("PyInt_AsLong"),
            finally_discriminator),
        Type::getInt8Ty(fbuilder_->context_),
        "unwind_reason");
    fbuilder_->DecRef(finally_discriminator);
    // Save the unwind reason for when we jump to the unwind block.
    fbuilder_->builder_.CreateStore(unwind_reason,
                                    fbuilder_->unwind_reason_addr_);
    // Check if we need to pop the return value or loop target.
    BasicBlock *pop_retval = fbuilder_->CreateBasicBlock("pop_retval");
    llvm::SwitchInst *should_pop_retval =
        fbuilder_->builder_.CreateSwitch(unwind_reason,
                                         fbuilder_->unwind_block_, 2);
    should_pop_retval->addCase(
        ConstantInt::get(Type::getInt8Ty(fbuilder_->context_),
                         UNWIND_RETURN),
        pop_retval);
    should_pop_retval->addCase(
        ConstantInt::get(Type::getInt8Ty(fbuilder_->context_),
                         UNWIND_CONTINUE),
        pop_retval);

    fbuilder_->builder_.SetInsertPoint(pop_retval);
    // We're continuing a return or continue.  Retrieve its argument.
    fbuilder_->builder_.CreateStore(fbuilder_->Pop(), fbuilder_->retval_addr_);
    fbuilder_->builder_.CreateBr(fbuilder_->unwind_block_);

    fbuilder_->builder_.SetInsertPoint(test_exception);
    Value *is_exception_or_string = fbuilder_->CreateCall(
        fbuilder_->GetGlobalFunction<int(PyObject *)>(
            "_PyLlvm_WrapIsExceptionOrString"),
        finally_discriminator);
    fbuilder_->builder_.CreateCondBr(
        fbuilder_->IsNonZero(is_exception_or_string),
        reraise_exception, check_none);

    fbuilder_->builder_.SetInsertPoint(reraise_exception);
    Value *err_type = finally_discriminator;
    Value *err_value = fbuilder_->Pop();
    Value *err_traceback = fbuilder_->Pop();
    fbuilder_->CreateCall(
        fbuilder_->GetGlobalFunction<void(PyObject *, PyObject *, PyObject *)>(
            "PyErr_Restore"),
        err_type, err_value, err_traceback);
    // This is a "re-raise" rather than a new exception, so we don't
    // jump to the propagate_exception_block_.
    fbuilder_->builder_.CreateStore(fbuilder_->GetNull<PyObject*>(),
                                    fbuilder_->retval_addr_);
    fbuilder_->builder_.CreateStore(
        ConstantInt::get(Type::getInt8Ty(fbuilder_->context_),
                         UNWIND_EXCEPTION),
        fbuilder_->unwind_reason_addr_);
    fbuilder_->builder_.CreateBr(fbuilder_->unwind_block_);

    fbuilder_->builder_.SetInsertPoint(check_none);
    // The contents of the try block push None onto the stack just
    // before falling through to the finally block.  If we didn't get
    // an unwind reason or an exception, we expect to fall through,
    // but for sanity we also double-check that the None is present.
    Value *is_none = fbuilder_->builder_.CreateICmpEQ(
        finally_discriminator,
        fbuilder_->GetGlobalVariableFor(&_Py_NoneStruct));
    fbuilder_->DecRef(finally_discriminator);
    fbuilder_->builder_.CreateCondBr(is_none, finally_fallthrough, not_none);

    fbuilder_->builder_.SetInsertPoint(not_none);
    // If we didn't get a None, raise a SystemError.
    Value *system_error = fbuilder_->builder_.CreateLoad(
        fbuilder_->GET_GLOBAL_VARIABLE(PyObject *, PyExc_SystemError));
    Value *err_msg = fbuilder_->llvm_data_->GetGlobalStringPtr(
        "'finally' pops bad exception");
    fbuilder_->CreateCall(
        fbuilder_->GetGlobalFunction<void(PyObject *, const char *)>(
            "PyErr_SetString"),
        system_error, err_msg);
    fbuilder_->builder_.CreateStore(
        ConstantInt::get(Type::getInt8Ty(fbuilder_->context_),
                         UNWIND_EXCEPTION),
        fbuilder_->unwind_reason_addr_);
    fbuilder_->builder_.CreateBr(fbuilder_->unwind_block_);

    // After falling through into a finally block, we also fall
    // through out of the block.  This has the nice side-effect of
    // avoiding jumps and switch instructions in the common case,
    // although returning out of a finally may still be slower than
    // ideal.
    fbuilder_->builder_.SetInsertPoint(finally_fallthrough);
}

void
OpcodeBlock::WITH_CLEANUP()
{
    /* At the top of the stack are 1-3 values indicating
       how/why we entered the finally clause:
       - TOP = None
       - (TOP, SECOND) = (WHY_{RETURN,CONTINUE}), retval
       - TOP = WHY_*; no retval below it
       - (TOP, SECOND, THIRD) = exc_info()
       Below them is EXIT, the context.__exit__ bound method.
       In the last case, we must call
       EXIT(TOP, SECOND, THIRD)
       otherwise we must call
       EXIT(None, None, None)

       In all cases, we remove EXIT from the stack, leaving
       the rest in the same order.

       In addition, if the stack represents an exception,
       *and* the function call returns a 'true' value, we
       "zap" this information, to prevent END_FINALLY from
       re-raising the exception. (But non-local gotos
       should still be resumed.)
    */

    Value *exc_type = fbuilder_->CreateAllocaInEntryBlock(
        PyTypeBuilder<PyObject*>::get(fbuilder_->context_),
        NULL, "WITH_CLEANUP_exc_type");
    Value *exc_value = fbuilder_->CreateAllocaInEntryBlock(
        PyTypeBuilder<PyObject*>::get(fbuilder_->context_),
        NULL, "WITH_CLEANUP_exc_value");
    Value *exc_traceback = fbuilder_->CreateAllocaInEntryBlock(
        PyTypeBuilder<PyObject*>::get(fbuilder_->context_),
        NULL, "WITH_CLEANUP_exc_traceback");
    Value *exit_func = fbuilder_->CreateAllocaInEntryBlock(
        PyTypeBuilder<PyObject*>::get(fbuilder_->context_),
        NULL, "WITH_CLEANUP_exit_func");

    BasicBlock *handle_none =
        fbuilder_->CreateBasicBlock("WITH_CLEANUP_handle_none");
    BasicBlock *check_int =
        fbuilder_->CreateBasicBlock("WITH_CLEANUP_check_int");
    BasicBlock *handle_int =
        fbuilder_->CreateBasicBlock("WITH_CLEANUP_handle_int");
    BasicBlock *handle_ret_cont =
        fbuilder_->CreateBasicBlock("WITH_CLEANUP_handle_ret_cont");
    BasicBlock *handle_default =
        fbuilder_->CreateBasicBlock("WITH_CLEANUP_handle_default");
    BasicBlock *handle_else =
        fbuilder_->CreateBasicBlock("WITH_CLEANUP_handle_else");
    BasicBlock *main_block =
        fbuilder_->CreateBasicBlock("WITH_CLEANUP_main_block");

    Value *none = fbuilder_->GetGlobalVariableFor(&_Py_NoneStruct);
    fbuilder_->builder_.CreateStore(fbuilder_->Pop(), exc_type);

    Value *is_none = fbuilder_->builder_.CreateICmpEQ(
        fbuilder_->builder_.CreateLoad(exc_type), none,
        "reason_is_none");
    fbuilder_->builder_.CreateCondBr(is_none, handle_none, check_int);

    fbuilder_->builder_.SetInsertPoint(handle_none);
    fbuilder_->builder_.CreateStore(fbuilder_->Pop(), exit_func);
    fbuilder_->Push(fbuilder_->builder_.CreateLoad(exc_type));
    fbuilder_->builder_.CreateStore(none, exc_value);
    fbuilder_->builder_.CreateStore(none, exc_traceback);
    fbuilder_->builder_.CreateBr(main_block);

    fbuilder_->builder_.SetInsertPoint(check_int);
    Value *is_int = fbuilder_->CreateCall(
        fbuilder_->GetGlobalFunction<int(PyObject *)>("_PyLlvm_WrapIntCheck"),
        fbuilder_->builder_.CreateLoad(exc_type),
        "WITH_CLEANUP_pyint_check");
    fbuilder_->builder_.CreateCondBr(fbuilder_->IsNonZero(is_int),
                                     handle_int, handle_else);

    fbuilder_->builder_.SetInsertPoint(handle_int);
    Value *unboxed = fbuilder_->builder_.CreateTrunc(
        fbuilder_->CreateCall(
            fbuilder_->GetGlobalFunction<long(PyObject *)>("PyInt_AsLong"),
            fbuilder_->builder_.CreateLoad(exc_type)),
        Type::getInt8Ty(fbuilder_->context_),
        "unboxed_unwind_reason");
    // The LLVM equivalent of
    // switch (reason)
    //   case UNWIND_RETURN:
    //   case UNWIND_CONTINUE:
    //     ...
    //     break;
    //   default:
    //     break;
    llvm::SwitchInst *unwind_kind =
        fbuilder_->builder_.CreateSwitch(unboxed, handle_default, 2);
    unwind_kind->addCase(ConstantInt::get(Type::getInt8Ty(fbuilder_->context_),
                                          UNWIND_RETURN),
                         handle_ret_cont);
    unwind_kind->addCase(ConstantInt::get(Type::getInt8Ty(fbuilder_->context_),
                                          UNWIND_CONTINUE),
                         handle_ret_cont);

    fbuilder_->builder_.SetInsertPoint(handle_ret_cont);
    Value *retval = fbuilder_->Pop();
    fbuilder_->builder_.CreateStore(fbuilder_->Pop(), exit_func);
    fbuilder_->Push(retval);
    fbuilder_->Push(fbuilder_->builder_.CreateLoad(exc_type));
    fbuilder_->builder_.CreateStore(none, exc_type);
    fbuilder_->builder_.CreateStore(none, exc_value);
    fbuilder_->builder_.CreateStore(none, exc_traceback);
    fbuilder_->builder_.CreateBr(main_block);

    fbuilder_->builder_.SetInsertPoint(handle_default);
    fbuilder_->builder_.CreateStore(fbuilder_->Pop(), exit_func);
    fbuilder_->Push(fbuilder_->builder_.CreateLoad(exc_type));
    fbuilder_->builder_.CreateStore(none, exc_type);
    fbuilder_->builder_.CreateStore(none, exc_value);
    fbuilder_->builder_.CreateStore(none, exc_traceback);
    fbuilder_->builder_.CreateBr(main_block);

    // This is the (TOP, SECOND, THIRD) = exc_info() case above.
    fbuilder_->builder_.SetInsertPoint(handle_else);
    fbuilder_->builder_.CreateStore(fbuilder_->Pop(), exc_value);
    fbuilder_->builder_.CreateStore(fbuilder_->Pop(), exc_traceback);
    fbuilder_->builder_.CreateStore(fbuilder_->Pop(), exit_func);
    fbuilder_->Push(fbuilder_->builder_.CreateLoad(exc_traceback));
    fbuilder_->Push(fbuilder_->builder_.CreateLoad(exc_value));
    fbuilder_->Push(fbuilder_->builder_.CreateLoad(exc_type));
    fbuilder_->builder_.CreateBr(main_block);

    fbuilder_->builder_.SetInsertPoint(main_block);
    // Build a vector because there is no CreateCall5().
    // This is easier than building the tuple ourselves, but doing so would
    // probably be faster.
    std::vector<Value*> args;
    args.push_back(fbuilder_->builder_.CreateLoad(exit_func));
    args.push_back(fbuilder_->builder_.CreateLoad(exc_type));
    args.push_back(fbuilder_->builder_.CreateLoad(exc_value));
    args.push_back(fbuilder_->builder_.CreateLoad(exc_traceback));
    args.push_back(fbuilder_->GetNull<PyObject*>());
    Value *ret = fbuilder_->CreateCall(
        fbuilder_->GetGlobalFunction<PyObject *(PyObject *, ...)>(
            "PyObject_CallFunctionObjArgs"),
        args.begin(), args.end());
    fbuilder_->DecRef(fbuilder_->builder_.CreateLoad(exit_func));
    fbuilder_->PropagateExceptionOnNull(ret);

    BasicBlock *check_silence =
        fbuilder_->CreateBasicBlock("WITH_CLEANUP_check_silence");
    BasicBlock *no_silence =
        fbuilder_->CreateBasicBlock("WITH_CLEANUP_no_silence");
    BasicBlock *cleanup =
        fbuilder_->CreateBasicBlock("WITH_CLEANUP_cleanup");
    BasicBlock *next =
        fbuilder_->CreateBasicBlock("WITH_CLEANUP_next");

    // Don't bother checking whether to silence the exception if there's
    // no exception to silence.
    fbuilder_->builder_.CreateCondBr(
        fbuilder_->builder_.CreateICmpEQ(
            fbuilder_->builder_.CreateLoad(exc_type), none),
        no_silence, check_silence);

    fbuilder_->builder_.SetInsertPoint(no_silence);
    fbuilder_->DecRef(ret);
    fbuilder_->builder_.CreateBr(next);

    fbuilder_->builder_.SetInsertPoint(check_silence);
    fbuilder_->builder_.CreateCondBr(fbuilder_->IsPythonTrue(ret),
                                     cleanup, next);

    fbuilder_->builder_.SetInsertPoint(cleanup);
    // There was an exception and a true return. Swallow the exception.
    fbuilder_->Pop();
    fbuilder_->Pop();
    fbuilder_->Pop();
    fbuilder_->IncRef(none);
    fbuilder_->Push(none);
    fbuilder_->DecRef(fbuilder_->builder_.CreateLoad(exc_type));
    fbuilder_->DecRef(fbuilder_->builder_.CreateLoad(exc_value));
    fbuilder_->DecRef(fbuilder_->builder_.CreateLoad(exc_traceback));
    fbuilder_->builder_.CreateBr(next);

    fbuilder_->builder_.SetInsertPoint(next);
}

}
