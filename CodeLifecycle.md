## Creating code ##
  1. Python source code is parsed and compiled to bytecode when it's imported for the first time.
  1. The bytecode is cached in `.pyc` files, which let us skip parsing and the first compilation stage on subsequent imports.
  1. Inside `PyCode_New()` (in `Objects/codeobject.c`) (whether it was called by the bytecode compiler or by loading the .pyc file), if `-j always` is on, we call `_PyCode_Recompile(co, Py_OptimizeFlag)`, which compiles the Python bytecode to LLVM IR using `_PyCode_To_Llvm()`. The IR gets stored in `PyCodeObject::co_llvm_function`.  At this stage, we _don't_ compile the function all the way to machine code.
  1. Each time a function is called, `PyEval_EvalCodeEx` (in Python/eval.cc) calls mark\_called\_and\_maybe\_compile(). This increments the code object's call count. If `-j whenhot` is on and the call count passes the hotness threshold (currently defined by Py\_HOT\_OR\_NOT in the same file), then we compile the code to IR.
  1. Inside mark\_called\_and\_maybe\_compile(), we check whether anyone (either `PyCode_New`, the hotness threshold, or the user directly) has set co\_use\_llvm.  If so, we ask LLVM to translate it to machine code with `_LlvmFunction_Jit` (defined in `Objects/_llvmfunctionobject.cc`). This currently uses `ExecutionEngine::getPointerToFunction`, but we may eventually do something more amenable to recompilation.  The pointer to the native function is cached as `PyCodeObject::co_native_function`.

All of this happens in a single thread, which lets us assume that the Module doesn't change out from under us in any particular phase. (This will eventually be a bottleneck to threaded compilation, but we'll deal with that when we get there.)

The output IR refers to Module globals, including constants, CPython-controlled variables, and eventually inlinable Functions.

## Changing code ##

Once an IR Function is translated from bytecode, its semantics never change.  It may be optimized further, but there's currently no way to re-JIT a Function to machine code.

## Destroying code ##
LLVM assumes certain things about when it can destroy or change Values, especially Functions, and other things about when users can destroy Values, and Unladen Swallow needs to be compatible with that.

Llvm Functions are referenced by a PyLlvmFunctionObject, which is referenced by a PyCodeObject, which is referenced by one or more PyFunctionObjects. A referenced llvm::Function has external linkage to prevent module-level optimizations from deleting it or changing its signature. While a function is executing, its thread keeps a reference to the PyFunctionObject which keeps the PyLlvmFunctionObject alive. When the PyLlvmFunctionObject loses its last reference, we currently 1) set the llvm::Function to internal and 2) if the use-list is empty, free the JITted code if any and erase the Function.

We don't yet, but we want to run various Module-level optimization passes. nlewycky has said that these won't delete external Functions even if they're unreferenced, but he wasn't 100% sure that they don't call ReplaceAllUsesWith (RAUW) on external Functions. The JIT assumes that any Function it has compiled will live until the machine code is freed. If we run global optimizations that delete even internal unused Functions, we could run into last week's crash again. So, until the JIT is guaranteed safe, we can't run any Module-wide optimizations that may delete or RAUW code. Once the JIT is guaranteed safe, we'll need to update any internal Value\*s to use ValueHandles instead of direct pointers.

## Machine code ##

We call `ExecutionEngine::getPointerToFunction(function)` to retrieve machine code for any LLVM IR we want to run. The Execution keeps the function pointer around as a handle to the machine code until we call `freeMachineCodeForFunction(function)`, so it's illegal to delete `function` until then. Functions can be deleted by ModulePasses in addition to user calls to `eraseFromParent()`, so for now we have to manually inspect each ModulePass we want to run to make sure it won't delete global functions. Marking all functions external would work, but would cause a memory leak.

If we reoptimize a Function, we have to be careful about other frames, either in the current thread or other threads, that may be inside that Function's machine code. There are two ways to emit new machine code for a reoptimized function.
  1. Set a flag on the function, and when there are no frames in any thread inside the function, call `ExecutionEngine::recompileAndRelinkFunction()`.
  1. Move/copy the original function's IR into a new function and call getPointerToFunction on the _new_ function. Leave the old function around as a declaration just to occupy its address until the last frame exits, then free its machine code and delete it. Declarations are especially prone to being optimized away, so this makes it even harder to run ModulePasses.

When a function is recompiled, we have to forward calls to the old address to the new address. recompileAndRelinkFunction does that for us by overwriting the beginning of the old machine code with a jump to the new machine code. (This means it leaks that stub on every use.) If we copy functions instead, we have to make all calls through a pointer in memory, and we update that pointer atomically when we recompile its function. I think we just give up on propagating reoptimizations to inlined functions and instead rely on their containing function being reoptimized.

## Clang-compiled code ##

We intend to compile some C code with clang to LLVM IR, and use the cpp backend to emit C++ classes that can load this IR into our main module. Then we'll emit calls to these functions from our generated IR. It's absolutely essential that no optimization change the meaning of these functions or delete them, since there's no way to get back the original. Nick has warned that ModulePasses may, for example, notice that a global is never written to in any code LLVM can see and so optimize loads, but he says this should not happen to external globals. To prevent LLVM's optimizations from messing up clang-compiled code, we'll declare all of it external. We'd like to declare it [available\_externally](http://llvm.org/docs/LangRef.html#available_externally), but that still allows LLVM to delete the definition if it's unused, which we don't want. Nick says we can add a use to @llvm.used which should prevent changes. (Does this make the symbol effectively external??)