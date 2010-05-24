#include "Python.h"

#include "JIT/opcodes/attributes.h"
#include "JIT/llvm_fbuilder.h"

#include "JIT/ConstantMirror.h"
#include "JIT/global_llvm_data.h"
#include "JIT/PyTypeBuilder.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BasicBlock.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/Value.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/raw_ostream.h"

using llvm::BasicBlock;
using llvm::ConstantInt;
using llvm::Function;
using llvm::Value;
using llvm::array_endof;
using llvm::errs;

#ifdef Py_WITH_INSTRUMENTATION

class AccessAttrStats {
public:
    AccessAttrStats()
        : loads(0), stores(0), optimized_loads(0), optimized_stores(0),
          no_opt_no_data(0), no_opt_no_mcache(0), no_opt_overrode_access(0),
          no_opt_polymorphic(0), no_opt_nonstring_name(0) {
    }

    ~AccessAttrStats() {
        errs() << "\nLOAD/STORE_ATTR optimization:\n";
        errs() << "Total opcodes: " << (this->loads + this->stores) << "\n";
        errs() << "Optimized opcodes: "
               << (this->optimized_loads + this->optimized_stores) << "\n";
        errs() << "LOAD_ATTR opcodes: " << this->loads << "\n";
        errs() << "Optimized LOAD_ATTR opcodes: "
               << this->optimized_loads << "\n";
        errs() << "STORE_ATTR opcodes: " << this->stores << "\n";
        errs() << "Optimized STORE_ATTR opcodes: "
               << this->optimized_stores << "\n";
        errs() << "No opt: no data: " << this->no_opt_no_data << "\n";
        errs() << "No opt: no mcache support: "
               << this->no_opt_no_mcache << "\n";
        errs() << "No opt: overrode getattr: "
               << this->no_opt_overrode_access << "\n";
        errs() << "No opt: polymorphic: " << this->no_opt_polymorphic << "\n";
        errs() << "No opt: non-string name: "
               << this->no_opt_nonstring_name << "\n";
    }

    // Total number of LOAD_ATTR opcodes compiled.
    unsigned loads;
    // Total number of STORE_ATTR opcodes compiled.
    unsigned stores;
    // Number of loads we optimized.
    unsigned optimized_loads;
    // Number of stores we optimized.
    unsigned optimized_stores;
    // Number of opcodes we were unable to optimize due to missing data.
    unsigned no_opt_no_data;
    // Number of opcodes we were unable to optimize because the type didn't
    // support the method cache.
    unsigned no_opt_no_mcache;
    // Number of opcodes we were unable to optimize because the type overrode
    // tp_getattro.
    unsigned no_opt_overrode_access;
    // Number of opcodes we were unable to optimize due to polymorphism.
    unsigned no_opt_polymorphic;
    // Number of opcodes we were unable to optimize because the attribute name
    // was not a string.
    unsigned no_opt_nonstring_name;
};

static llvm::ManagedStatic<AccessAttrStats> access_attr_stats;

#define ACCESS_ATTR_INC_STATS(field) access_attr_stats->field++
#else
#define ACCESS_ATTR_INC_STATS(field)
#endif /* Py_WITH_INSTRUMENTATION */

namespace py {

OpcodeAttributes::OpcodeAttributes(LlvmFunctionBuilder *fbuilder) :
    fbuilder_(fbuilder), state_(fbuilder->state())
{
}

void
OpcodeAttributes::LOAD_ATTR(int names_index)
{
    ACCESS_ATTR_INC_STATS(loads);
    if (!this->LOAD_ATTR_fast(names_index)) {
        this->LOAD_ATTR_safe(names_index);
    }
}

void
OpcodeAttributes::LOAD_ATTR_safe(int names_index)
{
    Value *attr = fbuilder_->LookupName(names_index);
    Value *obj = fbuilder_->Pop();
    Function *pyobj_getattr = state_->GetGlobalFunction<
        PyObject *(PyObject *, PyObject *)>("PyObject_GetAttr");
    Value *result = state_->CreateCall(
        pyobj_getattr, obj, attr, "LOAD_ATTR_result");
    state_->DecRef(obj);
    fbuilder_->PropagateExceptionOnNull(result);
    fbuilder_->Push(result);
}

bool
OpcodeAttributes::LOAD_ATTR_fast(int names_index)
{
    PyObject *name =
        PyTuple_GET_ITEM(fbuilder_->code_object_->co_names, names_index);
    AttributeAccessor accessor(fbuilder_, name, ATTR_ACCESS_LOAD);

    // Check that we can optimize this load.
    if (!accessor.CanOptimizeAttrAccess()) {
        return false;
    }
    ACCESS_ATTR_INC_STATS(optimized_loads);

    // Emit the appropriate guards.
    Value *obj_v = fbuilder_->Pop();
    BasicBlock *do_load = state_->CreateBasicBlock("LOAD_ATTR_do_load");
    accessor.GuardAttributeAccess(obj_v, do_load);

    // Call the inline function that deals with the lookup.  LLVM propagates
    // these constant arguments through the body of the function.
    fbuilder_->builder_.SetInsertPoint(do_load);
    PyConstantMirror &mirror = fbuilder_->llvm_data_->constant_mirror();
    Value *descr_get_v = mirror.GetGlobalForFunctionPointer<descrgetfunc>(
            (void*)accessor.descr_get_, "");
    Value *getattr_func = state_->GetGlobalFunction<
        PyObject *(PyObject *obj, PyTypeObject *type, PyObject *name,
                   long dictoffset, PyObject *descr, descrgetfunc descr_get,
                   char is_data_descr)>("_PyLlvm_Object_GenericGetAttr");
    Value *args[] = {
        obj_v,
        accessor.guard_type_v_,
        accessor.name_v_,
        accessor.dictoffset_v_,
        accessor.descr_v_,
        descr_get_v,
        accessor.is_data_descr_v_
    };
    Value *result = state_->CreateCall(getattr_func,
                                       args, array_endof(args));

    // Put the result on the stack and possibly propagate an exception.
    state_->DecRef(obj_v);
    fbuilder_->PropagateExceptionOnNull(result);
    fbuilder_->Push(result);
    return true;
}

void
OpcodeAttributes::STORE_ATTR(int names_index)
{
    ACCESS_ATTR_INC_STATS(stores);
    if (!this->STORE_ATTR_fast(names_index)) {
        this->STORE_ATTR_safe(names_index);
    }
}

void
OpcodeAttributes::STORE_ATTR_safe(int names_index)
{
    Value *attr = fbuilder_->LookupName(names_index);
    Value *obj = fbuilder_->Pop();
    Value *value = fbuilder_->Pop();
    Function *pyobj_setattr = state_->GetGlobalFunction<
        int(PyObject *, PyObject *, PyObject *)>("PyObject_SetAttr");
    Value *result = state_->CreateCall(
        pyobj_setattr, obj, attr, value, "STORE_ATTR_result");
    state_->DecRef(obj);
    state_->DecRef(value);
    fbuilder_->PropagateExceptionOnNonZero(result);
}

bool
OpcodeAttributes::STORE_ATTR_fast(int names_index)
{
    PyObject *name =
        PyTuple_GET_ITEM(fbuilder_->code_object_->co_names, names_index);
    AttributeAccessor accessor(fbuilder_, name, ATTR_ACCESS_STORE);

    // Check that we can optimize this store.
    if (!accessor.CanOptimizeAttrAccess()) {
        return false;
    }
    ACCESS_ATTR_INC_STATS(optimized_stores);

    // Emit appropriate guards.
    Value *obj_v = fbuilder_->Pop();
    BasicBlock *do_store = state_->CreateBasicBlock("STORE_ATTR_do_store");
    accessor.GuardAttributeAccess(obj_v, do_store);

    // Call the inline function that deals with the lookup.  LLVM propagates
    // these constant arguments through the body of the function.
    fbuilder_->builder_.SetInsertPoint(do_store);
    Value *val_v = fbuilder_->Pop();
    PyConstantMirror &mirror = fbuilder_->llvm_data_->constant_mirror();
    Value *descr_set_v = mirror.GetGlobalForFunctionPointer<descrsetfunc>(
        (void*)accessor.descr_set_, "");
    Value *setattr_func = state_->GetGlobalFunction<
        int (PyObject *obj, PyObject *val, PyTypeObject *type, PyObject *name,
             long dictoffset, PyObject *descr, descrsetfunc descr_set,
             char is_data_descr)>("_PyLlvm_Object_GenericSetAttr");
    Value *args[] = {
        obj_v,
        val_v,
        accessor.guard_type_v_,
        accessor.name_v_,
        accessor.dictoffset_v_,
        accessor.descr_v_,
        descr_set_v,
        accessor.is_data_descr_v_
    };
    Value *result = state_->CreateCall(setattr_func, args,
                                       array_endof(args));

    state_->DecRef(obj_v);
    state_->DecRef(val_v);
    fbuilder_->PropagateExceptionOnNonZero(result);
    return true;
}

bool
AttributeAccessor::CanOptimizeAttrAccess()
{
    // Only optimize string attribute loads.  This leaves unicode hanging for
    // now, but most objects are still constructed with string objects.  If it
    // becomes a problem, our instrumentation will detect it.
    if (!PyString_Check(this->name_)) {
        ACCESS_ATTR_INC_STATS(no_opt_nonstring_name);
        return false;
    }

    // Only optimize monomorphic load sites with data.
    const PyRuntimeFeedback *feedback = this->fbuilder_->GetFeedback();
    if (feedback == NULL) {
        ACCESS_ATTR_INC_STATS(no_opt_no_data);
        return false;
    }

    if (feedback->ObjectsOverflowed()) {
        ACCESS_ATTR_INC_STATS(no_opt_polymorphic);
        return false;
    }
    llvm::SmallVector<PyObject*, 3> types_seen;
    feedback->GetSeenObjectsInto(types_seen);
    if (types_seen.size() != 1) {
        ACCESS_ATTR_INC_STATS(no_opt_polymorphic);
        return false;
    }

    // During the course of the compilation, we borrow a reference to the type
    // object from the feedback.  When compilation finishes, we listen for type
    // object modifications.  When a type object is freed, it notifies its
    // listeners, and the code object will be invalidated.  All other
    // references are borrowed from the type object, which cannot change
    // without invalidating the code.
    PyObject *type_obj = types_seen[0];
    assert(PyType_Check(type_obj));
    PyTypeObject *type = this->guard_type_ = (PyTypeObject*)type_obj;

    // The type must support the method cache so we can listen for
    // modifications to it.
    if (!PyType_HasFeature(type, Py_TPFLAGS_HAVE_VERSION_TAG)) {
        ACCESS_ATTR_INC_STATS(no_opt_no_mcache);
        return false;
    }

    // Don't optimize attribute lookup for types that override tp_getattro or
    // tp_getattr.
    bool overridden = true;
    if (this->access_kind_ == ATTR_ACCESS_LOAD) {
        overridden = (type->tp_getattro != &PyObject_GenericGetAttr);
    } else if (this->access_kind_ == ATTR_ACCESS_STORE) {
        overridden = (type->tp_setattro != &PyObject_GenericSetAttr);
    } else {
        assert(0 && "invalid enum!");
    }
    if (overridden) {
        ACCESS_ATTR_INC_STATS(no_opt_overrode_access);
        return false;
    }

    // Do the lookups on the type.
    this->CacheTypeLookup();

    // If we find a descriptor with a getter, make sure its type supports the
    // method cache, or we won't be able to receive updates.
    if (this->descr_get_ != NULL &&
        !PyType_HasFeature(this->guard_descr_type_,
                           Py_TPFLAGS_HAVE_VERSION_TAG)) {
        ACCESS_ATTR_INC_STATS(no_opt_no_mcache);
        return false;
    }

    // Now that we know for sure that we are going to optimize this lookup, add
    // the type to the list of types we need to listen for modifications from
    // and make the llvm::Values.
    this->fbuilder_->types_used_.insert(type);
    MakeLlvmValues();

    return true;
}

void
AttributeAccessor::CacheTypeLookup()
{
    this->dictoffset_ = this->guard_type_->tp_dictoffset;
    this->descr_ = _PyType_Lookup(this->guard_type_, this->name_);
    if (this->descr_ != NULL) {
        this->guard_descr_type_ = this->descr_->ob_type;
        if (PyType_HasFeature(this->guard_descr_type_, Py_TPFLAGS_HAVE_CLASS)) {
            this->descr_get_ = this->guard_descr_type_->tp_descr_get;
            this->descr_set_ = this->guard_descr_type_->tp_descr_set;
            if (this->descr_get_ != NULL && PyDescr_IsData(this->descr_)) {
                this->is_data_descr_ = true;
            }
        }
    }
}

void
AttributeAccessor::MakeLlvmValues()
{
    LlvmFunctionState *state = this->fbuilder_->state();
    this->guard_type_v_ =
        state->EmbedPointer<PyTypeObject*>(this->guard_type_);
    this->name_v_ = state->EmbedPointer<PyObject*>(this->name_);
    this->dictoffset_v_ =
        ConstantInt::get(PyTypeBuilder<long>::get(this->fbuilder_->context_),
                         this->dictoffset_);
    this->descr_v_ = state->EmbedPointer<PyObject*>(this->descr_);
    this->is_data_descr_v_ =
        ConstantInt::get(PyTypeBuilder<char>::get(this->fbuilder_->context_),
                         this->is_data_descr_);
}

void
AttributeAccessor::GuardAttributeAccess(
    Value *obj_v, BasicBlock *do_access)
{
    LlvmFunctionBuilder *fbuilder = this->fbuilder_;
    BuilderT &builder = this->fbuilder_->builder();
    LlvmFunctionState *state = this->fbuilder_->state();

    BasicBlock *bail_block = state->CreateBasicBlock("ATTR_bail_block");
    BasicBlock *guard_type = state->CreateBasicBlock("ATTR_check_valid");
    BasicBlock *guard_descr = state->CreateBasicBlock("ATTR_check_descr");

    // Make sure that the code object is still valid.  This may fail if the
    // code object is invalidated inside of a call to the code object.
    Value *use_jit = builder.CreateLoad(fbuilder->use_jit_addr_,
                                        "co_use_jit");
    builder.CreateCondBr(state->IsNonZero(use_jit), guard_type, bail_block);

    // Compare ob_type against type and bail if it's the wrong type.  Since
    // we've subscribed to the type object for modification updates, the code
    // will be invalidated before the type object is freed.  Therefore we don't
    // need to incref it, or any of its members.
    builder.SetInsertPoint(guard_type);
    Value *type_v = builder.CreateLoad(ObjectTy::ob_type(builder, obj_v));
    Value *is_right_type = builder.CreateICmpEQ(type_v, this->guard_type_v_);
    builder.CreateCondBr(is_right_type, guard_descr, bail_block);

    // If there is a descriptor, we need to guard on the descriptor type.  This
    // means emitting one more guard as well as subscribing to changes in the
    // descriptor type.
    builder.SetInsertPoint(guard_descr);
    if (this->descr_ != NULL) {
        fbuilder->types_used_.insert(this->guard_descr_type_);
        Value *descr_type_v =
            builder.CreateLoad(ObjectTy::ob_type(builder, this->descr_v_));
        Value *guard_descr_type_v =
            state->EmbedPointer<PyTypeObject*>(this->guard_descr_type_);
        Value *is_right_descr_type =
            builder.CreateICmpEQ(descr_type_v, guard_descr_type_v);
        builder.CreateCondBr(is_right_descr_type, do_access, bail_block);
    } else {
        builder.CreateBr(do_access);
    }

    // Fill in the bail bb.
    builder.SetInsertPoint(bail_block);
    fbuilder->Push(obj_v);
    fbuilder->CreateGuardBailPoint(_PYGUARD_ATTR);
}

void
OpcodeAttributes::DELETE_ATTR(int index)
{
    Value *attr = fbuilder_->LookupName(index);
    Value *obj = fbuilder_->Pop();
    Value *value = state_->GetNull<PyObject*>();
    Function *pyobj_setattr = state_->GetGlobalFunction<
        int(PyObject *, PyObject *, PyObject *)>("PyObject_SetAttr");
    Value *result = state_->CreateCall(
        pyobj_setattr, obj, attr, value, "DELETE_ATTR_result");
    state_->DecRef(obj);
    fbuilder_->PropagateExceptionOnNonZero(result);
}

}
