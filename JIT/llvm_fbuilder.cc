#include "JIT/llvm_fbuilder.h"

#include "Python.h"
#include "code.h"
#include "opcode.h"
#include "frameobject.h"

#include "JIT/ConstantMirror.h"
#include "JIT/global_llvm_data.h"
#include "JIT/PyTypeBuilder.h"
#include "Util/EventTimer.h"

#include "JIT/opcodes/attributes.h"
#include "JIT/opcodes/binops.h"
#include "JIT/opcodes/block.h"
#include "JIT/opcodes/call.h"
#include "JIT/opcodes/closure.h"
#include "JIT/opcodes/cmpops.h"
#include "JIT/opcodes/container.h"
#include "JIT/opcodes/control.h"
#include "JIT/opcodes/globals.h"
#include "JIT/opcodes/locals.h"
#include "JIT/opcodes/loop.h"
#include "JIT/opcodes/name.h"
#include "JIT/opcodes/slice.h"
#include "JIT/opcodes/stack.h"
#include "JIT/opcodes/unaryops.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BasicBlock.h"
#include "llvm/Constant.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/Function.h"
#include "llvm/GlobalAlias.h"
#include "llvm/Instructions.h"
#include "llvm/Intrinsics.h"
#include "llvm/Module.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Type.h"

#include <vector>

#ifndef DW_LANG_Python
// Python has an official ID number in the draft Dwarf4 spec.
#define DW_LANG_Python 0x0014
#endif

struct PyExcInfo;

namespace Intrinsic = llvm::Intrinsic;
using llvm::BasicBlock;
using llvm::CallInst;
using llvm::Constant;
using llvm::ConstantExpr;
using llvm::ConstantInt;
using llvm::Function;
using llvm::FunctionType;
using llvm::GlobalVariable;
using llvm::Module;
using llvm::Type;
using llvm::Value;
using llvm::array_endof;
using llvm::errs;

// Use like "this->GET_GLOBAL_VARIABLE(Type, variable)".
#define GET_GLOBAL_VARIABLE(TYPE, VARIABLE) \
    GetGlobalVariable<TYPE>(&VARIABLE, #VARIABLE)

namespace py {

static std::string
pystring_to_std_string(PyObject *str)
{
    assert(PyString_Check(str));
    return std::string(PyString_AS_STRING(str), PyString_GET_SIZE(str));
}

static llvm::StringRef
pystring_to_stringref(const PyObject* str)
{
    assert(PyString_CheckExact(str));
    return llvm::StringRef(PyString_AS_STRING(str), PyString_GET_SIZE(str));
}

static const FunctionType *
get_function_type(Module *module)
{
    std::string function_type_name("__function_type");
    const FunctionType *result =
        llvm::cast_or_null<FunctionType>(
            module->getTypeByName(function_type_name));
    if (result != NULL)
        return result;

    result = PyTypeBuilder<PyObject*(PyFrameObject*)>::get(
        module->getContext());
    module->addTypeName(function_type_name, result);
    return result;
}

LlvmFunctionBuilder::LlvmFunctionBuilder(
    PyGlobalLlvmData *llvm_data, PyCodeObject *code_object)
    : uses_delete_fast(false),
      llvm_data_(llvm_data),
      code_object_(code_object),
      context_(this->llvm_data_->context()),
      module_(this->llvm_data_->module()),
      function_(Function::Create(
                    get_function_type(this->module_),
                    llvm::GlobalValue::ExternalLinkage,
                    // Prefix names with #u# to avoid collisions
                    // with runtime functions.
                    "#u#" + pystring_to_std_string(code_object->co_name),
                    this->module_)),
      builder_(this->context_,
               llvm::TargetFolder(
                   llvm_data_->getExecutionEngine()->getTargetData())),
      is_generator_(code_object->co_flags & CO_GENERATOR),
      debug_info_(llvm_data->DebugInfo()),
      debug_compile_unit_(this->debug_info_.CreateCompileUnit(
                              DW_LANG_Python,
                              PyString_AS_STRING(code_object->co_filename),
                              "",  // Directory
                              "Unladen Swallow " PY_VERSION,
                              false, // Not main.
                              false, // Not optimized
                              "")),
      debug_subprogram_(this->debug_info_.CreateSubprogram(
                            debug_compile_unit_,
                            PyString_AS_STRING(code_object->co_name),
                            PyString_AS_STRING(code_object->co_name),
                            PyString_AS_STRING(code_object->co_name),
                            debug_compile_unit_,
                            code_object->co_firstlineno,
                            llvm::DIType(),
                            false,  // Not local to unit.
                            true))  // Is definition.
{
    Function::arg_iterator args = this->function_->arg_begin();
    this->frame_ = args++;
    assert(args == this->function_->arg_end() &&
           "Unexpected number of arguments");
    this->frame_->setName("frame");

    BasicBlock *entry = this->CreateBasicBlock("entry");
    this->unreachable_block_ =
        this->CreateBasicBlock("unreachable");
    this->bail_to_interpreter_block_ =
        this->CreateBasicBlock("bail_to_interpreter");
    this->propagate_exception_block_ =
        this->CreateBasicBlock("propagate_exception");
    this->unwind_block_ = this->CreateBasicBlock("unwind_block");
    this->do_return_block_ = this->CreateBasicBlock("do_return");

    this->builder_.SetInsertPoint(entry);
    // CreateAllocaInEntryBlock will insert alloca's here, before
    // any other instructions in the 'entry' block.

    this->stack_pointer_addr_ = this->builder_.CreateAlloca(
        PyTypeBuilder<PyObject**>::get(this->context_),
        NULL, "stack_pointer_addr");
    this->tmp_stack_pointer_addr_ = this->builder_.CreateAlloca(
        PyTypeBuilder<PyObject**>::get(this->context_),
        NULL, "tmp_stack_pointer_addr");
    this->retval_addr_ = this->builder_.CreateAlloca(
        PyTypeBuilder<PyObject*>::get(this->context_),
        NULL, "retval_addr");
    this->unwind_reason_addr_ = this->builder_.CreateAlloca(
        Type::getInt8Ty(this->context_), NULL, "unwind_reason_addr");
    this->unwind_target_index_addr_ = this->builder_.CreateAlloca(
        Type::getInt32Ty(this->context_), NULL, "unwind_target_index_addr");
    this->blockstack_addr_ = this->builder_.CreateAlloca(
        PyTypeBuilder<PyTryBlock>::get(this->context_),
        ConstantInt::get(Type::getInt32Ty(this->context_), CO_MAXBLOCKS),
        "blockstack_addr");
    this->num_blocks_addr_ = this->builder_.CreateAlloca(
        PyTypeBuilder<char>::get(this->context_), NULL, "num_blocks_addr");
    for (int i = 0; i < code_object->co_nlocals; ++i) {
        PyObject *local_name = PyTuple_GET_ITEM(code_object->co_varnames, i);
        this->locals_.push_back(
            this->builder_.CreateAlloca(
                PyTypeBuilder<PyObject*>::get(this->context_),
                NULL,
                "local_" + pystring_to_stringref(local_name)));
    }

    this->tstate_ = this->CreateCall(
        this->GetGlobalFunction<PyThreadState*()>(
            "_PyLlvm_WrapPyThreadState_GET"));
    this->stack_bottom_ = this->builder_.CreateLoad(
        FrameTy::f_valuestack(this->builder_, this->frame_),
        "stack_bottom");
    this->llvm_data_->tbaa_stack.MarkInstruction(this->stack_bottom_);
    if (this->is_generator_) {
        // When we're re-entering a generator, we have to copy the stack
        // pointer, block stack and locals from the frame.
        this->CopyFromFrameObject();
    } else {
        // If this isn't a generator, the stack pointer always starts at
        // the bottom of the stack.
        this->builder_.CreateStore(this->stack_bottom_,
                                   this->stack_pointer_addr_);
        /* f_stacktop remains NULL unless yield suspends the frame. */
        this->builder_.CreateStore(
            this->GetNull<PyObject **>(),
            FrameTy::f_stacktop(this->builder_, this->frame_));

        this->builder_.CreateStore(
            ConstantInt::get(PyTypeBuilder<char>::get(this->context_), 0),
            this->num_blocks_addr_);

        // If this isn't a generator, we only need to copy the locals.
        this->CopyLocalsFromFrameObject();
    }

    Value *use_tracing = this->builder_.CreateLoad(
        ThreadStateTy::use_tracing(this->builder_, this->tstate_),
        "use_tracing");
    BasicBlock *trace_enter_function =
        this->CreateBasicBlock("trace_enter_function");
    BasicBlock *continue_entry =
        this->CreateBasicBlock("continue_entry");
    this->builder_.CreateCondBr(this->IsNonZero(use_tracing),
                                trace_enter_function, continue_entry);

    this->builder_.SetInsertPoint(trace_enter_function);
    // Don't touch f_lasti since we just entered the function..
    this->builder_.CreateStore(
        ConstantInt::get(PyTypeBuilder<char>::get(this->context_),
                         _PYFRAME_TRACE_ON_ENTRY),
        FrameTy::f_bailed_from_llvm(this->builder_, this->frame_));
    this->builder_.CreateBr(this->GetBailBlock());

    this->builder_.SetInsertPoint(continue_entry);
    Value *frame_code = this->builder_.CreateLoad(
        FrameTy::f_code(this->builder_, this->frame_),
        "frame->f_code");
    this->use_jit_addr_ = CodeTy::co_use_jit(this->builder_, frame_code);
#ifndef NDEBUG
    // Assert that the code object we pull out of the frame is the
    // same as the one passed into this object.
    Value *passed_in_code_object =
        ConstantInt::get(Type::getInt64Ty(this->context_),
                         reinterpret_cast<uintptr_t>(this->code_object_));
    this->Assert(this->builder_.CreateICmpEQ(
        this->builder_.CreatePtrToInt(frame_code,
                                      Type::getInt64Ty(this->context_)),
                     passed_in_code_object),
                 "Called with unexpected code object.");
#endif  // NDEBUG
    this->varnames_ = this->GetGlobalVariableFor(
        this->code_object_->co_varnames);

    Value *names_tuple = this->builder_.CreateBitCast(
        this->GetGlobalVariableFor(this->code_object_->co_names),
        PyTypeBuilder<PyTupleObject*>::get(this->context_),
        "names");
    // Get the address of the names_tuple's first item as well.
    this->names_ = this->GetTupleItemSlot(names_tuple, 0);

    // The next GEP-magic assigns &frame_[0].f_localsplus[0] to
    // this->fastlocals_.
    Value *localsplus = FrameTy::f_localsplus(this->builder_, this->frame_);
    this->llvm_data_->tbaa_locals.MarkInstruction(localsplus);
    this->fastlocals_ = this->builder_.CreateStructGEP(
        localsplus, 0, "fastlocals");
    Value *nlocals = ConstantInt::get(PyTypeBuilder<int>::get(this->context_),
                                      this->code_object_->co_nlocals);
    this->freevars_ =
        this->builder_.CreateGEP(this->fastlocals_, nlocals, "freevars");
    this->globals_ =
        this->builder_.CreateBitCast(
            this->builder_.CreateLoad(
                FrameTy::f_globals(this->builder_, this->frame_)),
            PyTypeBuilder<PyObject *>::get(this->context_));
    this->builtins_ =
        this->builder_.CreateBitCast(
            this->builder_.CreateLoad(
                FrameTy::f_builtins(this->builder_,this->frame_)),
            PyTypeBuilder<PyObject *>::get(this->context_));
    this->f_lineno_addr_ = FrameTy::f_lineno(this->builder_, this->frame_);
    this->f_lasti_addr_ = FrameTy::f_lasti(this->builder_, this->frame_);

    BasicBlock *start = this->CreateBasicBlock("body_start");
    if (this->is_generator_) {
      // Support generator.throw().  If frame->f_throwflag is set, the
      // caller has set an exception, and we're supposed to propagate
      // it.
      BasicBlock *propagate_generator_throw =
          this->CreateBasicBlock("propagate_generator_throw");
      BasicBlock *continue_generator_or_start_func =
          this->CreateBasicBlock("continue_generator_or_start_func");

      Value *throwflag = this->builder_.CreateLoad(
          FrameTy::f_throwflag(this->builder_, this->frame_),
          "f_throwflag");
      this->builder_.CreateCondBr(
          this->IsNonZero(throwflag),
          propagate_generator_throw, continue_generator_or_start_func);

      this->builder_.SetInsertPoint(propagate_generator_throw);
      PropagateException();

      this->builder_.SetInsertPoint(continue_generator_or_start_func);
      Value *resume_block = this->builder_.CreateLoad(
          this->f_lasti_addr_, "resume_block");
      // Each use of a YIELD_VALUE opcode will add a new case to this
      // switch.  eval.cc just assigns the new IP, allowing wild jumps,
      // but LLVM won't let us do that so we default to jumping to the
      // unreachable block.
      this->yield_resume_switch_ =
          this->builder_.CreateSwitch(resume_block, this->unreachable_block_);

      this->yield_resume_switch_->addCase(
          ConstantInt::getSigned(PyTypeBuilder<int>::get(this->context_), -1),
          start);
    } else {
      // This function is not a generator, so we just jump to the start.
      this->builder_.CreateBr(start);
    }

    this->builder_.SetInsertPoint(this->unreachable_block_);
#ifndef NDEBUG
    // In debug mode, die when we get to unreachable code.  In
    // optimized mode, let the LLVM optimizers get rid of it.
    this->Abort("Jumped to unreachable code.");
#endif  // NDEBUG
    this->builder_.CreateUnreachable();

    FillBailToInterpreterBlock();
    FillPropagateExceptionBlock();
    FillUnwindBlock();
    FillDoReturnBlock();

    this->builder_.SetInsertPoint(start);
#ifdef WITH_TSC
    this->LogTscEvent(CALL_ENTER_LLVM);
#endif
}

void
LlvmFunctionBuilder::FillPropagateExceptionBlock()
{
    this->builder_.SetInsertPoint(this->propagate_exception_block_);
    this->builder_.CreateStore(this->GetNull<PyObject*>(), this->retval_addr_);
    this->builder_.CreateStore(ConstantInt::get(Type::getInt8Ty(this->context_),
                                                UNWIND_EXCEPTION),
                               this->unwind_reason_addr_);
    this->CreateCall(
        this->GetGlobalFunction<int(PyFrameObject*)>("PyTraceBack_Here"),
        this->frame_);
    BasicBlock *call_exc_trace =
        this->CreateBasicBlock("call_exc_trace");
    Value *tracefunc = this->builder_.CreateLoad(
        ThreadStateTy::c_tracefunc(this->builder_, this->tstate_));
    this->builder_.CreateCondBr(this->IsNull(tracefunc),
                                this->unwind_block_, call_exc_trace);

    this->builder_.SetInsertPoint(call_exc_trace);
    this->CreateCall(
        this->GetGlobalFunction<void(PyThreadState *, PyFrameObject *)>(
            "_PyEval_CallExcTrace"),
        this->tstate_, this->frame_);
    this->builder_.CreateBr(this->unwind_block_);
}

void
LlvmFunctionBuilder::FillUnwindBlock()
{
    // Handles, roughly, the eval.cc JUMPTO macro.
    BasicBlock *goto_unwind_target_block =
        this->CreateBasicBlock("goto_unwind_target");
    this->builder_.SetInsertPoint(goto_unwind_target_block);
    Value *unwind_target_index =
        this->builder_.CreateLoad(this->unwind_target_index_addr_,
                                  "unwind_target_index");
    // Each call to AddUnwindTarget() will add a new case to this
    // switch.  eval.cc just assigns the new IP, allowing wild jumps,
    // but LLVM won't let us do that so we default to jumping to the
    // unreachable block.
    this->unwind_target_switch_ = this->builder_.CreateSwitch(
        unwind_target_index, this->unreachable_block_);

    // Code that needs to unwind the stack will jump here.
    // (e.g. returns, exceptions, breaks, and continues).
    this->builder_.SetInsertPoint(this->unwind_block_);
    Value *unwind_reason =
        this->builder_.CreateLoad(this->unwind_reason_addr_, "unwind_reason");

    BasicBlock *pop_remaining_objects =
        this->CreateBasicBlock("pop_remaining_objects");
    {  // Implements the fast_block_end loop toward the end of
       // PyEval_EvalFrame().  This pops blocks off the block-stack
       // and values off the value-stack until it finds a block that
       // wants to handle the current unwind reason.
        BasicBlock *unwind_loop_header =
            this->CreateBasicBlock("unwind_loop_header");
        BasicBlock *unwind_loop_body =
            this->CreateBasicBlock("unwind_loop_body");

        this->FallThroughTo(unwind_loop_header);
        // Continue looping if we still have blocks left on the blockstack.
        Value *blocks_left = this->builder_.CreateLoad(this->num_blocks_addr_);
        this->builder_.CreateCondBr(this->IsPositive(blocks_left),
                                    unwind_loop_body, pop_remaining_objects);

        this->builder_.SetInsertPoint(unwind_loop_body);
        Value *popped_block = this->CreateCall(
            this->GetGlobalFunction<PyTryBlock *(PyTryBlock *, char *)>(
                "_PyLlvm_Frame_BlockPop"),
            this->blockstack_addr_,
            this->num_blocks_addr_);
        Value *block_type = this->builder_.CreateLoad(
            PyTypeBuilder<PyTryBlock>::b_type(this->builder_, popped_block),
            "block_type");
        Value *block_handler = this->builder_.CreateLoad(
            PyTypeBuilder<PyTryBlock>::b_handler(this->builder_,
                                                     popped_block),
            "block_handler");
        Value *block_level = this->builder_.CreateLoad(
            PyTypeBuilder<PyTryBlock>::b_level(this->builder_,
                                                   popped_block),
            "block_level");

        // Handle SETUP_LOOP with UNWIND_CONTINUE.
        BasicBlock *not_continue =
            this->CreateBasicBlock("not_continue");
        BasicBlock *unwind_continue =
            this->CreateBasicBlock("unwind_continue");
        Value *is_setup_loop = this->builder_.CreateICmpEQ(
            block_type,
            ConstantInt::get(block_type->getType(), ::SETUP_LOOP),
            "is_setup_loop");
        Value *is_continue = this->builder_.CreateICmpEQ(
            unwind_reason,
            ConstantInt::get(Type::getInt8Ty(this->context_), UNWIND_CONTINUE),
            "is_continue");
        this->builder_.CreateCondBr(
            this->builder_.CreateAnd(is_setup_loop, is_continue),
            unwind_continue, not_continue);

        this->builder_.SetInsertPoint(unwind_continue);
        // Put the loop block back on the stack, clear the unwind reason,
        // then jump to the proper FOR_ITER.
        Value *args[] = {
            this->blockstack_addr_,
            this->num_blocks_addr_,
            block_type,
            block_handler,
            block_level
        };
        this->CreateCall(
            this->GetGlobalFunction<void(PyTryBlock *, char *, int, int, int)>(
                "_PyLlvm_Frame_BlockSetup"),
            args, array_endof(args));
        this->builder_.CreateStore(
            ConstantInt::get(Type::getInt8Ty(this->context_), UNWIND_NOUNWIND),
            this->unwind_reason_addr_);
        // Convert the return value to the unwind target. This is in keeping
        // with eval.cc. There's probably some LLVM magic that will allow
        // us to skip the boxing/unboxing, but this will work for now.
        Value *boxed_retval = this->builder_.CreateLoad(this->retval_addr_);
        Value *unbox_retval = this->builder_.CreateTrunc(
            this->CreateCall(
                this->GetGlobalFunction<long(PyObject *)>("PyInt_AsLong"),
                boxed_retval),
            Type::getInt32Ty(this->context_),
            "unboxed_retval");
        this->DecRef(boxed_retval);
        this->builder_.CreateStore(unbox_retval,
                                   this->unwind_target_index_addr_);
        this->builder_.CreateBr(goto_unwind_target_block);

        this->builder_.SetInsertPoint(not_continue);
        // Pop values back to where this block started.
        this->PopAndDecrefTo(
            this->builder_.CreateGEP(this->stack_bottom_, block_level));

        BasicBlock *handle_loop =
            this->CreateBasicBlock("handle_loop");
        BasicBlock *handle_except =
            this->CreateBasicBlock("handle_except");
        BasicBlock *handle_finally =
            this->CreateBasicBlock("handle_finally");
        BasicBlock *push_exception =
            this->CreateBasicBlock("push_exception");
        BasicBlock *goto_block_handler =
            this->CreateBasicBlock("goto_block_handler");

        llvm::SwitchInst *block_type_switch = this->builder_.CreateSwitch(
            block_type, this->unreachable_block_, 3);
        const llvm::IntegerType *block_type_type =
            llvm::cast<llvm::IntegerType>(block_type->getType());
        block_type_switch->addCase(
            ConstantInt::get(block_type_type, ::SETUP_LOOP),
            handle_loop);
        block_type_switch->addCase(
            ConstantInt::get(block_type_type, ::SETUP_EXCEPT),
            handle_except);
        block_type_switch->addCase(
            ConstantInt::get(block_type_type, ::SETUP_FINALLY),
            handle_finally);

        this->builder_.SetInsertPoint(handle_loop);
        Value *unwinding_break = this->builder_.CreateICmpEQ(
            unwind_reason, ConstantInt::get(Type::getInt8Ty(this->context_),
                                            UNWIND_BREAK),
            "currently_unwinding_break");
        this->builder_.CreateCondBr(unwinding_break,
                                    goto_block_handler, unwind_loop_header);

        this->builder_.SetInsertPoint(handle_except);
        // We only consider visiting except blocks when an exception
        // is being unwound.
        Value *unwinding_exception = this->builder_.CreateICmpEQ(
            unwind_reason, ConstantInt::get(Type::getInt8Ty(this->context_),
                                            UNWIND_EXCEPTION),
            "currently_unwinding_exception");
        this->builder_.CreateCondBr(unwinding_exception,
                                    push_exception, unwind_loop_header);

        this->builder_.SetInsertPoint(push_exception);
        // We need an alloca here so _PyLlvm_FastEnterExceptOrFinally
        // can return into it.  This alloca _won't_ be optimized by
        // mem2reg because its address is taken.
        Value *exc_info = this->CreateAllocaInEntryBlock(
            PyTypeBuilder<PyExcInfo>::get(this->context_), NULL, "exc_info");
        this->CreateCall(
            this->GetGlobalFunction<void(PyExcInfo*, int)>(
                "_PyLlvm_FastEnterExceptOrFinally"),
            exc_info,
            block_type);
        this->Push(this->builder_.CreateLoad(
                       this->builder_.CreateStructGEP(
                           exc_info, PyTypeBuilder<PyExcInfo>::FIELD_TB)));
        this->Push(this->builder_.CreateLoad(
                       this->builder_.CreateStructGEP(
                           exc_info,
                           PyTypeBuilder<PyExcInfo>::FIELD_VAL)));
        this->Push(this->builder_.CreateLoad(
                       this->builder_.CreateStructGEP(
                           exc_info,
                           PyTypeBuilder<PyExcInfo>::FIELD_EXC)));
        this->builder_.CreateBr(goto_block_handler);

        this->builder_.SetInsertPoint(handle_finally);
        // Jump to the finally block, with the stack prepared for
        // END_FINALLY to continue unwinding.

        BasicBlock *push_retval =
            this->CreateBasicBlock("push_retval");
        BasicBlock *handle_finally_end =
            this->CreateBasicBlock("handle_finally_end");
        llvm::SwitchInst *should_push_retval = this->builder_.CreateSwitch(
            unwind_reason, handle_finally_end, 2);
        // When unwinding for an exception, we have to save the
        // exception onto the stack.
        should_push_retval->addCase(
            ConstantInt::get(Type::getInt8Ty(this->context_), UNWIND_EXCEPTION),
            push_exception);
        // When unwinding for a return or continue, we have to save
        // the return value or continue target onto the stack.
        should_push_retval->addCase(
            ConstantInt::get(Type::getInt8Ty(this->context_), UNWIND_RETURN),
            push_retval);
        should_push_retval->addCase(
            ConstantInt::get(Type::getInt8Ty(this->context_), UNWIND_CONTINUE),
            push_retval);

        this->builder_.SetInsertPoint(push_retval);
        this->Push(this->builder_.CreateLoad(this->retval_addr_, "retval"));

        this->FallThroughTo(handle_finally_end);
        // END_FINALLY expects to find the unwind reason on the top of
        // the stack.  There's probably a way to do this that doesn't
        // involve allocating an int for every unwind through a
        // finally block, but imitating CPython is simpler.
        Value *unwind_reason_as_pyint = this->CreateCall(
            this->GetGlobalFunction<PyObject *(long)>("PyInt_FromLong"),
            this->builder_.CreateZExt(unwind_reason,
                                      PyTypeBuilder<long>::get(this->context_)),
            "unwind_reason_as_pyint");
        this->Push(unwind_reason_as_pyint);

        this->FallThroughTo(goto_block_handler);
        // Clear the unwind reason while running through the block's
        // handler.  mem2reg should never actually decide to use this
        // value, but having it here should make such forgotten stores
        // more obvious.
        this->builder_.CreateStore(
            ConstantInt::get(Type::getInt8Ty(this->context_), UNWIND_NOUNWIND),
            this->unwind_reason_addr_);
        // The block's handler field holds the index of the block
        // defining this finally or except, or the tail of the loop we
        // just broke out of.  Jump to it through the unwind switch
        // statement defined above.
        this->builder_.CreateStore(block_handler,
                                   this->unwind_target_index_addr_);
        this->builder_.CreateBr(goto_unwind_target_block);
    }  // End unwind loop.

    // If we fall off the end of the unwind loop, there are no blocks
    // left and it's time to pop the rest of the value stack and
    // return.
    this->builder_.SetInsertPoint(pop_remaining_objects);
    this->PopAndDecrefTo(this->stack_bottom_);

    // Unless we're returning (or yielding which comes into the
    // do_return_block_ through another path), the retval should be
    // NULL.
    BasicBlock *reset_retval =
        this->CreateBasicBlock("reset_retval");
    Value *unwinding_for_return =
        this->builder_.CreateICmpEQ(
            unwind_reason, ConstantInt::get(Type::getInt8Ty(this->context_),
                                            UNWIND_RETURN));
    this->builder_.CreateCondBr(unwinding_for_return,
                                this->do_return_block_, reset_retval);

    this->builder_.SetInsertPoint(reset_retval);
    this->builder_.CreateStore(this->GetNull<PyObject*>(), this->retval_addr_);
    this->builder_.CreateBr(this->do_return_block_);
}

void
LlvmFunctionBuilder::FillDoReturnBlock()
{
    this->builder_.SetInsertPoint(this->do_return_block_);
    BasicBlock *check_frame_exception =
        this->CreateBasicBlock("check_frame_exception");
    BasicBlock *trace_leave_function =
        this->CreateBasicBlock("trace_leave_function");
    BasicBlock *tracer_raised =
        this->CreateBasicBlock("tracer_raised");

    // Trace exiting from this function, if tracing is turned on.
    Value *use_tracing = this->builder_.CreateLoad(
        ThreadStateTy::use_tracing(this->builder_, this->tstate_));
    this->builder_.CreateCondBr(this->IsNonZero(use_tracing),
                                trace_leave_function, check_frame_exception);

    this->builder_.SetInsertPoint(trace_leave_function);
    Value *unwind_reason =
        this->builder_.CreateLoad(this->unwind_reason_addr_);
    Value *is_return = this->builder_.CreateICmpEQ(
        unwind_reason, ConstantInt::get(Type::getInt8Ty(this->context_),
                                        UNWIND_RETURN),
        "is_return");
    Value *is_yield = this->builder_.CreateICmpEQ(
        unwind_reason, ConstantInt::get(Type::getInt8Ty(this->context_),
                                        UNWIND_YIELD),
        "is_yield");
    Value *is_exception = this->builder_.CreateICmpEQ(
        unwind_reason, ConstantInt::get(Type::getInt8Ty(this->context_),
                                        UNWIND_EXCEPTION),
        "is_exception");
    Value *is_yield_or_return = this->builder_.CreateOr(is_return, is_yield);
    Value *traced_retval = this->builder_.CreateLoad(this->retval_addr_);
    Value *trace_args[] = {
        this->tstate_,
        this->frame_,
        traced_retval,
        this->builder_.CreateIntCast(
            is_yield_or_return, PyTypeBuilder<char>::get(this->context_),
            false /* unsigned */),
        this->builder_.CreateIntCast(
            is_exception, PyTypeBuilder<char>::get(this->context_),
            false /* unsigned */)
    };
    Value *trace_result = this->CreateCall(
        this->GetGlobalFunction<int(PyThreadState *, struct _frame *,
                                    PyObject *, char, char)>(
                                        "_PyEval_TraceLeaveFunction"),
        trace_args, array_endof(trace_args));
    this->builder_.CreateCondBr(this->IsNonZero(trace_result),
                                tracer_raised, check_frame_exception);

    this->builder_.SetInsertPoint(tracer_raised);
    this->XDecRef(traced_retval);
    this->builder_.CreateStore(this->GetNull<PyObject*>(), this->retval_addr_);
    this->builder_.CreateBr(check_frame_exception);

    this->builder_.SetInsertPoint(check_frame_exception);
    // If this frame raised and caught an exception, it saved it into
    // sys.exc_info(). The calling frame may also be in the process of
    // handling an exception, in which case we don't want to clobber
    // its sys.exc_info().  See eval.cc's _PyEval_ResetExcInfo for
    // details.
    BasicBlock *have_frame_exception =
        this->CreateBasicBlock("have_frame_exception");
    BasicBlock *no_frame_exception =
        this->CreateBasicBlock("no_frame_exception");
    BasicBlock *finish_return =
        this->CreateBasicBlock("finish_return");
    Value *tstate_frame = this->builder_.CreateLoad(
        ThreadStateTy::frame(this->builder_, this->tstate_),
        "tstate->frame");
    Value *f_exc_type = this->builder_.CreateLoad(
        FrameTy::f_exc_type(this->builder_, tstate_frame),
        "tstate->frame->f_exc_type");
    this->builder_.CreateCondBr(this->IsNull(f_exc_type),
                                no_frame_exception, have_frame_exception);

    this->builder_.SetInsertPoint(have_frame_exception);
    // The frame did have an exception, so un-clobber the caller's exception.
    this->CreateCall(
        this->GetGlobalFunction<void(PyThreadState*)>("_PyEval_ResetExcInfo"),
        this->tstate_);
    this->builder_.CreateBr(finish_return);

    this->builder_.SetInsertPoint(no_frame_exception);
    // The frame did not have an exception.  In debug mode, check for
    // consistency.
#ifndef NDEBUG
    Value *f_exc_value = this->builder_.CreateLoad(
        FrameTy::f_exc_value(this->builder_, tstate_frame),
        "tstate->frame->f_exc_value");
    Value *f_exc_traceback = this->builder_.CreateLoad(
        FrameTy::f_exc_traceback(this->builder_, tstate_frame),
        "tstate->frame->f_exc_traceback");
    this->Assert(this->IsNull(f_exc_value),
                 "Frame's exc_type was null but exc_value wasn't");
    this->Assert(this->IsNull(f_exc_traceback),
                 "Frame's exc_type was null but exc_traceback wasn't");
#endif
    this->builder_.CreateBr(finish_return);

    this->builder_.SetInsertPoint(finish_return);
    // Grab the return value and return it.
    Value *retval = this->builder_.CreateLoad(this->retval_addr_, "retval");
    this->CreateRet(retval);
}

// Before jumping to this block, make sure frame->f_lasti points to
// the opcode index at which to resume.
void
LlvmFunctionBuilder::FillBailToInterpreterBlock()
{
    this->builder_.SetInsertPoint(this->bail_to_interpreter_block_);
    // Don't just immediately jump back to the JITted code.
    this->builder_.CreateStore(
        ConstantInt::get(PyTypeBuilder<int>::get(this->context_), 0),
        FrameTy::f_use_jit(this->builder_, this->frame_));
    // Fill the frame object with any information that was in allocas here.
    this->CopyToFrameObject();

    // Tail-call back to the interpreter.  As of 2009-06-12 this isn't
    // codegen'ed as a tail call
    // (http://llvm.org/docs/CodeGenerator.html#tailcallopt), but that
    // should improve eventually.
    CallInst *bail = this->CreateCall(
        this->GetGlobalFunction<PyObject*(PyFrameObject*)>("PyEval_EvalFrame"),
        this->frame_);
    bail->setTailCall(true);
    this->CreateRet(bail);
}

llvm::BasicBlock *
LlvmFunctionBuilder::GetBailBlock() const
{
    // TODO(collinwinter): bail block chaining needs to change this.
    return this->bail_to_interpreter_block_;
}

llvm::BasicBlock *
LlvmFunctionBuilder::GetExceptionBlock() const
{
    // TODO(collinwinter): exception block chaining needs to change this.
    return this->propagate_exception_block_;
}

void
LlvmFunctionBuilder::PopAndDecrefTo(Value *target_stack_pointer)
{
    BasicBlock *pop_loop = this->CreateBasicBlock("pop_loop");
    BasicBlock *pop_block = this->CreateBasicBlock("pop_stack");
    BasicBlock *pop_done = this->CreateBasicBlock("pop_done");

    this->FallThroughTo(pop_loop);
    Value *stack_pointer = this->builder_.CreateLoad(this->stack_pointer_addr_);
    this->llvm_data_->tbaa_stack.MarkInstruction(stack_pointer);

    Value *finished_popping = this->builder_.CreateICmpULE(
        stack_pointer, target_stack_pointer);
    this->builder_.CreateCondBr(finished_popping, pop_done, pop_block);

    this->builder_.SetInsertPoint(pop_block);
    this->XDecRef(this->Pop());
    this->builder_.CreateBr(pop_loop);

    this->builder_.SetInsertPoint(pop_done);
}

Value *
LlvmFunctionBuilder::CreateAllocaInEntryBlock(
    const Type *alloca_type, Value *array_size, const char *name="")
{
    // In order for LLVM to optimize alloca's, we should emit alloca
    // instructions in the function entry block. We can get at the
    // block with this->function_->begin(), but it will already have a
    // 'br' instruction at the end. Instantiating the AllocaInst class
    // directly, we pass it the begin() iterator of the entry block,
    // causing it to insert itself right before the first instruction
    // in the block.
    return new llvm::AllocaInst(alloca_type, array_size, name,
                                this->function_->begin()->begin());
}

void
LlvmFunctionBuilder::MemCpy(llvm::Value *target,
                            llvm::Value *array, llvm::Value *N)
{
    const Type *len_type[] = { Type::getInt64Ty(this->context_) };
    Value *memcpy = Intrinsic::getDeclaration(
        this->module_, Intrinsic::memcpy, len_type, 1);
    assert(target->getType() == array->getType() &&
           "memcpy's source and destination should have the same type.");
    // Calculate the length as int64_t(&array_type(NULL)[N]).
    Value *length = this->builder_.CreatePtrToInt(
        this->builder_.CreateGEP(Constant::getNullValue(array->getType()), N),
        Type::getInt64Ty(this->context_));
    const Type *char_star_type = PyTypeBuilder<char*>::get(this->context_);
    this->CreateCall(
        memcpy,
        this->builder_.CreateBitCast(target, char_star_type),
        this->builder_.CreateBitCast(array, char_star_type),
        length,
        // Unknown alignment.
        ConstantInt::get(Type::getInt32Ty(this->context_), 0));
}

void
LlvmFunctionBuilder::CopyToFrameObject()
{
    // Save the current stack pointer into the frame.
    // Note that locals are mirrored to the frame as they're modified.
    Value *stack_pointer = this->builder_.CreateLoad(this->stack_pointer_addr_);
    Value *f_stacktop = FrameTy::f_stacktop(this->builder_, this->frame_);
    this->builder_.CreateStore(stack_pointer, f_stacktop);
    Value *num_blocks = this->builder_.CreateLoad(this->num_blocks_addr_);
    this->builder_.CreateStore(num_blocks,
                               FrameTy::f_iblock(this->builder_, this->frame_));
    this->MemCpy(this->builder_.CreateStructGEP(
                     FrameTy::f_blockstack(this->builder_, this->frame_), 0),
                 this->blockstack_addr_, num_blocks);
}

void
LlvmFunctionBuilder::CopyFromFrameObject()
{
    Value *f_stacktop = FrameTy::f_stacktop(this->builder_, this->frame_);
    Value *stack_pointer =
        this->builder_.CreateLoad(f_stacktop,
                                  "stack_pointer_from_frame");
    this->builder_.CreateStore(stack_pointer, this->stack_pointer_addr_);
    /* f_stacktop remains NULL unless yield suspends the frame. */
    this->builder_.CreateStore(this->GetNull<PyObject**>(), f_stacktop);

    Value *num_blocks = this->builder_.CreateLoad(
        FrameTy::f_iblock(this->builder_, this->frame_));
    this->builder_.CreateStore(num_blocks, this->num_blocks_addr_);
    this->MemCpy(this->blockstack_addr_,
                 this->builder_.CreateStructGEP(
                     FrameTy::f_blockstack(this->builder_, this->frame_), 0),
                 num_blocks);

    this->CopyLocalsFromFrameObject();
}

int
LlvmFunctionBuilder::GetParamCount() const
{
    int co_flags = this->code_object_->co_flags;
    return this->code_object_->co_argcount +
        bool(co_flags & CO_VARARGS) + bool(co_flags & CO_VARKEYWORDS);
}


// Rules for copying locals from the frame:
// - If this is a generator, copy everything from the frame.
// - If this is a regular function, only copy the function's parameters; these
//   can never be NULL. Set all other locals to NULL explicitly. This gives
//   LLVM's optimizers more information.
//
// TODO(collinwinter): when LLVM's metadata supports it, mark all parameters
// as "not-NULL" so that constant propagation can have more information to work
// with.
void
LlvmFunctionBuilder::CopyLocalsFromFrameObject()
{
    const Type *int_type = Type::getInt32Ty(this->context_);
    Value *locals =
        this->builder_.CreateStructGEP(
                 FrameTy::f_localsplus(this->builder_, this->frame_), 0);
    this->llvm_data_->tbaa_locals.MarkInstruction(locals);

    Value *null = this->GetNull<PyObject*>();

    // Figure out how many total parameters we have.
    int param_count = this->GetParamCount();

    for (int i = 0; i < this->code_object_->co_nlocals; ++i) {
        PyObject *pyname =
            PyTuple_GET_ITEM(this->code_object_->co_varnames, i);

        if (this->is_generator_ || i < param_count) {
            Value *local_slot = this->builder_.CreateLoad(
                this->builder_.CreateGEP(
                    locals, ConstantInt::get(int_type, i)),
                "local_" + std::string(PyString_AsString(pyname)));

            this->builder_.CreateStore(local_slot, this->locals_[i]);
        }
        else {
            this->builder_.CreateStore(null, this->locals_[i]);
        }
    }
}

void
LlvmFunctionBuilder::SetLasti(int current_instruction_index)
{
    this->f_lasti_ = current_instruction_index;
}

void
LlvmFunctionBuilder::SetLineNumber(int line)
{
    BasicBlock *this_line = this->CreateBasicBlock("line_start");

    this->builder_.CreateStore(
        this->GetSigned<int>(line),
        this->f_lineno_addr_);
    this->SetDebugStopPoint(line);

    this->MaybeCallLineTrace(this_line, _PYFRAME_LINE_TRACE);

    this->builder_.SetInsertPoint(this_line);
}

void
LlvmFunctionBuilder::FillBackedgeLanding(BasicBlock *backedge_landing,
                                         BasicBlock *target,
                                         bool to_start_of_line,
                                         int line_number)
{
    BasicBlock *continue_backedge = NULL;
    if (to_start_of_line) {
        continue_backedge = target;
    }
    else {
        continue_backedge = this->CreateBasicBlock(
                backedge_landing->getName() + ".cont");
    }

    this->builder_.SetInsertPoint(backedge_landing);
    this->CheckPyTicker(continue_backedge);

    if (!to_start_of_line) {
        continue_backedge->moveAfter(backedge_landing);
        this->builder_.SetInsertPoint(continue_backedge);
        // Record the new line number.  This is after _Py_Ticker, so
        // exceptions from signals will appear to come from the source of
        // the backedge.
        this->builder_.CreateStore(
            ConstantInt::getSigned(PyTypeBuilder<int>::get(this->context_),
                                   line_number),
            this->f_lineno_addr_);
        this->SetDebugStopPoint(line_number);

        // If tracing has been turned on, jump back to the interpreter.
        this->MaybeCallLineTrace(target, _PYFRAME_BACKEDGE_TRACE);
    }
}

void
LlvmFunctionBuilder::MaybeCallLineTrace(BasicBlock *fallthrough_block,
                                        char direction)
{
    BasicBlock *call_trace = this->CreateBasicBlock("call_trace");

    Value *tracing_possible = this->builder_.CreateLoad(
        this->GET_GLOBAL_VARIABLE(int, _Py_TracingPossible));
    this->builder_.CreateCondBr(this->IsNonZero(tracing_possible),
                                call_trace, fallthrough_block);

    this->builder_.SetInsertPoint(call_trace);
    this->CreateBailPoint(direction);
}

void
LlvmFunctionBuilder::BailIfProfiling(llvm::BasicBlock *fallthrough_block)
{
    BasicBlock *profiling = this->CreateBasicBlock("profiling");

    Value *profiling_possible = this->builder_.CreateLoad(
        this->GET_GLOBAL_VARIABLE(int, _Py_ProfilingPossible));
    this->builder_.CreateCondBr(this->IsNonZero(profiling_possible),
                                profiling, fallthrough_block);

    this->builder_.SetInsertPoint(profiling);
    this->CreateBailPoint(_PYFRAME_CALL_PROFILE);
}

void
LlvmFunctionBuilder::FallThroughTo(BasicBlock *next_block)
{
    if (this->builder_.GetInsertBlock()->getTerminator() == NULL) {
        // If the block doesn't already end with a branch or
        // return, branch to the next block.
        this->builder_.CreateBr(next_block);
    }
    this->builder_.SetInsertPoint(next_block);
}

ConstantInt *
LlvmFunctionBuilder::AddUnwindTarget(llvm::BasicBlock *target,
                                     int target_opindex)
{
    // The size of the switch instruction will give us a small unique
    // number for each target block.
    ConstantInt *target_index =
            ConstantInt::get(Type::getInt32Ty(this->context_), target_opindex);
    if (!this->existing_unwind_targets_.test(target_opindex)) {
        this->unwind_target_switch_->addCase(target_index, target);
        this->existing_unwind_targets_.set(target_opindex);
    }
    return target_index;
}

void
LlvmFunctionBuilder::Return(Value *retval)
{
    this->builder_.CreateStore(retval, this->retval_addr_);
    this->builder_.CreateStore(ConstantInt::get(Type::getInt8Ty(this->context_),
                                                UNWIND_RETURN),
                               this->unwind_reason_addr_);
    this->builder_.CreateBr(this->unwind_block_);
}

void
LlvmFunctionBuilder::PropagateException()
{
    this->builder_.CreateBr(this->GetExceptionBlock());
}

void
LlvmFunctionBuilder::SetDebugStopPoint(int line_number)
{
    this->builder_.SetCurrentDebugLocation(
        this->debug_info_.CreateLocation(line_number, 0,
                                         this->debug_subprogram_,
                                         llvm::DILocation(NULL)).getNode());
}

const PyTypeObject *
LlvmFunctionBuilder::GetTypeFeedback(unsigned arg_index) const
{
    const PyRuntimeFeedback *feedback = this->GetFeedback(arg_index);
    if (feedback == NULL || feedback->ObjectsOverflowed())
        return NULL;

    llvm::SmallVector<PyObject*, 3> types;
    feedback->GetSeenObjectsInto(types);
    if (types.size() != 1)
        return NULL;

    if (!PyType_CheckExact(types[0]))
        return NULL;

    return (PyTypeObject*)types[0];
}

void
LlvmFunctionBuilder::LOAD_CONST(int index)
{
    OpcodeLocals locals(this);
    locals.LOAD_CONST(index);
}

void
LlvmFunctionBuilder::LOAD_GLOBAL(int name_index)
{
    OpcodeGlobals globals(this);
    globals.LOAD_GLOBAL(name_index);
}

void
LlvmFunctionBuilder::STORE_GLOBAL(int name_index)
{
    OpcodeGlobals globals(this);
    globals.STORE_GLOBAL(name_index);
}

void
LlvmFunctionBuilder::DELETE_GLOBAL(int name_index)
{
    OpcodeGlobals globals(this);
    globals.DELETE_GLOBAL(name_index);
}

void
LlvmFunctionBuilder::LOAD_NAME(int index)
{
    OpcodeName name(this);
    name.LOAD_NAME(index);
}

void
LlvmFunctionBuilder::STORE_NAME(int index)
{
    OpcodeName name(this);
    name.STORE_NAME(index);
}

void
LlvmFunctionBuilder::DELETE_NAME(int index)
{
    OpcodeName name(this);
    name.DELETE_NAME(index);
}

void
LlvmFunctionBuilder::LOAD_ATTR(int names_index)
{
    OpcodeAttributes attr(this);
    attr.LOAD_ATTR(names_index);
}

void
LlvmFunctionBuilder::STORE_ATTR(int names_index)
{
    OpcodeAttributes attr(this);
    attr.STORE_ATTR(names_index);
}

void
LlvmFunctionBuilder::DELETE_ATTR(int index)
{
    OpcodeAttributes attr(this);
    attr.DELETE_ATTR(index);
}

void
LlvmFunctionBuilder::LOAD_FAST(int index)
{
    OpcodeLocals locals(this);
    locals.LOAD_FAST(index);
}

void
LlvmFunctionBuilder::WITH_CLEANUP()
{
    OpcodeBlock block(this);
    block.WITH_CLEANUP();
}

void
LlvmFunctionBuilder::LOAD_CLOSURE(int freevars_index)
{
    OpcodeClosure closure(this);
    closure.LOAD_CLOSURE(freevars_index);
}

void
LlvmFunctionBuilder::MAKE_CLOSURE(int num_defaults)
{
    OpcodeClosure closure(this);
    closure.MAKE_CLOSURE(num_defaults);
}

#ifdef WITH_TSC
void
LlvmFunctionBuilder::LogTscEvent(_PyTscEventId event_id) {
    Function *timer_function = this->GetGlobalFunction<void (int)>(
            "_PyLog_TscEvent");
    // Int8Ty doesn't seem to work here, so we use Int32Ty instead.
    Value *enum_ir = ConstantInt::get(Type::getInt32Ty(this->context_),
                                      event_id);
    this->CreateCall(timer_function, enum_ir);
}
#endif

const PyRuntimeFeedback *
LlvmFunctionBuilder::GetFeedback(unsigned arg_index) const
{
    const PyFeedbackMap *map = this->code_object_->co_runtime_feedback;
    if (map == NULL)
        return NULL;
    return map->GetFeedbackEntry(this->f_lasti_, arg_index);
}

void
LlvmFunctionBuilder::CALL_FUNCTION(int oparg)
{
    OpcodeCall call(this);
    call.CALL_FUNCTION(oparg);
}

void
LlvmFunctionBuilder::CALL_FUNCTION_VAR(int oparg)
{
    OpcodeCall call(this);
    call.CALL_FUNCTION_VAR(oparg);
}

void
LlvmFunctionBuilder::CALL_FUNCTION_KW(int oparg)
{
    OpcodeCall call(this);
    call.CALL_FUNCTION_KW(oparg);
}

void
LlvmFunctionBuilder::CALL_FUNCTION_VAR_KW(int oparg)
{
    OpcodeCall call(this);
    call.CALL_FUNCTION_VAR_KW(oparg);
}

void
LlvmFunctionBuilder::IMPORT_NAME()
{
    OpcodeContainer cont(this);
    cont.IMPORT_NAME();
}

void
LlvmFunctionBuilder::LOAD_DEREF(int index)
{
    OpcodeClosure closure(this);
    closure.LOAD_DEREF(index);
}

void
LlvmFunctionBuilder::STORE_DEREF(int index)
{
    OpcodeClosure closure(this);
    closure.STORE_DEREF(index);
}

void
LlvmFunctionBuilder::JUMP_ABSOLUTE(llvm::BasicBlock *target,
                                   llvm::BasicBlock *fallthrough)
{
    OpcodeControl control(this);
    control.JUMP_ABSOLUTE(target, fallthrough);
}

void
LlvmFunctionBuilder::POP_JUMP_IF_FALSE(unsigned target_idx,
                                       unsigned fallthrough_idx,
                                       BasicBlock *target,
                                       BasicBlock *fallthrough)
{
    OpcodeControl control(this);
    control.POP_JUMP_IF_FALSE(target_idx, fallthrough_idx,
                              target, fallthrough);
}

void
LlvmFunctionBuilder::POP_JUMP_IF_TRUE(unsigned target_idx,
                                      unsigned fallthrough_idx,
                                      BasicBlock *target,
                                      BasicBlock *fallthrough)
{
    OpcodeControl control(this);
    control.POP_JUMP_IF_TRUE(target_idx, fallthrough_idx,
                             target, fallthrough);
}

void
LlvmFunctionBuilder::JUMP_IF_FALSE_OR_POP(unsigned target_idx,
                                          unsigned fallthrough_idx,
                                          BasicBlock *target,
                                          BasicBlock *fallthrough)
{
    OpcodeControl control(this);
    control.JUMP_IF_FALSE_OR_POP(target_idx, fallthrough_idx,
                                 target, fallthrough);
}

void
LlvmFunctionBuilder::JUMP_IF_TRUE_OR_POP(unsigned target_idx,
                                         unsigned fallthrough_idx,
                                         BasicBlock *target,
                                         BasicBlock *fallthrough)
{
    OpcodeControl control(this);
    control.JUMP_IF_TRUE_OR_POP(target_idx, fallthrough_idx,
                                target, fallthrough);
}

void
LlvmFunctionBuilder::CreateBailPoint(unsigned bail_idx, char reason)
{
    this->builder_.CreateStore(
        // -1 so that next_instr gets set right in EvalFrame.
        this->GetSigned<int>(bail_idx - 1),
        this->f_lasti_addr_);
    this->builder_.CreateStore(
        ConstantInt::get(PyTypeBuilder<char>::get(this->context_), reason),
        FrameTy::f_bailed_from_llvm(this->builder_, this->frame_));
    this->builder_.CreateBr(this->GetBailBlock());
}

void
LlvmFunctionBuilder::CreateGuardBailPoint(unsigned bail_idx, char reason)
{
#ifdef Py_WITH_INSTRUMENTATION
    this->builder_.CreateStore(
        ConstantInt::get(PyTypeBuilder<char>::get(this->context_), reason),
        FrameTy::f_guard_type(this->builder_, this->frame_));
#endif
    this->CreateBailPoint(bail_idx, _PYFRAME_GUARD_FAIL);
}

void
LlvmFunctionBuilder::STORE_FAST(int index)
{
    OpcodeLocals locals(this);
    locals.STORE_FAST(index);
}

void
LlvmFunctionBuilder::DELETE_FAST(int index)
{
    OpcodeLocals locals(this);
    locals.DELETE_FAST(index);
}

void
LlvmFunctionBuilder::SETUP_LOOP(llvm::BasicBlock *target,
                                int target_opindex,
                                llvm::BasicBlock *fallthrough)
{
    OpcodeBlock block(this);
    block.SETUP_LOOP(target, target_opindex, fallthrough);
}

void
LlvmFunctionBuilder::GET_ITER()
{
    OpcodeLoop loop(this);
    loop.GET_ITER();
}

void
LlvmFunctionBuilder::FOR_ITER(llvm::BasicBlock *target,
                              llvm::BasicBlock *fallthrough)
{
    OpcodeLoop loop(this);
    loop.FOR_ITER(target, fallthrough);
}

void
LlvmFunctionBuilder::POP_BLOCK()
{
    OpcodeBlock block(this);
    block.POP_BLOCK();
}

void
LlvmFunctionBuilder::SETUP_EXCEPT(llvm::BasicBlock *target,
                                  int target_opindex,
                                  llvm::BasicBlock *fallthrough)
{
    OpcodeBlock block(this);
    block.SETUP_EXCEPT(target, target_opindex, fallthrough);
}

void
LlvmFunctionBuilder::SETUP_FINALLY(llvm::BasicBlock *target,
                                   int target_opindex,
                                   llvm::BasicBlock *fallthrough)
{
    OpcodeBlock block(this);
    block.SETUP_FINALLY(target, target_opindex, fallthrough);
}

void
LlvmFunctionBuilder::END_FINALLY()
{
    OpcodeBlock block(this);
    block.END_FINALLY();
}

void
LlvmFunctionBuilder::CONTINUE_LOOP(llvm::BasicBlock *target,
                                   int target_opindex,
                                   llvm::BasicBlock *fallthrough)
{
    OpcodeLoop loop(this);
    loop.CONTINUE_LOOP(target, target_opindex, fallthrough);
}

void
LlvmFunctionBuilder::BREAK_LOOP()
{
    OpcodeLoop loop(this);
    loop.BREAK_LOOP();
}

void
LlvmFunctionBuilder::RETURN_VALUE()
{
    OpcodeControl control(this);
    control.RETURN_VALUE();
}

void
LlvmFunctionBuilder::YIELD_VALUE()
{
    OpcodeControl control(this);
    control.YIELD_VALUE();
}

void
LlvmFunctionBuilder::RAISE_VARARGS_ZERO()
{
    OpcodeControl control(this);
    control.RAISE_VARARGS_ZERO();
}

void
LlvmFunctionBuilder::RAISE_VARARGS_ONE()
{
    OpcodeControl control(this);
    control.RAISE_VARARGS_ONE();
}

void
LlvmFunctionBuilder::RAISE_VARARGS_TWO()
{
    OpcodeControl control(this);
    control.RAISE_VARARGS_TWO();
}

void
LlvmFunctionBuilder::RAISE_VARARGS_THREE()
{
    OpcodeControl control(this);
    control.RAISE_VARARGS_THREE();
}

void
LlvmFunctionBuilder::STORE_SUBSCR()
{
    OpcodeContainer cont(this);
    cont.STORE_SUBSCR();
}

void
LlvmFunctionBuilder::DELETE_SUBSCR()
{
    OpcodeContainer cont(this);
    cont.DELETE_SUBSCR();
}

#define BINOP_METH(OPCODE)              \
void                                    \
LlvmFunctionBuilder::OPCODE()           \
{                                       \
    OpcodeBinops binops(this);          \
    binops.OPCODE();                    \
}

BINOP_METH(BINARY_ADD)
BINOP_METH(BINARY_SUBTRACT)
BINOP_METH(BINARY_MULTIPLY)
BINOP_METH(BINARY_DIVIDE)
BINOP_METH(BINARY_MODULO)
BINOP_METH(BINARY_SUBSCR)

BINOP_METH(BINARY_TRUE_DIVIDE)
BINOP_METH(BINARY_LSHIFT)
BINOP_METH(BINARY_RSHIFT)
BINOP_METH(BINARY_OR)
BINOP_METH(BINARY_XOR)
BINOP_METH(BINARY_AND)
BINOP_METH(BINARY_FLOOR_DIVIDE)

BINOP_METH(INPLACE_ADD)
BINOP_METH(INPLACE_SUBTRACT)
BINOP_METH(INPLACE_MULTIPLY)
BINOP_METH(INPLACE_TRUE_DIVIDE)
BINOP_METH(INPLACE_DIVIDE)
BINOP_METH(INPLACE_MODULO)
BINOP_METH(INPLACE_LSHIFT)
BINOP_METH(INPLACE_RSHIFT)
BINOP_METH(INPLACE_OR)
BINOP_METH(INPLACE_XOR)
BINOP_METH(INPLACE_AND)
BINOP_METH(INPLACE_FLOOR_DIVIDE)

#undef BINOP_METH

void
LlvmFunctionBuilder::BINARY_POWER()
{
    OpcodeBinops binops(this);
    binops.BINARY_POWER();
}

void
LlvmFunctionBuilder::INPLACE_POWER()
{
    OpcodeBinops binops(this);
    binops.INPLACE_POWER();
}

#define UNARYOP_METH(NAME)                              \
void							\
LlvmFunctionBuilder::NAME()				\
{							\
    OpcodeUnaryops unary(this);                         \
    unary.NAME();                                       \
}

UNARYOP_METH(UNARY_CONVERT)
UNARYOP_METH(UNARY_INVERT)
UNARYOP_METH(UNARY_POSITIVE)
UNARYOP_METH(UNARY_NEGATIVE)
UNARYOP_METH(UNARY_NOT)
#undef UNARYOP_METH

void
LlvmFunctionBuilder::POP_TOP()
{
    OpcodeStack stack(this);
    stack.POP_TOP();
}

void
LlvmFunctionBuilder::DUP_TOP()
{
    OpcodeStack stack(this);
    stack.DUP_TOP();
}

void
LlvmFunctionBuilder::DUP_TOP_TWO()
{
    OpcodeStack stack(this);
    stack.DUP_TOP_TWO();
}

void
LlvmFunctionBuilder::DUP_TOP_THREE()
{
    OpcodeStack stack(this);
    stack.DUP_TOP_THREE();
}

void
LlvmFunctionBuilder::ROT_TWO()
{
    OpcodeStack stack(this);
    stack.ROT_TWO();
}

void
LlvmFunctionBuilder::ROT_THREE()
{
    OpcodeStack stack(this);
    stack.ROT_THREE();
}

void
LlvmFunctionBuilder::ROT_FOUR()
{
    OpcodeStack stack(this);
    stack.ROT_FOUR();
}

void
LlvmFunctionBuilder::COMPARE_OP(int cmp_op)
{
    OpcodeCmpops cmpops(this);
    cmpops.COMPARE_OP(cmp_op);
}

void
LlvmFunctionBuilder::LIST_APPEND()
{
    OpcodeContainer cont(this);
    cont.LIST_APPEND();
}

void
LlvmFunctionBuilder::STORE_MAP()
{
    OpcodeContainer cont(this);
    cont.STORE_MAP();
}

Value *
LlvmFunctionBuilder::GetListItemSlot(Value *lst, int idx)
{
    Value *listobj = this->builder_.CreateBitCast(
        lst, PyTypeBuilder<PyListObject *>::get(this->context_));
    // Load the target of the ob_item PyObject** into list_items.
    Value *list_items = this->builder_.CreateLoad(
        ListTy::ob_item(this->builder_, listobj));
    // GEP the list_items PyObject* up to the desired item
    const Type *int_type = Type::getInt32Ty(this->context_);
    return this->builder_.CreateGEP(list_items,
                                    ConstantInt::get(int_type, idx),
                                    "list_item_slot");
}

Value *
LlvmFunctionBuilder::GetTupleItemSlot(Value *tup, int idx)
{
    Value *tupobj = this->builder_.CreateBitCast(
        tup, PyTypeBuilder<PyTupleObject*>::get(this->context_));
    // Make CreateGEP perform &tup_item_indices[0].ob_item[idx].
    Value *tuple_items = TupleTy::ob_item(this->builder_, tupobj);
    return this->builder_.CreateStructGEP(tuple_items, idx,
                                          "tuple_item_slot");
}

void
LlvmFunctionBuilder::BUILD_LIST(int size)
{
    OpcodeContainer cont(this);
    cont.BUILD_LIST(size);
}

void
LlvmFunctionBuilder::BUILD_TUPLE(int size)
{
    OpcodeContainer cont(this);
    cont.BUILD_TUPLE(size);
}

void
LlvmFunctionBuilder::BUILD_MAP(int size)
{
    OpcodeContainer cont(this);
    cont.BUILD_MAP(size);
}

void
LlvmFunctionBuilder::SLICE_BOTH()
{
    OpcodeSlice slice(this);
    slice.SLICE_BOTH();
}

void
LlvmFunctionBuilder::SLICE_LEFT()
{
    OpcodeSlice slice(this);
    slice.SLICE_LEFT();
}

void
LlvmFunctionBuilder::SLICE_RIGHT()
{
    OpcodeSlice slice(this);
    slice.SLICE_RIGHT();
}

void
LlvmFunctionBuilder::SLICE_NONE()
{
    OpcodeSlice slice(this);
    slice.SLICE_NONE();
}

void
LlvmFunctionBuilder::STORE_SLICE_BOTH()
{
    OpcodeSlice slice(this);
    slice.STORE_SLICE_BOTH();
}

void
LlvmFunctionBuilder::STORE_SLICE_LEFT()
{
    OpcodeSlice slice(this);
    slice.STORE_SLICE_LEFT();
}

void
LlvmFunctionBuilder::STORE_SLICE_RIGHT()
{
    OpcodeSlice slice(this);
    slice.STORE_SLICE_RIGHT();
}

void
LlvmFunctionBuilder::STORE_SLICE_NONE()
{
    OpcodeSlice slice(this);
    slice.STORE_SLICE_NONE();
}

void
LlvmFunctionBuilder::DELETE_SLICE_BOTH()
{
    OpcodeSlice slice(this);
    slice.DELETE_SLICE_BOTH();
}

void
LlvmFunctionBuilder::DELETE_SLICE_LEFT()
{
    OpcodeSlice slice(this);
    slice.DELETE_SLICE_LEFT();
}

void
LlvmFunctionBuilder::DELETE_SLICE_RIGHT()
{
    OpcodeSlice slice(this);
    slice.DELETE_SLICE_RIGHT();
}

void
LlvmFunctionBuilder::DELETE_SLICE_NONE()
{
    OpcodeSlice slice(this);
    slice.DELETE_SLICE_NONE();
}

void
LlvmFunctionBuilder::BUILD_SLICE_TWO()
{
    OpcodeSlice slice(this);
    slice.BUILD_SLICE_TWO();
}

void
LlvmFunctionBuilder::BUILD_SLICE_THREE()
{
    OpcodeSlice slice(this);
    slice.BUILD_SLICE_THREE();
}

void
LlvmFunctionBuilder::UNPACK_SEQUENCE(int size)
{
    OpcodeContainer cont(this);
    cont.UNPACK_SEQUENCE(size);
}

void
LlvmFunctionBuilder::IncRef(Value *value)
{
    Function *incref = this->GetGlobalFunction<void(PyObject*)>(
        "_PyLlvm_WrapIncref");
    this->CreateCall(incref, value);
}

void
LlvmFunctionBuilder::DecRef(Value *value)
{
    Function *decref = this->GetGlobalFunction<void(PyObject*)>(
        "_PyLlvm_WrapDecref");
    this->CreateCall(decref, value);
}

void
LlvmFunctionBuilder::XDecRef(Value *value)
{
    Function *xdecref = this->GetGlobalFunction<void(PyObject*)>(
        "_PyLlvm_WrapXDecref");
    this->CreateCall(xdecref, value);
}

void
LlvmFunctionBuilder::Push(Value *value)
{
    Value *stack_pointer = this->builder_.CreateLoad(this->stack_pointer_addr_);
    this->llvm_data_->tbaa_stack.MarkInstruction(stack_pointer);

    this->builder_.CreateStore(value, stack_pointer);
    Value *new_stack_pointer = this->builder_.CreateGEP(
        stack_pointer, ConstantInt::get(Type::getInt32Ty(this->context_), 1));
    this->llvm_data_->tbaa_stack.MarkInstruction(stack_pointer);

    this->builder_.CreateStore(new_stack_pointer, this->stack_pointer_addr_);
}

Value *
LlvmFunctionBuilder::Pop()
{
    Value *stack_pointer = this->builder_.CreateLoad(this->stack_pointer_addr_);
    this->llvm_data_->tbaa_stack.MarkInstruction(stack_pointer);

    Value *new_stack_pointer = this->builder_.CreateGEP(
        stack_pointer, ConstantInt::getSigned(Type::getInt32Ty(this->context_),
                                              -1));
    this->llvm_data_->tbaa_stack.MarkInstruction(new_stack_pointer);

    Value *former_top = this->builder_.CreateLoad(new_stack_pointer);
    this->builder_.CreateStore(new_stack_pointer, this->stack_pointer_addr_);
    return former_top;
}

Value *
LlvmFunctionBuilder::GetStackLevel()
{
    Value *stack_pointer = this->builder_.CreateLoad(this->stack_pointer_addr_);
    Value *level64 =
        this->builder_.CreatePtrDiff(stack_pointer, this->stack_bottom_);
    // The stack level is stored as an int, not an int64.
    return this->builder_.CreateTrunc(level64,
                                      PyTypeBuilder<int>::get(this->context_),
                                      "stack_level");
}

void
LlvmFunctionBuilder::CheckPyTicker(BasicBlock *next_block)
{
    if (next_block == NULL) {
        next_block = this->CreateBasicBlock("ticker_dec_end");
    }
    Value *pyticker_result = this->builder_.CreateCall(
        this->GetGlobalFunction<int(PyThreadState*)>(
            "_PyLlvm_DecAndCheckPyTicker"),
        this->tstate_);
    this->builder_.CreateCondBr(this->IsNegative(pyticker_result),
                                this->GetExceptionBlock(),
                                next_block);
    this->builder_.SetInsertPoint(next_block);
}

void
LlvmFunctionBuilder::DieForUndefinedOpcode(const char *opcode_name)
{
    std::string message("Undefined opcode: ");
    message.append(opcode_name);
    this->Abort(message);
}

void
LlvmFunctionBuilder::Assert(llvm::Value *should_be_true,
                            const std::string &failure_message)
{
#ifndef NDEBUG
    BasicBlock *assert_passed =
            this->CreateBasicBlock(failure_message + "_assert_passed");
    BasicBlock *assert_failed =
            this->CreateBasicBlock(failure_message + "_assert_failed");
    this->builder_.CreateCondBr(should_be_true, assert_passed, assert_failed);

    this->builder_.SetInsertPoint(assert_failed);
    this->Abort(failure_message);
    this->builder_.CreateUnreachable();

    this->builder_.SetInsertPoint(assert_passed);
#endif
}

void
LlvmFunctionBuilder::Abort(const std::string &failure_message)
{
    this->CreateCall(
        GetGlobalFunction<int(const char*)>("puts"),
        this->llvm_data_->GetGlobalStringPtr(failure_message));
    this->CreateCall(GetGlobalFunction<void()>("abort"));
}

Constant *
LlvmFunctionBuilder::GetGlobalVariableFor(PyObject *obj)
{
    return this->llvm_data_->constant_mirror().GetGlobalVariableFor(obj);
}

// For llvm::Functions, copy callee's calling convention and attributes to
// callsite; for non-Functions, leave the default calling convention and
// attributes in place (ie, do nothing). We require this for function pointers.
llvm::CallInst *
TransferAttributes(llvm::CallInst *callsite, const llvm::Value* callee)
{
    if (const llvm::GlobalAlias *alias =
            llvm::dyn_cast<llvm::GlobalAlias>(callee))
        callee = alias->getAliasedGlobal();

    if (const llvm::Function *func = llvm::dyn_cast<llvm::Function>(callee)) {
        callsite->setCallingConv(func->getCallingConv());
        callsite->setAttributes(func->getAttributes());
    }
    return callsite;
}

llvm::CallInst *
LlvmFunctionBuilder::CreateCall(llvm::Value *callee, const char *name)
{
    llvm::CallInst *call = this->builder_.CreateCall(callee, name);
    return TransferAttributes(call, callee);
}

llvm::CallInst *
LlvmFunctionBuilder::CreateCall(llvm::Value *callee, llvm::Value *arg1,
                                const char *name)
{
    llvm::CallInst *call = this->builder_.CreateCall(callee, arg1, name);
    return TransferAttributes(call, callee);
}

llvm::CallInst *
LlvmFunctionBuilder::CreateCall(llvm::Value *callee, llvm::Value *arg1,
                                llvm::Value *arg2, const char *name)
{
    llvm::CallInst *call = this->builder_.CreateCall2(callee, arg1, arg2, name);
    return TransferAttributes(call, callee);
}

llvm::CallInst *
LlvmFunctionBuilder::CreateCall(llvm::Value *callee, llvm::Value *arg1,
                                llvm::Value *arg2, llvm::Value *arg3,
                                const char *name)
{
    llvm::CallInst *call = this->builder_.CreateCall3(callee, arg1, arg2, arg3,
                                                      name);
    return TransferAttributes(call, callee);
}

llvm::CallInst *
LlvmFunctionBuilder::CreateCall(llvm::Value *callee, llvm::Value *arg1,
                                llvm::Value *arg2, llvm::Value *arg3,
                                llvm::Value *arg4, const char *name)
{
    llvm::CallInst *call = this->builder_.CreateCall4(callee, arg1, arg2, arg3,
                                                      arg4, name);
    return TransferAttributes(call, callee);
}

llvm::ReturnInst *
LlvmFunctionBuilder::CreateRet(llvm::Value *retval)
{
    return this->builder_.CreateRet(retval);
}

llvm::BasicBlock *
LlvmFunctionBuilder::CreateBasicBlock(const llvm::Twine &name)
{
    return BasicBlock::Create(this->context_, name, this->function_);
}

Value *
LlvmFunctionBuilder::IsNull(Value *value)
{
    return this->builder_.CreateICmpEQ(
        value, Constant::getNullValue(value->getType()));
}

Value *
LlvmFunctionBuilder::IsNonZero(Value *value)
{
    return this->builder_.CreateICmpNE(
        value, Constant::getNullValue(value->getType()));
}

Value *
LlvmFunctionBuilder::IsNegative(Value *value)
{
    return this->builder_.CreateICmpSLT(
        value, ConstantInt::getSigned(value->getType(), 0));
}

Value *
LlvmFunctionBuilder::IsPositive(Value *value)
{
    return this->builder_.CreateICmpSGT(
        value, ConstantInt::getSigned(value->getType(), 0));
}

Value *
LlvmFunctionBuilder::IsInstanceOfFlagClass(llvm::Value *value, int flag)
{
    Value *type = this->builder_.CreateBitCast(
        this->builder_.CreateLoad(
            ObjectTy::ob_type(this->builder_, value),
            "type"),
        PyTypeBuilder<PyTypeObject *>::get(this->context_));
    Value *type_flags = this->builder_.CreateLoad(
        TypeTy::tp_flags(this->builder_, type),
        "type_flags");
    Value *is_instance = this->builder_.CreateAnd(
        type_flags,
        ConstantInt::get(type_flags->getType(), flag));
    return this->IsNonZero(is_instance);
}

void
LlvmFunctionBuilder::PropagateExceptionOnNull(Value *value)
{
    BasicBlock *propagate =
        this->CreateBasicBlock("PropagateExceptionOnNull_propagate");
    BasicBlock *pass =
        this->CreateBasicBlock("PropagateExceptionOnNull_pass");
    this->builder_.CreateCondBr(this->IsNull(value), propagate, pass);

    this->builder_.SetInsertPoint(propagate);
    this->PropagateException();

    this->builder_.SetInsertPoint(pass);
}

void
LlvmFunctionBuilder::PropagateExceptionOnNegative(Value *value)
{
    BasicBlock *propagate =
        this->CreateBasicBlock("PropagateExceptionOnNegative_propagate");
    BasicBlock *pass =
        this->CreateBasicBlock("PropagateExceptionOnNegative_pass");
    this->builder_.CreateCondBr(this->IsNegative(value), propagate, pass);

    this->builder_.SetInsertPoint(propagate);
    this->PropagateException();

    this->builder_.SetInsertPoint(pass);
}

void
LlvmFunctionBuilder::PropagateExceptionOnNonZero(Value *value)
{
    BasicBlock *propagate =
        this->CreateBasicBlock("PropagateExceptionOnNonZero_propagate");
    BasicBlock *pass =
        this->CreateBasicBlock("PropagateExceptionOnNonZero_pass");
    this->builder_.CreateCondBr(this->IsNonZero(value), propagate, pass);

    this->builder_.SetInsertPoint(propagate);
    this->PropagateException();

    this->builder_.SetInsertPoint(pass);
}

Value *
LlvmFunctionBuilder::LookupName(int name_index)
{
    Value *name = this->builder_.CreateLoad(
        this->builder_.CreateGEP(
            this->names_, ConstantInt::get(Type::getInt32Ty(this->context_),
                                           name_index),
            "constant_name"));
    return name;
}

llvm::Value *
LlvmFunctionBuilder::IsPythonTrue(Value *value)
{
    BasicBlock *not_py_true =
        this->CreateBasicBlock("IsPythonTrue_is_not_PyTrue");
    BasicBlock *not_py_false =
        this->CreateBasicBlock("IsPythonTrue_is_not_PyFalse");
    BasicBlock *decref_value =
        this->CreateBasicBlock("IsPythonTrue_decref_value");
    BasicBlock *done =
        this->CreateBasicBlock("IsPythonTrue_done");

    Value *result_addr = this->CreateAllocaInEntryBlock(
        Type::getInt1Ty(this->context_), NULL, "IsPythonTrue_result");
    Value *py_false = this->GetGlobalVariableFor((PyObject*)&_Py_ZeroStruct);
    Value *py_true = this->GetGlobalVariableFor((PyObject*)&_Py_TrueStruct);

    Value *is_PyTrue = this->builder_.CreateICmpEQ(
        py_true, value, "IsPythonTrue_is_PyTrue");
    this->builder_.CreateStore(is_PyTrue, result_addr);
    this->builder_.CreateCondBr(is_PyTrue, decref_value, not_py_true);

    this->builder_.SetInsertPoint(not_py_true);
    Value *is_not_PyFalse = this->builder_.CreateICmpNE(
        py_false, value, "IsPythonTrue_is_PyFalse");
    this->builder_.CreateStore(is_not_PyFalse, result_addr);
    this->builder_.CreateCondBr(is_not_PyFalse, not_py_false, decref_value);

    this->builder_.SetInsertPoint(not_py_false);
    Function *pyobject_istrue =
        this->GetGlobalFunction<int(PyObject *)>("PyObject_IsTrue");
    Value *istrue_result = this->CreateCall(
        pyobject_istrue, value, "PyObject_IsTrue_result");
    this->DecRef(value);
    this->PropagateExceptionOnNegative(istrue_result);
    this->builder_.CreateStore(
        this->IsPositive(istrue_result),
        result_addr);
    this->builder_.CreateBr(done);

    this->builder_.SetInsertPoint(decref_value);
    this->DecRef(value);
    this->builder_.CreateBr(done);

    this->builder_.SetInsertPoint(done);
    return this->builder_.CreateLoad(result_addr);
}

int
LlvmFunctionBuilder::FinishFunction()
{
    // If the code object doesn't need to watch any dicts, it shouldn't be
    // invalidated when those dicts change.
    PyCodeObject *code = this->code_object_;
    if (code->co_watching) {
        for (unsigned i = 0; i < NUM_WATCHING_REASONS; ++i) {
            if (!this->uses_watched_dicts_.test(i)) {
                _PyCode_IgnoreDict(code, (ReasonWatched)i);
            }
        }
    }

    // We need to register to become invalidated from any types we've touched.
    for (llvm::SmallPtrSet<PyTypeObject*, 5>::const_iterator
         i = this->types_used_.begin(), e = this->types_used_.end();
         i != e; ++i) {
        // TODO(rnk): When we support recompilation, we will need to remove
        // ourselves from the code listeners list, or we may be invalidated due
        // to changes on unrelated types.  This requires remembering a list of
        // associated types in the code object.
        if (_PyType_AddCodeListener(*i, (PyObject*)code) < 0) {
            return -1;
        }
    }

    return 0;
}

}  // namespace py
