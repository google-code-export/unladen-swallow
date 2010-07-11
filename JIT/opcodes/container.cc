#include "Python.h"

#include "JIT/opcodes/binops.h"
#include "JIT/opcodes/container.h"
#include "JIT/llvm_fbuilder.h"

#include "llvm/BasicBlock.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/raw_ostream.h"

using llvm::BasicBlock;
using llvm::ConstantInt;
using llvm::Function;
using llvm::Type;
using llvm::Value;
using llvm::errs;

#ifdef Py_WITH_INSTRUMENTATION
class ImportNameStats {
public:
    ImportNameStats()
        : total(0), optimized(0) {
    }

    ~ImportNameStats() {
        errs() << "\nIMPORT_NAME opcodes:\n";
        errs() << "Total: " << this->total << "\n";
        errs() << "Optimized: " << this->optimized << "\n";
    }

    // Total number of IMPORT_NAME opcodes compiled.
    unsigned total;
    // Number of imports successfully optimized.
    unsigned optimized;
};

static llvm::ManagedStatic<ImportNameStats> import_name_stats;

#define IMPORT_NAME_INC_STATS(field) import_name_stats->field++
#else
#define IMPORT_NAME_INC_STATS(field)
#endif  /* Py_WITH_INSTRUMENTATION */

namespace py {

OpcodeContainer::OpcodeContainer(LlvmFunctionBuilder *fbuilder) :
    fbuilder_(fbuilder), state_(fbuilder->state())
{
}

void
OpcodeContainer::BuildSequenceLiteral(
    int size, const char *createname,
    Value *(LlvmFunctionState::*getitemslot)(Value*, int))
{
    const Type *IntSsizeTy =
        PyTypeBuilder<Py_ssize_t>::get(fbuilder_->context_);
    Value *seqsize = ConstantInt::getSigned(IntSsizeTy, size);

    Function *create =
        state_->GetGlobalFunction<PyObject *(Py_ssize_t)>(createname);
    Value *seq = state_->CreateCall(create, seqsize, "sequence_literal");
    fbuilder_->PropagateExceptionOnNull(seq);

    // XXX(twouters): do this with a memcpy?
    while (--size >= 0) {
        Value *itemslot = (state_->*getitemslot)(seq, size);
        fbuilder_->builder_.CreateStore(fbuilder_->Pop(), itemslot);
    }
    fbuilder_->Push(seq);
}

void
OpcodeContainer::BUILD_LIST(int size)
{
   this->BuildSequenceLiteral(size, "PyList_New",
                              &LlvmFunctionState::GetListItemSlot);
}

void
OpcodeContainer::BUILD_TUPLE(int size)
{
   this->BuildSequenceLiteral(size, "PyTuple_New",
                              &LlvmFunctionState::GetTupleItemSlot);
}

void
OpcodeContainer::BUILD_MAP(int size)
{
    Value *sizehint = ConstantInt::getSigned(
        PyTypeBuilder<Py_ssize_t>::get(fbuilder_->context_), size);
    Function *create_dict = state_->GetGlobalFunction<
        PyObject *(Py_ssize_t)>("_PyDict_NewPresized");
    Value *result = state_->CreateCall(create_dict, sizehint,
                                       "BULD_MAP_result");
    fbuilder_->PropagateExceptionOnNull(result);
    fbuilder_->Push(result);
}

void
OpcodeContainer::UNPACK_SEQUENCE(int size)
{
    // TODO(twouters): We could do even better by combining this opcode and the
    // STORE_* ones that follow into a single block of code circumventing the
    // stack altogether. And omitting the horrible external stack munging that
    // UnpackIterable does.
    Value *iterable = fbuilder_->Pop();
    Function *unpack_iterable = state_->GetGlobalFunction<
        int(PyObject *, int, PyObject **)>("_PyLlvm_FastUnpackIterable");
    Value *new_stack_pointer = fbuilder_->builder_.CreateGEP(
        fbuilder_->builder_.CreateLoad(fbuilder_->stack_pointer_addr_),
        ConstantInt::getSigned(
            PyTypeBuilder<Py_ssize_t>::get(fbuilder_->context_), size));
    fbuilder_->llvm_data_->tbaa_stack.MarkInstruction(new_stack_pointer);

    Value *result = state_->CreateCall(
        unpack_iterable, iterable,
        ConstantInt::get(
            PyTypeBuilder<int>::get(fbuilder_->context_), size, true),
        // _PyLlvm_FastUnpackIterable really takes the *new* stack pointer as
        // an argument, because it builds the result stack in reverse.
        new_stack_pointer);
    state_->DecRef(iterable);
    fbuilder_->PropagateExceptionOnNonZero(result);
    // Not setting the new stackpointer on failure does mean that if
    // _PyLlvm_FastUnpackIterable failed after pushing some values onto the
    // stack, and it didn't clean up after itself, we lose references.  This
    // is what eval.cc does as well.
    fbuilder_->builder_.CreateStore(new_stack_pointer,
                                    fbuilder_->stack_pointer_addr_);
}

#define INT_OBJ_OBJ_OBJ int(PyObject*, PyObject*, PyObject*)

void
OpcodeContainer::STORE_SUBSCR_list_int()
{
    BasicBlock *success = state_->CreateBasicBlock("STORE_SUBSCR_success");
    BasicBlock *bailpoint = state_->CreateBasicBlock("STORE_SUBSCR_bail");

    Value *key = fbuilder_->Pop();
    Value *obj = fbuilder_->Pop();
    Value *value = fbuilder_->Pop();
    Function *setitem =
        state_->GetGlobalFunction<INT_OBJ_OBJ_OBJ>(
            "_PyLlvm_StoreSubscr_List");

    Value *result = state_->CreateCall(setitem, obj, key, value,
                                       "STORE_SUBSCR_result");
    fbuilder_->builder_.CreateCondBr(state_->IsNonZero(result),
                                     bailpoint, success);

    fbuilder_->builder_.SetInsertPoint(bailpoint);
    fbuilder_->Push(value);
    fbuilder_->Push(obj);
    fbuilder_->Push(key);
    fbuilder_->CreateGuardBailPoint(_PYGUARD_STORE_SUBSCR);

    fbuilder_->builder_.SetInsertPoint(success);
    state_->DecRef(value);
    state_->DecRef(obj);
    state_->DecRef(key);
}

void
OpcodeContainer::STORE_SUBSCR_safe()
{
    // Performing obj[key] = val
    Value *key = fbuilder_->Pop();
    Value *obj = fbuilder_->Pop();
    Value *value = fbuilder_->Pop();
    Function *setitem =
        state_->GetGlobalFunction<INT_OBJ_OBJ_OBJ>("PyObject_SetItem");
    Value *result = state_->CreateCall(setitem, obj, key, value,
                                       "STORE_SUBSCR_result");
    state_->DecRef(value);
    state_->DecRef(obj);
    state_->DecRef(key);
    fbuilder_->PropagateExceptionOnNonZero(result);
}

#undef INT_OBJ_OBJ_OBJ

void
OpcodeContainer::STORE_SUBSCR()
{
    OpcodeBinops::IncStatsTotal();

    const PyTypeObject *lhs_type = fbuilder_->GetTypeFeedback(0);
    const PyTypeObject *rhs_type = fbuilder_->GetTypeFeedback(1);

    if (lhs_type == &PyList_Type && rhs_type == &PyInt_Type) {
        OpcodeBinops::IncStatsOptimized();
        this->STORE_SUBSCR_list_int();
        return;
    }
    else {
        OpcodeBinops::IncStatsOmitted();
        this->STORE_SUBSCR_safe();
        return;
    }
}

void
OpcodeContainer::DELETE_SUBSCR()
{
    Value *key = fbuilder_->Pop();
    Value *obj = fbuilder_->Pop();
    Function *delitem = state_->GetGlobalFunction<
          int(PyObject *, PyObject *)>("PyObject_DelItem");
    Value *result = state_->CreateCall(delitem, obj, key,
                                       "DELETE_SUBSCR_result");
    state_->DecRef(obj);
    state_->DecRef(key);
    fbuilder_->PropagateExceptionOnNonZero(result);
}

void
OpcodeContainer::LIST_APPEND()
{
    Value *item = fbuilder_->Pop();
    Value *listobj = fbuilder_->Pop();
    Function *list_append = state_->GetGlobalFunction<
        int(PyObject *, PyObject *)>("PyList_Append");
    Value *result = state_->CreateCall(list_append, listobj, item,
                                       "LIST_APPEND_result");
    state_->DecRef(listobj);
    state_->DecRef(item);
    fbuilder_->PropagateExceptionOnNonZero(result);
}

void
OpcodeContainer::STORE_MAP()
{
    Value *key = fbuilder_->Pop();
    Value *value = fbuilder_->Pop();
    Value *dict = fbuilder_->Pop();
    fbuilder_->Push(dict);
    Value *dict_type = fbuilder_->builder_.CreateLoad(
        ObjectTy::ob_type(fbuilder_->builder_, dict));
    Value *is_exact_dict = fbuilder_->builder_.CreateICmpEQ(
        dict_type, state_->GetGlobalVariableFor((PyObject*)&PyDict_Type));
    state_->Assert(is_exact_dict,
                   "dict argument to STORE_MAP is not exactly a PyDict");
    Function *setitem = state_->GetGlobalFunction<
        int(PyObject *, PyObject *, PyObject *)>("PyDict_SetItem");
    Value *result = state_->CreateCall(setitem, dict, key, value,
                                       "STORE_MAP_result");
    state_->DecRef(value);
    state_->DecRef(key);
    fbuilder_->PropagateExceptionOnNonZero(result);
}

#define FUNC_TYPE PyObject *(PyObject *, PyObject *, PyObject *)

void
OpcodeContainer::IMPORT_NAME()
{
    IMPORT_NAME_INC_STATS(total);

    if (this->IMPORT_NAME_fast())
        return;

    Value *mod_name = fbuilder_->Pop();
    Value *names = fbuilder_->Pop();
    Value *level = fbuilder_->Pop();

    Value *module = state_->CreateCall(
        state_->GetGlobalFunction<FUNC_TYPE>("_PyEval_ImportName"),
        level, names, mod_name);
    state_->DecRef(level);
    state_->DecRef(names);
    state_->DecRef(mod_name);
    fbuilder_->PropagateExceptionOnNull(module);
    fbuilder_->Push(module);
}

#undef FUNC_TYPE

bool
OpcodeContainer::IMPORT_NAME_fast()
{
    PyCodeObject *code = fbuilder_->code_object_;

    // If we're not already monitoring the builtins dict, monitor it.  Normally
    // we pick it up from the eval loop, but if it isn't here, then we make a
    // guess.  If we are wrong, we will bail.
    if (code->co_watching == NULL ||
        code->co_watching[WATCHING_BUILTINS] == NULL) {
        PyObject *builtins = PyThreadState_GET()->interp->builtins;
        _PyCode_WatchDict(code, WATCHING_BUILTINS, builtins);
    }

    const PyRuntimeFeedback *feedback = fbuilder_->GetFeedback();
    if (feedback == NULL || feedback->ObjectsOverflowed()) {
        return false;
    }

    llvm::SmallVector<PyObject *, 3> objects;
    feedback->GetSeenObjectsInto(objects);
    if (objects.size() != 1 || !PyModule_Check(objects[0])) {
        return false;
    }
    PyObject *module = objects[0];

    // We need to invalidate this function if someone changes sys.modules.
    if (code->co_watching[WATCHING_SYS_MODULES] == NULL) {
        PyObject *sys_modules = PyImport_GetModuleDict();
        if (sys_modules == NULL) {
            return false;
        }

        if (_PyCode_WatchDict(code,
                              WATCHING_SYS_MODULES,
                              sys_modules)) {
            PyErr_Clear();
            return false;
        }

        fbuilder_->uses_watched_dicts_.set(WATCHING_BUILTINS);
        fbuilder_->uses_watched_dicts_.set(WATCHING_SYS_MODULES);
    }

    BasicBlock *keep_going =
        state_->CreateBasicBlock("IMPORT_NAME_keep_going");
    BasicBlock *invalid_assumptions =
        state_->CreateBasicBlock("IMPORT_NAME_invalid_assumptions");

    Value *use_jit = fbuilder_->builder_.CreateLoad(fbuilder_->use_jit_addr_);
    fbuilder_->builder_.CreateCondBr(state_->IsNonZero(use_jit),
                                     keep_going,
                                     invalid_assumptions);

    /* Our assumptions about the state of sys.modules no longer hold;
       bail back to the interpreter. */
    fbuilder_->builder_.SetInsertPoint(invalid_assumptions);
    fbuilder_->CreateBailPoint(_PYFRAME_FATAL_GUARD_FAIL);

    fbuilder_->builder_.SetInsertPoint(keep_going);
    /* TODO(collinwinter): we pop to get rid of the inputs to IMPORT_NAME.
       Find a way to omit this work. */
    state_->DecRef(fbuilder_->Pop());
    state_->DecRef(fbuilder_->Pop());
    state_->DecRef(fbuilder_->Pop());

    Value *mod = state_->GetGlobalVariableFor(module);
    state_->IncRef(mod);
    fbuilder_->Push(mod);

    IMPORT_NAME_INC_STATS(optimized);
    return true;
}

}
