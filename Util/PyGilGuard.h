// -*- C++ -*-
//
// Defines PyGilGuard, which is a simple C++ RAII-style class for holding the
// Python GIL.
#ifndef UTIL_PYGILGUARD_H
#define UTIL_PYGILGUARD_H

// Simple RAII-style C++ class that acquires and releases the Python GIL and
// swaps in the provided thread state, if it is not NULL.
class PyGilGuard {
    PyThreadState *tstate_;

    PyGilGuard(const PyGilGuard &);     // DO NOT IMPLEMENT
    void operator=(const PyGilGuard &); // DO NOT IMPLEMENT

public:
    PyGilGuard(PyThreadState *tstate) : tstate_(tstate) {
        if (this->tstate_ != NULL) {
            PyEval_AcquireThread(this->tstate_);
        }
    }

    ~PyGilGuard() {
        if (this->tstate_ != NULL) {
            PyEval_ReleaseThread(this->tstate_);
        }
    }
};

#endif // UTIL_PYGILGUARD_H
