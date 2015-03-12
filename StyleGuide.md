C code continues to follow [PEP 7](http://www.python.org/dev/peps/pep-0007/). C++ should follow PEP 7 as much as possible. For things PEP 7 doesn't cover, try to follow [the LLVM style guide](http://llvm.org/docs/CodingStandards.html). For things that neither the LLVM style guide nor PEP 7 cover, consult [the Google C++ style guide](http://google-styleguide.googlecode.com/svn/trunk/cppguide.xml). We also have a few rules below for things they don't cover.

# Always use `this`. #

In C++, inside a class, you're allowed to call member functions and access member variables without explicitly using the `this` keyword. We prefer to keep things explicit for three reasons.

  1. Python requires `self`, so requiring `this` is consistent.
  1. It's easier to know where to look for a function when the prefix tells you where it lives.
  1. There are [certain cases in C++ where `this` is required](http://www.parashift.com/c++-faq-lite/templates.html#faq-35.19), and always requiring it avoids confusion when you run into those cases.


# Don't throw exceptions. #

We don't throw exceptions because so much CPython code isn't exception safe. However, we _may_ start catching particular exceptions thrown by the standard library like std::bad\_alloc in order to convert them to a form Python can deal with inside the language.

# Capitalization #

Class and struct names, member function names (all of public, private, and static), PyPublic\_Function, and `_`PyInternal\_Function names should all be `CamelCased`. Static functions within a .cc file are `lower_case_with_underscores`. Variable names are `lower_case_with_underscores`. Member variable names `end_with_an_underscore_`.

# Smart Pointers #

For now, we're only using [llvm::OwningPtr](http://llvm.org/doxygen/classllvm_1_1OwningPtr.html) (and OwningArrayPtr). `std::auto_ptr` does nearly the same thing, but has confusing assignment and copy construction semantics, so we're avoiding it. We don't have a smart pointer for Python refcounting because we couldn't use it in the C code anyway, and we haven't yet discussed the requirements for a non-PyObject refcounting pointer.

# C++ is Optional #

Any C++ used in Unladen Swallow should be disabled when configured with `--without-llvm`. Accordingly, any C++ should be guarded with `#ifdef WITH_LLVM`.