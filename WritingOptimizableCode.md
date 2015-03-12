No matter how many optimizations we implement for Unladen Swallow, it can all be defeated by antagonistic code. This code may not even be malicious: it may simply be poor style, or poor coding practices that we've chosen not to optimize for. Below we collect a number of tips, tricks and pitfalls for writing Python code that our optimizations can pick up and run with.

<ul>
<li><b>Don't pass state via globals.</b> Using a module's global namespace as a write-heavy scratchpad for your function is a bad idea, not just for readability's sake, but because it defeats caching of globals and builtins that Unladen Swallow would otherwise perform. Reading globals is fine: writing globals slows things down.<br>
<br>
Bad code (reduced from Python's <code>Lib/test/test_mutants.py</code>):<br>
<pre><code>def test_one(n):<br>
    global mutate<br>
<br>
    mutate = 0<br>
    maybe_mutate()<br>
    mutate = 1<br>
    # More code follows<br>
<br>
def maybe_mutate():<br>
    global mutate<br>
    if mutate == 0:<br>
       return<br>
    # More code follows<br>
    mutate = 0<br>
    # Code code code<br>
    mutate = 1<br>
</code></pre>

Storing this state in an object with <code>test_one()</code> and <code>maybe_mutate()</code> methods would be cleaner and allow more aggressive optimization of Python's globals and builtins.</li>
</ul>

This list will be expanded as we find new ways of pessimizing Python code or as we build additional optimizations.