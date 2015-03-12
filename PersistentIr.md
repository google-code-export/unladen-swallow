# Replacing .pyc files with LLVM bitcode #

From Talin's email:

## Module structure ##

An LLVM module is the basic compilation unit. If we weren't planning on exporting compiled code to disk, I'd say put everything into a single module. Otherwise, I think a 1:1 correspondence from Python module to LLVM module would work.

In addition to per-module optimized bitcode, we'll also want to allow cross-module optimizations. This could go in init.pyc files for package-level optimized code, or in the main module's .pyc file.

## Non-executable metadata ##
Another issue is how to store the non-executable metadata along with the generated IR. There are 3 options:

<ol>
<li>Store the metadata as global constants or initialized global variables declared in the module file. In other words, create an IR representation of every metadata property and store it in the LLVM module. You can then use the LLVM BitCodeReader/Writer classes to read in the module.<br>
<br>
The drawback of this approach is that building up data structures in IR can be rather tedious, especially as each value has to be marshalled into an IR datatype. It can be considerably easier if there is some data-driven way to do the translation between the in-memory format of the metadata and the IR format.</li>

<li>Embed the bitcode within a larger file that contains the metadata. In other words, you serialize your own metadata using whatever format you like, and somewhere inside the file there's a big binary blob representing the LLVM module. You subclass one of the LLVM stream classes and hook up a BitCodeReader/Writer to read that blob.<br>
<br>
Looks like <a href='http://llvm.org/docs/BitCodeFormat.html#wrapper'>http://llvm.org/docs/BitCodeFormat.html#wrapper</a> describes how to do this.</li>

<li>Store the metadata and the bitcode as separate files with different extensions. This is probably the easiest to implement, but requires that you have two generated files for each source file.</li>
</ol>