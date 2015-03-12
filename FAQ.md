## Q: Why base Unladen Swallow on 2.6.1 instead of 3.x? ##

A: We're based on the 2.6.1 branch because of internal considerations. Google's currently running 2.4 internally, and we want to offer our internal users an easy and stable target for porting. Python 2.7 is still in flux, and will be through the entire life of this project, and 3.0 would be too big a jump for our existing client applications. We want to make it as easy as possible for large applications to obtain the performance benefits we're offering.

That said, we _will_ port our changes to 3.0 for inclusion in mainline CPython. Whether our changes land in CPython 2.x or 3.y is a question for the CPython community -- we have to play by their rules on this.

## Q: Why branch CPython? Why not use Jython, IronPython or PyPy? ##

A: We looked at that. We wanted to offer Google Python applications better performance, and we obviously don't want to do more work than we have to. However, Google has a massive body of C++ code exposed to Python applications via [SWIG](http://www.swig.org), none of which would work with any of the other Python implementations. Google has many of these systems implemented in Java, too, but the people on the hook for these applications were uncomfortable with that much change in their code base; porting to Jython and Java infrastructure would not have been a small change, and the app owners don't want to be paged at 3AM if they can help it.

Ultimately, the reason for working off of CPython is compatibility: compatibility with pure-Python code, and also compatibility with the vast array of C extension modules in production.

## Q: What is your relationship with Google? ##

A: Unladen Swallow is Google-sponsored, but not Google-owned. The engineers on the project are full-time Google engineers, but ultimately this an open-source project, not really that different from [Chrome](http://code.google.com/chromium/) or [Google Web Toolkit](http://code.google.com/webtoolkit/). We are pushing patches back into mainline CPython as quickly as we can. These patches are all code-reviewed by the CPython community, and we cannot merge the patches without their consent.

## Q: Do I need to sign a Contributor License Agreement? ##

A: Yes. You should sign both [Google's CLA](http://code.google.com/legal/individual-cla-v1.0.html) and the [Python Software Foundation's CLA](http://www.python.org/psf/contrib/contrib-form/), since all the code that goes into Unladen Swallow is intended to end up back in CPython. At the very minimum, though, you would need to sign Google's CLA (linked above) and just be aware that we will probably ship your code in our patches back to the PSF-licensed CPython.

If you are a Google employee, your contributions are automatically covered by Google's blanket CLA with the PSF.