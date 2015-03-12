# Unladen Swallow 2009Q3 #

Unladen Swallow 2009Q3 is the second release of Unladen Swallow to use [LLVM](http://llvm.org/) for native code generation, and the first to use runtime feedback for optimization. To obtain the 2009Q3 release, run
```
svn checkout http://unladen-swallow.googlecode.com/svn/branches/release-2009Q3-maint unladen-2009Q3
```

The Unladen Swallow team does not recommend wide adoption of the 2009Q3 release. This is intended as a checkpoint of our progress, a milestone on the long path to our eventual performance goals. Note that Unladen Swallow tracks LLVM trunk fairly closely, and will not build against LLVM 2.5 or 2.6.

**Highlights:**
  * Unladen Swallow 2009Q3 uses up to 930% less memory than the 2009Q2 release.
  * Execution performance has improved by 15-70%, depending on benchmark.
  * Unladen Swallow 2009Q3 integrates with [gdb 7.0](http://www.gnu.org/software/gdb/download/ANNOUNCEMENT) to better support debugging of JIT-compiled code.
  * Unladen Swallow 2009Q3 integrates with [OProfile 0.9.4](http://oprofile.sourceforge.net/news/) and later to provide seemless profiling across Python and C code, if configured with `--with-oprofile=<oprofile-prefix>`.
  * Many bugs and restrictions in LLVM's JIT have been fixed. In particular, the 2009Q2 limitation of 16MB of machine code has been lifted.
  * Unladen Swallow 2009Q3 passes the tests for all the third-party tools and libraries listed on the [Testing](Testing.md) page. Significantly for many projects, this includes compatibility with Twisted, Django, NumPy and Swig.

**Lowlights:**
  * LLVM's JIT and other infrastructure needed more work than was expected. As a result, we did not have time to improve performance as much as we would have liked.
  * Memory usage is still 2-3x that of Python 2.6.1. However, there is more overhead that can be eliminated for the 2009Q4 release.


## Memory Usage ##

In the memory benchmarks, we compared the fastest configuration for Q3 against the fastest configuration for Q2. The Q2 configuration is the same as what was reported in [Release2009Q2](Release2009Q2.md). The performance degradation/improvement is calculated using `((new - old) / new)`. Units are kilobytes.

**2009Q2 vs 2009Q3**<br>
slowspitfire:<br>
$ ./perf.py -r -b slowspitfire --args "-j always," --track_memory ../q2/python ../q3/python<br>
Mem max: 212344.000 -> 96884.000: 119.17% smaller<br>
Usage over time: <a href='http://tinyurl.com/yfy3w3p'>http://tinyurl.com/yfy3w3p</a>

ai:<br>
$ ./perf.py -r -b ai --args "-j always," --track_memory ../q2/python ../q3/python <br>
Mem max: 95012.000 -> 14020.000: 577.69% smaller<br>
Usage over time: <a href='http://tinyurl.com/yz7v4xj'>http://tinyurl.com/yz7v4xj</a>

slowpickle:<br>
$ ./perf.py -r -b slowpickle --args "-j always," --track_memory ../q2/python ../q3/python <br>
Mem max: 96876.000 -> 18996.000: 409.98% smaller<br>
Usage over time: <a href='http://tinyurl.com/yf4a3sj'>http://tinyurl.com/yf4a3sj</a>

slowunpickle:<br>
$ ./perf.py -r -b slowunpickle --args "-j always," --track_memory ../q2/python ../q3/python <br>
Mem max: 96876.000 -> 14076.000: 588.24% smaller<br>
Usage over time: <a href='http://tinyurl.com/yfzv2mn'>http://tinyurl.com/yfzv2mn</a>

django:<br>
$ ./perf.py -r -b django --args "-j always," --track_memory ../q2/python ../q3/python <br>
Mem max: 159064.000 -> 27160.000: 485.66% smaller<br>
Usage over time: <a href='http://tinyurl.com/ykdmdml'>http://tinyurl.com/ykdmdml</a>

rietveld:<br>
$ ./perf.py -r -b rietveld --args "-j always," --track_memory ../q2/python ../q3/python <br>
Mem max: 575116.000 -> 55952.000: 927.87% smaller<br>
Usage over time: <a href='http://tinyurl.com/yf3rcbb'>http://tinyurl.com/yf3rcbb</a>


<h2>GDB Support</h2>

The Unladen Swallow team added support to gdb 7.0 that allow JIT compilers to emit DWARF debugging information so that gdb can function properly in the presence of JIT-compiled code. This interface should be sufficiently generic that any JIT compiler can take advantage of it.<br>
<br>
Example backtrace before:<br>
<br>
<pre><code>Program received signal SIGSEGV, Segmentation fault.<br>
[Switching to Thread 0x2aaaabdfbd10 (LWP 25476)]<br>
0x00002aaaabe7d1a8 in ?? ()<br>
(gdb) bt<br>
#0  0x00002aaaabe7d1a8 in ?? ()<br>
#1  0x0000000000000003 in ?? ()<br>
#2  0x0000000000000004 in ?? ()<br>
#3  0x00032aaaabe7cfd0 in ?? ()<br>
#4  0x00002aaaabe7d12c in ?? ()<br>
#5  0x00022aaa00000003 in ?? ()<br>
#6  0x00002aaaabe7d0aa in ?? ()<br>
#7  0x01000002abe7cff0 in ?? ()<br>
#8  0x00002aaaabe7d02c in ?? ()<br>
#9  0x0100000000000001 in ?? ()<br>
#10 0x00000000014388e0 in ?? ()<br>
#11 0x00007fff00000001 in ?? ()<br>
#12 0x0000000000b870a2 in llvm::JIT::runFunction (this=0x1405b70,<br>
F=0x14024e0, ArgValues=@0x7fffffffe050)<br>
   at /home/rnk/llvm-gdb/lib/ExecutionEngine/JIT/JIT.cpp:395<br>
#13 0x0000000000baa4c5 in llvm::ExecutionEngine::runFunctionAsMain<br>
(this=0x1405b70, Fn=0x14024e0, argv=@0x13f06f8, envp=0x7fffffffe3b0)<br>
   at /home/rnk/llvm-gdb/lib/ExecutionEngine/ExecutionEngine.cpp:377<br>
#14 0x00000000007ebd52 in main (argc=2, argv=0x7fffffffe398,<br>
envp=0x7fffffffe3b0) at /home/rnk/llvm-gdb/tools/lli/lli.cpp:208<br>
</code></pre>

And a backtrace after this patch:<br>
<pre><code>Program received signal SIGSEGV, Segmentation fault.<br>
0x00002aaaabe7d1a8 in baz ()<br>
(gdb) bt<br>
#0  0x00002aaaabe7d1a8 in baz ()<br>
#1  0x00002aaaabe7d12c in bar ()<br>
#2  0x00002aaaabe7d0aa in foo ()<br>
#3  0x00002aaaabe7d02c in main ()<br>
#4  0x0000000000b870a2 in llvm::JIT::runFunction (this=0x1405b70,<br>
F=0x14024e0, ArgValues=...)<br>
   at /home/rnk/llvm-gdb/lib/ExecutionEngine/JIT/JIT.cpp:395<br>
#5  0x0000000000baa4c5 in llvm::ExecutionEngine::runFunctionAsMain<br>
(this=0x1405b70, Fn=0x14024e0, argv=..., envp=0x7fffffffe3c0)<br>
   at /home/rnk/llvm-gdb/lib/ExecutionEngine/ExecutionEngine.cpp:377<br>
#6  0x00000000007ebd52 in main (argc=2, argv=0x7fffffffe3a8,<br>
envp=0x7fffffffe3c0) at /home/rnk/llvm-gdb/tools/lli/lli.cpp:208<br>
</code></pre>

So much nicer.<br>
<br>
See <a href='http://llvm.org/docs/DebuggingJITedCode.html'>http://llvm.org/docs/DebuggingJITedCode.html</a> for more details. Thanks to our intern, Reid Kleckner, for doing the heavy lifting on this feature!<br>
<br>
<br>
<h2>Benchmarks</h2>

2009Q3 uses a more sophisticated system for determining which functions to compile than did 2009Q2. Accordingly, we no longer use Unladen Swallow's <code>-j always</code> option when benchmarking 2009Q3.<br>
<br>
<a href='Benchmarks.md'>Benchmarking</a> was done on an Intel Core 2 Duo 6600 @ 2.40GHz with 4GB RAM with a 32-bit userspace. The performance degradation/improvement is calculated using <code>((new - old) / new)</code>.<br>
<br>
<b>2009Q2 vs 2009Q3</b><br>

slowspitfire:<br>
$ ./perf.py -r -b slowspitfire --args "-j always," ../q2/python ../q3/python <br>
Min: 0.690717 -> 0.622342: 10.99% faster<br>
Avg: 0.692846 -> 0.624929: 10.87% faster<br>
Significant (t=165.901211, a=0.95)<br>
Stddev: 0.00348 -> 0.00215: 62.23% smaller<br>
<br>
ai:<br>
$ ./perf.py -r -b ai --args "-j always," ../q2/python ../q3/python <br>
Min: 0.525973 -> 0.459890: 14.37% faster<br>
Avg: 0.529790 -> 0.464647: 14.02% faster<br>
Significant (t=69.943861, a=0.95)<br>
Stddev: 0.00238 -> 0.00900: 73.55% larger<br>
<br>
slowpickle:<br>
$ ./perf.py -r -b slowpickle --args "-j always," ../q2/python ../q3/python <br>
Min: 0.732290 -> 0.597355: 22.59% faster<br>
Avg: 0.733397 -> 0.615644: 19.13% faster<br>
Significant (t=13.096018, a=0.95)<br>
Stddev: 0.00208 -> 0.08989: 97.68% larger<br>
<br>
slowunpickle:<br>
$ ./perf.py -r -b slowunpickle --args "-j always," ../q2/python ../q3/python <br>
Min: 0.314137 -> 0.264590: 18.73% faster<br>
Avg: 0.314825 -> 0.276463: 13.88% faster<br>
Significant (t=9.762778, a=0.95)<br>
Stddev: 0.00100 -> 0.03928: 97.45% larger<br>
<br>
django:<br>
$ ./perf.py -r -b django --args "-j always," ../q2/python ../q3/python <br>
Min: 1.095181 -> 0.946080: 15.76% faster<br>
Avg: 1.096714 -> 0.949940: 15.45% faster<br>
Significant (t=315.826693, a=0.95)<br>
Stddev: 0.00088 -> 0.00456: 80.82% larger<br>
<br>
rietveld:<br>
$ ./perf.py -r -b rietveld --args "-j always," ../q2/python ../q3/python <br>
Min: 0.578493 -> 0.516558: 11.99% faster<br>
Avg: 0.583965 -> 0.619006: 5.66% slower<br>
Significant (t=-2.009135, a=0.95)<br>
Stddev: 0.00804 -> 0.17422: 95.39% larger<br>
<br>
call_simple:<br>
$ ./perf.py -r -b call_simple --args "-j always," ../q2/python ../q3/python <br>
Min: 1.618273 -> 0.908331: 78.16% faster<br>
Avg: 1.632256 -> 0.924890: 76.48% faster<br>
Significant (t=433.008411, a=0.95)<br>
Stddev: 0.00847 -> 0.01397: 39.38% larger