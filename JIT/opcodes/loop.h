// -*- C++ -*-
#ifndef OPCODE_LOOP_H_
#define OPCODE_LOOP_H_

#ifndef __cplusplus
#error This header expects to be included only in C++ source
#endif

namespace llvm {
    class BasicBlock;
}

namespace py {

class LlvmFunctionBuilder;

// This class contains most loop related opcodes.
// SETUP_LOOP can be found in OpcodeBlock.
class OpcodeLoop
{
public:
    OpcodeLoop(LlvmFunctionBuilder *fbuilder);
    
    void GET_ITER();
    void FOR_ITER(llvm::BasicBlock *target, llvm::BasicBlock *fallthrough);


    void CONTINUE_LOOP(llvm::BasicBlock *target,
                       int target_opindex,
                       llvm::BasicBlock *fallthrough);

    void BREAK_LOOP();

private:
    LlvmFunctionBuilder *fbuilder_;
};

}

#endif /* OPCODE_LOOP_H_ */
