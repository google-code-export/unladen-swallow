# Unladen Swallow Benchmarks #

The Unladen Swallow benchmark suite is kept in the [tests/](http://code.google.com/p/unladen-swallow/source/browse/#svn/tests) directory (note that this is in the `/svn/tests/` tree, parallel to trunk). `tests/perf.py` is the main interface to the tests, with the individual benchmarks stored under the `tests/performance/` directory.

To check out the latest version of the benchmarks:

```
svn checkout http://unladen-swallow.googlecode.com/svn/tests unladen-bmarks
```

Example `perf.py` command:

```
python2.5 unladen-bmarks/perf.py -r --benchmarks=2to3,django control/python experiment/python
```

This will run the 2to3 and Django template benchmarks in rigorous mode (lots of iterations), taking `control/python` as the baseline and `experiment/python` as the binary you've been mucking around with. `perf.py` will take care of comparing the performance and running statistics on the result to determine statistical significance.


## Quick-Start Guide ##

Not all benchmarks are created equal: some of the benchmarks listed below are more useful than others. If you're interested in overall system performance, the best guide is this:

```
python unladen-bmarks/perf.py -r -b default control/python experiment/python
```

That will run the benchmarks we consider the most important headline indicators of performance.

There's an additional collection of whole-app benchmarks that are important, but take longer to run:

```
python unladen-bmarks/perf.py -r -b apps control/python experiment/python
```


## Benchmarks ##

  * **2to3** - have [the 2to3 tool](http://svn.python.org/view/sandbox/trunk/2to3/) translate itself.
  * **calls** - collection of function and method call microbenchmarks:
    * **call\_simple** - positional arguments-only function calls.
    * **call\_method** - positional arguments-only method calls.
    * **call\_method\_slots** - method calls on classes that use `__slots__`.
    * **call\_method\_unknown** - method calls where the receiver cannot be predicted.
  * **django** - use the [Django template system](http://www.djangoproject.com/) to build a 150x150-cell HTML table.
  * **float** - artificial, floating point-heavy benchmark originally used by [[Factor](http://factor-language.blogspot.com/2009/08/performance-comparison-between-factor.html).
  * **html5lib** - parse [the HTML 5 spec](http://svn.whatwg.org/webapps/index) using [html5lib](http://code.google.com/p/html5lib/).
  * **html5lib\_warmup** - like `html5lib`, but gives the JIT a chance to warm up by doing the iterations in the same process.
  * **nbody** - the [N-body Shootout benchmark](http://shootout.alioth.debian.org/u64q/benchmark.php?test=nbody&lang=python&id=4). Microbenchmark for floating point operations.
  * **nqueens** - small solver for the N-Queens problem.
  * **pickle** - use the `cPickle` module to pickle a variety of datasets.
  * **pickle\_dict** - microbenchmark; use the `cPickle` module to pickle a lot of dicts.
  * **pickle\_list** - microbenchmark; use the `cPickle` module to pickle a lot of lists.
  * **pybench** - run the standard Python [PyBench](http://svn.python.org/projects/python/trunk/Tools/pybench/README) benchmark suite. This is considered an unreliable, unrepresentative benchmark; do not base decisions off it. It is included only for completeness.
  * **regex** - collection of regex benchmarks:
    * **regex\_compile** - stress the performance of Python's regex compiler, rather than the regex execution speed.
    * **regex\_effbot** - some of the original benchmarks used to tune mainline Python's current regex engine.
    * **regex\_v8** - Python port of [V8's regex benchmark](http://code.google.com/p/v8/source/browse/trunk/benchmarks/regexp.js).
  * **richards** - the classic Richards benchmark.
  * **rietveld** - macrobenchmark for Django using the [Rietveld](http://code.google.com/p/rietveld/) code review app.
  * **slowpickle** - use the pure-Python `pickle` module to pickle a variety of datasets.
  * **slowspitfire** - use the [Spitfire template system](http://code.google.com/p/spitfire/) to build a 1000x1000-cell HTML table. Unlike the `spitfire` benchmark listed below, `slowspitfire` does not use Psyco.
  * **slowunpickle** - use the pure-Python `pickle` module to unpickle a variety of datasets.
  * **spitfire** - use the [Spitfire template system](http://code.google.com/p/spitfire/) to build a 1000x1000-cell HTML table, taking advantage of Psyco for acceleration.
  * **spambayes** - run a canned mailbox through a [SpamBayes](http://spambayes.sourceforge.net/) ham/spam classifier.
  * **startup** - collection of microbenchmarks focused on Python interpreter start-up time:
    * **bzr\_startup** - get [Bazaar](http://bazaar.canonical.com/)'s help screen.
    * **hg\_startup** - get [Mercurial](http://mercurial.selenic.com/)'s help screen.
    * **normal\_startup** - start Python, then exit immediately.
    * **startup\_nosite** - start Python with the `-S` option, then exit immediately.
  * **threading** - collection of microbenchmarks for Python's threading support. These benchmarks come in pairs: an iterative version (iterative\_foo), and a multithreaded version (threaded\_foo).
    * **threaded\_count**, **iterative\_count** - spin in a while loop, counting down from a large number.
  * **unpack\_sequence** - microbenchmark for unpacking lists and tuples.
  * **unpickle** - use the `cPickle` module to unpickle a variety of datasets.

## Benchmark Groups ##

We have grouped the above benchmarks into a number of categories. These categories are called "benchmark groups" in perf.py, and are runnable just like the individual benchmarks; running a benchmark group will run all benchmarks in that group.

Groups:
  * **apps**: 2to3, html5lib, rietveld, spambayes
  * **calls**: call\_simple, call\_method, call\_method\_slots, call\_method\_unknown
  * **cpickle**: pickle, unpickle
  * **default**: 2to3, django, nbody, slowspitfire, slowpickle, slowunpickle, spambayes
  * **math**: float, nbody
  * **regex**: regex\_compile, regex\_effbot, regex\_v8
  * **startup**: bzr\_startup, hg\_startup, normal\_startup, startup\_nosite
  * **threading**: iterative\_count, threaded\_count

The `default` benchmark group is the main group we use to assess pure-Python application performance. Other groups are more specialized. Use the group most appropriate to your optimization, but always check for an impact on the `default` group.

## Memory benchmarking ##

`perf.py` supports a `--track_memory` option that will continuously sample the benchmark process's memory usage throughout the process's lifetime. It will then compare the maximum memory usage for the control and experiment Python binaries, and will give the user a link to follow to see memory usage over time. Example graph:

![http://chart.apis.google.com/chart?cht=lc&chs=350x200&chxt=x,y&chxr=1,11396,106272&chco=FF0000,0000FF&chdl=obj/python|obj10000/python&chds=11396,106272&chd=t:11496,23952,23956,19308,19384,19424,19460,19460,19556,19592,19628,19672,22564,20884,23372,37552,30476,30488,36408,32204,32240,32280,32292,32356,32428,32452,39460,39660,35328,35368,35364,35364,35364,35364,35364,40780,48384,104888,106172,85212,85328,85380,85624,85668,85848,86000,86164,86320,86488,86668,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86832|12872,15868,17576,18268,16788,18128,17288,18520,17288,18748,17288,18520,17288,18608,17288,18292,17288,18520,17288,18292,17288,18520,17288,18520,17288,18520,18644,17288,18520,17288,18660,17288,18520,17288,18520,17288,18768,17288,18288,17288,18520,17288,18520,17288,18640,17288,18292,17288,18640,17288,18752,18628,17288,18520,17288,18644,17288,18644,17288,18600,17288,18520,17288,18292,17288,18292,17288,18740,17288,18520,17288,18520,18736,17288,18520,17288,18520,17288,18520,17288,18768,17288,18520,17288,18644,17288,18712,17288,18520,17288,18656,17288,18520,17288,18776,17288,18292,18520,17288,18604&f=.png](http://chart.apis.google.com/chart?cht=lc&chs=350x200&chxt=x,y&chxr=1,11396,106272&chco=FF0000,0000FF&chdl=obj/python|obj10000/python&chds=11396,106272&chd=t:11496,23952,23956,19308,19384,19424,19460,19460,19556,19592,19628,19672,22564,20884,23372,37552,30476,30488,36408,32204,32240,32280,32292,32356,32428,32452,39460,39660,35328,35368,35364,35364,35364,35364,35364,40780,48384,104888,106172,85212,85328,85380,85624,85668,85848,86000,86164,86320,86488,86668,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86788,86832|12872,15868,17576,18268,16788,18128,17288,18520,17288,18748,17288,18520,17288,18608,17288,18292,17288,18520,17288,18292,17288,18520,17288,18520,17288,18520,18644,17288,18520,17288,18660,17288,18520,17288,18520,17288,18768,17288,18288,17288,18520,17288,18520,17288,18640,17288,18292,17288,18640,17288,18752,18628,17288,18520,17288,18644,17288,18644,17288,18600,17288,18520,17288,18292,17288,18292,17288,18740,17288,18520,17288,18520,18736,17288,18520,17288,18520,17288,18520,17288,18768,17288,18520,17288,18644,17288,18712,17288,18520,17288,18656,17288,18520,17288,18776,17288,18292,18520,17288,18604&f=.png)

The Y axis is memory usage in kilobytes, the X axis corresponds to time.


## Benchmarks we don't use ##

We do not include [PyBench](http://svn.python.org/view/python/trunk/Tools/pybench/), [PyStone](http://code.google.com/p/unladen-swallow/source/browse/tests/performance/pystone.py) or [Richards](http://code.google.com/p/unladen-swallow/source/browse/tests/performance/richards.py) in our default benchmark suite. PyStone and Richards are synthetic benchmarks that may or may not translate into improved performance for real-world applications. We would like to avoid basing decisions on PyStone or Richards, only to find out that a real application sees no benefit -- or worse, is slowed down. In both cases, these benchmarks have a long history and have gone through many translations: PyStone was originally written in Ada, then translated to C, then translated to Python and does not represent idiomatic Python code or its performance hot spots. Richards was originally written in BCPL, then translated to Smalltalk, then to C++, then to Java and finally to Python; it does a little better at testing OO performance, but doesn't involve string processing at all, something that many Python applications rely on heavily. Also, it is not idiomatic Python code.

While PyBench may be an acceptable collection of microbenchmarks, it is not a reliable or precise benchmark. We have observed swings of up to 10% between runs on unloaded machines using the same version of Python; we would like to detect performance differences of 1% accurately. For us, the final nail in PyBench's coffin was when experimenting with gcc's feedback-directed optimization tools, we were able to produce a universal 15% performance increase across our macrobenchmarks; using the same training workload, PyBench got 10% slower. For this reason, we do not factor in PyBench results to our decision-making.

Beyond these benchmarks, there are also a variety of workloads we're explicitly **not** interested in benchmarking. Unladen Swallow is focused on improving the performance of pure Python code, so the performance of extension modules like [numpy](http://numpy.scipy.org/) is uninteresting since numpy's core routines are implemented in C. Similarly, workloads that involve a lot of IO like GUIs, databases or socket-heavy apps would, we feel, be inappropriate. That said, there's certainly room to improve the performance of C-language extensions modules in the standard library; we've done this for `cPickle` and will do this for `re`. The performance of non-standard extension modules, though, is less interesting.