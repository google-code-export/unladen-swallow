<font size='1'>NB: all cited papers are linked on <a href='RelevantPapers.md'>RelevantPapers</a>. A version of this document is also <a href='http://danmarner.yo2.cn/unladen-swallow-project-plan/'>available in Chinese</a>, though we can't vouch for the translation.</font>



# Goals #

We want to make Python faster, but we also want to make it easy for large, well-established applications to switch to Unladen Swallow.

  1. Produce a version of Python at least 5x faster than CPython.
  1. Python application performance should be stable.
  1. Maintain source-level compatibility with CPython applications.
  1. Maintain source-level compatibility with CPython extension modules.
  1. We do not want to maintain a Python implementation forever; we view our work as a branch, not a fork.


# Overview #

In order to achieve our combination of performance and compatibility goals, we opt to modify CPython, rather than start our own implementation from scratch. In particular, we opt to start working on CPython 2.6.1: Python 2.6 nestles nicely between 2.4/2.5 (which most interesting applications are using) and 3.x (which is the eventual future). Starting from a CPython release allows us to avoid reimplementing a wealth of built-in functions, objects and standard library modules, and allows us to reuse the existing, well-used CPython C extension API. Starting from a 2.x CPython release allows us to more easily migrate existing applications; if we were to start with 3.x, and ask large application maintainers to first port their application, we feel this would be a non-starter for our intended audience.

The majority of our work will focus on speeding the execution of Python code, while spending comparatively little time on the Python runtime library. Our long-term proposal is to supplement CPython's custom virtual machine with a JIT built on top of [LLVM](http://llvm.org/), while leaving the rest of the Python runtime relatively intact. We have observed that Python applications spend a large portion of their time in the main eval loop. In particular, even relatively minor changes to VM components such as opcode dispatch have a significant effect on Python application performance. We believe that compiling Python to machine code via LLVM's JIT engine will deliver large performance benefits.

Some of the obvious benefits:
<ul>
<blockquote><li>Using a JIT will also allow us to move Python from a stack-based machine to a register machine, which has been shown to improve performance in other similar languages (Ierusalimschy et al, 2005; Shi et al, 2005).</li>
<li>Eliminating the need to fetch and dispatch opcodes should alone be a win, even if we do nothing else. See <a href='http://bugs.python.org/issue4753'>http://bugs.python.org/issue4753</a> for a discussion of CPython's current sensitivity to opcode dispatch changes.</li>
<li>The current CPython VM opcode fetch/dispatch overhead makes implementing additional optimizations prohibitive. For example, we would like to implement type feedback and dynamic recompilation ala SELF-93 (Hölzle, Chambers and Ungar, 1992), but we feel that implementing the polymorphic inline caches in terms of CPython bytecode would be unacceptably slow.</li>
<li>LLVM in particular is interesting because of its easy-to-use codegen available for multiple platforms and its ability to compile C and C++ to the same intermediate representation we'll be targeting with Python. This will allows us to do inlining and analysis across what is currently a Python/C language barrier.</li>
</ul></blockquote>

With the infrastructure to generate machine code comes the possibility of compiling Python into a much more efficient implementation than what would be possible in the current bytecode-based representation. For example, take the snippet

```
for i in range(3):
  foo(i)
```
This currently desugars to something like
```
$x = range(3)
while True:
  try:
    $y = $x.next()
  except StopIteration:
    break
  i = $y
  foo(i)
```
Once we have a mechanism to know that `range()` means the `range()` builtin function, we can turn this into something more akin to
```
for (i = 0; i < 3; i++)
  foo(i)
```
in C, possibly using unboxed types for the math. We can then unroll the loop to yield
```
foo(0)
foo(1)
foo(2)
```

We intend to structure Unladen Swallow's internals to assume that multiple cores are available for our use. Servers are only going to acquire more and more cores, and we want to exploit that to do more and more work in parallel. For example, we would like to have a concurrent code optimizer that applies increasingly-expensive (and -beneficial!) optimizations in parallel with code execution, using another core to do the work. We are also considering [a concurrent garbage collector](GarbageCollector.md) that would, again, utilize additional cores to offload work units. Since most production server machines are shipping with between 4 and 32 cores, we believe this avenue of optimization is potentially lucrative. However, we will have to be sensitive to the needs of highly-parallel applications and not consume extra cores blindly.

Note that many of the areas we will need to address have been considered and developed by the other dynamic language implementations like [MacRuby](http://www.macruby.org/blog/2009/03/28/experimental-branch.html), [JRuby](http://jruby.codehaus.org/), [Rubinius](http://rubini.us/) and [Parrot](http://www.parrot.org/), and in particular other Python implementations like [Jython](http://www.jython.org/Project/), [PyPy](http://codespeak.net/pypy/dist/pypy/doc/), and [IronPython](http://www.codeplex.com/Wiki/View.aspx?ProjectName=IronPython). In particular, we're looking at these other implementations for ideas on debug information, regex performance ideas, and generally useful performance ideas for dynamic languages. This is all fairly well-trodden ground, and we want to avoid reinventing the wheel as much as possible.


# Milestones #

Unladen Swallow will be released every three months, with bugfix releases in between as necessary.

## 2009 Q1 ##

Q1 will be spent making relatively minor tweaks to the existing CPython implementation. We aim for a 25-35% performance improvement over our baseline. Our goals for this quarter are conservative, and are aimed at delivering tangible performance benefits to client applications as soon as possible, that is, without waiting until the completion of the project.

Ideas for achieving this goal:
  * Re-implement the eval loop in terms of [vmgen](http://www.complang.tuwien.ac.at/anton/vmgen/).
  * Experiment with compiler options such as 64 bits, LLVM's LTO support, and gcc 4.4's FDO support.
  * Replace rarely-used opcodes with functions, saving critical code space.
  * Improve GC performance (see http://bugs.python.org/issue4074).
  * Improve cPickle performance. Many large websites use this heavily for interacting with memcache.
  * Simplify frame objects to make frame alloc/dealloc faster.
  * Implement one of the several proposed schemes for speeding lookups of globals and builtins.

The 2009Q1 release can be found in the [release-2009Q1-maint branch](http://code.google.com/p/unladen-swallow/source/browse/branches/release-2009Q1-maint). See [Release2009Q1](Release2009Q1.md) for our performance relative to CPython 2.6.1.

## 2009 Q2 ##

Q2 focused on supplementing the Python VM with a functionally-equivalent implementation in terms of LLVM. We anticipated some performance improvement, but that was not the primary focus of the 2009Q2 release; the focus was just on getting something working on top of LLVM. Making it faster will come in subsequent quarters.

Goals:
  * Addition of an LLVM-based JIT.
  * All tests in the standard library regression suite pass when run via the JIT.
  * Source compatibility maintained with existing applications, C extension modules.
  * 10% performance improvement.
  * Stretch goal: 25% performance improvement.

The 2009Q2 release can be found in the [release-2009Q2-maint branch](http://code.google.com/p/unladen-swallow/source/browse/branches/release-2009Q2-maint). See [Release2009Q2](Release2009Q2.md) for our performance relative to 2009Q1.

## 2009 Q3 ##

Q3's development process did not go as originally expected. We had planned that, with the Python->LLVM JIT compiler in place, we could begin optimizing aggressively, exploiting type feedback in all sorts of wonderful ways. That proved somewhat optimistic. We found serious deficiencies in LLVM's just-in-time infrastructure that required a major detour away from our earlier, performance-centric goals. The two most serious problems were, a) a hard limit of 16MB of machine code over the lifetime of the process, and b) a bug in LLVM's x86-64 code generation that led to difficult-to-diagnose segfaults ([LLVM PR5201](http://llvm.org/PR5201)).

Given those obstacles, we were relatively happy with the outcome of the 2009Q3 release:

  * Unladen Swallow 2009Q3 uses up to 930% less memory than the 2009Q2 release.
  * Execution performance has improved by 15-70%, depending on benchmark.
  * Unladen Swallow 2009Q3 integrates with [gdb 7.0](http://www.gnu.org/software/gdb/download/ANNOUNCEMENT) to better support debugging of JIT-compiled code.
  * Unladen Swallow 2009Q3 integrates with [OProfile 0.9.4](http://oprofile.sourceforge.net/news/) and later to provide seemless profiling across Python and C code, if configured with `--with-oprofile=<oprofile-prefix>`.
  * Many bugs and restrictions in LLVM's JIT have been fixed. In particular, the 2009Q2 limitation of 16MB of machine code has been lifted.
  * Unladen Swallow 2009Q3 passes the tests for all the third-party tools and libraries listed on the [Testing](Testing.md) page. Significantly for many projects, this includes compatibility with Twisted, Django, NumPy and Swig.

The 2009Q3 release can be found in the [release-2009Q3-maint branch](http://code.google.com/p/unladen-swallow/source/browse/branches/release-2009Q3-maint). See [Release2009Q3](Release2009Q3.md) for our performance relative to 2009Q2. Hearty congratulations go out to our intern, Reid Kleckner, for his vital contributions to the Q3 release.


## 2009 Q4 ##

Based on the relative immaturity of LLVM's just-in-time infrastructure, we anticipate spending more time fixing fundamental problems with this area of LLVM. (We note that the rest of LLVM is a paradise by comparison.) We anticipate a modest performance increase over Q3, though most of our time will go toward ensuring a high-quality, ultra-stable product so that we have a solid footing for merger with CPython in 2010. We are choosing to shift our focus in this way -- stability now, performance later -- so that all our necessary changes can be incorporated into LLVM 2.7, which will then form the baseline for an LLVM-based CPython 3.x.

Areas for performance improvement (non-exhaustive):
  * Binary operations via type feedback.
  * Attribute/method lookup via type feedback.
  * Moving compilation to a non-block background thread.
  * Import optimizations (surprisingly important to some benchmarks).

We intend to tag the 2009Q4 release in early January 2010 (to allow for holidays in the United States).


## Long-Term Plans ##

The plan for Q3 onwards is to simply iterate over [the literature](RelevantPapers.md). We aspire to do no original work, instead using as much of the last 30 years of research as possible. See RelevantPapers for a partial (by no means complete) list of the papers we plan to implement or draw upon.

We plan to address performance considerations in the regular expression engine, as well as any other extension modules found to be bottlenecks. However, regular expressions are already known to be a good target for our work and will be considered first for optimization.

Our long-term goal is to make Python fast enough to start moving performance-important types and functions from C back to Python.



## Global Interpreter Lock ##

From an earlier draft of this document:

<blockquote>
In addition, we intend to remove the GIL and fix the state of multithreading in Python.<br>
We believe this is possible through the implementation of a more sophisticated GC<br>
system, something like IBM's Recycler (Bacon et al, 2001).<br>
</blockquote>

Our original plans for dealing with the GIL centered around [Recycler](http://citeseerx.ist.psu.edu/viewdoc/summary?doi=10.1.1.19.1065), a garbage collection scheme proposed by IBM researchers. This appeared at first blush to be an excellent match for Python, and we were excited about prospects for success. Further investigation and private communications revealed that [safethread](http://code.google.com/p/python-safethread/), an experimental branch of CPython, had also implemented Recycler, but with minimal success. The author relayed that he had demonstrated a speed-up on a dual-core system, but performance degraded sharply at four cores.

Accordingly, we are no longer as optimistic about our chances of removing the GIL completely. We now favor a more incremental approach improving the shortcomings [pointed out by Dave Beazley](http://www.dabeaz.com/python/GIL.pdf), among others. In any case, work on the GIL should be done directly in mainline CPython, or on a very close branch of Python 3.x: the sensitive nature of the work recommends a minimal delta, and doing the work and then porting it from 2.x to 3.y (as would be the case for Unladen Swallow) is a sure-fire way of introducing exceedingly-subtle bugs.

Longer-term, we believe that CPython should drop reference counting and move to a pure garbage collection system. There is a large volume of classic literature and on-going research into garbage collection that could be more effectively exploited in a pure-GC system. Even without the GIL, the current volume of refcount update operations will make scaling effectively to many-core systems a challenge; we believe a pure garbage collection scheme would alleviate these pressures somewhat.


# Detailed Plans #

## JIT Compilation ##

We plan to start with a simple, easy-to-implement JIT compiler, then add complexity and sophistication as warranted. We will start by implementing a simple, function-at-a-time compiler that takes [the CPython bytecode](http://docs.python.org/library/dis.html) and converts it to machine code via LLVM's internal intermediate representation (IR). The initial implementation of this bytecode-to-machine code compiler will be done by translating the code in CPython's interpreter loop to calls to [LLVM's IRBuilder](http://llvm.org/docs/tutorial/JITTutorial2.html). We will apply only a small subset of LLVM's available optimization passes ([current passes](http://code.google.com/p/unladen-swallow/source/browse/trunk/Python/global_llvm_data.cc)), since it's not clear that, for unoptimized Python IR, the extra compilation time spent in the optimizers will be compensated with increased execution performance. We've experimented with using LLVM's fast instruction selector (`FastISel`), but it fails over to the default, slower instruction selection DAG with our current IR; we may revisit this in the future.

We've chosen a function-at-a-time JIT instead of a tracing JIT (Gal et al, 2007) because we believe whole-function compilation is easier to implement, given the existing structure of CPython bytecode. That is not to say we are opposed to a tracing JIT; on the contrary, implementing a whole-function compiler will provide a large amount of the necessary infrastructure for a tracing JIT, and a whole-function JIT will serve as a valuable baseline for comparing the performance of any proposed tracing implementations. LLVM's analysis libraries already have some support for specially optimizing hot traces ([in Trace.cpp](https://llvm.org/svn/llvm-project/llvm/trunk/lib/Analysis/Trace.cpp)) that we may be able to take advantage of if we pursue a tracing JIT. We can get some of the benefits from having the instrumentation for the planned feedback-directed optimization system track taken/not-taken branches and pruning the not-taken branches from the generated machine code.

Since we only wish to spend time optimizing code that will benefit the most -- the program's hot spots -- we will need a way to model hotness. Like in the rest of the JIT compiler, we plan to start simple and add complexity and sophistication as the benchmark results warrant. Our initial model will be very simple: if a function is called 10000 times, it is considered hot and is sent to LLVM for compilation to machine code. This model is obviously deficient, but will serve as a baseline for improvement. Enhancements we're considering: using interpreter loop ticks instead of function calls; aggregating the hotness level of leaf functions up the call tree. In the case of long-running loops, we probably won't try to replace those loops mid-execution, though it should be possible to do this if we desire.

Even at its most naive, the generated machine code differs from its analogue in the interpreter loop in important ways:
  * The machine code does not support the line-level tracing used by Python's [pdb module](http://docs.python.org/library/pdb.html) and others. If the machine code detects that tracing has turned on, it will bail to the interpreter loop, which picks up execution at the same place the machine code left off.
  * In the machine code, thread switching and signal handling are done at function calls and loop backedges (see [r699](http://code.google.com/p/unladen-swallow/source/detail?r=699)), rather than every 100ish opcodes as in the interpreter loops. There's plenty of room to optimize this: we can eliminate  thread switching/signal handling code on loop backedges where the loop contains function calls, or where we have multiple function calls back-to-back. Support threads and signals in the machine code imposes fairly low overhead (2-4%), so we probably won't start optimizing this unless we find that it inhibits additional optimizations.

In the initial draft of the JIT compiler, we will block execution while compiling hot functions. This is expensive. We will shift compilation to a separate worker thread using FIFO work queues. This will include instrumentation for work unit throughput, execution pause times, and temporal clustering so that we can measure the impact on execution time. [Rubinius](http://rubini.us/) developed this technique independently, has seen success using it to reduce pause times in execution. We believe this strategy will allow us to perform potentially more expensive optimizations than if we had to always block execution on compilation/optimization (ala Self). This work is being tracked in [issue 40](https://code.google.com/p/unladen-swallow/issues/detail?id=40).


## Feedback-Directed Optimization ##

We wish to make use of the wealth of information available at runtime to optimize Python programs. Sophisticated implementations of Self, Java and JavaScript have demonstrated the real-world applicability of these techniques, and Psyco has demonstrated some applicability to Python. Accordingly, we believe it will be profitable to make use of runtime information when optimizing Unladen Swallow's native code generation.

### Background ###

Unladen Swallow compiles Python code to a bytecode representation that is amenable to reasonably-performant execution in a custom virtual machine. Once a piece of code has been deemed hot, we compile the bytecode to native code using LLVM. It is this native code that we seek to optimize. Optimizing the execution of the generated bytecode is less interesting since the system should be selecting most performance-critical functions for compilation to native code. However, modifications to the bytecode to enable easier compilation/profiling are fair game.

### Points ###
  * We wish to gather as much information at runtime as possible, not merely type information (as implied by the more specific name "type feedback"). The representation used should allow for sufficient flexibility to record function pointer addresses, branch-taken statistics, etc, potentially any and every piece of information available at runtime.
  * The gathered information should live in the code object so that it lasts as long as the relevant bytecode.
  * Hölzle 1994 includes an analysis showing that the vast majority of Self call sites are monomorphic (see section 3). Recent [analysis of Ruby programs](http://gist.github.com/134687) has observed a similar distribution of call site arity in Ruby. We believe that Python programs are sufficiently similar to Self and Ruby in this regard. Based on these findings from other languages, we will want to limit our optimization attempts to call sites with arity < 3. Our implementation of feedback-directed optimization should gather enough data to conduct a similar analysis for Python.
  * Due to the nature of Python's bytecode format, we believe it would be unprofitable to implement the desired level of data gathering inline, that is, as separate opcodes. Instead, the bodies of interesting opcodes in the Python VM will be modified to record the data they use. This will be faster (avoids opcode fetch/dispatch overhead) and easier to reason about (no need to track multi-byte opcode sequences).
  * We will optimize for the common case, as determined by the data-gathering probes we will add to interesting opcodes. Guards will detect the uncommon case and fail back to the interpreter. This allows our assumptions about the common case to propagate to later code. Again, see Hölzle 1994.

The initial round of data-gathering infrastructure (for types and branches) was added in [r778](https://code.google.com/p/unladen-swallow/source/detail?r=778).

### Places we would like to optimize (non-exhaustive) ###
  * Operators. If both operands are ints, we would like to inline the math operations into the generated machine code, rather than going through indirect calls. If both operands are strings, we would like to call directly to the appropriate function (possibly compiled with Clang and using the fastcc calling convention) rather than going through the indirection in `PyNumber_Add()`. This optimization is being tracked in [issue 73](https://code.google.com/p/unladen-swallow/issues/detail?id=73).
  * UNPACK\_SEQUENCE. Knowing the type of the sequence being unpacked could allow us to inline the subsequent STORE\_FOO opcodes and avoid a lot of stack manipulation.
  * Calls to builtins. We would like to be able to inline calls to builtin functions, or at the very least, avoid looking the function up via LOAD\_GLOBAL. Ideally we would also be able to inline some of these calls where that is deemed profitable. For example, inlining `len()` could save not only the LOAD\_GLOBAL lookup but also the layers of indirection incurred in `PyObject_Size()`. In the best case, a call to `len()` on lists or tuples (or other builtin types) could be turned into `((PyVarObject *)(ob))->ob_size`. This optimization is being tracked in [issue 67](https://code.google.com/p/unladen-swallow/issues/detail?id=67) (LOAD\_GLOBAL improvements) and [issue 75](https://code.google.com/p/unladen-swallow/issues/detail?id=75) (inlining simple builtins).
  * Branches. If a branch is always taken in a given direction, we can omit the machine code for the uncommon case, falling back to the interpreter instead. This can be used to simplify the control-flow graph and thus allow greater optimization of the common case and the code that follows it. This optimization is being tracked in [issue 72](https://code.google.com/p/unladen-swallow/issues/detail?id=72).
  * Method dispatch. If we know the most-likely receiver types for a given method invocation, we can potentially avoid the method lookup overhead or inline the call entirely. Note that in Python 2.6 and higher, method lookups are cached in the type objects so the potential savings of skipping some steps in the cache check process may be minimal. Better to reuse this information for possible inlining efforts.
  * Function calls. If we know the parameter signature of the function being invoked, we can avoid the overhead of taking the arguments and matching them up with the formal parameters. This logic can be fairly expensive, since it is designed to be as general as possible to support the wide variety of legal function call/definition combinations. If we don't need to be so general, we can be faster. This optimization is being tracked in [issue 74](https://code.google.com/p/unladen-swallow/issues/detail?id=74).


## Regular Expressions ##

While regexes aren't a traditional performance hotspot, we've found that most regex users expect them to be faster than they are, resulting in surprising performance degradation. For this reason, we'd like to invest some resources in speeding up CPython's regex engine.

CPython's current regex engine is a stack-based bytecode interpreter. It does not take advantage of any form of modern techniques to improve opcode dispatch performance (Bell 1973; Ertl & Gregg, 2003; Berndl et al, 2005) and is in other respects a traditional, straightforward virtual machine. We believe that many of the techniques being applied to speed up pure-Python performance are equally applicable to regex performance, starting at improved opcode dispatch all the way through JIT-compiling regexes down to machine code.

Recent work in the Javascript community has confirmed our belief. Google's V8 engine now includes [Irregexp](http://blog.chromium.org/2009/02/irregexp-google-chromes-new-regexp.html), a JIT regex compiler, and the new [SquirrelFish Extreme](http://webkit.org/blog/214/introducing-squirrelfish-extreme/) includes a new regex engine based on the same principle: trade JIT compilation time for execution time. Both of these show impressive gains on the regex section of the various Javascript benchmarks. We would like to replicate these results for CPython.

We also considered using Thompson NFAs for very simple regexes, as [advocated by Russ Cox](http://209.85.173.132/search?q=cache:XQrcPV-4kngJ:swtch.com/~rsc/regexp/regexp1.html+thompson+NFA&cd=1&hl=en&ct=clnk&gl=us). This would create a multi-engine regex system that could choose the fastest way of implementation any given pattern. The V8 team also considered such a hybrid system when working on Irregexp but rejected it, [saying](http://blog.chromium.org/2009/02/irregexp-google-chromes-new-regexp.html#c4843826268005492354)

> The problem we ran into is that not only backreferences but also basic operators like `|` and `*` are defined in terms of backtracking. To get the right behavior you may need backtracking even for seemingly simple regexps. Based on the data we have for how regexps are used on the web and considering the optimizations we already had in place we decided that the subset of regexps that would benefit from this was too small.

One problem that needs to be overcome before any work on the CPython regex engine begins is that Python lacks a regex benchmark suite. We might be able to reuse [the regexp.js component of the V8 benchmarks](http://v8.googlecode.com/svn/data/benchmarks/v3/regexp.js), but we would first need to verify that these are representative of the kind of regular expressions written in Python programs. We have no reason to believe that regexes used in Python programs differ significantly from those written in Javascript, Ruby, Perl, etc programs, but we would still need to be sure.


## Start-up Time ##

In talking to a number of heavy Python users, we've gotten a lot of interest in improving Python's start-up time. This comes from both very large-scale websites (who want faster server restart times) and from authors of command-line tools (where Python start time might dwarf the actual work done).

Start-up time is currently dominated by imports, especially for large applications like [Bazaar](http://bazaar-vcs.org/). Python offers a lot of flexibility by deferring imports to runtime and providing a lot of hooks for configuring exactly how imports will work and where modules can be imported from. The price for that flexibility is slower imports.

For large users that don't take advantage of that flexibility -- in particular servers, where imports shouldn't change between restarts -- we might provide a way to opt in to stricter, faster import semantics. One idea is to ship all required modules in a single, self-contained "binary". This would both a) avoid multiple filesystem calls for each import, and b) open up the possibility of Python-level [link-time optimization](http://www.airs.com/blog/archives/100), resulting in faster code via inter-module inlining and similar optimizations. Self-contained images like this would be especially attractive for large Python users in the server application space, where hermetic builds and deployments are already considered essential.

A less invasive optimization would be to speed up [Python's marshal module](http://docs.python.org/library/marshal.html), which is used for `.pyc` and `.pyo` files. Based on Unladen Swallow's work speeding up `cPickle`, similarly low-hanging fruit probably exists in `marshal`.

We already have benchmarks tracking start-up time in a number of configurations. We will probably also add microbenchmarks focusing specifically on imports, since imports currently dominate CPython start time.


# Testing and Measurement #

## Performance ##

Unladen Swallow maintains a directory of interesting performance tests under the [tests](http://code.google.com/p/unladen-swallow/source/browse/#svn/tests) directory. `perf.py` is the main interface to the benchmarks we care about, and will take care of priming runs, clearing `*.py[co]` files and running interesting statistics over the results.

[Unladen Swallow's benchmark suite](Benchmarks.md) is focused on the hot spots in major Python applications, in particular web applications. The major web applications we have surveyed have indicated that they bottleneck primarily on template systems, and hence our initial benchmark suite focuses on them:
  * **Django and Spitfire templates**. Two very different ways of implementing a template language.
  * **2to3**. Translates Python 2 syntax to Python 3. Has an interesting, pure-Python kernel that makes heavy use of objects and method dispatch.
  * **Pickling and unpickling**. Large-scale web applications rely on memcache, which in turns uses Python's Pickle format for serialization.

There are also a number of microbenchmarks, for example, an N-Queens solver, an alphametics solver and several start-up time benchmarks.

Apart from these, our benchmark suite includes several crap benchmarks like Richards, PyStone and PyBench; these are only included for completeness and comparison with other Python implementations, which have tended to use them. Unladen Swallow does not consider these benchmarks to be representative of real Python applications or Python implementation performance, and does not run them by default or make decisions based on them.

For charting the long-term performance trend of the project, Unladen Swallow makes use of Google's standard internal performance measurement framework. Project members will post regular performance updates to the mailing lists. For testing individual changes, however, using `perf.py` as described on the [Benchmarks](Benchmarks.md) page is sufficient.

## Correctness ##

In order to ensure correctness of the implementation, Unladen Swallow uses both the standard Python test suite, plus a number of third-party libraries that are known-good on Python 2.6. In particular, we test third-party C extension modules, since these are the easiest to break via unwitting changes at the C level.

As work on the JIT implementation moves forward, we will incorporate a fuzzer into our regular test run. We plan to reuse Victor Stinner's [Fusil](http://fusil.hachoir.org/svn/trunk/) Python fuzzer as much as possible, since it a) exists, and b) has been demonstrated to find real bugs in Python.

Unladen Swallow will come with a `--jit` option that can be used to control when the JIT kicks in. For example, `--jit=never` would disable the JIT entirely, while `--jit=always` would skip the warm-up interpreted executions and jump straight into native code generation; `--jit=once` will disable recompilation. These options will be used to test the various execution strategies in isolation. Our goal is to avoid JIT bugs that are never encountered because the buggy function isn't hot enough, as have been observed in the JVM (likewise for the interpreted mode).

Unladen Swallow maintains a [BuildBot](http://buildbot.net/trac) instance that runs the above tests against every commit to trunk.

## Complexity ##

One of CPython's virtues is its simplicity: modifying CPython's VM and compiler is relatively simple and straight-forward. Our work with LLVM will inevitably introduce more complexity into CPython's implementation. In order to measure the productivity trade-offs that may result from this extra machinery, the Unladen Swallow team will periodically take ideas from the `python-dev` and `python-ideas` mailing lists and implement them. If implementation is significantly more difficult that the corresponding change to CPython, that's obviously something that we'll need to address before merger. We may also get non-team members to do the implementations so that we get a less biased perspective.


# Risks #

  * **May not be able to merge back into mainline.** There are vocal, conservative senior members of the Python core development community who may oppose the merger of our work, since it will represent such a significant change. This is a good thing! Resistance to change can be very healthy in situations like this, as it will force a thorough, public examination of our patches and their possible long-term impact on the maintenance of CPython -- this is open source, and another set of eyes is always welcome. We believe we can justify the changes we're proposing, and by keeping in close coordination with Guido and other senior members of the community we hope to limit our work to only changes that have a good chance of being accepted. However: there is still the chance that some patches will be rejected. Accordingly, we may be stuck supporting a de facto separate implementation of Python, or as a compromise, not being as fast as we'd like. C'est la vie.
  * **LLVM comes with a lot of unknowns:**  Impact on extension modules? JIT behaviour in multithreaded apps? Impact on Python start-up time?
  * **Windows support:** CPython currently has good Windows support, and we'll have to maintain that in order for our patches to be merged into mainline. Since none of the Unladen Swallow engineers have any/much Windows experience or even Windows machines, keeping Windows support at an acceptable level may slow down our forward progress or force us to disable some performance-beneficial code on Windows. Community contributions may be able to help with this.
  * **Specialized platforms:** CPython currently runs on a wide range of hardware and software platforms, from big iron server machines down to Nokia phones. We would like to maintain that kind of hard-won portability and flexibility as much as possible. We already know that LLVM (or even a hand-written JIT compiler) will increase memory usage and Python's binary footprint, possibly to a degree that makes it prohibitive to run Unladen Swallow on previously-support platforms. To mitigate this risk, Unladen Swallow will include a `./configure` flag to disable LLVM integration entirely and forcing the use of the traditional eval loop.


# Lessons Learned #

This section attempts to list the ways that our plans have changed as work has progressed, as we've read more papers and talked to more people. This list is incomplete and will only grow.

  * Early in our planning, we had considered completely removing the custom CPython virtual machine and replacing it with LLVM. The benefit would be that there would be less code to maintain, and only a single encoding of Python's semantics. The theory was that we could either a) generate slow-to-run but fast-to-compile machine code, or b) generate LLVM IR and run it through LLVM's interpreter. Both of these turned out to be impractical: even at its fastest (using no optimization passes and LLVM's `FastISel` instruction selector), compiling to native code was too slow. LLVM's IR interpreter was both too slow and did not support all of LLVM's IR. Preserving the CPython VM also allows Unladen to keep compatibility with the unfortunate number of Python packages that parse CPython bytecode.

  * There are a number of Python programs that parse or otherwise interact with CPython bytecode. Since the exact opcode set, semantics and layout are considered an implementation detail of CPython, we were inclined to disregard any breakage we may inflict on these packages (a pox upon their houses, etc). However, some packages that are too important to break deal with CPython bytecode, among them Twisted and setuptools. This has forced us to be more cautious than we would otherwise like when changing the bytecode.

  * Initially we generated LLVM IR at the same time as the Python bytecode, using hooks from the Python compiler. The idea was that we would move to generating the LLVM IR from the AST instead, allowing us to generate more efficient IR. The overhead of generating LLVM IR and the decision not to get rid of the CPython VM pre-empted that move, however, and instead we now generate LLVM IR from the Python bytecode. Besides allowing us to generate LLVM IR for any code object (including hand-crafted ones) and not requiring us to keep the AST for a codeblock around, it also means the existing (bytecode) peephole optimizer ends up optimizing the bytecode before it is turned into LLVM IR. The peephole optimizer, with its intimate knowledge of Python semantics, does a good job optimizing things that would be a lot harder to do in LLVM optimization passes.

# Communication #

All communication about Unladen Swallow should take place on the [Unladen Swallow list](http://groups.google.com/group/unladen-swallow). This is where design issues will be discussed, as well as notifications of continuous build results, performance numbers, code reviews, and all the other details of an open-source project. If you add a comment below, on this page, we'll probably miss it. Sorry. Please mail our list instead.