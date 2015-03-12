# CPython #

## Patches reviewed, submitted ##
  * [Issue 6042](http://bugs.python.org/issue6042). Document and simplify lnotab-based tracing.
  * [Issue 5670](http://bugs.python.org/issue5670). Improve performance of cPickle when pickling dicts.
  * [Issue 5665](http://bugs.python.org/issue5665). Add more tests for pickling, including backwards-compatibility tests.
  * [Issue 5588](http://bugs.python.org/issue5588). Add --randseed to regrtest.py.
  * [Issue 5176](http://bugs.python.org/issue5176). Special-case string formatting in BINARY\_MODULO implementation.
  * [Issue 4884](http://bugs.python.org/issue4884). Work around gethostbyaddr\_r bug.
  * [Issue 4597](http://bugs.python.org/issue4597). EvalFrameEx fails to set 'why' for some exceptions.
  * [Issue 4477](http://bugs.python.org/issue4477). Speed up PyEval\_EvalFrameEx when tracing is off.

## Patches under review ##
  * [Issue 5683](http://bugs.python.org/issue5683). Speed up cPickle's pickling generally via a simplified buffering scheme.
  * [Issue 5671](http://bugs.python.org/issue5671). Improve performance of cPickle when pickling lists.
  * [Issue 5575](http://bugs.python.org/issue5575). Add env vars for controlling building sqlite, hashlib and ssl.
  * [Issue 5572](http://bugs.python.org/issue5572). Make distutils correctly use the LIBS configure env var.
  * [Issue 5372](http://bugs.python.org/issue5372). Fix distutils to not inappropriately reuse .o files between extension modules.
  * [Issue 5362](http://bugs.python.org/issue5362). Add configure option to disable Py3k warnings.

## Yet to be extracted ##

Note that the listed revisions are necessary, but might not be sufficient; later revisions may have addressed style issues, etc.
  * [r194](https://code.google.com/p/unladen-swallow/source/detail?r=194), [r196](https://code.google.com/p/unladen-swallow/source/detail?r=196). Simpler, faster buffering structure for unpickling.
  * [r195](https://code.google.com/p/unladen-swallow/source/detail?r=195), [r199](https://code.google.com/p/unladen-swallow/source/detail?r=199). A pickle optimizer: trade pickling speed for unpickling speed.
  * [r200](https://code.google.com/p/unladen-swallow/source/detail?r=200), [r229](https://code.google.com/p/unladen-swallow/source/detail?r=229), [r230](https://code.google.com/p/unladen-swallow/source/detail?r=230). Replace the pickler's memo dict with a custom hashtable.
  * [r207](https://code.google.com/p/unladen-swallow/source/detail?r=207). Replace the unpickler's memo dict with a custom hashtable.


# LLVM #

## Patches reviewed, submitted ##
  * [r72426](http://llvm.org/viewvc/llvm-project?view=rev&revision=72426). Avoid quadratic space use in codegen.  This patch also inspired [r72411](http://llvm.org/viewvc/llvm-project?view=rev&revision=72411), which removed one large variable entirely.
  * [r70898](http://llvm.org/viewvc/llvm-project?view=rev&revision=70898). Fix codegen for conditional branches with `llc -march=cpp`.
  * [r70610](http://llvm.org/viewvc/llvm-project?view=rev&revision=70610). Fix an issue where llc -march=cpp fails on input containing the x86\_fp80 type.
  * [r70084](http://llvm.org/viewvc/llvm-project?view=rev&revision=70084). Add a new TypeBuilder helper class, which eases making LLVM IR types.
  * [r69958](http://llvm.org/viewvc/llvm-project?view=rev&revision=69958). Add ConstantInt::getSigned().
  * [r68768](http://llvm.org/viewvc/llvm-project?view=rev&revision=68768). Add a CreatePtrDiff() method to IRBuilder.
  * [r68277](http://llvm.org/viewvc/llvm-project?view=rev&revision=68277). Fix overflow checks in SmallVector.


# Rietveld #

## Patches reviewed, submitted ##
  * [Issue 40053](http://codereview.appspot.com/40053). Make editing a branch actually edit, rather than creating a new branch

## Patches under review ##
  * [Issue 40054](http://codereview.appspot.com/40054). Add the ability to edit and delete repositories.
  * [Issue 40050](http://codereview.appspot.com/40050). Make upload.py send svn cp'd and svn mv'd files correctly.