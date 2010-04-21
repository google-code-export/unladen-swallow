#include "Python.h"

#include "JIT/opcodes/globals.h"
#include "JIT/llvm_fbuilder.h"

#include "llvm/BasicBlock.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"

using llvm::BasicBlock;
using llvm::Function;
using llvm::Value;

namespace py {

OpcodeGlobals::OpcodeGlobals(LlvmFunctionBuilder *fbuilder) :
    fbuilder_(fbuilder)
{
}

void OpcodeGlobals::LOAD_GLOBAL(int index)
{
    // A code object might not have co_watching set if
    // a) it was compiled by setting co_optimization, or
    // b) we couldn't watch the globals/builtins dicts.
    PyObject **watching = fbuilder_->code_object_->co_watching;
    if (watching && watching[WATCHING_GLOBALS] && watching[WATCHING_BUILTINS])
        this->LOAD_GLOBAL_fast(index);
    else
        this->LOAD_GLOBAL_safe(index);
}

void OpcodeGlobals::LOAD_GLOBAL_fast(int index)
{
    PyCodeObject *code = fbuilder_->code_object_;
    assert(code->co_watching != NULL);
    assert(code->co_watching[WATCHING_GLOBALS]);
    assert(code->co_watching[WATCHING_BUILTINS]);

    PyObject *name = PyTuple_GET_ITEM(code->co_names, index);
    PyObject *obj = PyDict_GetItem(code->co_watching[WATCHING_GLOBALS], name);
    if (obj == NULL) {
        obj = PyDict_GetItem(code->co_watching[WATCHING_BUILTINS], name);
        if (obj == NULL) {
            /* This isn't necessarily an error: it's legal Python
               code to refer to globals that aren't yet defined at
               compilation time. Is it a bad idea? Almost
               certainly. Is it legal? Unfortunatley. */
            this->LOAD_GLOBAL_safe(index);
            return;
        }
    }
    fbuilder_->uses_watched_dicts_.set(WATCHING_GLOBALS);
    fbuilder_->uses_watched_dicts_.set(WATCHING_BUILTINS);

    BasicBlock *keep_going =
        fbuilder_->CreateBasicBlock("LOAD_GLOBAL_keep_going");
    BasicBlock *invalid_assumptions =
        fbuilder_->CreateBasicBlock("LOAD_GLOBAL_invalid_assumptions");

#ifdef WITH_TSC
    fbuilder_->LogTscEvent(LOAD_GLOBAL_ENTER_LLVM);
#endif
    Value *use_jit = fbuilder_->builder_.CreateLoad(fbuilder_->use_jit_addr_,
                                               "co_use_jit");
    fbuilder_->builder_.CreateCondBr(fbuilder_->IsNonZero(use_jit),
                                keep_going,
                                invalid_assumptions);

    /* Our assumptions about the state of the globals/builtins no longer hold;
       bail back to the interpreter. */
    fbuilder_->builder_.SetInsertPoint(invalid_assumptions);
    fbuilder_->CreateBailPoint(_PYFRAME_FATAL_GUARD_FAIL);

    /* Our assumptions are still valid; encode the result of the lookups as an
       immediate in the IR. */
    fbuilder_->builder_.SetInsertPoint(keep_going);
    Value *global = fbuilder_->EmbedPointer<PyObject*>(obj);
    fbuilder_->IncRef(global);
    fbuilder_->Push(global);

#ifdef WITH_TSC
    fbuilder_->LogTscEvent(LOAD_GLOBAL_EXIT_LLVM);
#endif
}

void OpcodeGlobals::LOAD_GLOBAL_safe(int index)
{
    BasicBlock *global_missing =
            fbuilder_->CreateBasicBlock("LOAD_GLOBAL_global_missing");
    BasicBlock *global_success =
            fbuilder_->CreateBasicBlock("LOAD_GLOBAL_global_success");
    BasicBlock *builtin_missing =
            fbuilder_->CreateBasicBlock("LOAD_GLOBAL_builtin_missing");
    BasicBlock *builtin_success =
            fbuilder_->CreateBasicBlock("LOAD_GLOBAL_builtin_success");
    BasicBlock *done = fbuilder_->CreateBasicBlock("LOAD_GLOBAL_done");
#ifdef WITH_TSC
    fbuilder_->LogTscEvent(LOAD_GLOBAL_ENTER_LLVM);
#endif
    Value *name = fbuilder_->LookupName(index);
    Function *pydict_getitem = fbuilder_->GetGlobalFunction<
        PyObject *(PyObject *, PyObject *)>("PyDict_GetItem");
    Value *global = fbuilder_->CreateCall(
        pydict_getitem, fbuilder_->globals_, name, "global_variable");
    fbuilder_->builder_.CreateCondBr(fbuilder_->IsNull(global),
                                global_missing, global_success);

    fbuilder_->builder_.SetInsertPoint(global_success);
    fbuilder_->IncRef(global);
    fbuilder_->Push(global);
    fbuilder_->builder_.CreateBr(done);

    fbuilder_->builder_.SetInsertPoint(global_missing);
    // This ignores any exception set by PyDict_GetItem (and similarly
    // for the builtins dict below,) but this is what ceval does too.
    Value *builtin = fbuilder_->CreateCall(
        pydict_getitem, fbuilder_->builtins_, name, "builtin_variable");
    fbuilder_->builder_.CreateCondBr(fbuilder_->IsNull(builtin),
                                builtin_missing, builtin_success);

    fbuilder_->builder_.SetInsertPoint(builtin_missing);
    Function *do_raise = fbuilder_->GetGlobalFunction<
        void(PyObject *)>("_PyEval_RaiseForGlobalNameError");
    fbuilder_->CreateCall(do_raise, name);
    fbuilder_->PropagateException();

    fbuilder_->builder_.SetInsertPoint(builtin_success);
    fbuilder_->IncRef(builtin);
    fbuilder_->Push(builtin);
    fbuilder_->builder_.CreateBr(done);

    fbuilder_->builder_.SetInsertPoint(done);
#ifdef WITH_TSC
    fbuilder_->LogTscEvent(LOAD_GLOBAL_EXIT_LLVM);
#endif
}

void OpcodeGlobals::STORE_GLOBAL(int index)
{
    Value *name = fbuilder_->LookupName(index);
    Value *value = fbuilder_->Pop();
    Function *pydict_setitem = fbuilder_->GetGlobalFunction<
        int(PyObject *, PyObject *, PyObject *)>("PyDict_SetItem");
    Value *result = fbuilder_->CreateCall(
        pydict_setitem, fbuilder_->globals_, name, value,
        "STORE_GLOBAL_result");
    fbuilder_->DecRef(value);
    fbuilder_->PropagateExceptionOnNonZero(result);
}

void OpcodeGlobals::DELETE_GLOBAL(int index)
{
    BasicBlock *failure = fbuilder_->CreateBasicBlock("DELETE_GLOBAL_failure");
    BasicBlock *success = fbuilder_->CreateBasicBlock("DELETE_GLOBAL_success");
    Value *name = fbuilder_->LookupName(index);
    Function *pydict_setitem = fbuilder_->GetGlobalFunction<
        int(PyObject *, PyObject *)>("PyDict_DelItem");
    Value *result = fbuilder_->CreateCall(
        pydict_setitem, fbuilder_->globals_, name, "STORE_GLOBAL_result");
    fbuilder_->builder_.CreateCondBr(fbuilder_->IsNonZero(result),
                                     failure, success);

    fbuilder_->builder_.SetInsertPoint(failure);
    Function *do_raise = fbuilder_->GetGlobalFunction<
        void(PyObject *)>("_PyEval_RaiseForGlobalNameError");
    fbuilder_->CreateCall(do_raise, name);
    fbuilder_->PropagateException();

    fbuilder_->builder_.SetInsertPoint(success);
}

}
