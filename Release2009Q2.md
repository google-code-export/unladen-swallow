# Unladen Swallow 2009Q2 #

Unladen Swallow 2009Q2 is the first release of Unladen Swallow to use [LLVM](http://llvm.org/) for native code generation. To obtain the 2009Q2 release, run
```
svn checkout http://unladen-swallow.googlecode.com/svn/branches/release-2009Q2-maint unladen-2009Q2
```

The Unladen Swallow team does not recommend wide adoption of the 2009Q2 release. This is intended as a checkpoint of our progress, a milestone on the long path to our eventual performance goals. 2009Q2 can compile all pure-Python code to correct native machine code, but is intended to set the stage for more significant performance improvements in the 2009Q3 release that will take advantage of the LLVM-based compiler infrastructure built in Q2.

**Highlights:**
  * Unladen Swallow 2009Q2 uses LLVM to compile hot functions (anything called more than 10000 times) to machine code. A `-j always` command-line option is available to force all functions though LLVM.
  * Unladen Swallow 2009Q2 starts up faster than 2009Q1.
  * A number of buggy corner cases in the 2009Q1 version of `cPickle` have been fixed.
  * Unladen Swallow 2009Q2 passes the tests for all the third-party tools and libraries listed on the [Testing](Testing.md) page. Significantly for many projects, this includes compatibility with Twisted, Django, NumPy and Swig.

**Lowlights:**
  * Memory usage has increased by 10x. We have thus far spent no time improving this; lowering memory usage is a goal for the 2009Q3 release.
  * LLVM's JIT memory manager is limited to 16MB of native code. This is not a problem in practice, but interferes with `regrtest.py` runs. This is being fixed upstream in LLVM, and the result patch will be backported to the 2009Q2 release branch.


## Benchmarks ##

2009Q2 uses a very simple function to determine whether to compile a given function to machine code. Accordingly, we use Unladen Swallow's `-j always` flag to force all functions through LLVM, which gives us a more accurate picture of how our native code generation facility is performing.

[Benchmarking](Benchmarks.md) was done on an Intel Core 2 Duo 6600 @ 2.40GHz with 4GB RAM. The performance degradation/improvement is calculated using `((new - old) / new)`.

**2009Q1 vs 2009Q2**<br>
(32-bit; gcc 4.0.3; perf.py -r --args ",-j always -O2")<br>
<br>
ai:<br>
Min: 0.490245 -> 0.477799: 2.60% faster<br>
Avg: 0.492445 -> 0.481081: 2.36% faster<br>
Significant (t=42.490318, a=0.95)<br>
Stddev: 0.00075 -> 0.00257: 70.92% larger<br>
<br>
django:<br>
Min: 1.097285 -> 1.031586: 6.37% faster<br>
Avg: 1.099378 -> 1.034914: 6.23% faster<br>
Significant (t=191.190350, a=0.95)<br>
Stddev: 0.00142 -> 0.00306: 53.49% larger<br>
<br>
slowpickle:<br>
Min: 0.735551 -> 0.652740: 12.69% faster<br>
Avg: 0.737914 -> 0.653076: 12.99% faster<br>
Significant (t=258.803262, a=0.95)<br>
Stddev: 0.00327 -> 0.00023: 1320.90% smaller<br>
<br>
slowspitfire:<br>
Min: 0.788618 -> 0.663307: 18.89% faster<br>
Avg: 0.790304 -> 0.665141: 18.82% faster<br>
Significant (t=338.460137, a=0.95)<br>
Stddev: 0.00294 -> 0.00224: 31.54% smaller<br>
<br>
slowunpickle:<br>
Min: 0.317278 -> 0.279072: 13.69% faster<br>
Avg: 0.318411 -> 0.280351: 13.58% faster<br>
Significant (t=174.904639, a=0.95)<br>
Stddev: 0.00088 -> 0.00199: 55.74% larger<br>
<br>
<br>
<b>2009Q1 vs 2009Q2</b><br>
(32-bit; gcc 4.0.3; perf.py -r)<br>
<br>
normal_startup:<br>
Min: 0.378594 -> 0.294137: 28.71% faster<br>
Avg: 0.400236 -> 0.306967: 30.38% faster<br>
Significant (t=22.105565, a=0.95)<br>
Stddev: 0.00915 -> 0.04119: 77.80% larger