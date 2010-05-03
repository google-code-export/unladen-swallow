#include "Python.h"

#include "JIT/opcodes/stack.h"
#include "JIT/llvm_fbuilder.h"

#include "llvm/BasicBlock.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"

using llvm::BasicBlock;
using llvm::ConstantInt;
using llvm::Function;
using llvm::Type;
using llvm::Value;

namespace py {

OpcodeStack::OpcodeStack(LlvmFunctionBuilder *fbuilder) :
    fbuilder_(fbuilder)
{
}

void
OpcodeStack::POP_TOP()
{
    fbuilder_->DecRef(fbuilder_->Pop());
}

void
OpcodeStack::DUP_TOP()
{
    Value *first = fbuilder_->Pop();
    fbuilder_->IncRef(first);
    fbuilder_->Push(first);
    fbuilder_->Push(first);
}

void
OpcodeStack::DUP_TOP_TWO()
{
    Value *first = fbuilder_->Pop();
    Value *second = fbuilder_->Pop();
    fbuilder_->IncRef(first);
    fbuilder_->IncRef(second);
    fbuilder_->Push(second);
    fbuilder_->Push(first);
    fbuilder_->Push(second);
    fbuilder_->Push(first);
}

void
OpcodeStack::DUP_TOP_THREE()
{
    Value *first = fbuilder_->Pop();
    Value *second = fbuilder_->Pop();
    Value *third = fbuilder_->Pop();
    fbuilder_->IncRef(first);
    fbuilder_->IncRef(second);
    fbuilder_->IncRef(third);
    fbuilder_->Push(third);
    fbuilder_->Push(second);
    fbuilder_->Push(first);
    fbuilder_->Push(third);
    fbuilder_->Push(second);
    fbuilder_->Push(first);
}

void
OpcodeStack::ROT_TWO()
{
    Value *first = fbuilder_->Pop();
    Value *second = fbuilder_->Pop();
    fbuilder_->Push(first);
    fbuilder_->Push(second);
}

void
OpcodeStack::ROT_THREE()
{
    Value *first = fbuilder_->Pop();
    Value *second = fbuilder_->Pop();
    Value *third = fbuilder_->Pop();
    fbuilder_->Push(first);
    fbuilder_->Push(third);
    fbuilder_->Push(second);
}

void
OpcodeStack::ROT_FOUR()
{
    Value *first = fbuilder_->Pop();
    Value *second = fbuilder_->Pop();
    Value *third = fbuilder_->Pop();
    Value *fourth = fbuilder_->Pop();
    fbuilder_->Push(first);
    fbuilder_->Push(fourth);
    fbuilder_->Push(third);
    fbuilder_->Push(second);
}

}
