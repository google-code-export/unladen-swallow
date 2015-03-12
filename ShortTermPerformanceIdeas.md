# Introduction #

Phase One of Unladen Swallow will be based on making the current implementation faster; Phase Two will rip all of this out and replace the current compiler with LLVM, which will solve all problems, including allergies and cancer. In the meantime, we still have to make this thing faster:


## To-be-implemented ##
<ol>
<li><b>Lighter PyFrameObject</b>. Make these quicker to create, use less memory.</li>
</ol>

## To-be-reviewed ##
(Other people's patches)
<ol>
<li>GC changes: <a href='http://bugs.python.org/issue4688'>http://bugs.python.org/issue4688</a></li>
<li>Localized type inference for list.append: <a href='http://bugs.python.org/issue4264'>http://bugs.python.org/issue4264</a></li>
<li>Speed up for/while/if with better bytecode: <a href='http://bugs.python.org/issue2459'>http://bugs.python.org/issue2459</a></li>
<li>Faster globals/builtins access: <a href='http://bugs.python.org/issue1518'>http://bugs.python.org/issue1518</a></li>
<li>Speed up function calls: <a href='http://bugs.python.org/issue1479611'>http://bugs.python.org/issue1479611</a>, <a href='http://bugs.python.org/issue1107887'>http://bugs.python.org/issue1107887</a></li>
</ol>


## Implemented/In progress ##
<ol>
<li>Convert the Python eval loop's dispatch mechanism to threaded code. This will provide much better CPU-level branch prediction. This was merged in <a href='https://code.google.com/p/unladen-swallow/source/detail?r=172'>r172</a> for a ~5% speedup (+/- a lot depending on the compiler and benchmark).</li>
<li>Merged <a href='http://bugs.python.org/issue4074'>http://bugs.python.org/issue4074</a> into trunk. Much better GC behaviour in the face of large numbers of long-lived objects.</li>
<li>Replace slow/rarely-used opcodes with function calls. Hypothesis: by eliminating stack variables and shrinking the physical size of PyEval_EvalFrameEx, we a) make it faster to call EvalFrameEx, b) save icache space. This is now in trunk. The speedup is marginal, but cleans up the eval loop.</li>
</ol>


## Considered, Rejected ##
<ol>
<li>Import <a href='http://svn.python.org/view?rev=67818&view=rev'>r67818</a> from mainline Python. This isn't a clear win: some benchmarks are faster, some are slower. Attempts to pinpoint the slowdowns are inconclusive.</li>
<li>Iterative eval loop. Hypothesis: if we can reuse a single "instance" of the PyEval_EvalFrameEx C stack frame for Python-to-Python function calls, that will be faster than creating a new stack frame for each Python function call. Making this work with the current implementation would have introduced a prohibitively-high amount of new complication for what we estimate to have been a relatively minor performance improvement.</li>
</ol>