

# Introduction #

[OProfile](http://oprofile.sourceforge.net/) is a system-wide sampling-based profiler for Linux systems. You need to be root to use it.

OProfile has [supported JIT compilers](http://oprofile.sourceforge.net/doc/devel/index.html) since the 0.9.4 release, but 0.9.5 fixed a bug in line number handling on 64-bit systems.  [0.9.6](http://prdownloads.sourceforge.net/oprofile/oprofile-0.9.6.tar.gz) is the most recent release, and it seems to work fine. Install it somewhere on your system.

Pass `--with-oprofile=/prefix/path` to the LLVM or Unladen Swallow `configure` script. So, if you built OProfile with its default prefix, you'll pass `--with-oprofile=/usr/local`. If you installed OProfile from your distribution's packaging system, you should be able to use just `--with-oprofile`, since it defaults to the `/usr` prefix.

Assuming `$PWD` is a built Unladen Swallow directory, and you've [checked out the tests](http://unladen-swallow.googlecode.com/svn/tests) to `../tests`, you'll see:

```
$ sudo opcontrol --reset; sudo opcontrol --start-daemon; sudo opcontrol --start; \
PYTHONPATH=../tests/lib/django/ ./python ../tests/performance/bm_django.py \
-n 100; sudo opcontrol --stop

Signalling daemon... done
Profiler running.
...
Stopping profiling.

$ opreport -g -l ./python |less
CPU: Core 2, speed 1998 MHz (estimated)
Counted CPU_CLK_UNHALTED events (Clock cycles when not halted) with a unit mask of 0x00 (Unhalted core cycles) count 100000
samples  %        linenr info                 image name               symbol name
186745    3.4261  obmalloc.c:1275             python                   write_size_t
135246    2.4813  frameobject.c:411           python                   frame_dealloc
133457    2.4484  frameobject.c:586           python                   PyFrame_New
132905    2.4383  eval.cc:3151                python                   PyEval_EvalCodeEx
123259    2.2613  obmalloc.c:1260             python                   read_size_t
123123    2.2589  tupleobject.c:28            python                   PyTuple_New
118909    2.1815  object.c:2147               python                   _Py_ForgetReference
117928    2.1635  encoding.py:44              26545.jo                 #u#force_unicode
116538    2.1380  methodobject.c:18           python                   PyCFunction_NewEx
105709    1.9394  eval.cc:4350                python                   _PyEval_CallFunction
103707    1.9026  typeobject.c:1169           python                   PyType_IsSubtype
99938     1.8335  obmalloc.c:1437             python                   _PyObject_DebugCheckAddress
95842     1.7583  stringobject.c:164          python                   PyString_FromFormatV
95285     1.7481  tupleobject.c:162           python                   tupledealloc
91328     1.6755  object.c:59                 python                   _Py_AddToAllObjects
88539     1.6244  (no location information)   libc-2.7.so              memset
87952     1.6136  defaulttags.py:116          26545.jo                 #u#render5
82625     1.5159  object.c:1316               python                   PyObject_GenericGetAttr
80607     1.4788  dictobject.c:392            python                   lookdict_string
74722     1.3709  object.c:2172               python                   _Py_Dealloc
71016     1.3029  methodobject.c:181          python                   meth_dealloc
68272     1.2525  __init__.py:534             26545.jo                 #u#resolve
67952     1.2467  obmalloc.c:725              python                   PyObject_Malloc
67392     1.2364  getargs.c:1495              python                   vgetargskeywords
66798     1.2255  gcmodule.c:1393             python                   PyObject_GC_UnTrack
63645     1.1677  typeobject.c:2495           python                   _PyType_Lookup
...
```

Here, `#u#force_unicode` and `#u#render5` are Python functions named `force_unicode()` and `render()`, and defined at encoding.py:44 and defaulttags.py:116.

You can also run oprofile with the `-d` flag to get instruction-level timings.

http://code.google.com/p/jrfonseca/wiki/Gprof2Dot may be useful in visualizing profile results.

# Troubleshooting #

## Using oProfile on Ubuntu and Debian ##

The oProfile packages in Debian Sid and Ubuntu Hardy, Jaunty, Karmic, and
Lucid are all broken. You can go to https://edge.launchpad.net/~statik/+archive/ppa to get fixed oProfile 0.9.6 Ubuntu packages for Hardy, Jaunty, Karmic, and Lucid. That page has instructions on how to install packages from that PPA (personal package archive).

More details passed on from Elliot Murphy, an Ubuntu developer:

> Debian sid has oProfile (0.9.6-1), but the package does not currently
> create the oprofile user and group needed for JIT profiler to work. If
> you are using this version of the package, you will need to manually
> create an oprofile user, otherwise JIT profiling just silently doesn't
> work. I will be sending a patch to Debian to so that the package
> automatically creates the necessary system user when the package is
> installed.

> In Ubuntu, oProfile package was very old, 0.9.4+cvs20090629-2.1ubuntu3
> or so, and this package has several major problems: doesn't work on 64
> bit, doesn't install the opjitconv command, and doesn't create the
> oprofile user. I am working to fix this so that Lucid (Ubuntu 10.04)
> will have a modern version of oProfile with all of these problems
> fixed. For Unladen Swallow hackers who want to easily use oProfile on
> Ubuntu now without having to install from source, I have built fixed
> packages in my personal archive.

## Tips/Tricks ##

  * If oprofile is not working and you are working on a laptop that has been suspended/resumed, try running `opcontrol --shutdown` and then running oProfile again.