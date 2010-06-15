#include "Python.h"
#include "opcode.h"

#include "JIT/opcodes/cmpops.h"
#include "JIT/llvm_fbuilder.h"
#include "Util/Instrumentation.h"

#include "llvm/BasicBlock.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/raw_ostream.h"

using llvm::BasicBlock;
using llvm::ConstantInt;
using llvm::Function;
using llvm::Value;
using llvm::errs;

#ifdef Py_WITH_INSTRUMENTATION
// These are used as template parameter and are required to have external
// linkage. As const char[] default to static in C++, we have to force it.
extern const char cmpop_full[], cmpop_short[];
const char cmpop_full[] = "Compare operators";
const char cmpop_short[] = "compare";

static llvm::ManagedStatic<
    OpStats<cmpop_full, cmpop_short> > compare_operator_stats;

#define CMPOP_INC_STATS(field) compare_operator_stats->field++
#else
#define CMPOP_INC_STATS(field)
#endif  /* Py_WITH_INSTRUMENTATION */


namespace py {

OpcodeCmpops::OpcodeCmpops(LlvmFunctionBuilder *fbuilder) :
    fbuilder_(fbuilder), state_(fbuilder->state())
{
}

bool
OpcodeCmpops::COMPARE_OP_fast(int cmp_op, const PyTypeObject *lhs_type,
                                     const PyTypeObject *rhs_type)
{
    const char *api_func = NULL;

    switch (cmp_op) {
#define CMPOP_NAME(op) { \
    case op: \
        api_func = #op; \
        break; \
}
        CMPOP_NAME(PyCmp_IS)
        CMPOP_NAME(PyCmp_IS_NOT)
        CMPOP_NAME(PyCmp_IN)
        CMPOP_NAME(PyCmp_NOT_IN)
        CMPOP_NAME(PyCmp_EXC_MATCH)
        CMPOP_NAME(PyCmp_EQ)
        CMPOP_NAME(PyCmp_NE)
        CMPOP_NAME(PyCmp_LT)
        CMPOP_NAME(PyCmp_LE)
        CMPOP_NAME(PyCmp_GT)
        CMPOP_NAME(PyCmp_GE)
        default:
            // There's a check for this in llvm_compile.cc; if this call ever
            // triggers, something has gone horribly wrong.
            Py_FatalError("unknown COMPARE_OP oparg");
            return false;  // Not reached.
#undef CMPOP_NAME
    }

    const char *name = fbuilder_->llvm_data_->optimized_ops.
        Find(api_func, lhs_type, rhs_type);

    if (name == NULL) {
        return false;
    }

    CMPOP_INC_STATS(optimized);

    BasicBlock *success = state_->CreateBasicBlock("CMPOP_OPT_success");
    BasicBlock *bailpoint = state_->CreateBasicBlock("CMPOP_OPT_bail");

    Value *rhs = fbuilder_->Pop();
    Value *lhs = fbuilder_->Pop();

    Function *op =
        state_->GetGlobalFunction<PyObject*(PyObject*, PyObject*)>(name);
    Value *result = state_->CreateCall(op, lhs, rhs, "cmpop_result");
    fbuilder_->builder_.CreateCondBr(state_->IsNull(result),
                                     bailpoint, success);

    fbuilder_->builder_.SetInsertPoint(bailpoint);
    fbuilder_->Push(lhs);
    fbuilder_->Push(rhs);
    fbuilder_->CreateBailPoint(_PYFRAME_GUARD_FAIL);

    fbuilder_->builder_.SetInsertPoint(success);
    state_->DecRef(lhs);
    state_->DecRef(rhs);
    fbuilder_->Push(result);
    return true;
}

void
OpcodeCmpops::COMPARE_OP(int cmp_op)
{
    CMPOP_INC_STATS(total);
    const PyTypeObject *lhs_type = fbuilder_->GetTypeFeedback(0);
    const PyTypeObject *rhs_type = fbuilder_->GetTypeFeedback(1);
    if (lhs_type != NULL && rhs_type != NULL) {
        // Returning true means the op was successfully optimized.
        if (this->COMPARE_OP_fast(cmp_op, lhs_type, rhs_type)) {
            return;
        }
        CMPOP_INC_STATS(omitted);
    }
    else {
        CMPOP_INC_STATS(unpredictable);
    }

    this->COMPARE_OP_safe(cmp_op);
}

void
OpcodeCmpops::COMPARE_OP_safe(int cmp_op)
{
    Value *rhs = fbuilder_->Pop();
    Value *lhs = fbuilder_->Pop();
    Value *result;
    switch (cmp_op) {
    case PyCmp_IS:
        result = fbuilder_->builder_.CreateICmpEQ(lhs, rhs,
                                             "COMPARE_OP_is_same");
        state_->DecRef(lhs);
        state_->DecRef(rhs);
        break;
    case PyCmp_IS_NOT:
        result = fbuilder_->builder_.CreateICmpNE(lhs, rhs,
                                             "COMPARE_OP_is_not_same");
        state_->DecRef(lhs);
        state_->DecRef(rhs);
        break;
    case PyCmp_IN:
        // item in seq -> ContainerContains(seq, item)
        result = this->ContainerContains(rhs, lhs);
        break;
    case PyCmp_NOT_IN:
    {
        Value *inverted_result = this->ContainerContains(rhs, lhs);
        result = fbuilder_->builder_.CreateICmpEQ(
            inverted_result, ConstantInt::get(inverted_result->getType(), 0),
            "COMPARE_OP_not_in_result");
        break;
    }
    case PyCmp_EXC_MATCH:
        result = this->ExceptionMatches(lhs, rhs);
        break;
    case PyCmp_EQ:
    case PyCmp_NE:
    case PyCmp_LT:
    case PyCmp_LE:
    case PyCmp_GT:
    case PyCmp_GE:
        this->RichCompare(lhs, rhs, cmp_op);
        return;
    default:
        // There's a check for this in llvm_compile.cc; if this call ever
        // triggers, something has gone horribly wrong.
        Py_FatalError("unknown COMPARE_OP oparg");
        return;  // Not reached.
    }
    Value *value = fbuilder_->builder_.CreateSelect(
        result,
        state_->GetGlobalVariableFor((PyObject*)&_Py_TrueStruct),
        state_->GetGlobalVariableFor((PyObject*)&_Py_ZeroStruct),
        "COMPARE_OP_result");
    state_->IncRef(value);
    fbuilder_->Push(value);
}

void
OpcodeCmpops::RichCompare(Value *lhs, Value *rhs, int cmp_op)
{
    Function *pyobject_richcompare = state_->GetGlobalFunction<
        PyObject *(PyObject *, PyObject *, int)>("PyObject_RichCompare");
    Value *result = state_->CreateCall(
        pyobject_richcompare, lhs, rhs,
        ConstantInt::get(PyTypeBuilder<int>::get(fbuilder_->context_), cmp_op),
        "COMPARE_OP_RichCompare_result");
    state_->DecRef(lhs);
    state_->DecRef(rhs);
    fbuilder_->PropagateExceptionOnNull(result);
    fbuilder_->Push(result);
}

Value *
OpcodeCmpops::ContainerContains(Value *container, Value *item)
{
    Function *contains =
        state_->GetGlobalFunction<int(PyObject *, PyObject *)>(
            "PySequence_Contains");
    Value *result = state_->CreateCall(
        contains, container, item, "ContainerContains_result");
    state_->DecRef(item);
    state_->DecRef(container);
    fbuilder_->PropagateExceptionOnNegative(result);
    return state_->IsPositive(result);
}

// TODO(twouters): test this (used in exception handling.)
Value *
OpcodeCmpops::ExceptionMatches(Value *exc, Value *exc_type)
{
    Function *exc_matches = state_->GetGlobalFunction<
        int(PyObject *, PyObject *)>("_PyEval_CheckedExceptionMatches");
    Value *result = state_->CreateCall(
        exc_matches, exc, exc_type, "ExceptionMatches_result");
    state_->DecRef(exc_type);
    state_->DecRef(exc);
    fbuilder_->PropagateExceptionOnNegative(result);
    return state_->IsPositive(result);
}

}
