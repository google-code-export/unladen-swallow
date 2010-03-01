/* Forward declares some functions using PyGlobalLlvmData so that C
   files can use them.  See global_llvm_data.h for the full C++
   interface. */
#ifndef PYTHON_GLOBAL_LLVM_DATA_FWD_H
#define PYTHON_GLOBAL_LLVM_DATA_FWD_H

#include "Python.h"
#include "llvm_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef WITH_LLVM

struct PyLlvmFuncs {
    int loaded;  /* true if the JIT is loaded; false otherwise. */

    /* Record data for use in generating optimized machine code. */
    void (*inc_feedback_counter)(PyCodeObject *co, int expected_opcode,
                                 int opcode_index, int counter_id);
    void (*record_func)(PyCodeObject *co, int expected_opcode,
                        int opcode_index, int arg_index, PyObject *func);
    void (*record_object)(PyCodeObject *co, int expected_opcode,
                          int opcode_index, int arg_index, PyObject *obj);
    void (*record_type)(PyCodeObject *co, int expected_opcode,
                        int opcode_index, int arg_index, PyObject *obj);

    /* Backend of maybe_compile */
    Py_CompileResult (*jit_compile)(PyCodeObject *co, PyFrameObject *f,
                                    int opt_level, Py_ShouldBlock block);

    /* Global Data */
    struct PyGlobalLlvmData* (*global_data_get)(void);
    void (*global_data_clear)(struct PyGlobalLlvmData *);
    void (*global_data_free)(struct PyGlobalLlvmData *);
    int (*global_data_optimize)(_LlvmFunction *, int);
    void (*global_data_collect_unused_globals)(struct PyGlobalLlvmData *);

    /* Finalizes LLVM. */
    void (*llvm_fini)(void);
    /* Sets LLVM debug output on or off. */
    int (*set_debug)(int on);

    PyObject* llvmfunction_type;
    PyObject* (*llvmfunction_fromcodeobject)(PyObject *);
    void (*llvmfunction_dealloc)(_LlvmFunction *);

    /* Background thread control.  */
    void (*llvmthread_start)(void);
    void (*llvmthread_stop)(void);
    void (*llvmthread_pause)(void);
    void (*llvmthread_unpause)(void);
    void (*llvmthread_reset_after_fork)(void);
    void (*llvmthread_apply_finished_jobs)(Py_ShouldBlock block);
    void (*llvmthread_wait_for_jit)(void);

#ifdef Py_WITH_INSTRUMENTATION
    /* JIT instrumentation functions. */
    void (*feedback_map_inc_counter)(void);
    void (*add_hot_code)(PyCodeObject *co);
    void (*record_fatal_bail)(PyCodeObject *code);
    void (*record_watcher_count)(size_t watcher_count);
    void (*record_bail)(PyFrameObject *frame, _PyFrameBailReason bail_reason);
#endif /* Py_WITH_INSTRUMENTATION */
};

/* Pointer to the PyLlvmFuncs struct, if the _llvmjit module has been loaded.
 * If the module has not been loaded, then this is NULL.  */
PyAPI_DATA(struct PyLlvmFuncs) _PyLlvmFuncs;

#define PyGlobalLlvmData_GET() (_PyLlvmFuncs.global_data_get())

#define Py_MIN_LLVM_OPT_LEVEL 0
#define Py_DEFAULT_JIT_OPT_LEVEL 2
#define Py_MAX_LLVM_OPT_LEVEL 3

#endif  /* WITH_LLVM */

#ifdef __cplusplus
}
#endif
#endif  /* PYTHON_GLOBAL_LLVM_DATA_FWD_H */
