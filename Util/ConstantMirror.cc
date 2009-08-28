#include "Util/ConstantMirror.h"

#include "Util/PyTypeBuilder.h"

#include "llvm/DerivedTypes.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Module.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/Support/ValueHandle.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetData.h"

using llvm::ArrayType;
using llvm::CallbackVH;
using llvm::Constant;
using llvm::ConstantFP;
using llvm::ConstantInt;
using llvm::ConstantStruct;
using llvm::ExecutionEngine;
using llvm::GlobalValue;
using llvm::GlobalVariable;
using llvm::IntegerType;
using llvm::StructType;
using llvm::Type;
using llvm::User;
using llvm::Value;
using llvm::cast;
using llvm::isa;
using llvm::raw_string_ostream;

PyConstantMirror::PyConstantMirror(PyGlobalLlvmData *llvm_data)
    : llvm_data_(*llvm_data),
      engine_(*llvm_data->getExecutionEngine()),
      target_data_(*this->engine_.getTargetData()),
      python_shutting_down_(false)
{
}

Constant *
PyConstantMirror::GetConstantFor(PyObject *obj)
{
    PyTypeObject *type = Py_TYPE(obj);
    if (type == &PyType_Type)
        return this->GetConstantFor((PyTypeObject *)obj);
    if (type == &PyCode_Type)
        return this->GetConstantFor((PyCodeObject *)obj);
    if (type == &PyTuple_Type)
        return this->GetConstantFor((PyTupleObject *)obj);
    if (type == &PyString_Type)
        return this->GetConstantFor((PyStringObject *)obj);
    if (type == &PyUnicode_Type)
        return this->GetConstantFor((PyUnicodeObject *)obj);
    if (type == &PyInt_Type)
        return this->GetConstantFor((PyIntObject *)obj);
    if (type == &PyLong_Type)
        return this->GetConstantFor((PyLongObject *)obj);
    if (type == &PyFloat_Type)
        return this->GetConstantFor((PyFloatObject *)obj);
    if (type == &PyComplex_Type)
        return this->GetConstantFor((PyComplexObject *)obj);
    // We could just emit an inttoptr here, but the crash is nice
    // because it flags things we haven't implemented.  We could
    // compromise by only crashing in debug builds.
    std::string error_msg;
    raw_string_ostream(error_msg)
        << "Unexpected object type " << type->tp_name
        << ". Add a case to PyConstantMirror::GetConstantFor(PyObject*).";
    Py_FatalError(error_msg.c_str());
}

Constant *
PyConstantMirror::GetConstantFor(PyCodeObject *obj)
{
    // Register subobjects with the ExecutionEngine so it emits a
    // Constant that refers to them.
    this->GetGlobalVariableFor((PyObject*)obj->ob_type);
    this->GetGlobalVariableFor((PyObject*)obj->co_varnames);
    this->GetGlobalVariableFor((PyObject*)obj->co_names);

    const llvm::StructType *code_type = PyTypeBuilder<PyCodeObject>::get();
    return this->ConstantFromMemory(code_type, obj);
}

// Given a StructType in the form of most Python PyVarObjects, with a flexible
// array as its last member, returns a new StructType with that flexible array
// resized to the right dynamic number of elements.
static StructType *
ResizeVarObjectType(const StructType *type, unsigned dynamic_size)
{
    std::vector<const Type*> contents(
        type->element_begin(), type->element_end());
    if (type->isPacked()) {
        while (!contents.empty() && !isa<ArrayType>(contents.back())) {
            // Clang sometimes puts extra fields after the flexible array member
            // in order to be explicit about the struct's size.  Once we resize
            // the flexible array, these extra fields could extend off the end
            // of the allocated space, so we remove them.
            contents.pop_back();
        }
    }
    assert(!contents.empty() && "PyVarObject must contain at least 1 field");
    const Type *& flexible_array = contents.back();
    flexible_array =
        ArrayType::get(cast<ArrayType>(flexible_array)->getElementType(),
                       dynamic_size);
    return StructType::get(contents);
}

Constant *
PyConstantMirror::GetConstantFor(PyTupleObject *obj)
{
    this->GetGlobalVariableFor((PyObject*)obj->ob_type);
    const Py_ssize_t tuple_size = PyTuple_GET_SIZE(obj);
    for (Py_ssize_t i = 0; i < tuple_size; ++i) {
        this->GetGlobalVariableFor(obj->ob_item[i]);
    }

    const StructType *tuple_type = PyTypeBuilder<PyTupleObject>::get();
    const StructType *resized_tuple_type =
        ResizeVarObjectType(tuple_type, tuple_size);
    return this->ConstantFromMemory(resized_tuple_type, obj);
}

Constant *
PyConstantMirror::GetConstantFor(PyStringObject *obj)
{
    this->GetGlobalVariableFor((PyObject*)obj->ob_type);
    const StructType *string_type = PyTypeBuilder<PyStringObject>::get();
    const Py_ssize_t string_size = PyString_GET_SIZE(obj);
    const StructType *resized_string_type =
        // +1 for the '\0' at the end.
        ResizeVarObjectType(string_type, string_size + 1);
    return this->ConstantFromMemory(resized_string_type, obj);
}

Constant *
PyConstantMirror::GetConstantFor(PyUnicodeObject *obj)
{
    this->GetGlobalVariableFor((PyObject*)obj->ob_type);
    this->GetGlobalVariableFor(obj->defenc);
    const StructType *unicode_type = PyTypeBuilder<PyUnicodeObject>::get();
    return this->ConstantFromMemory(unicode_type, obj);
}

Constant *
PyConstantMirror::GetConstantFor(PyIntObject *obj)
{
    this->GetGlobalVariableFor((PyObject*)obj->ob_type);

    const StructType *int_type = PyTypeBuilder<PyIntObject>::get();
    return this->ConstantFromMemory(int_type, obj);
}

Constant *
PyConstantMirror::GetConstantFor(PyLongObject *obj)
{
    this->GetGlobalVariableFor((PyObject*)Py_TYPE(obj));
    const StructType *long_type = PyTypeBuilder<PyLongObject>::get();
    const Py_ssize_t long_size = Py_SIZE(obj);
    // See longintrepr.h for the meaning of long's ob_size field.
    const StructType *resized_long_type =
        ResizeVarObjectType(long_type, abs(long_size));
    return this->ConstantFromMemory(resized_long_type, obj);
}

Constant *
PyConstantMirror::GetConstantFor(PyFloatObject *obj)
{
    this->GetGlobalVariableFor((PyObject*)obj->ob_type);

    const StructType *float_type = PyTypeBuilder<PyFloatObject>::get();
    return this->ConstantFromMemory(float_type, obj);
}

Constant *
PyConstantMirror::GetConstantFor(PyComplexObject *obj)
{
    this->GetGlobalVariableFor((PyObject*)obj->ob_type);

    const StructType *complex_type = PyTypeBuilder<PyComplexObject>::get();
    return this->ConstantFromMemory(complex_type, obj);
}

Constant *
PyConstantMirror::GetConstantFor(PyTypeObject *obj)
{
    const llvm::StructType *type_type = PyTypeBuilder<PyTypeObject>::get();
    // Register subobjects with the ExecutionEngine so it emits a
    // Constant that refers to them.
    this->GetGlobalVariableForOwned(obj->tp_as_number, (PyObject*)obj);
    this->GetGlobalVariableForOwned(obj->tp_as_sequence, (PyObject*)obj);
    this->GetGlobalVariableForOwned(obj->tp_as_mapping, (PyObject*)obj);
    this->GetGlobalVariableForOwned(obj->tp_as_buffer, (PyObject*)obj);
    return this->ConstantFromMemory(type_type, obj);
}

llvm::Constant *
PyConstantMirror::GetConstantFor(PyNumberMethods *obj)
{
    return this->ConstantFromMemory(PyTypeBuilder<PyNumberMethods>::get(), obj);
}

llvm::Constant *
PyConstantMirror::GetConstantFor(PySequenceMethods *obj)
{
    return this->ConstantFromMemory(
        PyTypeBuilder<PySequenceMethods>::get(), obj);
}

llvm::Constant *
PyConstantMirror::GetConstantFor(PyMappingMethods *obj)
{
    return this->ConstantFromMemory(
        PyTypeBuilder<PyMappingMethods>::get(), obj);
}

llvm::Constant *
PyConstantMirror::GetConstantFor(PyBufferProcs *obj)
{
    return this->ConstantFromMemory(PyTypeBuilder<PyBufferProcs>::get(), obj);
}

template<typename T>
static T
read_as(const void *memory)
{
    T result;
    memcpy(&result, memory, sizeof(result));
    return result;
}

Constant *
PyConstantMirror::ConstantFromMemory(const Type *type, const void *memory) const
{
    switch (type->getTypeID()) {
    case Type::FloatTyID:
        return ConstantFP::get(type, read_as<float>(memory));
    case Type::DoubleTyID:
        return ConstantFP::get(type, read_as<double>(memory));
    case Type::IntegerTyID: {
        const IntegerType *int_type = cast<IntegerType>(type);
        switch(int_type->getBitWidth()) {
        case 8:
            return ConstantInt::get(int_type, read_as<uint8_t>(memory));
        case 16:
            return ConstantInt::get(int_type, read_as<uint16_t>(memory));
        case 32:
            return ConstantInt::get(int_type, read_as<uint32_t>(memory));
        case 64:
            return ConstantInt::get(int_type, read_as<uint64_t>(memory));
        }
        break;
    }
    case Type::StructTyID: {
        const StructType *const struct_type = cast<StructType>(type);
        std::vector<Constant*> contents;
        contents.reserve(struct_type->getNumElements());
        const llvm::StructLayout &layout =
            *this->target_data_.getStructLayout(struct_type);
        const char *const cmemory = static_cast<const char*>(memory);
        for (unsigned i = 0, end = struct_type->getNumElements();
             i < end; ++i) {
            uint64_t offset = layout.getElementOffset(i);
            contents.push_back(
                this->ConstantFromMemory(struct_type->getElementType(i),
                                         cmemory + offset));
        }
        return ConstantStruct::get(struct_type, contents);
    }
    case Type::ArrayTyID: {
        const ArrayType *const array_type = cast<ArrayType>(type);
        const Type *const element_type = array_type->getElementType();
        const uint64_t element_size =
            this->target_data_.getTypeAllocSize(element_type);
        std::vector<Constant*> contents;
        contents.reserve(array_type->getNumElements());
        const char *const cmemory = static_cast<const char*>(memory);
        for (unsigned i = 0, end = array_type->getNumElements();
             i < end; ++i) {
            uint64_t offset = i * element_size;
            contents.push_back(
                this->ConstantFromMemory(element_type, cmemory + offset));
        }
        return llvm::ConstantArray::get(array_type, contents);
    }
    case Type::PointerTyID: {
        void *the_pointer = read_as<void*>(memory);
        // Try to find a GlobalValue that's mapped to this address.
        // This will let LLVM's optimizers pull values out of here.
        if (const GlobalValue *known_constant =
            this->engine_.getGlobalValueAtAddress(the_pointer)) {
            // You can't put a const Constant into another Constant,
            // so make it non-const.
            return const_cast<GlobalValue*>(known_constant);
        }
        // If we don't already have a mapping for the requested
        // address, emit it as an inttoptr.
        return llvm::ConstantExpr::getIntToPtr(
            ConstantInt::get(Type::Int64Ty,
                             reinterpret_cast<intptr_t>(the_pointer)),
            type);
    }
    default:
        break;
    }

    // Can only get here if we can't handle the type.
    std::string type_dump;
    raw_string_ostream(type_dump)
        << "Can't emit type " << type << " to memory.";
    Py_FatalError(type_dump.c_str());
}

// This can only be allocated by new, and it deletes itself when the
// GlobalVariable it's attached to is destroyed.
class PyConstantMirror::RemovePyObjFromGlobalMappingsVH
    : public llvm::CallbackVH {
public:
    RemovePyObjFromGlobalMappingsVH(PyConstantMirror *parent,
                                    GlobalValue *global, PyObject *obj)
        : CallbackVH(global), parent_(*parent), obj_(obj) {}

    virtual void deleted()
    {
        // Before allowing the object's memory to be re-used, remove it from the
        // cache.
        this->parent_.engine_.updateGlobalMapping(
            cast<GlobalValue>(this->getValPtr()), NULL);
        if (!this->parent_.python_shutting_down_)
            Py_DECREF(this->obj_);
        this->setValPtr(NULL);
        delete this;
    }

private:
    PyConstantMirror &parent_;
    PyObject *const obj_;
};

llvm::Constant *
PyConstantMirror::GetGlobalVariableFor(PyObject *obj)
{
    return GetGlobalVariableForOwned(obj, obj);
}

template<typename T>
Constant *
PyConstantMirror::GetGlobalVariableForOwned(T *ptr, PyObject *owner)
{
    if (ptr == NULL) {
        return Constant::getNullValue(PyTypeBuilder<T*>::get());
    }
    GlobalValue *result =
        const_cast<GlobalValue*>(this->engine_.getGlobalValueAtAddress(ptr));
    if (result == NULL) {
        Constant *initializer = this->GetConstantFor(ptr);
        result = new GlobalVariable(*this->llvm_data_.module(),
                                    initializer->getType(),
                                    false,  // Not constant.
                                    GlobalValue::InternalLinkage, initializer,
                                    /*name=*/"");
        Py_INCREF(owner);
        this->engine_.addGlobalMapping(result, ptr);
        // The object created on the next line owns itself:
        new RemovePyObjFromGlobalMappingsVH(this, result, owner);
    }
    return result;
}