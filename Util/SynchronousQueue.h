#ifndef UTIL_SYNCHRONOUSQUEUE_H
#define UTIL_SYNCHRONOUSQUEUE_H

#include "Python.h"
#include "pythread.h"

#include <deque>

// Simple C++ class that acquires and releases a pythreads lock.  This is a
// straightforward copy of LLVM's MutexGuard.
class PyMutexGuard {
    PyThread_type_lock lock_;
    PyMutexGuard(const PyMutexGuard &);   // DO NOT IMPLEMENT
    void operator=(const PyMutexGuard &); // DO NOT IMPLEMENT

public:
    PyMutexGuard(PyThread_type_lock lock) : lock_(lock) {
        // Passing 1 as the second argument causes the acquire to block until
        // we can acquire the lock.
        PyThread_acquire_lock(this->lock_, 1);
    }

    ~PyMutexGuard() {
        PyThread_release_lock(this->lock_);
    }
};

// This is a simple thread-safe queue.  It is designed for communication between
// many producers and a single consumer, and will not work properly if there are
// multiple consumers.
template <class T>
class SynchronousQueue {
public:
    SynchronousQueue() {
        this->lock_ = PyThread_allocate_lock();
        this->push_event_ = PyThread_allocate_lock();
        this->closed_ = false;
    }

    ~SynchronousQueue() {
        // The queue may or may not be closed at destruction time.
        PyThread_free_lock(this->lock_);
        PyThread_free_lock(this->push_event_);
    }

    // Close the queue so that it can take no more input.  Call this when the
    // queue reader is about to terminate, to prevent any more writes.  The
    // caller must have acquired the lock on the queue by calling
    // acquire_queue.
    void close() {
        assert(!PyThread_acquire_lock(this->lock_, 0) &&
               "Caller did not hold lock while closing queue!");
        this->closed_ = true;
    }

    // Open the queue so it can take input again.
    void open() {
        PyMutexGuard locked(this->lock_);
        this->closed_ = false;
    }

    // Push an element onto the front of the queue, or return -1 if no one is
    // reading from the queue.
    int push_front(const T &elt) {
        PyMutexGuard locked(this->lock_);
        if (this->closed_) {
            return -1;
        }
        this->queue_.push_front(elt);
        this->push_notify();
        return 0;
    }

    // Push an element onto the end of the queue, or return -1 if no one is
    // reading from the queue.
    int push_back(const T &elt) {
        PyMutexGuard locked(this->lock_);
        if (this->closed_) {
            return -1;
        }
        this->queue_.push_back(elt);
        this->push_notify();
        return 0;
    }

    // Pop an element off of the front of the queue and return it.  If the queue
    // is empty, block until there is an element to remove.
    T pop_front() {
        while (true) {
            // Extra scope so that we release the lock while we wait.
            {
                PyMutexGuard locked(this->lock_);
                assert(!this->closed_ &&
                       "Tried to pop elements from a closed queue!");
                if (!this->queue_.empty()) {
                    T elt = queue_[0];
                    this->queue_.pop_front();
                    return elt;
                }
            }

            // Wait for notification from a thread pushing an element.  If the
            // lock was left in a released state, then we will go through the
            // loop a second time, which is OK.
            PyThread_acquire_lock(this->push_event_, 1);
        }
    }

    // Acquire the main lock and return the queue.  This allows users to do
    // things like emptying the queue without acquiring and releasing the lock
    // several times.
    std::deque<T> *acquire_queue(bool block) {
        bool acquired =
                PyThread_acquire_lock(this->lock_, block);
        if (!acquired)
            return NULL;
        return &this->queue_;
    }

    // This is how the user releases the lock after they are done with the
    // queue.
    void release_queue() {
        PyThread_release_lock(this->lock_);
    }

    // Reset the locks on the queue in the child process after a fork.  The
    // locks should be held across the fork to ensure that the deque is not left
    // in an undefined state.  You would think if the locks were held that they
    // could just be released, but it is unfortunately buggy on some platforms
    // to release pythreads locks held across forks.
    // TODO: Instead of leaking the memory, if there were a pythreads call that
    // would reinitialize a lock, we could just reuse the memory.
    void reset_after_fork() {
        this->lock_ = PyThread_allocate_lock();
        this->push_event_ = PyThread_allocate_lock();
    }

private:
    // The lock guarding the queue.  This is a pythreads lock because it must be
    // safe to acquire and release from different threads after a fork.
    PyThread_type_lock lock_;

    // The lock that the consumer waits on, and the producer releases.  We use
    // this to implement an event variable on top of the pythreads API, as is
    // done in the pure Python threading module.
    PyThread_type_lock push_event_;

    // The underlying queue.
    std::deque<T> queue_;

    // Whether or not the queue is closed to further input.  Clients can use
    // this to block further input after the consumer thread terminates.
    bool closed_;

    // Notify any threads waiting on this lock.  The caller must hold
    // this->lock_.  It is important that we don't release the push event lock
    // twice, so we do a non-blocking attempt to acquire the lock.  These are
    // the possible cases:
    //
    // - If the lock was already acquired, then we fail to acquire it and we
    //   release it, notifying the other thread.
    // - If no one is waiting for an element, then we will leave the lock
    //   released, which is its initial state.
    // - If a consumer thread tries to acquire the lock at the end of pop_front,
    //   it will be able to successfully acquire the lock after we release it.
    void push_notify() {
        assert(!PyThread_acquire_lock(this->lock_, 0) &&
               "Caller did not hold lock during push_notify!");
        PyThread_acquire_lock(this->push_event_, 0);
        PyThread_release_lock(this->push_event_);
    }
};

#endif // UTIL_SYNCHRONOUSQUEUE_H
