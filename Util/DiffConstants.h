// -*- C++ -*-
#ifndef UTIL_DIFFCONSTANTS_H
#define UTIL_DIFFCONSTANTS_H

#ifndef __cplusplus
#error This header expects to be included only in C++ source
#endif

namespace llvm {
class Constant;
}

class PyDiffVisitor {
public:
    virtual void Visit(const llvm::Constant *C1, const llvm::Constant *C2) = 0;
protected:
    virtual ~PyDiffVisitor();
};

// Traverses C1 and C2, which must be of the same type, and calls
// Visitor at each node where we find a leaf node that differs.  So,
// given {i32 1, i32* inttoptr (i32 12345)} and {i32 1, i32* @gv},
// we'll call Visitor(i32* inttoptr (i32 12345), i32* @gv).
void PyDiffConstants(const llvm::Constant *C1, const llvm::Constant *C2,
                     PyDiffVisitor &Visitor);

#endif  // UTIL_DIFFCONSTANTS_H
