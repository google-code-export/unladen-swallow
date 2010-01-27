#!/bin/bash
#
# Use this script to install LLVM to a temporary directory. This is useful for
# making Unladen Swallow build faster. cd Util/llvm and run one of these:
#
# ./install-llvm.sh debug --prefix=/tmp/llvm-dbg --other-configure-arg
# ./install-llvm.sh release --prefix=/tmp/llvm-rel --some-other-option=foo
# ./install-llvm.sh release+symbols --prefix=/tmp/llvm-syms
#
# debug == passing --with-pydebug to Python's ./configure
# release == omitting --with-pydebug to Python's ./configure
# release+symbols == like release, but with debug symbols
#
# The release mode is the closest to what will be shipped by package managers.
#
# You can then use Unladen Swallow's --with-llvm=/tmp/llvm-dbg option.
# The {debug,release,release+symbols} option you pass to this script should
# match up with whether you use --with-pydebug with ./configure.

# Keep these arguments in sync with the top-level configure.in.
LLVM_ARGS="--enable-jit --enable-targets=host --enable-bindings=none"
DEBUG_ARGS="--disable-optimized --enable-debug-runtime --enable-assertions"
REL_SYMS_ARGS="--enable-optimized --disable-assertions --enable-debug-symbols"
RELEASE_ARGS="--enable-optimized --disable-assertions --disable-debug-symbols"
LLVM_DIR=`dirname $0`

case "$1" in
    "debug") LLVM_ARGS="$LLVM_ARGS $DEBUG_ARGS";;
    "release") LLVM_ARGS="$LLVM_ARGS $RELEASE_ARGS";;
    "release+symbols") LLVM_ARGS="$LLVM_ARGS $REL_SYMS_ARGS";;
    *) echo "Invalid build type: '$1'"; \
       exit 1;;
esac

shift  # Take the first argument off the front of $@.
$LLVM_DIR/configure $LLVM_ARGS $@ && make && make install
