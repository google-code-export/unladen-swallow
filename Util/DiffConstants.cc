#include "Util/DiffConstants.h"

#include "llvm/Constants.h"
#include "llvm/Instruction.h"
#include "llvm/Operator.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::Constant;
using llvm::ConstantExpr;
using llvm::GEPOperator;
using llvm::Instruction;
using llvm::OverflowingBinaryOperator;
using llvm::SDivOperator;
using llvm::Value;
using llvm::cast;
using llvm::dyn_cast;
using llvm::llvm_report_error;

PyDiffVisitor::~PyDiffVisitor() {}

void PyDiffConstants(const Constant *C1, const Constant *C2,
                     PyDiffVisitor &Visitor) {
    assert(C1->getType() == C2->getType() &&
           "Can only traverse constants of the same type.");
    // Equal constants will never differ, of course.
    if (C1 == C2)
        return;
    if (C1->getValueID() != C2->getValueID()) {
        // We've found a place the constant trees differ: call the
        // visitor.  Then return since we won't be able to traverse in
        // parallel anymore.
        Visitor.Visit(C1, C2);
        return;
    }
    switch (C1->getValueID()) {
    // The following constant types don't contain further recursive
    // constants, so we call the Visitor if they're unequal.
    case Value::FunctionVal:
    case Value::GlobalAliasVal:
    case Value::GlobalVariableVal:
    case Value::BlockAddressVal:
    case Value::ConstantIntVal:
    case Value::ConstantFPVal:
        if (C1 != C2)
            Visitor.Visit(C1, C2);
        break;

    // The following constant types only have one instance per type,
    // so they can't possibly be different.
    case Value::UndefValueVal:
    case Value::ConstantAggregateZeroVal:
    case Value::ConstantPointerNullVal:
        assert(C1 == C2 && "This type of constant should only have one value.");
        break;

    // These types are simple sequences of other constants.  We know
    // the lengths of the sequences are the same because the types are
    // the same, so we always recurse on matching sub-constants.
    case Value::ConstantArrayVal:
    case Value::ConstantStructVal:
    case Value::ConstantVectorVal:
        for (unsigned i = 0, e = C1->getNumOperands(); i < e; ++i) {
            const Constant *C1Op = cast<Constant>(C1->getOperand(i));
            const Constant *C2Op = cast<Constant>(C2->getOperand(i));
            PyDiffConstants(C1Op, C2Op, Visitor);
        }
        break;

    case Value::ConstantExprVal: {
        const ConstantExpr *CE1 = cast<ConstantExpr>(C1);
        const ConstantExpr *CE2 = cast<ConstantExpr>(C2);
        // Unequal opcodes mean we've found the most precise difference.
        if (CE1->getOpcode() != CE2->getOpcode()) {
            Visitor.Visit(CE1, CE2);
            break;
        }
        if (CE1->isCompare() &&
            CE1->getPredicate() != CE2->getPredicate()) {
            Visitor.Visit(CE1, CE2);
            break;
        }
        if (CE1->hasIndices() &&
            CE1->getIndices() != CE2->getIndices()) {
            Visitor.Visit(CE1, CE2);
            break;
        }

        // Check that instruction attributes are equal.
        if (const GEPOperator *GO1 = dyn_cast<GEPOperator>(CE1)) {
            if (GO1->isInBounds() != cast<GEPOperator>(CE2)->isInBounds()) {
                Visitor.Visit(CE1, CE2);
                break;
            }
        }
        if (const SDivOperator *SD1 = dyn_cast<SDivOperator>(CE1)) {
            if (SD1->isExact() != cast<SDivOperator>(CE2)->isExact()) {
                Visitor.Visit(CE1, CE2);
                break;
            }
        }
        if (const OverflowingBinaryOperator *OBO1 =
            dyn_cast<OverflowingBinaryOperator>(CE1)) {
            const OverflowingBinaryOperator *OBO2 =
                cast<OverflowingBinaryOperator>(CE2);
            if (OBO1->hasNoUnsignedWrap() != OBO2->hasNoUnsignedWrap() ||
                OBO1->hasNoSignedWrap() != OBO2->hasNoSignedWrap()) {
                Visitor.Visit(CE1, CE2);
                break;
            }
        }

        // Diff the operands of otherwise-equal ConstantExprs.
        for (unsigned i = 0, e = CE1->getNumOperands(); i < e; ++i) {
            const Constant *C1Op = cast<Constant>(CE1->getOperand(i));
            const Constant *C2Op = cast<Constant>(CE2->getOperand(i));
            PyDiffConstants(C1Op, C2Op, Visitor);
        }
        break;
    }
    default:
        llvm_report_error("Impossible Value type for Constant.");
        break;
    }
}
