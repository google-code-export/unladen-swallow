#include "Python.h"

#include "Python/global_llvm_data.h"

#include "Util/EventTimer.h"
#include "Util/SynchronousQueue.h"

#include "_llvmfunctionobject.h"
#include "code.h"
#include "llvm_compile.h"
#include "llvm_thread.h"
#include "pythread.h"

#ifdef WITH_LLVM

#include "llvm/ADT/OwningPtr.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/Function.h"
#include "llvm/Support/MutexGuard.h"
#include "llvm/System/Mutex.h"

// This is the base class of an inheritence tree of job types.  Each job has
// two virtual methods: a Run method, and an Apply method.  The Run method
// implements the computation that must be done in the background thread, where
// it has access to LLVM's state.  The Apply method is called from any
// foreground thread that holds the GIL.  Jobs are either periodically applied,
// or applied immediately if someone is waiting for the job.
struct PyJitJob {
    // If this is non-NULL, we will release this lock when compilation is
    // finished, so that we can notify the thread that is waiting for this job.
    // The job does *not* own this pointer, whoever is waiting does.
    PyThread_type_lock waiter_lock_;

    // Computation in the background thread may throw exceptions, but using the
    // standard Python exception methods is not threadsafe, so we store the
    // exception information here.
    PyLlvmError *err_;

    PyJitJob() : waiter_lock_(NULL), err_(NULL) {}

    virtual ~PyJitJob() {}

    // The result of a job.  This is used to communicate back to the background
    // thread.
    enum JobResult {
        JOB_OK,     // Everything is fine.
        JOB_ERROR,  // An error occurred.  This implies that err_ is filled in.
        JOB_EXIT    // This tells the background thread to exit.
    };

    // This method implements the part of the job that uses LLVM and therefore
    // must be done in the background thread.  It must be careful not to access
    // data that can be mutated outside the background thread.
    virtual JobResult Run(PyGlobalLlvmData *llvm_data) = 0;

    // Subclasses can override this to customize whether the job should get run
    // after and during shutdown.  By default, they do.
    virtual void RunAfterShutdown(PyGlobalLlvmData *llvm_data) {
        this->Run(llvm_data);
    }

    // Wrapper for ApplyImpl.
    void Apply() {
        PyEval_AssertLockHeld();
        this->ApplyImpl();
    }

    // Return whether the job should be prioritized above others.  This is just
    // a hack to prioritize exit jobs above others, which should be replaced in
    // the future with a more sophisticated priority queue.  Most jobs should be
    // pushed onto the back of the queue.
    virtual bool ShouldPushFront() {
        return false;
    }

protected:
    // This method applies the result of the background compilation in a
    // foreground thread.  This method is called while holding the GIL.
    virtual void ApplyImpl() {};
};

// This is a no-op job that just sits on the queue until all jobs before it are
// done.
struct WaitJob : public PyJitJob {
    WaitJob() : PyJitJob() {}

    // This job does nothing, it is just waiting to be processed by the queue.
    virtual JobResult Run(PyGlobalLlvmData *llvm_data) {
        return JOB_OK;
    }
};

// This job will terminate the background thread.
struct ExitJob : public PyJitJob {
    ExitJob() : PyJitJob() {}

    // Return the sentinel value that will exit the background thread.
    virtual JobResult Run(PyGlobalLlvmData *llvm_data) {
        return JOB_EXIT;
    }

    // ExitJobs should be pushed onto the front of the queue so that we can
    // quickly exit the background thread.
    virtual bool ShouldPushFront() {
        return true;
    }
};

// This job will pause the background thread by causing it to acquire the
// unpause event lock twice.
struct PauseJob : public PyJitJob {
    PyThread_type_lock unpause_event_;
    SynchronousQueue<PyJitJob*> &in_queue_;
    SynchronousQueue<PyJitJob*> &out_queue_;

    PauseJob(PyThread_type_lock unpause_event,
             SynchronousQueue<PyJitJob*> &in_queue,
             SynchronousQueue<PyJitJob*> &out_queue)
        : PyJitJob(),
          unpause_event_(unpause_event),
          in_queue_(in_queue),
          out_queue_(out_queue) {}

    // Acquire the unpause event lock so that we wait for it.
    virtual JobResult Run(PyGlobalLlvmData *llvm_data) {
        // Prepare to block by acquiring the unpause event lock twice.
        assert(PyThread_acquire_lock(this->unpause_event_, /*block=*/0) &&
               "Unpause event lock was already acquired!");

        // Acquire the lock on the queues so that no one can use them while
        // we're paused.  Also note that order matters, and in_queue_ should be
        // acquired before out_queue_.
        this->in_queue_.acquire_queue(/*block=*/true);
        this->out_queue_.acquire_queue(/*block=*/true);

        // We need to notify whoever is waiting on this job before we block.
        assert(this->waiter_lock_ && "Pause jobs require a waiter lock.");
        PyThread_release_lock(this->waiter_lock_);
        // NULL this out so we don't try to notify it later.
        this->waiter_lock_ = NULL;

        // At this point, there is sort of a race between the thread that paused
        // the compilation thread and this next acquire.  In either case, after
        // the acquire we will hold the lock because only one pause job can be
        // processed at a time.

        // Block until we are unpaused.
        PyThread_acquire_lock(this->unpause_event_, /*block=*/1);

        // Leave the unpause event lock in the released state.
        PyThread_release_lock(this->unpause_event_);

        // Release the locks on the queues.
        this->in_queue_.release_queue();
        this->out_queue_.release_queue();
        return JOB_OK;
    }

    // Pausing the queue has high priority.
    virtual bool ShouldPushFront() {
        return true;
    }

    // If we try to pause the thread after its stopped, acquire the lock once,
    // so that it can be properly released later.
    virtual void RunAfterShutdown(PyGlobalLlvmData *llvm_data) {
        PyThread_acquire_lock(this->unpause_event_, /*block=*/1);
    }
};

// This job compiles a code object into LLVM IR, optimizes it, and generates
// native code.
struct CompileJob : public PyJitJob {
    // The code object that we are compiling.  If this pointer is NULL, we do
    // nothing.
    PyCodeObject *code_;

    // Before compilation, this is llvm::Function wrapper object that was
    // already on the code object.  After compilation, this is the wrapper that
    // was the result of compilation.
    _LlvmFunction *llvm_function_;

    // This is a function pointer to the native code.
    PyEvalFrameFunction native_function_;

    // This is the old optimization level that was on the code object.  We don't
    // read it from the code object, because that might cause a data race.
    int old_opt_level_;

    // The new level of optimization we want to go to.
    int new_opt_level_;

#ifdef Py_WITH_INSTRUMENTATION
    // The number of compile jobs that have ever been run.
    static int compile_count;

    // The number of compile jobs that were skipped after closing the queue.
    static int skipped_compile_count;

    // TODO: Add instrumentation to find out how much time jobs spend on the
    // queue.
#endif  // Py_WITH_INSTRUMENTATION

    CompileJob(PyCodeObject *code, _LlvmFunction *llvm_func,
               PyEvalFrameFunction native_func, int old_level, int new_level)
        : PyJitJob(), code_(code), llvm_function_(llvm_func),
          native_function_(native_func), old_opt_level_(old_level),
          new_opt_level_(new_level) {
        Py_INCREF(this->code_);
    }

    // Decref the code object for this job if there is one.  This requires that
    // the caller be holding the GIL, so the compilation thread cannot delete
    // jobs.
    virtual ~CompileJob() {
        PyEval_AssertLockHeld();
        Py_XDECREF(this->code_);
    }

    // Compile the code.
    virtual JobResult Run(PyGlobalLlvmData *llvm_data) {
#ifdef Py_WITH_INSTRUMENTATION
        ++compile_count;
#endif

        // Create the IR.
        // TODO(rnk): Once we start clearing out function bodies, we'll need to
        // unconditionally regenerate and reoptimize the IR.
        if (this->llvm_function_ == NULL) {
            PY_LOG_TSC_EVENT(LLVM_COMPILE_START);
            // TODO(rnk): This will read feedback data from the code object
            // which has a potential race.
            this->llvm_function_ = _PyCode_ToLlvmIr(this->code_, this->err_,
                                                    llvm_data);
            PY_LOG_TSC_EVENT(LLVM_COMPILE_END);
            if (this->llvm_function_ == NULL) {
                return JOB_ERROR;
            }
        }

        // Optimize the IR.
        if (this->old_opt_level_ < this->new_opt_level_) {
            if (PyGlobalLlvmData_Optimize(llvm_data, this->llvm_function_,
                                          this->new_opt_level_) < 0) {
                this->err_->SetError(PyExc_SystemError, "Optimization failed");
                return JOB_ERROR;
            }
        }

        // JIT the IR to native code.
        // TODO: Deal with recompiling native code.
        PY_LOG_TSC_EVENT(JIT_START);
        this->native_function_ = _LlvmFunction_Jit(llvm_data,
                                                   this->llvm_function_);
        PY_LOG_TSC_EVENT(JIT_END);
        assert(this->native_function_ &&
               "Native code generation failed!");
        return JOB_OK;
    }

    // Do not run compile jobs during or after shutdown.
    virtual void RunAfterShutdown(PyGlobalLlvmData *llvm_data) {
#ifdef Py_WITH_INSTRUMENTATION
        ++skipped_compile_count;
#endif
    }

    // Update the code object to use the results of the compilation.
    virtual void ApplyImpl() {
        PyCodeObject *code = this->code_;
        code->co_being_compiled = 0;
        code->co_use_llvm = (this->native_function_ != NULL);
        code->co_optimization = this->new_opt_level_;
        code->co_llvm_function = this->llvm_function_;
        code->co_native_function = this->native_function_;
    }
};

#ifdef Py_WITH_INSTRUMENTATION
int CompileJob::compile_count = 0;
int CompileJob::skipped_compile_count = 0;
#endif  // Py_WITH_INSTRUMENTATION

// This job frees an _LlvmFunction wrapper, its native code, and its
// llvm::Function.  This must be done in the background, because it modifies the
// llvm::Module object and the ExecutionEngine.
struct FreeJob : public PyJitJob {
    // This is the _LlvmFunction that we want to free.
    _LlvmFunction *llvm_function_;

    FreeJob(_LlvmFunction *llvm_func) : PyJitJob(), llvm_function_(llvm_func) {}

    virtual JobResult Run(PyGlobalLlvmData *llvm_data) {
        assert(this->llvm_function_ && "Asked to deallocate NULL function!");
        llvm::Function *function = static_cast<llvm::Function *>(
                this->llvm_function_->lf_function);
        // Allow global optimizations to destroy the function.
        function->setLinkage(llvm::GlobalValue::InternalLinkage);
        if (function->use_empty()) {
            // Delete the function if it's already unused.
            // Free the machine code for the function first, or LLVM will try to
            // reuse it later.  This is probably a bug in LLVM. TODO(twouters):
            // fix the bug in LLVM and remove this workaround.
            llvm::ExecutionEngine *engine = llvm_data->getExecutionEngine();
            engine->freeMachineCodeForFunction(function);
            function->eraseFromParent();
        }
        delete this->llvm_function_;
        return JOB_OK;
    }
};

PyLlvmCompileThread::PyLlvmCompileThread(PyGlobalLlvmData *llvm_data)
    : llvm_data_(llvm_data), running_(false), tstate_(NULL) {
    // This assumes that the thread is constructed from a foreground Python
    // thread.
    this->interpreter_state_ = PyThreadState_GET()->interp;
    this->unpause_event_ = PyThread_allocate_lock();
}

PyLlvmCompileThread::~PyLlvmCompileThread()
{
    PyThread_free_lock(this->unpause_event_);
#ifdef Py_WITH_INSTRUMENTATION
    fprintf(stderr, "Compilation thread statistics:\n");
    fprintf(stderr, "compile jobs completed: %d\n", CompileJob::compile_count);
    fprintf(stderr, "compile jobs skipped at thread termination: %d\n",
            CompileJob::skipped_compile_count);
#endif
}

void
PyLlvmCompileThread::OutputFinishedJob(PyJitJob *job)
{
    // Pull the waiter lock out of the job before it hits the output queue,
    // because otherwise we're in a race with the main thread to read this
    // before it frees the job.
    PyThread_type_lock waiter_lock = job->waiter_lock_;

    // Put the result of the action on the output queue.  This takes
    // ownership of the job.
    this->out_queue_.push_back(job);

    // Alert anybody who might be blocked waiting for this job to finish.  This
    // should be done after we put the job on the output queue, so that anyone
    // waiting can call this->ApplyFinishedJobs(PY_BLOCK) to make the job take
    // effect.
    if (waiter_lock) {
        PyThread_release_lock(waiter_lock);
    }
}

void
PyLlvmCompileThread::Run()
{
    // Create a new thread state.
    assert(this->tstate_ == NULL);
    this->tstate_ = PyThreadState_New(this->interpreter_state_);

    // Consume and run jobs from the input queue until one tells us to exit.
    bool done = false;
    while (!done) {
        PyJitJob *job = this->in_queue_.pop_front();

        // If no one is listening for errors, we will catch and log them to
        // stderr.
        PyLlvmError err;
        if (job->err_ == NULL) {
            job->err_ = &err;
        }

        switch (job->Run(this->llvm_data_)) {
        default: assert(0 && "invalid enum value");
        case PyJitJob::JOB_EXIT:
            // Quickly run all the jobs left on the queue and break out of this
            // loop to exit the thread.
            this->DoCleanups();
            done = true;
            break;
        case PyJitJob::JOB_ERROR:
            if (job->err_ == &err) {
                // Print the exception to stderr if we're the one who signed up
                // to listen for these errors.
                job->err_->RaiseUnraisableIfError(this->tstate_);
            }
            break;
        case PyJitJob::JOB_OK:
            break;
        }

        this->OutputFinishedJob(job);
    }
}

void
PyLlvmCompileThread::DoCleanups()
{
    // Clear out the rest of the queue and abandon the jobs.
    std::deque<PyJitJob*> &queue =
            *(this->in_queue_.acquire_queue(/*block=*/true));
    while (!queue.empty()) {
        PyJitJob *job = queue[0];
        queue.pop_front();
        job->RunAfterShutdown(this->llvm_data_);
        this->OutputFinishedJob(job);
    }

    // Close the queue to any more jobs.  We have to hold the lock on the queue
    // when we do this, because if they were not atomic, foreground threads
    // would either put jobs on the queue after we've terminated, or prematurely
    // start freeing functions while we're still cleaning up.
    this->in_queue_.close();

    this->in_queue_.release_queue();

    PyThreadState_Clear(this->tstate_);
    PyThreadState_Delete(this->tstate_);
    this->tstate_ = NULL;
}

int
PyLlvmCompileThread::RunJob(PyJitJob *job)
{
    PyEval_AssertLockHeld();
    int r;
    if (job->ShouldPushFront()) {
        r = this->in_queue_.push_front(job);
    } else {
        r = this->in_queue_.push_back(job);
    }
    if (r < 0) {
        // If we fail to push the job on the queue, that means that the
        // background thread has terminated.  If the background thread is
        // terminated, then it's safe to modify the LLVM global data in any
        // thread holding the GIL, so we just do exactly what the background
        // thread would have done.
        job->RunAfterShutdown(this->llvm_data_);

        // Rather than pushing it on to the output queue, since we already hold
        // the GIL, we apply the job and delete it here.
        job->Apply();
        delete job;
        return -1;
    }
    return 0;
}

void
PyLlvmCompileThread::RunJobAndWait(PyJitJob *job)
{
    // Create a lock for the job, acquire it, and put the job on the queue.
    assert(job->waiter_lock_ == NULL && "Job already had a waiter_lock!");
    PyThread_type_lock lock = PyThread_allocate_lock();
    job->waiter_lock_ = lock;
    PyThread_acquire_lock(lock, 1);
    if (this->RunJob(job) < 0) {
        // If we couldn't put it on the queue, don't wait for it, because it's
        // already done.
        PyThread_free_lock(lock);
        return;
    }

    // Try to acquire the lock again to wait until the lock is released by the
    // background thread.  This may take awhile, so we release the GIL while we
    // wait.
    Py_BEGIN_ALLOW_THREADS;
    PyThread_acquire_lock(lock, 1);

    // When we get control back, free the lock and reacquire the GIL.
    PyThread_free_lock(lock);
    Py_END_ALLOW_THREADS;
}

void
PyLlvmCompileThread::RunJobAndApply(PyJitJob *job)
{
    this->RunJobAndWait(job);
    // Call this to make sure all compilations take effect before we return.
    this->ApplyFinishedJobs(PY_BLOCK);
}

void
PyLlvmCompileThread::ApplyFinishedJobs(Py_ShouldBlock block)
{
#ifdef WITH_BACKGROUND_COMPILATION
    std::deque<PyJitJob*> *maybe_queue =
            this->out_queue_.acquire_queue(block == PY_BLOCK);
    if (maybe_queue == NULL) {
        assert(block == PY_NO_BLOCK && "We should only fail to get the queue "
               "in a non-blocking application attempt.");
        return;  // We couldn't acquire the lock, so we'll try again later.
    }
    std::deque<PyJitJob*> &queue = *maybe_queue;
    while (!queue.empty()) {
        PyJitJob *job = queue[0];
        queue.pop_front();
        job->Apply();
        delete job;
    }
    this->out_queue_.release_queue();
#endif // WITH_BACKGROUND_COMPILATION
}

void
PyLlvmCompileThread::Terminate()
{
#ifdef WITH_BACKGROUND_COMPILATION
    this->RunJobAndApply(new ExitJob());
    this->running_ = false;
#endif // WITH_BACKGROUND_COMPILATION
}

void
PyLlvmCompileThread::WaitForJobs()
{
#ifdef WITH_BACKGROUND_COMPILATION
    this->RunJobAndApply(new WaitJob());
#endif // WITH_BACKGROUND_COMPILATION
}

Py_CompileResult
PyLlvmCompileThread::JitInBackground(PyCodeObject *code, int new_opt_level,
                                     Py_ShouldBlock block)
{
    CompileJob *job = new CompileJob(code, code->co_llvm_function,
                                     code->co_native_function,
                                     code->co_optimization, new_opt_level);

#ifdef WITH_BACKGROUND_COMPILATION
    if (block == PY_BLOCK) {
        // If we're going to block, then we want to properly report the
        // exception.
        llvm::OwningPtr<PyLlvmError> err(new PyLlvmError());
        job->err_ = err.get();
        this->RunJobAndApply(job);

        // If there was an exception, raise it in this foreground thread.
        if (err->RaiseIfError()) {
            return PY_COMPILE_ERROR;
        } else if (!code->co_llvm_function || !code->co_native_function) {
            PyErr_SetString(PyExc_SystemError, "Compilation had no effect;"
                            " is the compilation thread running?");
            return PY_COMPILE_SHUTDOWN;
        }
    } else if (block == PY_NO_BLOCK) {
        // We ignore the return value here because if we're shutting down then
        // we don't care about compilation.
        this->RunJob(job);
    } else {
        assert(0 && "invalid enum value");
    }
    return PY_COMPILE_OK;
#else // WITH_BACKGROUND_COMPILATION
    // Use an OwningPtr to delete on return.
    llvm::OwningPtr<PyLlvmError> err(new PyLlvmError());
    llvm::OwningPtr<PyJitJob> job_owner(job);
    job->err_ = err.get();
    PyJitJob::JobResult r = job->Run(this->llvm_data_);
    job->Apply();
    switch (r) {
    default: assert(0 && "invalid enum value");
    case PyJitJob::JOB_EXIT:
        assert(0 && "compile job should not return JOB_EXIT");
    case PyJitJob::JOB_OK:
        return PY_COMPILE_OK;
    case PyJitJob::JOB_ERROR:
        bool raised = err->RaiseIfError();
        raised = raised;  // Silence unused value warning in release mode.
        assert(raised && "Error code returned, but no exception!");
        return PY_COMPILE_ERROR;
    }
#endif // WITH_BACKGROUND_COMPILATION
}

void
PyLlvmCompileThread::FreeInBackground(_LlvmFunction *functionobj)
{
    PyJitJob *job = new FreeJob(functionobj);
#ifdef WITH_BACKGROUND_COMPILATION
    this->RunJob(job);
#else // WITH_BACKGROUND_COMPILATION
    job->Run(this->llvm_data_);
    job->Apply();
#endif // WITH_BACKGROUND_COMPILATION
}

void
PyLlvmCompileThread::Bootstrap(void *thread)
{
    ((PyLlvmCompileThread*)thread)->Run();
}

void
PyLlvmCompileThread::Start()
{
#ifdef WITH_BACKGROUND_COMPILATION
    if (this->running_) {
        return;
    }
    this->running_ = true;

    // Reopen the queue if we closed it before forking.
    this->in_queue_.open();

    PyEval_InitThreads(); /* Start the interpreter's thread-awareness */
    PyThread_start_new_thread(Bootstrap, this);
#endif // WITH_BACKGROUND_COMPILATION
}

void
PyLlvmCompileThread::Pause()
{
#ifdef WITH_BACKGROUND_COMPILATION
    this->RunJobAndWait(new PauseJob(this->unpause_event_, this->in_queue_,
                                     this->out_queue_));
    // Note that we leak the pause job in the child process during a fork.  This
    // is OK because we leak all kinds of other things, like locks and objects
    // owned by other threads.
#endif // WITH_BACKGROUND_COMPILATION
}

void
PyLlvmCompileThread::Unpause()
{
#ifdef WITH_BACKGROUND_COMPILATION
    PyThread_release_lock(this->unpause_event_);
#endif // WITH_BACKGROUND_COMPILATION
}

void
PyLlvmCompileThread::ResetAfterFork()
{
#ifdef WITH_BACKGROUND_COMPILATION
    // TODO: Rather than leak the lock's memory, we could reinitialize it.
    this->unpause_event_ = PyThread_allocate_lock();

    // Reset the queues which were acquired in the pause job.
    this->in_queue_.reset_after_fork();
    this->out_queue_.reset_after_fork();

    // Close the input queue as we would normally do if we were being
    // terminated.
    this->in_queue_.acquire_queue(/*block=*/true);
    this->in_queue_.close();
    this->in_queue_.release_queue();

    // Clear and free this because starting the thread reallocates it.
    if (this->tstate_ != NULL) {
        PyThreadState_Delete(this->tstate_);
        this->tstate_ = NULL;
    }

    // This must be set to false before calling Start.
    this->running_ = false;
#endif // WITH_BACKGROUND_COMPILATION
}

extern "C" {
    void
    PyLlvm_StartCompilation()
    {
        PyGlobalLlvmData::Get()->getCompileThread()->Start();
    }

    void
    PyLlvm_StopCompilation()
    {
        PyGlobalLlvmData::Get()->getCompileThread()->Terminate();
    }

    void
    PyLlvm_PauseCompilation()
    {
        PyGlobalLlvmData::Get()->getCompileThread()->Pause();
    }

    void
    PyLlvm_UnpauseCompilation()
    {
        PyGlobalLlvmData::Get()->getCompileThread()->Unpause();
    }

    void
    PyLlvm_ResetAfterFork()
    {
        PyGlobalLlvmData::Get()->getCompileThread()->ResetAfterFork();
    }

    Py_CompileResult
    PyLlvm_JitInBackground(PyCodeObject *code, int new_opt_level,
                           Py_ShouldBlock block)
    {
        return PyGlobalLlvmData::Get()->getCompileThread()->JitInBackground(
                code, new_opt_level, block);
    }

    void
    PyLlvm_ApplyFinishedJobs(Py_ShouldBlock block)
    {
        PyGlobalLlvmData::Get()->getCompileThread()->ApplyFinishedJobs(block);
    }

    void
    PyLlvm_WaitForJit()
    {
        PyGlobalLlvmData::Get()->getCompileThread()->WaitForJobs();
    }
}

#endif // WITH_LLVM
