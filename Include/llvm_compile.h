#ifndef PYTHON_LLVM_COMPILE_H
#define PYTHON_LLVM_COMPILE_H

#ifdef WITH_LLVM

#ifdef __cplusplus

// Python exceptions are not thread safe, so we wrap them up in this structure
// and define functions similar to the standard exception raising functions
// that take this wrapper, so that we have a place to put exceptions in the
// LLVM compilation thread.
class PyLlvmError {
public:
    // Initialize a PyLlvm_Error struct to NULL.
    PyLlvmError() {
        this->exc_type = NULL;
        this->exc_value = NULL;
    }

    // We don't hold references so we don't need to decref here.
    ~PyLlvmError() {}

    // This is a function similar to PyErr_SetString that stores data in this
    // instead of in the global thread state.
    void SetError(PyObject *type, const char *msg) {
        this->exc_type = type;
        this->exc_value = msg;
    }

    // Return true if an error occurred.
    bool ErrorOccurred() {
        return this->exc_type != NULL;
    }

    // If an error was signalled, raise it in this thread.  This method must be
    // called while holding the GIL and with a valid thread state installed.
    // Return -1 on error and 0 otherwise.
    int RaiseIfError() {
        PyEval_AssertLockHeld();
        if (!this->ErrorOccurred()) {
            return 0;
        }
        PyErr_SetString(this->exc_type, this->exc_value);
        return -1;
    }

    // If an error was signalled but it cannot be raised in a foreground thread,
    // it is unraisable and is simply printed to stderr.
    void RaiseUnraisableIfError(PyThreadState *tstate) {
        // As a fast path, return early if there is no error.
        if (!this->ErrorOccurred()) {
            return;
        }
        this->RaiseUnraisableIfErrorImpl(tstate);
    }

private:
    // Type of the exception to raise.
    PyObject *exc_type;

    // Message to pass to the exception constructor.
    const char *exc_value;

    // Out of line implementation of writing an unraisable error.
    void RaiseUnraisableIfErrorImpl(PyThreadState *tstate);
};

extern "C" {
#endif /* __cplusplus */

PyAPI_FUNC(_LlvmFunction *) _PyCode_ToLlvmIr(PyCodeObject *code,
                                             PyLlvmError *err,
                                             PyGlobalLlvmData *llvm_data);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* WITH_LLVM */

#endif /* PYTHON_LLVM_COMPILE_H */
