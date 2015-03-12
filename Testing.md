Our goal is to be as compatible with mainline CPython 2.6.1 as possible, with both pure-Python code and C-language extension modules able to migrate seemlessly, with at most a simple recompilation required. To that end, we have a number of open-source Python projects that are run against every commit to make sure we haven't broken anything.

## Running The Third-Party Tests ##

The third-party tests have to be run against an _installed_ python, not just a build directory. To build and install a test python, assuming you build outside the source directory, run something like:

```
$ cd $test_python_objdir
$ installdir=`pwd`/../install    # For example.
$ $srcdir/configure --prefix=$installdir --any-other-configure-options
$ make -j3
$ ./python -mtest.regrtest   # To make sure Python itself is healthy.
$ make install
```

Next, check out Unladen Swallow's `/tests/` directory, which contains all the third-party packages we run against Unladen Swallow:

```
$ svn co http://unladen-swallow.googlecode.com/svn/tests unladen-tests
```

This will check out both the performance tests and the correctness tests, plus their support scripts.

In order for some of the tests to run correctly, they need to actually be installed, so we'll do that with our `setup.py` wrapper script. This will take care of building and installing all the packages we care about with a single call. It also takes care of building packages without a `setup.py` script, like Swig.

```
$ cd unladen-tests
$ $installdir/bin/python setup.py install
```

Now run the tests:

```
$ $installdir/bin/python test.py
```

## Testing Correctness ##

At every commit we run the tests for the following projects.

  * Python's own regression test suite
  * These applications/libraries:
    * 2to3
    * Cheetah
    * cvs2svn
    * Django
    * Nose
    * NumPy
    * PyCrypto
    * pyOpenSSL
    * PyXML
    * Setuptools
    * SQLAlchemy
    * SWIG
    * SymPy
    * Twisted
    * ZODB

We add new projects as we find them relevant or sufficiently important; we don't want to waste time testing the same code paths over and over again. Some projects that we'd like to use -- like Mercurial -- have interesting tests, but they're flaky or very slow to run and so aren't included.

### Caveats ###

We try to use the most recent releases for the above projects, but sometimes that isn't feasible. If the most recent release doesn't work with mainline CPython 2.6.1 (which Unladen Swallow is based on), we'll pull in that project's trunk at HEAD. In some cases that _still_ doesn't result in a cleanly-passing test suite, so we'll disable the few failing tests after verifying that they're bugs in the application, not in CPython 2.6.1. In rare cases when we find projects making unsound assumptions about the Python implementation they're running on (like the length of pickled objects, for example), we'll fix those tests and send the patches upstream. Any changes that we make to a project will be noted in a `README.unladen` file ([example](http://code.google.com/p/unladen-swallow/source/browse/tests/lib/twisted/README.unladen)).


## Testing the JIT ##

Unladen Swallow exposes knobs for tuning which code objects are run through LLVM. This is fairly coarse-grained, but allows us to test both the bytecode virtual machine and the native code pathways extensively:

  * **python -j never**: run all code through the bytecode interpreter.
  * **python -j whenhot**: the default; only run hot code though JIT compilation.
  * **python -j always**: compile all executed Python functions to machine code.

There is also a dedicated test in the Unladen Swallow standard library test suite, `test_llvm`, that attempts to test code generation. This alone isn't a sufficient guarantee of correctness, so we use these `-j` flags to give code generation more of a stress test.