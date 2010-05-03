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
    fbuilder_(fbuilder)
{
}

void
OpcodeContainer::BuildSequenceLiteral(
    int size, const char *createname,
    Value *(LlvmFunctionBuilder::*getitemslot)(Value*, int))
{
    const Type *IntSsizeTy =
        PyTypeBuilder<Py_ssize_t>::get(fbuilder_->context_);
    Value *seqsize = ConstantInt::getSigned(IntSsizeTy, size);

    Function *create =
        fbuilder_->GetGlobalFunction<PyObject *(Py_ssize_t)>(createname);
    Value *seq = fbuilder_->CreateCall(create, seqsize, "sequence_literal");
    fbuilder_->PropagateExceptionOnNull(seq);

    // XXX(twouters): do this with a memcpy?
    while (--size >= 0) {
        Value *itemslot = (fbuilder_->*getitemslot)(seq, size);
        fbuilder_->builder_.CreateStore(fbuilder_->Pop(), itemslot);
    }
    fbuilder_->Push(seq);
}

void
OpcodeContainer::BUILD_LIST(int size)
{
   this->BuildSequenceLiteral(size, "PyList_New",
                              &LlvmFunctionBuilder::GetListItemSlot);
}

void
OpcodeContainer::BUILD_TUPLE(int size)
{
   this->BuildSequenceLiteral(size, "PyTuple_New",
                              &LlvmFunctionBuilder::GetTupleItemSlot);
}

void
OpcodeContainer::BUILD_MAP(int size)
{
    Value *sizehint = ConstantInt::getSigned(
        PyTypeBuilder<Py_ssize_t>::get(fbuilder_->context_), size);
    Function *create_dict = fbuilder_->GetGlobalFunction<
        PyObject *(Py_ssize_t)>("_PyDict_NewPresized");
    Value *result = fbuilder_->CreateCall(create_dict, sizehint,
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
    Function *unpack_iterable = fbuilder_->GetGlobalFunction<
        int(PyObject *, int, PyObject **)>("_PyLlvm_FastUnpackIterable");
    Value *new_stack_pointer = fbuilder_->builder_.CreateGEP(
        fbuilder_->builder_.CreateLoad(fbuilder_->stack_pointer_addr_),
        ConstantInt::getSigned(
            PyTypeBuilder<Py_ssize_t>::get(fbuilder_->context_), size));
    fbuilder_->llvm_data_->tbaa_stack.MarkInstruction(new_stack_pointer);

    Value *result = fbuilder_->CreateCall(
        unpack_iterable, iterable,
        ConstantInt::get(
            PyTypeBuilder<int>::get(fbuilder_->context_), size, true),
        // _PyLlvm_FastUnpackIterable really takes the *new* stack pointer as
        // an argument, because it builds the result stack in reverse.
        new_stack_pointer);
    fbuilder_->DecRef(iterable);
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
    BasicBlock *success = fbuilder_->CreateBasicBlock("STORE_SUBSCR_success");
    BasicBlock *bailpoint = fbuilder_->CreateBasicBlock("STORE_SUBSCR_bail");

    Value *key = fbuilder_->Pop();
    Value *obj = fbuilder_->Pop();
    Value *value = fbuilder_->Pop();
    Function *setitem =
        fbuilder_->GetGlobalFunction<INT_OBJ_OBJ_OBJ>(
            "_PyLlvm_StoreSubscr_List");

    Value *result = fbuilder_->CreateCall(setitem, obj, key, value,
                                     "STORE_SUBSCR_result");
    fbuilder_->builder_.CreateCondBr(fbuilder_->IsNonZero(result),
                                     bailpoint, success);

    fbuilder_->builder_.SetInsertPoint(bailpoint);
    fbuilder_->Push(value);
    fbuilder_->Push(obj);
    fbuilder_->Push(key);
    fbuilder_->CreateGuardBailPoint(_PYGUARD_STORE_SUBSCR);

    fbuilder_->builder_.SetInsertPoint(success);
    fbuilder_->DecRef(value);
    fbuilder_->DecRef(obj);
    fbuilder_->DecRef(key);
}

void
OpcodeContainer::STORE_SUBSCR_safe()
{
    // Performing obj[key] = val
    Value *key = fbuilder_->Pop();
    Value *obj = fbuilder_->Pop();
    Value *value = fbuilder_->Pop();
    Function *setitem =
        fbuilder_->GetGlobalFunction<INT_OBJ_OBJ_OBJ>("PyObject_SetItem");
    Value *result = fbuilder_->CreateCall(setitem, obj, key, value,
                                          "STORE_SUBSCR_result");
    fbuilder_->DecRef(value);
    fbuilder_->DecRef(obj);
    fbuilder_->DecRef(key);
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
    Function *delitem = fbuilder_->GetGlobalFunction<
          int(PyObject *, PyObject *)>("PyObject_DelItem");
    Value *result = fbuilder_->CreateCall(delitem, obj, key,
                                          "DELETE_SUBSCR_result");
    fbuilder_->DecRef(obj);
    fbuilder_->DecRef(key);
    fbuilder_->PropagateExceptionOnNonZero(result);
}

void
OpcodeContainer::LIST_APPEND()
{
    Value *item = fbuilder_->Pop();
    Value *listobj = fbuilder_->Pop();
    Function *list_append = fbuilder_->GetGlobalFunction<
        int(PyObject *, PyObject *)>("PyList_Append");
    Value *result = fbuilder_->CreateCall(list_append, listobj, item,
                                          "LIST_APPEND_result");
    fbuilder_->DecRef(listobj);
    fbuilder_->DecRef(item);
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
        dict_type, fbuilder_->GetGlobalVariableFor((PyObject*)&PyDict_Type));
    fbuilder_->Assert(is_exact_dict,
                      "dict argument to STORE_MAP is not exactly a PyDict");
    Function *setitem = fbuilder_->GetGlobalFunction<
        int(PyObject *, PyObject *, PyObject *)>("PyDict_SetItem");
    Value *result = fbuilder_->CreateCall(setitem, dict, key, value,
                                          "STORE_MAP_result");
    fbuilder_->DecRef(value);
    fbuilder_->DecRef(key);
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

    Value *module = fbuilder_->CreateCall(
        fbuilder_->GetGlobalFunction<FUNC_TYPE>("_PyEval_ImportName"),
        level, names, mod_name);
    fbuilder_->DecRef(level);
    fbuilder_->DecRef(names);
    fbuilder_->DecRef(mod_name);
    fbuilder_->PropagateExceptionOnNull(module);
    fbuilder_->Push(module);
}

#undef FUNC_TYPE

bool
OpcodeContainer::IMPORT_NAME_fast()
{
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
    if (fbuilder_->code_object_->co_watching[WATCHING_SYS_MODULES] == NULL) {
        PyObject *sys_modules = PyImport_GetModuleDict();
        if (sys_modules == NULL) {
            return false;
        }

        if (_PyCode_WatchDict(fbuilder_->code_object_,
                              WATCHING_SYS_MODULES,
                              sys_modules)) {
            PyErr_Clear();
            return false;
        }
        fbuilder_->uses_watched_dicts_.set(WATCHING_BUILTINS);
        fbuilder_->uses_watched_dicts_.set(WATCHING_SYS_MODULES);
    }
    // We start watching builtins when we begin compilation to LLVM IR (aka,
    // we're always watching it by this point).
    assert(fbuilder_->code_object_->co_watching[WATCHING_BUILTINS]);
    assert(fbuilder_->code_object_->co_watching[WATCHING_SYS_MODULES]);

    BasicBlock *keep_going =
        fbuilder_->CreateBasicBlock("IMPORT_NAME_keep_going");
    BasicBlock *invalid_assumptions =
        fbuilder_->CreateBasicBlock("IMPORT_NAME_invalid_assumptions");

    Value *use_jit = fbuilder_->builder_.CreateLoad(fbuilder_->use_jit_addr_);
    fbuilder_->builder_.CreateCondBr(fbuilder_->IsNonZero(use_jit),
                                     keep_going,
                                     invalid_assumptions);

    /* Our assumptions about the state of sys.modules no longer hold;
       bail back to the interpreter. */
    fbuilder_->builder_.SetInsertPoint(invalid_assumptions);
    fbuilder_->CreateBailPoint(_PYFRAME_FATAL_GUARD_FAIL);

    fbuilder_->builder_.SetInsertPoint(keep_going);
    /* TODO(collinwinter): we pop to get rid of the inputs to IMPORT_NAME.
       Find a way to omit this work. */
    fbuilder_->DecRef(fbuilder_->Pop());
    fbuilder_->DecRef(fbuilder_->Pop());
    fbuilder_->DecRef(fbuilder_->Pop());

    Value *mod = fbuilder_->GetGlobalVariableFor(module);
    fbuilder_->IncRef(mod);
    fbuilder_->Push(mod);

    IMPORT_NAME_INC_STATS(optimized);
    return true;
}

}
