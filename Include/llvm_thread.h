#ifndef PYTHON_LLVM_THREAD_H
#define PYTHON_LLVM_THREAD_H

#ifdef WITH_LLVM

#ifdef __cplusplus

#include "Util/SynchronousQueue.h"
#include "code.h"
#include "pythread.h"

struct PyGlobalLlvmData;
class PyJitJob;
class CompileToIrJob;

extern "C" {
#endif /* __cplusplus */

/* Enum for the result of JIT compilation.  */
typedef enum {
    PY_COMPILE_OK,       /* The compile finished successfully.  */
    PY_COMPILE_ERROR,    /* There was an error during compilation.  */
    PY_COMPILE_SHUTDOWN, /* The compilation thread has shut down.  */
    PY_COMPILE_REFUSED   /* Compilation was refused.  This is not an error.  */
} Py_CompileResult;

/* Whether or not we should block until JIT compilation is finished.  */
typedef enum {
    PY_NO_BLOCK,
    PY_BLOCK
} Py_ShouldBlock;

/* Simple extern "C" wrappers for the compile thread methods.  */
PyAPI_FUNC(void) PyLlvm_StartCompilation(void);
PyAPI_FUNC(void) PyLlvm_StopCompilation(void);
PyAPI_FUNC(void) PyLlvm_PauseCompilation(void);
PyAPI_FUNC(void) PyLlvm_UnpauseCompilation(void);
PyAPI_FUNC(void) PyLlvm_ResetAfterFork(void);
PyAPI_FUNC(void) PyLlvm_ApplyFinishedJobs(Py_ShouldBlock block);
PyAPI_FUNC(void) PyLlvm_WaitForJit(void);
PyAPI_FUNC(Py_CompileResult) PyLlvm_JitInBackground(PyCodeObject *code,
                                                    int new_opt_level,
                                                    Py_ShouldBlock block);
PyAPI_FUNC(Py_CompileResult) PyLlvm_CodeToOptimizedIr(PyCodeObject *code,
                                                      int new_opt_level);

#ifdef __cplusplus
}

// This class encapsulates all state required by the background compilation
// thread.  All public methods must be called while holding the GIL.
class PyLlvmCompileThread {
public:
    PyLlvmCompileThread(PyGlobalLlvmData *llvm_data);
    ~PyLlvmCompileThread();

    // This method starts the background thread.  Calling it multiple times has
    // no effect.
    void Start();

    // Terminate the background thread.  This will block until it terminates,
    // which may be after it finishes a single compile job.  After the thread
    // has has stopped, all of the public methods below will run the job in the
    // foreground or do nothing.  This method must be called before destroying
    // the thread object or the PyGlobalLlvmData object.  Calling this method
    // when the thread is already stopped has no effect.
    void Terminate();

    // Pause the background thread.  Puts a job on the queue that will cause the
    // background thread to acquire the unpause event lock twice.  This call
    // must be paired with an Unpause call from the same thread.
    void Pause();

    // Unpause the background thread.  This releases the unpause_event_ lock,
    // allowing the background thread to proceed.  This call must be paired with
    // a Pause call from the same thread.
    void Unpause();

    // Cleanup what is left of the background thread after a fork.  Forking is
    // only allowed to happen while the thread is paused, so we know exactly
    // what state we need to fix.
    void ResetAfterFork();

    // Apply the results of any JIT compilation done in the background thread.
    // If block is PY_NO_BLOCK, then this function returns immediately if it
    // cannot acquire the lock on the output job queue.  The thread that calls
    // this must hold the GIL.
    void ApplyFinishedJobs(Py_ShouldBlock block);

    // Compile this code object in the background thread.  If block is true,
    // then block until the compilation is done.  If there is an error and
    // block is PY_BLOCK, then we raise an exception and return
    // PY_COMPILE_ERROR.  If there is an error and block is PY_NO_BLOCK, then
    // the background thread will print an error to stderr, but this call will
    // still return PY_COMPILE_OK.  This is consistent with how exceptions in
    // Python __del__ methods and during shutdown are treated, and should only
    // happen if the user creates a malformed code object.  Finally, if the
    // compilation thread has shut down, then we raise an exception and return
    // PY_COMPILE_SHUTDOWN.  This is so that clients like the eval loop can
    // easily silence these exceptions, but users that try to compile code after
    // shutdown get an exception.
    Py_CompileResult JitInBackground(PyCodeObject *code, int new_opt_level,
                                     Py_ShouldBlock block);

    // Compile the code object in the background thread for the purposes of
    // reading the optimized LLVM IR.  This avoids clearing out the IR after
    // the compilation.
    Py_CompileResult CodeToOptimizedIr(PyCodeObject *code, int new_opt_level);

    // Deallocate this function object in the background thread.  This takes
    // ownership of the object.
    void FreeInBackground(_LlvmFunction *functionobj);

    // Block execution in this thread until all JIT jobs that are currently on
    // the queue are done.  This is exported to the _llvm Python module, and
    // it's mostly useful for testing LLVM compilation under '-j whenhot'.
    void WaitForJobs();

    PyThreadState *getThreadState() { return this->tstate_; }

private:
    PyGlobalLlvmData *llvm_data_;

    // This is the queue of incoming compilation jobs.  If you acquire both
    // in_queue_ and out_queue_, note that in_queue_ must be acquired first.
    SynchronousQueue<PyJitJob*> in_queue_;

    // This is the queue of finished compilation jobs.  It is read periodically
    // when the Python ticker expires, or when a thread waits for a job to
    // complete.  Again, if you acquire both in_queue_ and out_queue_, in_queue_
    // must be acquired first.
    SynchronousQueue<PyJitJob*> out_queue_;

    // Indicates whether the thread is running.
    bool running_;

    // The thread state for the compilation thread.  We don't use any of this
    // information, but we have to have an interpreter state to swap in when we
    // acquire the GIL.  Many Python calls assume that there is always a valid
    // thread state from which to get the interpreter state, so we must provide
    // one.
    PyThreadState *tstate_;

    // The interpreter state corresponding to this compilation thread.  We cache
    // this value because it is not safe to call PyThreadState_GET() from a
    // background thread.
    PyInterpreterState *interpreter_state_;

    // A lock that allows us to pause and unpause the background thread without
    // destroying and recreating the underlying OS-level thread.
    PyThread_type_lock unpause_event_;

    // This static method is the background compilation thread entry point.  It
    // simply wraps calling Run() so we can pass it as a function pointer to
    // PyThread_start_new_thread.
    static void Bootstrap(void *thread);

    // This method reads jobs from the queue and runs them in a loop.  It
    // terminates when it reads a JIT_EXIT job from the queue.  This method
    // must be called from the background thread, and the caller should not
    // hold the GIL.
    void Run();

    // Puts a finished job on the output queue and notifies anyone waiting for
    // it that it is now on the output queue.  This method takes ownership of
    // the job.  This method must be called from the background thread, and the
    // caller should not hold the GIL.
    void OutputFinishedJob(PyJitJob *job);

    // Do any necessary cleanup before we terminate the thread.  This means
    // closing the queue to any new writers, abandoning all of the compile jobs,
    // and doing only the cleanup jobs, like freeing functions.  This method
    // must be called from the background thread, and the caller should not
    // hold the GIL.
    void DoCleanups();

    // Helper method that puts a job on the queue.  If the queue is closed, it
    // calls the RunAfterShutdown method of the job and returns -1.  This
    // method takes ownership of the job.  The caller must hold the GIL.
    int RunJob(PyJitJob *job);

    // Helper method that puts a job on the queue and waits for it to be
    // finished.  This method takes ownership of the job.  The caller must hold
    // the GIL.
    void RunJobAndWait(PyJitJob *job);

    // Helper method that calls RunJobAndWait and then applies all finished
    // jobs.  This method takes ownership of the job.  The caller must hold the
    // GIL.
    void RunJobAndApply(PyJitJob *job);

    // Helper method that handles running CompileJobs and CompileToIrJobs.
    Py_CompileResult RunCompileJob(CompileToIrJob *job, Py_ShouldBlock block);
};

#endif /* __cplusplus */

#endif /* WITH_LLVM */

#endif /* PYTHON_LLVM_THREAD_H */
