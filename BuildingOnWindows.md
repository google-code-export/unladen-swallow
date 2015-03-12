Before building Unladen Swallow the bundled copy of LLVM needs to be compiled.  Then the python executable can be built by using the PCBuild/PCBuild.sln solution file as in CPython.

You will need CMake 2.6 or later (http://www.cmake.org/) in order to build the LLVM project files.  There are two versions of the instructions depending on whether you prefer the command line or the GUI tools.

# Building LLVM using the GUI tools #

These instructions assume you have Unladen in C:\unladen-swallow.  If not, adjust the paths as appropriate
  1. Run cmake-gui and enter `"C:/unladen-swallow/Util/llvm"` in the "Where is the source code" box.
  1. Enter `"C:/unladen-swallow/Util/llvm/obj"` in the "Where to build the binaries" box.
  1. Click "Configure", select `"Visual Studio 9 2008"` or `"Visual Studio 8 2005"` as the generator, and click Finish.
  1. If you are building the Q3 release, change LLVM\_TARGETS\_TO\_BUILD from `"X86"` to `"X86;CppBackend"`
  1. Click "Configure" again
  1. Click "Generate"
  1. Open C:\unladen-swallow\Util\llvm\obj\LLVM.sln in Visual Studio
  1. Click Build | Batch Build.  Put a check mark next to ALL\_BUILD | Release.
  1. If you are interested in building a Debug build of Unladen, you should additionally check ALL\_BUILD | Debug.
  1. Click "Build"

After LLVM has been built you can open PCBuild/PCBuild.sln and build Unladen.

# Building LLVM via the command line #

Instructions contributed by Baptiste Lepilleur.  Make sure CMake
is in your PATH or specify the full path to cmake.exe in the commands below.

```
> cd Util\llvm

Util\llvm> mkdir obj

Util\llvm> cd obj
```

If you are building the latest trunk:
```
Util\llvm\obj> cmake -G "Visual Studio 9 2008" ..
```
(or you can use `"Visual Studio 8 2005"` instead)

If you are building the Q3 release:
```
Util\llvm\obj> cmake -G "Visual Studio 9 2008" -D LLVM_TARGETS_TO_BUILD="X86;CppBackend" ..
```

Continuing:
```
-- Check for working C compiler: cl
-- Check for working C compiler: cl -- works
...
-- Configuring done
-- Generating done
-- Build files have been written to: E:/prg/thirdparties/unladen/Util/llvm/obj

Util\llvm\obj>"%VS90COMNTOOLS%\vsvars32.bat"
Setting environment for using Microsoft Visual Studio 2008 x86 tools.

Util\llvm\obj>vcbuild /M%NUMBER_OF_PROCESSORS% LLVM.sln "Release|Win32"
...
181>ALL_BUILD - up-to-date
Skipped building project E:\prg\thirdparties\unladen\Util\llvm\obj\.\INSTALL.vcproj
for solution configuration RELEASE|WIN32. This project is excluded from build for this
solution configuration.

Build complete: 91 Projects succeeded, 0 Projects failed, 26 Projects skipped
```

If you want to build only a release build of Unladen Swallow then you can skip the next step.

```
Util\llvm\obj>vcbuild /M%NUMBER_OF_PROCESSORS% LLVM.sln "Debug|Win32"
...
181>Build started: Project: ALL_BUILD, Configuration: Debug|Win32
181>ALL_BUILD - up-to-date
Skipped building project E:\prg\thirdparties\unladen\Util\llvm\obj\.\INSTALL.vcproj
for solution configuration DEBUG|WIN32. This project is excluded from build for this
solution configuration.

Build complete: 91 Projects succeeded, 0 Projects failed, 26 Projects skipped
```

At this point you can open PCBuild/PCBuild.sln and build Unladen.