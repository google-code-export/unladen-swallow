

## Installing LLVM ##

You need LLVM and Clang 2.7:
```
> wget 'http://llvm.org/releases/2.7/llvm-2.7.tgz'
> wget 'http://llvm.org/releases/2.7/clang-2.7.tgz'
> tar xzvf llvm-2.7.tgz
llvm-2.7/...
> tar xzvf clang-2.7.tgz
clang-2.7/...
> mv clang-2.7 llvm-2.7/tools/clang
> mkdir llvm_build_dbg
> cd llvm_build_dbg
```
If you have [OProfile >=0.9.5](UsingOProfile.md) installed, add `--with-oprofile=<prefix>` to the `configure` commands below.
```
> ../llvm-2.7/configure --enable-shared --disable-optimized --prefix=`pwd`/../llvm_install_dbg
> make -j4
> make check-lit && (cd tools/clang/; make test)  # Optional
> make install
> cd ..
```
The above built a "Debug" LLVM and Clang, and installed it to `.../llvm_install_dbg`. Now we'll build a "Release+Debug-Asserts" LLVM and Clang, and install it to `.../llvm_install_opt`. The "+Debug" in this second build mode indicates that it has debugging symbols despite being optimized.
```
> mkdir llvm_build_opt
> cd llvm_build_opt
> ../llvm-2.7/configure --enable-shared --enable-optimized --disable-assertions \
    --enable-debug-symbols --prefix=`pwd`/../llvm_install_opt
> make -j4
> make check-lit && (cd tools/clang/; make test)  # Optional, unittests will fail
> make install
> cd ..
```

You only have to do the above once per LLVM release, and you can share the resulting directories among many Unladen Swallow checkouts.

## The basics ##

Setting up Unladen Swallow uses nearly the same procedure as setting up CPython:

```
> mkdir unladen; cd unladen
> svn checkout http://unladen-swallow.googlecode.com/svn/trunk src
...
> mkdir dbg; cd dbg
> ../src/configure --with-pydebug --with-llvm=.../llvm_install_dbg
...
> make
...
> ./python.exe
Python 2.6.4 (r261:311:312M, Oct 14 2009, 23:24:25) 
[GCC 4.0.1 (Apple Inc. build 5490)] on darwin
[Unladen Swallow 2010Q1]
Type "help", "copyright", "credits" or "license" for more information.
>>>
```

To build an optimized binary for benchmarking, use:

```
> mkdir unladen; cd unladen
> svn checkout http://unladen-swallow.googlecode.com/svn/trunk src
...
> mkdir opt; cd opt
> ../src/configure --with-llvm=.../llvm_install_opt
...
> make
...
> ./python.exe
Python 2.6.4 (r261:1137, Mar 20 2010, 12:27:18) 
[GCC 4.2.4 (Ubuntu 4.2.4-1ubuntu4)] on linux2
[Unladen Swallow 2009Q4]
Type "help", "copyright", "credits" or "license" for more information.
>>> 
```

Note that our `tests/` top-level directory uses Subversion 1.5-style relative `svn:externals` properties; accordingly, you'll need [SVN 1.5 or higher](http://subversion.tigris.org/).

Other interesting checkout targets:
  * Release2009Q1: http://unladen-swallow.googlecode.com/svn/branches/release-2009Q1-maint
  * Release2009Q2: http://unladen-swallow.googlecode.com/svn/branches/release-2009Q2-maint
  * Release2009Q3: http://unladen-swallow.googlecode.com/svn/branches/release-2009Q3-maint
  * Trunk: http://unladen-swallow.googlecode.com/svn/trunk

Active development is being done in `trunk/`. We try to keep trunk stable and correct at all times, but there may be bugs that have yet to be addressed. Caveat downloader.

If you're building the 2009Q2 release on a 32/64-bit hybrid system (say, a 64-bit kernel but a 32-bit userspace), you'll need to run a different `../src/configure` command. In the case of a 32-bit userspace, something like this should work:
```
CFLAGS=-m32 CXXFLAGS=-m32 ../src/configure --build=i386-unknown-linux-gnu
```

See BuildingOnWindows for MSVC build instructions.


## Reducing build times ##

On OS X, Python comes with a suite of Carbon toolkit modules that we generally don't care about when working on Unladen Swallow. You can pass `--disable-toolbox-glue` to avoid wasting cycles building these modules you won't use. This brings build times down to what they are on Linux.


## Working on Unladen Swallow ##

### Important Files ###

Here are the primary files that you'll be working on as part of Unladen Swallow:
  * **[JIT/llvm\_notes.txt](http://code.google.com/p/unladen-swallow/source/browse/trunk/JIT/llvm_notes.txt)** - Deep dive into optimizations and general JIT operations.
  * **[JIT/llvm\_fbuilder.h](http://code.google.com/p/unladen-swallow/source/browse/trunk/JIT/llvm_fbuilder.h), [JIT/llvm\_fbuilder.cc](http://code.google.com/p/unladen-swallow/source/browse/trunk/JIT/llvm_fbuilder.cc)** - The heart of the compiler. These files take care of converting CPython bytecode to LLVM IR.
  * **[JIT/llvm\_inline\_functions.c](http://code.google.com/p/unladen-swallow/source/browse/trunk/JIT/llvm_inline_functions.c)** - Special-cased C functions to be inlined.
  * **[Lib/test/test\_llvm.py](http://code.google.com/p/unladen-swallow/source/browse/trunk/Lib/test/test_llvm.py)** - Tests for the JIT compiler. All code generation and optimization changes need tests. Tests should include both positive and negative cases.
  * **[Python/eval.cc](http://code.google.com/p/unladen-swallow/source/browse/trunk/Python/eval.cc)** - The interpreter loop. If you need to collect new types of runtime feedback for an optimization, you'll be modifying this file.
  * **[JIT/PyTypeBuilder.h](http://code.google.com/p/unladen-swallow/source/browse/trunk/JIT/PyTypeBuilder.h)** - Support code for exposing Python objects to LLVM analyzers and code generators.


Secondary files:
  * **[JIT/llvm\_compile.cc](http://code.google.com/p/unladen-swallow/source/browse/trunk/JIT/llvm_compile.cc)** - Primary entry point for JIT compilation. Code comes in here, gets optimized.
  * **[JIT/global\_llvm\_data.h](http://code.google.com/p/unladen-swallow/source/browse/trunk/JIT/global_llvm_data.h), [JIT/global\_llvm\_data.cc](http://code.google.com/p/unladen-swallow/source/browse/trunk/JIT/global_llvm_data.cc)** - Per-interpreter shared state used to compile Python code to LLVM IR.
  * **[Unittests/](http://code.google.com/p/unladen-swallow/source/browse/trunk/Unittests)** - Directory of C++ unittests. Write unittests, keep them passing.
  * **[Util/](http://code.google.com/p/unladen-swallow/source/browse/trunk/Util)** - Directory of utility code: statistics, abstract data types, etc.


### Getting Things Done ###

We maintain a list of [good volunteer projects](http://code.google.com/p/unladen-swallow/issues/list?can=2&q=label%3AStarterProject) in our issue tracker under the label [StarterProject](http://code.google.com/p/unladen-swallow/issues/list?can=2&q=label%3AStarterProject). Look over these and let us know if any strike your fancy.

There's also a category called [Beer](http://code.google.com/p/unladen-swallow/issues/list?can=2&q=label%3ABeer). The `Beer` tag indicates tasks that aren't exactly sexy, but need to get done. As a thank-you for taking on one of these tasks, the Googlers on the team will [buy you a round](http://en.wikipedia.org/wiki/Round_of_drinks) at a conference. Seriously.

Any patches should follow [our style guide](StyleGuide.md) and be put on http://codereview.appspot.com and sent to [unladen-swallow@googlegroups.com](http://groups.google.com/group/unladen-swallow) for pre-commit review.

To upload a patch, download [upload.py](http://codereview.appspot.com/static/upload.py) and go to your checkout directory.  Pick some project members as reviewers, and invoke upload.py like so:

```
upload.py -e EMAIL@gmail.com -r REVIEWERS --cc=unladen-swallow@googlegroups.com --send_mail
```

## Improving generated code ##

The first step to improving the code we generate is to look at it. In Unladen Swallow, every function has four representations. First, the Python code:

```
def sum(x):
  result = 0
  for i in x:
    result += i
  return result
```

This is compiled into CPython bytecode, which you can inspect with the `dis` module:

```
>>> import dis
>>> dis.dis(sum)
  2           0 LOAD_CONST               1 (0)
              3 STORE_FAST               1 (result)

  3           6 SETUP_LOOP              24 (to 33)
              9 LOAD_FAST                0 (x)
             12 GET_ITER            
        >>   13 FOR_ITER                16 (to 32)
             16 STORE_FAST               2 (i)

  4          19 LOAD_FAST                1 (result)
             22 LOAD_FAST                2 (i)
             25 INPLACE_ADD         
             26 STORE_FAST               1 (result)
             29 JUMP_ABSOLUTE           13
        >>   32 POP_BLOCK           

  5     >>   33 LOAD_FAST                1 (result)
             36 RETURN_VALUE        
>>> 
```

[Doc/library/dis.rst](http://code.google.com/p/unladen-swallow/source/browse/trunk/Doc/library/dis.rst) documents what the opcodes mean.

Third, when a function is hot, the bytecode gets compiled to [LLVM IR](http://llvm.org/docs/LangRef.html). You can force this compilation by setting `func.__code__.co_optimization` to an integer between -1 and 2 (which determines how much to optimize the code). Then print the bytecode with `func.__code__.co_llvm`:

```
>>> sum.__code__.co_optimization=1
>>> print sum.__code__.co_llvm

define %struct._object* @"#u#sum"(%struct._frame* %frame) {
entry:
	%exc_info = alloca %struct.PyExcInfo, align 4		; <%struct.PyExcInfo*> [#uses=4]
	%stack_pointer_addr = alloca %struct._object**, align 4		; <%struct._object***> [#uses=50]
	%call.i = call %struct._ts* @PyThreadState_Get() nounwind		; <%struct._ts*> [#uses=13]
	%use_tracing = getelementptr %struct._ts* %call.i, i32 0, i32 5		; <i32*> [#uses=1]
	%use_tracing1 = load i32* %use_tracing		; <i32> [#uses=1]
	%0 = icmp eq i32 %use_tracing1, 0		; <i1> [#uses=1]
	br i1 %0, label %continue_entry, label %trace_enter_function

... # Lots of IR

call_trace38:		; preds = %_PyLlvm_WrapXDecref.exit192
	%f_lasti39 = getelementptr %struct._frame* %frame, i32 0, i32 17		; <i32*> [#uses=1]
	store i32 13, i32* %f_lasti39
	%132 = call i32 @_PyLlvm_CallLineTrace(%struct._ts* %call.i, %struct._frame* %frame, %struct._object*** %stack_pointer_addr)		; <i32> [#uses=2]
	switch i32 %132, label %goto_line [
		i32 -2, label %propagate_exception
		i32 -1, label %JUMP_ABSOLUTE_target
	]
}

>>> 
```

Fourth, this code is JIT-compiled to native machine code. Unfortunately, there's no easy way to display this machine code. The easiest involves setting `PYTHONLLVMFLAGS=-debug-only=jit` before starting Python and running Python inside gdb with a breakpoint at `_llvmfunctionobject.cc:clear_body`. (This requires a debug build of Unladen Swallow/LLVM via `--with-pydebug`.) When the JIT kicks in and lowers your Python code to machine code, it will print out a huge amount of information along the way:

```
$ PYTHONLLVMFLAGS=-debug-only=jit gdb ./python.exe 
...
(gdb) break _llvmfunctionobject.cc:clear_body
Breakpoint 1 at 0x86e878e: file ../trunk/Objects/_llvmfunctionobject.cc, line 105.
(gdb) run
...
>>> def sum(x):
...   result = 0
...   for i in x:
...     result += i
...   return result
... 
>>> sum.__code__.__use_llvm__ = True  # Required!
>>> sum.__code__.co_optimization = 1
>>> sum([1,2,3])  # This will trigger code generation.
JIT: Starting CodeGen of Function #u#sum
...
JIT: Finished CodeGen of [0x2080020] Function: #u#sum: 2763 bytes of text, 214 relocations
JIT: Binary code:
JIT: 00000000: 56575355 e83cec83 fe0e2771 00147883 
...
JIT: 00000ac0: 8950244c 2fe9240c fffffc

Breakpoint 1, clear_body (function=0x9347570) at ../trunk/Objects/_llvmfunctionobject.cc:105
105	    llvm::SmallPtrSet<Value*, 8> globals;
(gdb)
```

Explanation of the next line:
  1. We take the address of the function from `Finished CodeGen of [0x2080020]`; this is `0x2080020`.
  1. We want to disassemble the whole function, so we grab the length from `2763 bytes of text`: the length is `2763`.
  1. We have to give `disassemble` the range of code to work on, so we add the length to the starting address of the function: `(0x2080020 + 2763)`.

LLVM's JIT will compile a number of functions; the one you care about is `#u#sum`, ignore the others.

```
(gdb) disassemble 0x2080020 (0x2080020 + 2763)
Dump of assembler code from 0x2080020 to 0x2080aeb:
0x02080020:	push   %ebp
0x02080021:	push   %ebx
0x02080022:	push   %edi
0x02080023:	push   %esi
0x02080024:	sub    $0x3c,%esp
...
0x02080adf:	mov    0x50(%esp),%ecx
0x02080ae3:	mov    %ecx,(%esp)
0x02080ae6:	jmp    0x208071a
End of assembler dump.
Current language:  auto; currently c++
(gdb) 
```

And there's the machine code for this function.  If you link LLVM with [libudis86](http://udis86.sourceforge.net/), it'll disassemble this for you in the JIT debug output, but getting that link to work is non-trivial.


## Performance analysis ##

Let's say you have a change you'd like to make to Python, and you'd like to see if it impacts performance. The main tool for this is the benchmarks available via `perf.py` (see [Benchmarks](Benchmarks.md) for checkout instructions).

This will compare the performance of two Python binaries, a control binary and an experiment binary, on a benchmark based on Django template rendering.

```
$ ./perf.py -r -b django control/python experiment/python
```

`perf.py -r` will run the benchmarks in a more rigorous mode. In practice, this usually means increasing the number of iterations. When making judgements about the performance improvement/degradation caused by your change, you should always use `-r`.

`perf.py` will run some basic stats on the results for you, yielding the minimum running time, the arithmetic mean running time, the standard deviation and a two-tailed T-test to determine significance. If `perf.py` tells you that the performance change is insignificant or the printed `t` value is low (the absolute value is less than, say, five), it's probably right. The larger the `t` value, the more confident we are in the result.

If you want to pass arguments to the control or experiment binaries, use `perf.py --args`. This will compare the performance of Unladen Swallow's -O2 and -O3 flags on the Django templates benchmark:

```
$ ./perf.py -r -b django --args "-O2,-O3" control/python control/python
```


### Improving startup performance ###

Python startup time is heavily dependent on the number of modules imported. If you can find a way to eliminate or delay an import (in either case, getting it out of the critical path for startup), that will usually improve startup time.

See which modules are required to do no work at all:

```
$ ./python.exe -v -c '' 2>&1 | grep ^import
import zipimport # builtin
import site # precompiled from /Users/collinwinter/src/us/trunk3/Lib/site.pyc
import os # precompiled from /Users/collinwinter/src/us/trunk3/Lib/os.pyc
import errno # builtin
import posix # builtin
import posixpath # precompiled from /Users/collinwinter/src/us/trunk3/Lib/posixpath.pyc
import stat # precompiled from /Users/collinwinter/src/us/trunk3/Lib/stat.pyc
import genericpath # precompiled from /Users/collinwinter/src/us/trunk3/Lib/genericpath.pyc
import copy_reg # precompiled from /Users/collinwinter/src/us/trunk3/Lib/copy_reg.pyc
import encodings # directory /Users/collinwinter/src/us/trunk3/Lib/encodings
import encodings # precompiled from /Users/collinwinter/src/us/trunk3/Lib/encodings/__init__.pyc
import codecs # precompiled from /Users/collinwinter/src/us/trunk3/Lib/codecs.pyc
import _codecs # builtin
import encodings.aliases # precompiled from /Users/collinwinter/src/us/trunk3/Lib/encodings/aliases.pyc
import encodings.utf_8 # precompiled from /Users/collinwinter/src/us/trunk3/Lib/encodings/utf_8.pyc
$
```

`perf.py` includes benchmarks for both normal startup and startup with the `-S` option (don't import `site.py`). These benchmarks are `-b normal_startup` and `-b startup_nosite` respectively, or use `-b startup` to run both.