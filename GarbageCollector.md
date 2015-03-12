# Garbage Collection Development Plan #

## Plan Update 25/03/2009 ##

Talin: The garbage collector has been split off into its own development project. The primary reason for this is that much of the work is not specific to Python, and there are a number of goals which are not Python related. It turns out that there are a lot of people out there who would like a general-purpose garbage collector that is compatible with LLVM.

The new project is called "Scarcity". The code project and source repository are still in the process of being set up, however a fair amount of work on the code has been accomplished already.

## Goals ##

The primary goal is to produce a workable concurrent garbage collector that can be used for an optimized Python. A secondary goal is to make the collector independent enough from Python to be usable for other languages.

  * Built on top of LLVM using the LLVM GC intrinsics (where appropriate).
  * Fully open-source.
  * Written in C++.
  * "Java-like" object model and semantics, meaning that the feature set is compatible with the requirements of Java, C#, Python, Ruby and similar languages. Although this does not preclude the GC from being used with languages such as Haskell, Prolog or Scheme, we won't be focusing any effort on supporting those languages.
  * Able to operate in multithreaded environments.
  * A flexible architecture that promotes innovation and incremental improvements from external contributors.
  * Support for weak references and finalization.

Note: The [terminology](http://chaoticjava.com/posts/parallel-and-concurrent-garbage-collectors/) used in the garbage collector literature is somewhat inconsistent with respect to concurrency. In this document, we will refer to a "concurrent" GC as one that runs at the same time as the mutator threads, whereas a "parallel" GC is one in which there are multiple collector threads.

## Overall Architecture ##

It is not our intent to come up with a novel algorithm for a GC, but rather to leverage the work that has already been done. Unfortunately, it is infeasible to evaluate the merits of all of the various collectors out there, since very few GCs are open source or even available in binary form.

However, what we can do is notice that over the last few years there has been a gradual convergences of GC architectures into a small number of designs which have gained widespread popularity. From this set, we are currently considering two contenders:

  1. Some form of advanced, asynchronous reference-counting, such as used in Recycler.
  1. A multi-generation, hybrid copying and mark-and-sweep collector.

(Insert discussion of pros and cons of each.)

The remainder of this document will outline the design of the generational collector.

## Heap Management ##

## Tracing ##

### Heap Object Tracing ###

From the point of view of the collector, the heap consists of a sequence of allocated blocks (each representing the storage of a single object, plus any administrative overhead needed by the heap manager), of which there are two types: _tagged_ blocks, which start with a standardized object header, and _raw_ blocks, which do not.

Tagged blocks are presumed to contain a pointer to the type information for the object contained in the block. The collector does not need to know the offset of the type pointer, nor does it need to know the format of data that it points to. Instead, the host environment must supply an inline function that can examine the object header and return a reference to a _pointer offset map_ for that object. This offset map specifies the offset of every pointer within the block, relative to the start of the block.

For raw blocks, the pointer location map is not derived from an examination of the block header, but instead must be passed in as a parameter to the tracer. This implies that the code that initiates the trace of a raw block must have knowledge of what type of object it is. In most cases a raw block will be referenced by a tagged block. The offset map for the tagged object includes an entry that describes reference to the raw block, and this entry contains a pointer to the raw block's offset map.

The offset map will have support for variable-length data structures. Most variable-length data structures consist of a fixed length header, followed a sequence of repeating elements, each identical in size and structure. The header part contains a field that indicates the number of repetitions, either as an integer count, or as a pair of pointers indicating the start and end of the array. In some cases, the fixed part and the variable-length part may be two separate allocations, in which case the variable length part is a raw block. The offset map for the fixed-length part will contain the offset of the count field, and the offset and stride of the elements in the variable-length part.

In some cases, the structure of the object may be too complex or variable to describe via offset maps, in which case the environment can specify a function pointer that can enumerate all of the pointers in an object. This function accepts as a parameter a visitor function pointer which should be called for each pointer field to be traced.

In designing the format of the offset map, an important consideration will be making the interpretation of that map as fast as possible, minimizing the number of pointer indirections or complex conditionals that have to be performed in order to interpret the data. At the same time, however, some runtime environments (ones that have a small number of fixed object layouts, such as Python) may want allow the offset maps to be defined manually using statically initialized data structures (as opposed to having the offset maps be generated by the compiler.) It may turn out that these two goals are in conflict.

One decision to be made is whether or not raw blocks can be queued for tracing. For tagged blocks, the information required to trace the block is a single pointer to the starting address of the allocated block. For raw blocks, however, the information needed is potentially two pointers and an integer: The pointer to the allocated block, the pointer to the offset map, and the repeat count. This increases the size of the individual entries in the queue of blocks not yet traced. However, if we mandate that all raw blocks must be traced synchronously (i.e. if you encounter a reference to a raw block, it is traced immediately rather than being placed on the queue), then this can be avoided. An alternative solution is to have two queues, one for tagged blocks and one for raw blocks.

Note that some write barrier designs may require an immediate re-tracing of a block. If the block to be traced is a raw block, then the write barrier will need to pass the information about the offset map to the tracer. Whether this information can "fit" within the parameters specified by the LLVM barrier intrinsics remains to be seen.

Some additional alternatives to consider:

  * Don't have raw objects at all, and mandate that every object be tagged.
  * Reserve space in the memory allocation header for the tag. Since the memory allocation header comes before the 'nominal' start address of the object, and thus is external to the object proper. (Since a free block header consumes more space than an allocated block header, there is generally some spare room in the allocated block header for things like this.) A downside of this approach is that raw blocks can never be defined statically, since it is illegal to apply a negative offset to statically defined data.
  * Use the size of the allocation itself (which is stored in the allocated block header) to determine the number of repeated elements, rather than requiring it to be passed in. Note that the size may be rounded up to the nearest minimal alignment boundary (because the low-bits of the size field are being used for control bits), so it may not be possible to determine the exact size of the object this way.

#### Heap Object Tracing (an entirely different approach) ####

An somewhat more complex approach to tracing, but which may yield significant performance benefits, is to have the compiler generate a series of custom trace functions for each class. Here's how this works: When the compiler compiles a new object type, it will also produce in LLVM IR form, a function that can enumerate all of the pointer fields of an object. Rather than taking a visitor pointer as an argument, we can avoid the indirect call by generating a separate version of this function for each possible visitor type. The collector supplies a generator function that generates the small sequence of LLVM IR instructions for the visitor function, and the class compiler calls this function once for every pointer field in the object.

For example, suppose a collector has three types of tracers: A "mark" tracer, which marks objects as reachable; A "forwarding" tracer which fixes up references to objects which have moved, and a "debug" tracer which prints out an object graph. The collector supplies 3 generator objects. The "mark function" generator generates the instructions which, given the address of a pointer, marks the object pointed to as reachable and adds it to the queue of objects to be scanned; The forwarding and debug tracer similarly generate LLVM IR instructions to do their work.

When a new class Foo is defined, the compiler also outputs 3 tracer functions, one for each tracer type. So we have a marker tracer for type Foo, a forwarding tracer for type Foo, and a debug tracer for type Foo. If class Foo has repeating elements, this is done merely by emitting code for a loop within the tracer function.

The result is that instead of a table of data that must be interpreted, the tracer functions are now inlined, optimized native code that can run extremely fast.

The primary cost of this approach is that it means that the choice of collector algorithm must be known at compile time. (In JIT environments it might be possible to generate the tracing functions at the time that the class is loaded, using the class reflection information.)

Also note that the issues with raw blocks requiring additional metadata to be passed around are not obviated by this technique - for a raw block, there is still the problem of identifying which tracer function to use, and determining the number of repeating elements for a variable-length raw block with no header.

Also, there are cache and locality considerations to consider when comparing the relative performance of the native code vs. data table approach.

### Stack Roots ###

LLVM provides a way to mark local variables as 'roots':

> `void @llvm.gcroot(i8** %ptrloc, i8* %metadata)`

LLVM doesn't do anything with these markers other than preserve them through the various optimization transforms. We will need to write a plugin class which gathers these markers and uses them to generate a map of pointer locations.

The format of the stack layout map will likely be identical to the offset map for heap allocated objects - we can simply treat each stack frame exactly like we would treat a raw block.

Although LLVM currently provides a method for locating pointers in a given stack frame (assuming that you know which function you are currently in), it does not provide any method for determining which stack map to use for an arbitrary stack frame. This means that we will need to develop our own methods for walking the stack and identifying which stack maps correspond to the various stack frames. Libunwind may be of some help in this area.

### Global Roots ###

For Python and similar languages, it is likely that there will be two basic kinds of global roots: Statically allocated data structures (such as built-in classes and extension classes), and objects which are created at the time the interpreter starts up.

It's possible that the root could consist of just a single pointer to a global namespace, which points to other namespaces, and so on. However, that may not be the most efficient solution, because it means that the collector will repeatedly try (and fail) to reclaim objects that are never going away, and thus wasting effort.

One strategy is to define an "immortal" memory region (similar to what MMTk does). Objects which are allocated within this region are scanned for pointers, but are never reclaimed.

## Synchronization ##

The initial implementation of the collector will utilize a "stop the world" strategy with explicit synchronization points that will be inserted into the generated code by LLVM.

The initial implementation of sync points will be done using reader/writer locks (such as provided by pthreads). The collector library initializes a global rwlock called "enable\_mutate". All mutator threads hold a read lock on this lock while they are running. A sync point consists of releasing and then immediately re-acquiring this lock.

The collector attempts to acquire a write lock before beginning a collection. This will block until all mutators have reached a sync point, at which time the collector can acquire the lock, thereby blocking all mutators until it is finished.

During a sync point, objects may move around in memory. A mutator must be prepared to handle the fact that pointer values will have changed during the sync point, so pointer values cached in registers will no longer be valid.

Long blocking operations such as i/o should release the lock while the operation is in progress (of course, any memory buffers involved in the i/o itself must be pinned before dropping the lock.)

## Barriers ##

## Pinning Support ##

## Weak Pointer Support ##

## Finalization Support ##