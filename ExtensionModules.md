# Compatibility Goals #

The goal is source-level compatibility. Most existing third-party extension modules should require only a recompilation, with minimal source changes.

Binary compatibility is not an option, because many of the performance improvements being contemplated require changing the layout of internal data structures, or otherwise changing the ABI between extension module and interpreter.

# Reference Counting #

In order to make Python efficient in a multiprocessor system, we'll need to change the way reference counts work. The current implementation of reference counts use non-atomic operations which require the GIL for consistency. Removing the GIL will require using a concurrency-safe strategy for managing object lifetimes, such as one of the following:

  * Traditional reference counting with atomic increment / decrement.
  * Thread-local Addref/Release queues, as described in the Recycler paper.
  * True concurrent garbage collection.

Currently extension modules manage reference counts via macros such as Py\_INCREF and Py\_DECREF, which are provided by the Python header files. We will need to replace these macros with ones that conform to the new collection strategy.

For the short term, traditional reference counting, but with atomic operations, appears to be the best choice since it can be implemented with relatively little effort. The main downside of this approach is that it has the worst performance. There is anecdotal evidence that Python with atomic operations slows down by a factor of 2, although this may still be a win on a 4-processor system. Also, it may be possible to reduce the number of addref/release pairs during native code generation by doing variable lifetime analysis during the optimization phase.

For the longer term, true garbage collection is likely to yield the best result.

In the context of a garbage collector, Python objects passed to external module functions would be pinned in memory before calling the external code, and unpinned on return. The Py\_INCREF and Py\_DECREF macros would then be repurposed to mean the reference count on the pinned state rather than on the object liveness: That is, the object would remain pinned until its reference count fell to zero, at which point it would not be deallocated but rather subject to normal collection cycles. While pinned, the object would also be considered part of the collector's root set.