LLVM functions need to know what parameters to expect from their environment. This is similar to the various C calling conventions defined in [PyMethodDef](http://docs.python.org/c-api/structures.html#PyMethodDef). Initially, I'm just using the PyFrameObject to communicate everything, so the signature of an LLVM-generated function is:

`PyObject *the_function(PyFrameObject *frame);`

We use the frame->f\_code (the PyCodeObject) to access lots of constants. This is dumb, of course, since those constants are known at compile time, but it's easy so we can get something working quickly.

Second, we rely on Python's existing argument-passing machinery. Python copies arguments into the frame's fastlocals array, and we just look them up there. Eventually, we'll want to pass arguments directly into the LLVM functions, but we'll need a new calling convention for that.