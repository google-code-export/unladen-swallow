Any third-party code that we'll want to ship with Unladen Swallow should be tracked with a vendor branch under `vendor/`. This document describes how to create and update these vendor branches.


## Updating LLVM and Clang ##

LLVM is stored under `vendor/llvm`, Clang is under `vendor/clang`. This assumes you have a revision you'd like to update to, called `$GOLDEN` in these examples. This also assumes that you've tested both LLVM and Clang using their `make check` and `make test` commands. Instructions for building and testing LLVM and Clang can be [found in Clang's 'Get Started' guide](http://clang.llvm.org/get_started.html).

  1. Export LLVM at the desired revision.
```
svn export -r $GOLDEN http://llvm.org/svn/llvm-project/llvm/trunk llvm-export
```
  1. Using the [svn\_load\_dirs.pl](http://code.google.com/p/unladen-swallow/source/browse/vendor/scripts/svn_load_dirs.pl) in `vendor/scripts/`, push the LLVM update to Google Code. This will run `svn commit` automatically.
```
vendor/scripts/svn_load_dirs.pl https://unladen-swallow.googlecode.com/svn/vendor llvm llvm-export -m "Update LLVM to r$GOLDEN."
```
  1. Export Clang at the desired revision.
```
svn export -r $GOLDEN http://llvm.org/svn/llvm-project/cfe/trunk clang-export
```
  1. Using the [svn\_load\_dirs.pl](http://code.google.com/p/unladen-swallow/source/browse/vendor/scripts/svn_load_dirs.pl) in `vendor/scripts/`, push the Clang update to Google Code. This will run `svn commit` automatically.
```
vendor/scripts/svn_load_dirs.pl https://unladen-swallow.googlecode.com/svn/vendor clang clang-export -m "Update Clang to r$GOLDEN."
```
  1. Both of the `svn_load_dirs.pl` steps should have given you a final revision number for their checkins. We need to merge those changes into `trunk/`.
```
cd ~/unladen-swallow/src/trunk/Util/llvm
svn merge -c $LLVM_REV https://unladen-swallow.googlecode.com/svn/vendor/llvm .
cd ~/unladen-swallow/src/trunk/Util/llvm/tools/clang
svn merge -c $CLANG_REV https://unladen-swallow.googlecode.com/svn/vendor/clang .
```
  1. Now back up to `~/unladen-swallow/src/trunk/`, apply any necessary compatibility patches to Unladen Swallow and run `make test` before committing the merger.
  1. Once you've committed the merge to `Util/llvm`, you should check whether `Util/llvm/lib/Transforms/IPO/GlobalDCE.cpp` has changed. If it has, merge the changes into our copy with:
```
svn merge Util/llvm/lib/Transforms/IPO/GlobalDCE.cpp Util/DeadGlobalElim.cc
```

Example revisions:
  * [r530](https://code.google.com/p/unladen-swallow/source/detail?r=530): update LLVM vendor branch.
  * [r531](https://code.google.com/p/unladen-swallow/source/detail?r=531): update Clang vendor branch.
  * [r532](https://code.google.com/p/unladen-swallow/source/detail?r=532): merge both updates to trunk.