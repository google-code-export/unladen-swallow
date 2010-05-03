// -*- C++ -*-
#ifndef PYTHON_LLVM_FBUILDER_H
#define PYTHON_LLVM_FBUILDER_H

#ifndef __cplusplus
#error This header expects to be included only in C++ source
#endif

#include "JIT/PyTypeBuilder.h"
#include "JIT/RuntimeFeedback.h"
#include "Util/EventTimer.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/DebugInfo.h"
#include "llvm/Constants.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Type.h"
#include "llvm/Support/IRBuilder.h"
#include "llvm/Support/TargetFolder.h"

#include <bitset>
#include <string>

struct PyCodeObject;
struct PyGlobalLlvmData;

namespace py {

class OpcodeAttributes;
class OpcodeBinops;
class OpcodeBlock;
class OpcodeCall;
class OpcodeClosure;
class OpcodeCmpops;
class OpcodeContainer;
class OpcodeControl;
class OpcodeGlobals;
class OpcodeLocals;
class OpcodeLoop;
class OpcodeName;
class OpcodeSlice;
class OpcodeStack;
class OpcodeUnaryops;

llvm::CallInst *
TransferAttributes(llvm::CallInst *callsite, const llvm::Value* callee);

/// Helps the compiler build LLVM functions corresponding to Python
/// functions.  This class maintains the IRBuilder and several Value*s
/// set up in the entry block.
class LlvmFunctionBuilder {
    LlvmFunctionBuilder(const LlvmFunctionBuilder &);  // Not implemented.
    void operator=(const LlvmFunctionBuilder &);  // Not implemented.

    friend class OpcodeAttributes;
    friend class OpcodeBinops;
    friend class OpcodeBlock;
    friend class OpcodeCall;
    friend class OpcodeClosure;
    friend class OpcodeCmpops;
    friend class OpcodeContainer;
    friend class OpcodeControl;
    friend class OpcodeGlobals;
    friend class OpcodeLocals;
    friend class OpcodeLoop;
    friend class OpcodeName;
    friend class OpcodeSlice;
    friend class OpcodeStack;
    friend class OpcodeUnaryops;

public:
    LlvmFunctionBuilder(PyGlobalLlvmData *global_data, PyCodeObject *code);

    llvm::Function *function() { return function_; }
    typedef llvm::IRBuilder<true, llvm::TargetFolder> BuilderT;
    BuilderT& builder() { return builder_; }
    llvm::BasicBlock *unreachable_block() { return unreachable_block_; }

    /// Sets the current instruction index.  This is only put into the
    /// frame object when tracing.
    void SetLasti(int current_instruction_index);

    /// Sets the current line number being executed.  This is used to
    /// make tracebacks correct and to get tracing to fire in the
    /// right places.
    void SetLineNumber(int line);

    /// Inserts a call to llvm.dbg.stoppoint.
    void SetDebugStopPoint(int line_number);

    /// Convenience wrapper for creating named basic blocks using the current
    /// context and function.
    llvm::BasicBlock *CreateBasicBlock(const llvm::Twine &name);

    /// This function fills the block that handles a backedge.  Each
    /// backedge needs to check if it needs to handle signals or
    /// switch threads.  If the backedge doesn't land at the start of
    /// a line, it also needs to update the line number and check
    /// whether line tracing has been turned on.  This function leaves
    /// the insert point in a block with a terminator already added,
    /// so the caller should re-set the insert point.
    void FillBackedgeLanding(llvm::BasicBlock *backedge_landing,
                             llvm::BasicBlock *target,
                             bool to_start_of_line,
                             int line_number);

    /// Sets the insert point to next_block, inserting an
    /// unconditional branch to there if the current block doesn't yet
    /// have a terminator instruction.
    void FallThroughTo(llvm::BasicBlock *next_block);

    /// Register callbacks that might invalidate native code based on the
    /// optimizations performed in the generated code.
    int FinishFunction();

    /// The following methods operate like the opcodes with the same
    /// name.
    void LOAD_CONST(int index);
    void LOAD_FAST(int index);
    void STORE_FAST(int index);
    void DELETE_FAST(int index);

    void SETUP_LOOP(llvm::BasicBlock *target, int target_opindex,
                    llvm::BasicBlock *fallthrough);
    void GET_ITER();
    void FOR_ITER(llvm::BasicBlock *target, llvm::BasicBlock *fallthrough);
    void POP_BLOCK();

    void SETUP_EXCEPT(llvm::BasicBlock *target, int target_opindex,
                      llvm::BasicBlock *fallthrough);
    void SETUP_FINALLY(llvm::BasicBlock *target, int target_opindex,
                       llvm::BasicBlock *fallthrough);
    void END_FINALLY();
    void WITH_CLEANUP();

    void JUMP_FORWARD(llvm::BasicBlock *target, llvm::BasicBlock *fallthrough) {
        this->JUMP_ABSOLUTE(target, fallthrough);
    }
    void JUMP_ABSOLUTE(llvm::BasicBlock *target, llvm::BasicBlock *fallthrough);

    void POP_JUMP_IF_FALSE(unsigned target_idx,
                           unsigned fallthrough_idx,
                           llvm::BasicBlock *target,
                           llvm::BasicBlock *fallthrough);
    void POP_JUMP_IF_TRUE(unsigned target_idx,
                          unsigned fallthrough_idx,
                          llvm::BasicBlock *target,
                          llvm::BasicBlock *fallthrough);
    void JUMP_IF_FALSE_OR_POP(unsigned target_idx,
                              unsigned fallthrough_idx,
                              llvm::BasicBlock *target,
                              llvm::BasicBlock *fallthrough);
    void JUMP_IF_TRUE_OR_POP(unsigned target_idx,
                             unsigned fallthrough_idx,
                             llvm::BasicBlock *target,
                             llvm::BasicBlock *fallthrough);
    void CONTINUE_LOOP(llvm::BasicBlock *target,
                       int target_opindex,
                       llvm::BasicBlock *fallthrough);

    void BREAK_LOOP();
    void RETURN_VALUE();
    void YIELD_VALUE();

    void POP_TOP();
    void DUP_TOP();
    void DUP_TOP_TWO();
    void DUP_TOP_THREE();
    void ROT_TWO();
    void ROT_THREE();
    void ROT_FOUR();

    void BINARY_ADD();
    void BINARY_SUBTRACT();
    void BINARY_MULTIPLY();
    void BINARY_TRUE_DIVIDE();
    void BINARY_DIVIDE();
    void BINARY_MODULO();
    void BINARY_POWER();
    void BINARY_LSHIFT();
    void BINARY_RSHIFT();
    void BINARY_OR();
    void BINARY_XOR();
    void BINARY_AND();
    void BINARY_FLOOR_DIVIDE();
    void BINARY_SUBSCR();

    void INPLACE_ADD();
    void INPLACE_SUBTRACT();
    void INPLACE_MULTIPLY();
    void INPLACE_TRUE_DIVIDE();
    void INPLACE_DIVIDE();
    void INPLACE_MODULO();
    void INPLACE_POWER();
    void INPLACE_LSHIFT();
    void INPLACE_RSHIFT();
    void INPLACE_OR();
    void INPLACE_XOR();
    void INPLACE_AND();
    void INPLACE_FLOOR_DIVIDE();

    void UNARY_CONVERT();
    void UNARY_INVERT();
    void UNARY_POSITIVE();
    void UNARY_NEGATIVE();
    void UNARY_NOT();

    void SLICE_NONE();
    void SLICE_LEFT();
    void SLICE_RIGHT();
    void SLICE_BOTH();
    void STORE_SLICE_NONE();
    void STORE_SLICE_LEFT();
    void STORE_SLICE_RIGHT();
    void STORE_SLICE_BOTH();
    void DELETE_SLICE_NONE();
    void DELETE_SLICE_LEFT();
    void DELETE_SLICE_RIGHT();
    void DELETE_SLICE_BOTH();
    void STORE_SUBSCR();
    void DELETE_SUBSCR();
    void STORE_MAP();
    void LIST_APPEND();
    void IMPORT_NAME();

    void COMPARE_OP(int cmp_op);
    
    void CALL_FUNCTION(int num_args);
    void CALL_FUNCTION_VAR(int num_args);
    void CALL_FUNCTION_KW(int num_args);
    void CALL_FUNCTION_VAR_KW(int num_args);

    void BUILD_TUPLE(int size);
    void BUILD_LIST(int size);
    void BUILD_MAP(int size);
    void BUILD_SLICE_TWO();
    void BUILD_SLICE_THREE();
    void UNPACK_SEQUENCE(int size);

    void LOAD_GLOBAL(int index);
    void STORE_GLOBAL(int index);
    void DELETE_GLOBAL(int index);

    void LOAD_NAME(int index);
    void STORE_NAME(int index);
    void DELETE_NAME(int index);

    void LOAD_ATTR(int index);
    void STORE_ATTR(int index);
    void DELETE_ATTR(int index);

    void LOAD_CLOSURE(int freevar_index);
    void MAKE_CLOSURE(int num_defaults);
    void LOAD_DEREF(int index);
    void STORE_DEREF(int index);

    void RAISE_VARARGS_ZERO();
    void RAISE_VARARGS_ONE();
    void RAISE_VARARGS_TWO();
    void RAISE_VARARGS_THREE();

    bool uses_delete_fast;

private:
    /// These two functions increment or decrement the reference count
    /// of a PyObject*. The behavior is undefined if the Value's type
    /// isn't PyObject* or a subclass.
    void IncRef(llvm::Value *value);
    void DecRef(llvm::Value *value);
    void XDecRef(llvm::Value *value);

    /// These two push or pop a value onto or off of the stack. The
    /// behavior is undefined if the Value's type isn't PyObject* or a
    /// subclass.  These do no refcount operations, which means that
    /// Push() consumes a reference and gives ownership of it to the
    /// new value on the stack, and Pop() returns a pointer that owns
    /// a reference (which it got from the stack).
    void Push(llvm::Value *value);
    llvm::Value *Pop();

    /// Takes a target stack pointer and pops values off the stack
    /// until it gets there, decref'ing as it goes.
    void PopAndDecrefTo(llvm::Value *target_stack_pointer);

    /// The PyFrameObject holds several values, like the block stack
    /// and stack pointer, that we store in allocas inside this
    /// function.  When we suspend or resume a generator, or bail out
    /// to the interpreter, we need to transfer those values between
    /// the frame and the allocas.
    void CopyToFrameObject();
    void CopyFromFrameObject();

    /// We copy the function's locals into an LLVM alloca so that LLVM can
    /// better reason about them.
    void CopyLocalsFromFrameObject();

    template<typename T>
    llvm::Constant *GetSigned(int64_t val) {
        return llvm::ConstantInt::getSigned(
                PyTypeBuilder<T>::get(this->context_),
                val);
    }

    /// Returns the difference between the current stack pointer and
    /// the base of the stack.
    llvm::Value *GetStackLevel();

    /// Get the runtime feedback for the current opcode (as set by SetLasti()).
    /// Opcodes with multiple feedback units should use the arg_index version
    /// to access individual units.
    const PyRuntimeFeedback *GetFeedback() const {
        return GetFeedback(0);
    }
    const PyRuntimeFeedback *GetFeedback(unsigned arg_index) const;
    const PyTypeObject *GetTypeFeedback(unsigned arg_index) const;

    // Look up a name in the function's names list, returning the
    // PyStringObject for the name_index.
    llvm::Value *LookupName(int name_index);

    /// Inserts a call that will print opcode_name and abort the
    /// program when it's reached.
    void DieForUndefinedOpcode(const char *opcode_name);

    /// How many parameters does the currently-compiling function have?
    int GetParamCount() const;

    /// Implements something like the C assert statement.  If
    /// should_be_true (an i1) is false, prints failure_message (with
    /// puts) and aborts.  Compiles to nothing in optimized mode.
    void Assert(llvm::Value *should_be_true,
                const std::string &failure_message);

    /// Prints failure_message (with puts) and aborts.
    void Abort(const std::string &failure_message);

    // Returns the global variable with type T, address 'var_address',
    // and name 'name'.  If the ExecutionEngine already knows of a
    // variable with the given address, we name and return it.
    // Otherwise the variable will be looked up in Python's C runtime.
    template<typename VariableType>
    llvm::Constant *GetGlobalVariable(
        void *var_address, const std::string &name);

    // Returns the global function with type T and name 'name'. The
    // function will be looked up in Python's C runtime.
    template<typename FunctionType>
    llvm::Function *GetGlobalFunction(const std::string &name)
    {
        return llvm::cast<llvm::Function>(
            this->module_->getOrInsertFunction(
                name, PyTypeBuilder<FunctionType>::get(this->context_)));
    }


    // Returns a global variable that represents 'obj'.  These get
    // cached in the ExecutionEngine's global mapping table, and they
    // incref the object so its address doesn't get re-used while the
    // GlobalVariable is still alive.  See JIT/ConstantMirror.h for
    // more details.  Use this in preference to GetGlobalVariable()
    // for PyObjects that may be immutable.
    llvm::Constant *GetGlobalVariableFor(PyObject *obj);

    // Copies the elements from array[0] to array[N-1] to target, bytewise.
    void MemCpy(llvm::Value *target, llvm::Value *array, llvm::Value *N);

    // Emits code to decrement _Py_Ticker and handle signals and
    // thread-switching when it expires.  Falls through to next_block (or a
    // new block if it's NULL) and leaves the insertion point there.
    void CheckPyTicker(llvm::BasicBlock *next_block = NULL);

    // These are just like the CreateCall* calls on IRBuilder, except they also
    // apply callee's calling convention and attributes to the call site.
    llvm::CallInst *CreateCall(llvm::Value *callee,
                               const char *name = "");
    llvm::CallInst *CreateCall(llvm::Value *callee,
                               llvm::Value *arg1,
                               const char *name = "");
    llvm::CallInst *CreateCall(llvm::Value *callee,
                               llvm::Value *arg1,
                               llvm::Value *arg2,
                               const char *name = "");
    llvm::CallInst *CreateCall(llvm::Value *callee,
                               llvm::Value *arg1,
                               llvm::Value *arg2,
                               llvm::Value *arg3,
                               const char *name = "");
    llvm::CallInst *CreateCall(llvm::Value *callee,
                               llvm::Value *arg1,
                               llvm::Value *arg2,
                               llvm::Value *arg3,
                               llvm::Value *arg4,
                               const char *name = "");
    template<typename InputIterator>
    llvm::CallInst *CreateCall(llvm::Value *callee,
                               InputIterator begin,
                               InputIterator end,
                               const char *name = "")
    {
        llvm::CallInst *call =
            this->builder_.CreateCall(callee, begin, end, name);
        return TransferAttributes(call, callee);
    }


    /// Marks the end of the function and inserts a return instruction.
    llvm::ReturnInst *CreateRet(llvm::Value *retval);

    /// Get the LLVM NULL Value for the given type.
    template<typename T>
    llvm::Value *GetNull()
    {
        return llvm::Constant::getNullValue(
            PyTypeBuilder<T>::get(this->context_));
    }


    // Returns an i1, true if value represents a NULL pointer.
    llvm::Value *IsNull(llvm::Value *value);
    // Returns an i1, true if value is a negative integer.
    llvm::Value *IsNegative(llvm::Value *value);
    // Returns an i1, true if value is a non-zero integer.
    llvm::Value *IsNonZero(llvm::Value *value);
    // Returns an i1, true if value is a positive (>0) integer.
    llvm::Value *IsPositive(llvm::Value *value);
    // Returns an i1, true if value is a PyObject considered true.
    // Steals the reference to value.
    llvm::Value *IsPythonTrue(llvm::Value *value);
    // Returns an i1, true if value is an instance of the class
    // represented by the flag argument.  flag should be something
    // like Py_TPFLAGS_INT_SUBCLASS.
    llvm::Value *IsInstanceOfFlagClass(llvm::Value *value, int flag);

    /// During stack unwinding it may be necessary to jump back into
    /// the function to handle a finally or except block.  Since LLVM
    /// doesn't allow us to directly store labels as data, we instead
    /// add the index->label mapping to a switch instruction and
    /// return the i32 for the index.
    llvm::ConstantInt *AddUnwindTarget(llvm::BasicBlock *target,
                                       int target_opindex);

    // Inserts a jump to the return block, returning retval.  You
    // should _never_ call CreateRet directly from one of the opcode
    // handlers, since doing so would fail to unwind the stack.
    void Return(llvm::Value *retval);

    // Propagates an exception by jumping to the unwind block with an
    // appropriate unwind reason set.
    void PropagateException();

    // Set up a block preceding the bail-to-interpreter block.
    void CreateBailPoint(unsigned bail_idx, char reason);
    void CreateBailPoint(char reason) {
        CreateBailPoint(this->f_lasti_, reason);
    }

    // Set up a block preceding the bail-to-interpreter block.
    void CreateGuardBailPoint(unsigned bail_idx, char reason);
    void CreateGuardBailPoint(char reason) {
        CreateGuardBailPoint(this->f_lasti_, reason);
    }

    // Only for use in the constructor: Fills in the block that
    // handles bailing out of JITted code back to the interpreter
    // loop.  Code jumping to this block must first:
    //  1) Set frame->f_lasti to the current opcode index.
    //  2) Set frame->f_bailed_from_llvm to a reason.
    void FillBailToInterpreterBlock();
    // Only for use in the constructor: Fills in the block that starts
    // propagating an exception.  Jump to this block when you want to
    // add a traceback entry for the current line.  Don't jump to this
    // block (and just set retval_addr_ and unwind_reason_addr_
    // directly) when you're re-raising an exception and you want to
    // use its traceback.
    void FillPropagateExceptionBlock();
    // Only for use in the constructor: Fills in the unwind block.
    void FillUnwindBlock();
    // Only for use in the constructor: Fills in the block that
    // actually handles returning from the function.
    void FillDoReturnBlock();

    // Create an alloca in the entry block, so that LLVM can optimize
    // it more easily, and return the resulting address. The signature
    // matches IRBuilder.CreateAlloca()'s.
    llvm::Value *CreateAllocaInEntryBlock(
        const llvm::Type *alloca_type,
        llvm::Value *array_size,
        const char *name);

    // If 'value' represents NULL, propagates the exception.
    // Otherwise, falls through.
    void PropagateExceptionOnNull(llvm::Value *value);
    // If 'value' represents a negative integer, propagates the exception.
    // Otherwise, falls through.
    void PropagateExceptionOnNegative(llvm::Value *value);
    // If 'value' represents a non-zero integer, propagates the exception.
    // Otherwise, falls through.
    void PropagateExceptionOnNonZero(llvm::Value *value);

    // Get the address of the idx'th item in a list or tuple object.
    llvm::Value *GetListItemSlot(llvm::Value *lst, int idx);
    llvm::Value *GetTupleItemSlot(llvm::Value *tup, int idx);

#ifdef WITH_TSC
    // Emit code to record a given event with the TSC EventTimer.h system.
    void LogTscEvent(_PyTscEventId event_id);
#endif

    /// Emits code to conditionally bail out to the interpreter loop
    /// if a line tracing function is installed.  If the line tracing
    /// function is not installed, execution will continue at
    /// fallthrough_block.  direction should be _PYFRAME_LINE_TRACE or
    /// _PYFRAME_BACKEDGE_TRACE.
    void MaybeCallLineTrace(llvm::BasicBlock *fallthrough_block,
                            char direction);

    /// Emits code to conditionally bail out to the interpreter loop if a
    /// profiling function is installed. If a profiling function is not
    /// installed, execution will continue at fallthrough_block.
    void BailIfProfiling(llvm::BasicBlock *fallthrough_block);

    /// Embed a pointer of some type directly into the LLVM IR.
    template <typename T>
    llvm::Value *EmbedPointer(void *ptr)
    {
        // We assume that the caller has ensured that ptr will stay live for the
        // life of this native code object.
        return this->builder_.CreateIntToPtr(
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(this->context_),
                             reinterpret_cast<intptr_t>(ptr)),
            PyTypeBuilder<T>::get(this->context_));
    }

    /// Return the BasicBlock we should jump to in order to bail to the
    /// interpreter.
    llvm::BasicBlock *GetBailBlock() const;

    /// Return the BasicBlock we should jump to in order to handle a Python
    /// exception.
    llvm::BasicBlock *GetExceptionBlock() const;

    PyGlobalLlvmData *const llvm_data_;
    // The code object is used for looking up peripheral information
    // about the function.  It's not used to examine the bytecode
    // string.
    PyCodeObject *const code_object_;
    llvm::LLVMContext &context_;
    llvm::Module *const module_;
    llvm::Function *const function_;
    BuilderT builder_;
    const bool is_generator_;
    llvm::DIFactory &debug_info_;
    const llvm::DICompileUnit debug_compile_unit_;
    const llvm::DISubprogram debug_subprogram_;

    // The most recent index we've started emitting an instruction for.
    int f_lasti_;

    // Flags to indicate whether the code object is watching any of the
    // watchable dicts.
    std::bitset<NUM_WATCHING_REASONS> uses_watched_dicts_;

    // The following pointers hold values created in the function's
    // entry block. They're constant after construction.
    llvm::Value *frame_;

    // Address of code_object_->co_use_jit, used for guards.
    llvm::Value *use_jit_addr_;

    llvm::Value *tstate_;
    llvm::Value *stack_bottom_;
    llvm::Value *stack_pointer_addr_;
    // The tmp_stack_pointer is used when we need to have another
    // function update the stack pointer.  Passing the stack pointer
    // directly would prevent mem2reg from working on it, so we copy
    // it to and from the tmp_stack_pointer around the call.
    llvm::Value *tmp_stack_pointer_addr_;
    llvm::Value *varnames_;
    llvm::Value *names_;
    llvm::Value *globals_;
    llvm::Value *builtins_;
    llvm::Value *fastlocals_;
    llvm::Value *freevars_;
    llvm::Value *f_lineno_addr_;
    llvm::Value *f_lasti_addr_;
    // These two fields correspond to the f_blockstack and f_iblock
    // fields in the frame object.  They get explicitly copied back
    // and forth when the frame escapes.
    llvm::Value *blockstack_addr_;
    llvm::Value *num_blocks_addr_;

    // Expose the frame's locals to LLVM. We copy them in on function-entry,
    // copy them out on write. We use a separate alloca for each local
    // because LLVM's scalar replacement of aggregates pass doesn't handle
    // array allocas.
    std::vector<llvm::Value*> locals_;

    llvm::BasicBlock *unreachable_block_;

    // In generators, we use this switch to jump back to the most
    // recently executed yield instruction.
    llvm::SwitchInst *yield_resume_switch_;

    llvm::BasicBlock *bail_to_interpreter_block_;

    llvm::BasicBlock *propagate_exception_block_;
    llvm::BasicBlock *unwind_block_;
    llvm::Value *unwind_target_index_addr_;
    llvm::SparseBitVector<> existing_unwind_targets_;
    llvm::SwitchInst *unwind_target_switch_;
    // Stores one of the UNWIND_XXX constants defined at the top of
    // llvm_fbuilder.cc
    llvm::Value *unwind_reason_addr_;
    llvm::BasicBlock *do_return_block_;
    llvm::Value *retval_addr_;

    llvm::SmallPtrSet<PyTypeObject*, 5> types_used_;
};

template<typename VariableType> llvm::Constant *
LlvmFunctionBuilder::GetGlobalVariable(
    void *var_address, const std::string &name)
{
    const llvm::Type *expected_type =
        PyTypeBuilder<VariableType>::get(this->context_);
    if (llvm::GlobalVariable *global = this->module_->getNamedGlobal(name)) {
        assert (expected_type == global->getType()->getElementType());
        return global;
    }
    if (llvm::GlobalValue *global = const_cast<llvm::GlobalValue*>(
            this->llvm_data_->getExecutionEngine()->
            getGlobalValueAtAddress(var_address))) {
        assert (expected_type == global->getType()->getElementType());
        if (!global->hasName())
            global->setName(name);
        return global;
    }
    return new llvm::GlobalVariable(*this->module_, expected_type,
                                    /*isConstant=*/false,
                                    llvm::GlobalValue::ExternalLinkage,
                                    NULL, name);
}


}  // namespace py

#endif  // PYTHON_LLVM_FBUILDER_H
