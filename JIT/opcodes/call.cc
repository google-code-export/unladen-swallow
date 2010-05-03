#include "Python.h"

#include "JIT/ConstantMirror.h"
#include "JIT/global_llvm_data.h"
#include "JIT/llvm_fbuilder.h"
#include "JIT/opcodes/call.h"

#include "llvm/BasicBlock.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/raw_ostream.h"

using llvm::BasicBlock;
using llvm::Constant;
using llvm::ConstantInt;
using llvm::Function;
using llvm::Type;
using llvm::Value;
using llvm::errs;

#ifdef Py_WITH_INSTRUMENTATION
class CallFunctionStats {
public:
    CallFunctionStats()
        : total(0), direct_calls(0), inlined(0),
          no_opt_kwargs(0), no_opt_params(0),
          no_opt_no_data(0), no_opt_polymorphic(0) {
    }

    ~CallFunctionStats() {
        errs() << "\nCALL_FUNCTION optimization:\n";
        errs() << "Total opcodes: " << this->total << "\n";
        errs() << "Direct C calls: " << this->direct_calls << "\n";
        errs() << "Inlined: " << this->inlined << "\n";
        errs() << "No opt: callsite kwargs: " << this->no_opt_kwargs << "\n";
        errs() << "No opt: function params: " << this->no_opt_params << "\n";
        errs() << "No opt: no data: " << this->no_opt_no_data << "\n";
        errs() << "No opt: polymorphic: " << this->no_opt_polymorphic << "\n";
    }

    // How many CALL_FUNCTION opcodes were compiled.
    unsigned total;
    // How many CALL_FUNCTION opcodes were optimized to direct calls to C
    // functions.
    unsigned direct_calls;
    // How many calls were inlined into the caller.
    unsigned inlined;
    // We only optimize call sites without keyword, *args or **kwargs arguments.
    unsigned no_opt_kwargs;
    // We only optimize METH_ARG_RANGE functions so far.
    unsigned no_opt_params;
    // We only optimize callsites where we've collected data. Note that since
    // we record only PyCFunctions, any call to a Python function will show up
    // as having no data.
    unsigned no_opt_no_data;
    // We only optimize monomorphic callsites so far.
    unsigned no_opt_polymorphic;
};

static llvm::ManagedStatic<CallFunctionStats> call_function_stats;

#define CF_INC_STATS(field) call_function_stats->field++
#else
#define CF_INC_STATS(field)
#endif  /* Py_WITH_INSTRUMENTATION */

namespace py {

OpcodeCall::OpcodeCall(LlvmFunctionBuilder *fbuilder) :
    fbuilder_(fbuilder)
{
}

void
OpcodeCall::CALL_FUNCTION_fast(int oparg,
                               const PyRuntimeFeedback *feedback)
{
    CF_INC_STATS(total);

    // Check for keyword arguments; we only optimize callsites with positional
    // arguments.
    if ((oparg >> 8) & 0xff) {
        CF_INC_STATS(no_opt_kwargs);
        this->CALL_FUNCTION_safe(oparg);
        return;
    }

    // Only optimize monomorphic callsites.
    llvm::SmallVector<PyMethodDef*, 3> fdo_data;
    feedback->GetSeenFuncsInto(fdo_data);
    if (fdo_data.size() != 1) {
#ifdef Py_WITH_INSTRUMENTATION
        if (fdo_data.size() == 0)
            CF_INC_STATS(no_opt_no_data);
        else
            CF_INC_STATS(no_opt_polymorphic);
#endif
        this->CALL_FUNCTION_safe(oparg);
        return;
    }

    PyMethodDef *func_record = fdo_data[0];

    // Only optimize calls to C functions with a known number of parameters,
    // where the number of arguments we have is in that range.
    int flags = func_record->ml_flags;
    int min_arity = func_record->ml_min_arity;
    int max_arity = func_record->ml_max_arity;
    int num_args = oparg & 0xff;
    if (!(flags & METH_ARG_RANGE &&
          min_arity <= num_args && num_args <= max_arity)) {
        CF_INC_STATS(no_opt_params);
        this->CALL_FUNCTION_safe(oparg);
        return;
    }
    assert(num_args <= PY_MAX_ARITY);

    PyCFunction cfunc_ptr = func_record->ml_meth;

    // Expose the C function pointer to LLVM. This is what will actually get
    // called.
    Constant *llvm_func =
        fbuilder_->llvm_data_->constant_mirror().GetGlobalForCFunction(
            cfunc_ptr,
            max_arity,
            func_record->ml_name);

    BasicBlock *not_profiling =
        fbuilder_->CreateBasicBlock("CALL_FUNCTION_not_profiling");
    BasicBlock *check_is_same_func =
        fbuilder_->CreateBasicBlock("CALL_FUNCTION_check_is_same_func");
    BasicBlock *invalid_assumptions =
        fbuilder_->CreateBasicBlock("CALL_FUNCTION_invalid_assumptions");
    BasicBlock *all_assumptions_valid =
        fbuilder_->CreateBasicBlock("CALL_FUNCTION_all_assumptions_valid");

    fbuilder_->BailIfProfiling(not_profiling);

    // Handle bailing back to the interpreter if the assumptions below don't
    // hold.
    fbuilder_->builder_.SetInsertPoint(invalid_assumptions);
    fbuilder_->CreateGuardBailPoint(_PYGUARD_CFUNC);

    fbuilder_->builder_.SetInsertPoint(not_profiling);
#ifdef WITH_TSC
    fbuilder_->LogTscEvent(CALL_START_LLVM);
#endif
    // Retrieve the function to call from the Python stack.
    Value *stack_pointer =
        fbuilder_->builder_.CreateLoad(fbuilder_->stack_pointer_addr_);
    fbuilder_->llvm_data_->tbaa_stack.MarkInstruction(stack_pointer);

    Value *actual_func = fbuilder_->builder_.CreateLoad(
        fbuilder_->builder_.CreateGEP(
            stack_pointer,
            ConstantInt::getSigned(
                Type::getInt64Ty(fbuilder_->context_),
                -num_args - 1)));

    // Make sure it's a PyCFunction; if not, bail.
    Value *is_cfunction = fbuilder_->CreateCall(
        fbuilder_->GetGlobalFunction<int(PyObject *)>(
            "_PyLlvm_WrapCFunctionCheck"),
        actual_func,
        "is_cfunction");
    Value *is_cfunction_guard = fbuilder_->builder_.CreateICmpEQ(
        is_cfunction, ConstantInt::get(is_cfunction->getType(), 1),
        "is_cfunction_guard");
    fbuilder_->builder_.CreateCondBr(is_cfunction_guard, check_is_same_func,
                                     invalid_assumptions);

    // Make sure we got the same underlying function pointer; if not, bail.
    fbuilder_->builder_.SetInsertPoint(check_is_same_func);
    Value *actual_as_pycfunc = fbuilder_->builder_.CreateBitCast(
        actual_func,
        PyTypeBuilder<PyCFunctionObject *>::get(fbuilder_->context_));
    Value *actual_method_def = fbuilder_->builder_.CreateLoad(
        CFunctionTy::m_ml(fbuilder_->builder_, actual_as_pycfunc),
        "CALL_FUNCTION_actual_method_def");
    Value *actual_func_ptr = fbuilder_->builder_.CreateLoad(
        MethodDefTy::ml_meth(fbuilder_->builder_, actual_method_def),
        "CALL_FUNCTION_actual_func_ptr");
    Value *is_same = fbuilder_->builder_.CreateICmpEQ(
        // TODO(jyasskin): change this to "llvm_func" when
        // http://llvm.org/PR5126 is fixed.
        fbuilder_->EmbedPointer<PyCFunction>((void*)cfunc_ptr),
        actual_func_ptr);
    fbuilder_->builder_.CreateCondBr(is_same,
        all_assumptions_valid, invalid_assumptions);

    // Once we get to this point, we know we can make some kind of fast call,
    // either, a) a specialized inline version, or b) a direct call to a C
    // function, bypassing the CPython function call machinery. We check them
    // in that order.
    fbuilder_->builder_.SetInsertPoint(all_assumptions_valid);

    // Check if we are calling a built-in function that can be specialized.
    if (cfunc_ptr == _PyBuiltin_Len) {
        // Feedback index 0 is the function itself, index 1 is the first
        // argument.
        const PyTypeObject *arg1_type = fbuilder_->GetTypeFeedback(1);
        const char *function_name = NULL;
        if (arg1_type == &PyString_Type)
            function_name = "_PyLlvm_BuiltinLen_String";
        else if (arg1_type == &PyUnicode_Type)
            function_name = "_PyLlvm_BuiltinLen_Unicode";
        else if (arg1_type == &PyList_Type)
            function_name = "_PyLlvm_BuiltinLen_List";
        else if (arg1_type == &PyTuple_Type)
            function_name = "_PyLlvm_BuiltinLen_Tuple";
        else if (arg1_type == &PyDict_Type)
            function_name = "_PyLlvm_BuiltinLen_Dict";

        if (function_name != NULL) {
            this->CALL_FUNCTION_fast_len(actual_func, stack_pointer,
                                         invalid_assumptions,
                                         function_name);
            CF_INC_STATS(inlined);
            return;
        }
    }

    // If we get here, we know we have a C function pointer
    // that takes some number of arguments: first the invocant, then some
    // PyObject *s. If the underlying function is nullary, we use NULL for the
    // second argument. Because "the invocant" differs between built-in
    // functions like len() and C-level methods like list.append(), we pull the
    // invocant (called m_self) from the PyCFunction object we popped
    // off the stack. Once the function returns, we patch up the stack pointer.
    Value *self = fbuilder_->builder_.CreateLoad(
        CFunctionTy::m_self(fbuilder_->builder_, actual_as_pycfunc),
        "CALL_FUNCTION_actual_self");
    llvm::SmallVector<Value*, PY_MAX_ARITY + 1> args;  // +1 for self.
    args.push_back(self);
    if (num_args == 0 && max_arity == 0) {
        args.push_back(fbuilder_->GetNull<PyObject *>());
    }
    for (int i = num_args; i >= 1; --i) {
        args.push_back(
            fbuilder_->builder_.CreateLoad(
                fbuilder_->builder_.CreateGEP(
                    stack_pointer,
                    ConstantInt::getSigned(
                        Type::getInt64Ty(fbuilder_->context_), -i))));
    }
    for(int i = 0; i < (max_arity - num_args); ++i) {
        args.push_back(fbuilder_->GetNull<PyObject *>());
    }

#ifdef WITH_TSC
    fbuilder_->LogTscEvent(CALL_ENTER_C);
#endif
    Value *result = fbuilder_->CreateCall(llvm_func, args.begin(), args.end());

    fbuilder_->DecRef(actual_func);
    // Decrefing args[0] will cause self to be double-decrefed, so avoid that.
    for (int i = 1; i <= num_args; ++i) {
        fbuilder_->DecRef(args[i]);
    }
    Value *new_stack_pointer = fbuilder_->builder_.CreateGEP(
        stack_pointer,
        ConstantInt::getSigned(
            Type::getInt64Ty(fbuilder_->context_),
            -num_args - 1));
    fbuilder_->builder_.CreateStore(new_stack_pointer,
                                    fbuilder_->stack_pointer_addr_);
    fbuilder_->PropagateExceptionOnNull(result);
    fbuilder_->Push(result);

    // Check signals and maybe switch threads after each function call.
    fbuilder_->CheckPyTicker();
    CF_INC_STATS(direct_calls);
}

void
OpcodeCall::CALL_FUNCTION_fast_len(Value *actual_func,
                                   Value *stack_pointer,
                                   BasicBlock *invalid_assumptions,
                                   const char *function_name)
{
    BasicBlock *success = fbuilder_->CreateBasicBlock("BUILTIN_LEN_success");

    Value *obj = fbuilder_->Pop();
    Function *builtin_len =
        fbuilder_->GetGlobalFunction<PyObject *(PyObject *)>(function_name);

    Value *result = fbuilder_->CreateCall(builtin_len, obj,
                                          "BUILTIN_LEN_result");
    fbuilder_->builder_.CreateCondBr(fbuilder_->IsNonZero(result),
                                     success, invalid_assumptions);

    fbuilder_->builder_.SetInsertPoint(success);
    fbuilder_->DecRef(actual_func);
    fbuilder_->DecRef(obj);

    Value *new_stack_pointer = fbuilder_->builder_.CreateGEP(
        stack_pointer,
        ConstantInt::getSigned(
            Type::getInt64Ty(fbuilder_->context_),
            -2));  // -1 for the function, -1 for the argument.
    fbuilder_->builder_.CreateStore(new_stack_pointer,
                                    fbuilder_->stack_pointer_addr_);

    fbuilder_->Push(result);

    // Check signals and maybe switch threads after each function call.
    fbuilder_->CheckPyTicker();
}

void
OpcodeCall::CALL_FUNCTION_safe(int oparg)
{
#ifdef WITH_TSC
    fbuilder_->LogTscEvent(CALL_START_LLVM);
#endif
    Value *stack_pointer =
        fbuilder_->builder_.CreateLoad(fbuilder_->stack_pointer_addr_);
    fbuilder_->llvm_data_->tbaa_stack.MarkInstruction(stack_pointer);

    int num_args = oparg & 0xff;
    int num_kwargs = (oparg>>8) & 0xff;
    Function *call_function = fbuilder_->GetGlobalFunction<
        PyObject *(PyObject **, int, int)>("_PyEval_CallFunction");
    Value *result = fbuilder_->CreateCall(
        call_function,
        stack_pointer,
        ConstantInt::get(PyTypeBuilder<int>::get(fbuilder_->context_),
                         num_args),
        ConstantInt::get(PyTypeBuilder<int>::get(fbuilder_->context_),
                         num_kwargs),
        "CALL_FUNCTION_result");
    Value *new_stack_pointer = fbuilder_->builder_.CreateGEP(
        stack_pointer,
        ConstantInt::getSigned(Type::getInt64Ty(fbuilder_->context_),
                               -num_args - 2*num_kwargs - 1));
    fbuilder_->builder_.CreateStore(new_stack_pointer,
                                    fbuilder_->stack_pointer_addr_);
    fbuilder_->PropagateExceptionOnNull(result);
    fbuilder_->Push(result);

    // Check signals and maybe switch threads after each function call.
    fbuilder_->CheckPyTicker();
}

void
OpcodeCall::CALL_FUNCTION(int oparg)
{
    const PyRuntimeFeedback *feedback = fbuilder_->GetFeedback();
    if (feedback == NULL || feedback->FuncsOverflowed())
        this->CALL_FUNCTION_safe(oparg);
    else
        this->CALL_FUNCTION_fast(oparg, feedback);
}

// Keep this in sync with eval.cc
#define CALL_FLAG_VAR 1
#define CALL_FLAG_KW 2

void
OpcodeCall::CallVarKwFunction(int oparg, int call_flag)
{
#ifdef WITH_TSC
    fbuilder_->LogTscEvent(CALL_START_LLVM);
#endif
    Value *stack_pointer =
        fbuilder_->builder_.CreateLoad(fbuilder_->stack_pointer_addr_);
    fbuilder_->llvm_data_->tbaa_stack.MarkInstruction(stack_pointer);

    int num_args = oparg & 0xff;
    int num_kwargs = (oparg>>8) & 0xff;
    Function *call_function = fbuilder_->GetGlobalFunction<
        PyObject *(PyObject **, int, int, int)>("_PyEval_CallFunctionVarKw");
    Value *result = fbuilder_->CreateCall(
        call_function,
        stack_pointer,
        ConstantInt::get(PyTypeBuilder<int>::get(fbuilder_->context_),
                         num_args),
        ConstantInt::get(PyTypeBuilder<int>::get(fbuilder_->context_),
                         num_kwargs),
        ConstantInt::get(PyTypeBuilder<int>::get(fbuilder_->context_),
                         call_flag),
        "CALL_FUNCTION_VAR_KW_result");
    int stack_items = num_args + 2 * num_kwargs + 1;
    if (call_flag & CALL_FLAG_VAR) {
        ++stack_items;
    }
    if (call_flag & CALL_FLAG_KW) {
        ++stack_items;
    }
    Value *new_stack_pointer = fbuilder_->builder_.CreateGEP(
        stack_pointer,
        ConstantInt::getSigned(Type::getInt64Ty(fbuilder_->context_),
                               -stack_items));
    fbuilder_->builder_.CreateStore(new_stack_pointer,
                                    fbuilder_->stack_pointer_addr_);
    fbuilder_->PropagateExceptionOnNull(result);
    fbuilder_->Push(result);

    // Check signals and maybe switch threads after each function call.
    fbuilder_->CheckPyTicker();
}

void
OpcodeCall::CALL_FUNCTION_VAR(int oparg)
{
#ifdef WITH_TSC
    fbuilder_->LogTscEvent(CALL_START_LLVM);
#endif
    this->CallVarKwFunction(oparg, CALL_FLAG_VAR);
}

void
OpcodeCall::CALL_FUNCTION_KW(int oparg)
{
#ifdef WITH_TSC
    fbuilder_->LogTscEvent(CALL_START_LLVM);
#endif
    this->CallVarKwFunction(oparg, CALL_FLAG_KW);
}

void
OpcodeCall::CALL_FUNCTION_VAR_KW(int oparg)
{
#ifdef WITH_TSC
    fbuilder_->LogTscEvent(CALL_START_LLVM);
#endif
    this->CallVarKwFunction(oparg, CALL_FLAG_KW | CALL_FLAG_VAR);
}

#undef CALL_FLAG_VAR
#undef CALL_FLAG_KW

}
