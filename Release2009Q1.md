# Unladen Swallow 2009Q1 #

The first release. Unladen Swallow 2009Q1 is based on CPython 2.6.1. This release is primarily a collection of tweaks to 2.6.1, many of which are now available in mainline Python trunk. What are not currently available are in the process of being pushed upstream.

## Performance ##

We recommend building Unladen Swallow with 64-bit mode for maximum performance. The following timings are comparing virgin Python 2.6.1 against Unladen Swallow 2009Q1 in 64-bit mode. Compiled with gcc 4.3.1 on Intel Core 2 Duo machines. See the [Benchmarks](Benchmarks.md) page for a description of each benchmark below. If you have access to gcc 4.4, we highly recommend using the FDO support (via `-fprofile-generate` and `-fprofile-use`). We have found that training it on 2to3, Django and SlowSpitfire (via `test.py`) adds additional 10-30% percent performance, depending on the benchmark. We have not included those improvements in our results since gcc 4.4 is not widely available; FDO is available in earlier versions of gcc, but is not as easy to use.

Note on the benchmark names: the SlowFoo benchmarks are pure-Python versions. So SlowPickle is a pure-Python version of Pickle, which uses the `cPickle` module.

The performance improvements are calculated using `((new - old) / new)`. We feel this is a better representation of performance improvements than `((new - old) / old)`; you can see the difference with `old=100` and `new=5` then `new=2`.

2to3:<br>
Min: 22.888 -> 20.299: 12.75% faster<br>
Avg: 22.926 -> 20.329: 12.77% faster<br>
Significant (t=145.835478, a=0.95)<br>
<br>
Django:<br>
Min: 0.596 -> 0.554: 7.52% faster<br>
Avg: 0.598 -> 0.557: 7.43% faster<br>
Significant (t=297.475166, a=0.95)<br>
<br>
Pickle (complex):<br>
Min: 1.023 -> 0.409: 150.36% faster<br>
Avg: 1.053 -> 0.410: 157.17% faster<br>
Significant (t=1102.029662, a=0.95)<br>
<br>
Pickle (simple):<br>
Min: 1.223 -> 0.868: 40.83% faster<br>
Avg: 1.229 -> 0.876: 40.20% faster<br>
Significant (t=695.483070, a=0.95)<br>
<br>
PyBench:<br>
Min: 46961 -> 38795: 21.05% faster<br>
Avg: 47775 -> 39635: 20.54% faster<br>
<br>
SlowPickle:<br>
Min: 1.236 -> 1.072: 15.22% faster<br>
Avg: 1.239 -> 1.076: 15.17% faster<br>
Significant (t=497.615245, a=0.95)<br>
<br>
SlowSpitfire:<br>
Min: 0.762 -> 0.670: 13.87% faster<br>
Avg: 0.764 -> 0.671: 13.80% faster<br>
Significant (t=452.978688, a=0.95)<br>
<br>
SlowUnpickle:<br>
Min: 0.606 -> 0.528: 14.63% faster<br>
Avg: 0.607 -> 0.530: 14.60% faster<br>
Significant (t=581.549445, a=0.95)<br>
<br>
Unpickle (complex):<br>
Min: 0.738 -> 0.536: 37.71% faster<br>
Avg: 0.746 -> 0.547: 36.24% faster<br>
Significant (t=122.112665, a=0.95)<br>
<br>
Unpickle (simple):<br>
Min: 0.756 -> 0.486: 55.60% faster<br>
Avg: 0.774 -> 0.493: 56.91% faster<br>
Significant (t=331.578243, a=0.95)<br>
<br>
<h2>Feedback-Directed Optimization</h2>

This table summarizes the benefits we saw from using gcc 4.4's feedback-directed optimizations to improve Python performance. We experimented with a number of training loads. Both training and measurement was done with <code>perf.py --rigorous</code>. We observed similar benefits across both 32-bit and 64-bit Python.<br>
<br>
In these charts, the columns indicate the training loads (SlowFoo means all the "slow" benchmarks: SlowSpitfire, SlowPickle, SlowUnpickle). "MEAN-PYBENCH-C" indicates the mean of the benchmarks if PyBench and the cPickle benchmarks are excluded. The percentage values are improvements over the baseline; negative numbers indicate performance regressions.<br>
<br>
<table><thead><th> </th><th> Training=SlowSpitfire </th><th> Training=2to3 </th><th> Training=PyBench </th><th> Training=2to3,Django,SlowFoo </th></thead><tbody>
<tr><td> 2to3 </td><td>4%</td><td>7%</td><td>5%</td><td>6%</td></tr>
<tr><td> Django </td><td>10%</td><td>10%</td><td>7%</td><td>13%</td></tr>
<tr><td> Pickle </td><td>-4%</td><td>0%</td><td>-1%</td><td>-1%</td></tr>
<tr><td>PyBench</td><td>-6%</td><td>2%</td><td>5%</td><td>-7%</td></tr>
<tr><td>SlowPickle</td><td>9%</td><td>4%</td><td>9%</td><td>8%</td></tr>
<tr><td>SlowSpitfire</td><td>32%</td><td>23%</td><td>27%</td><td>25%</td></tr>
<tr><td>SlowUnpickle</td><td>10%</td><td>4%</td><td>15%</td><td>12%</td></tr>
<tr><td>Unpickle</td><td>0%</td><td>6%</td><td>1%</td><td>6%</td></tr>
<tr><td>  </td></tr>
<tr><td>MEAN</td><td>6.88%</td><td>7.00%</td><td>8.50%</td><td>7.75%</td></tr>
<tr><td>MEAN-PYBENCH</td><td>8.71%</td><td>7.71%</td><td>9.00%</td><td>9.86%</td></tr>
<tr><td>MEAN-PYBENCH-C</td><td>13.00%</td><td>9.60%</td><td>12.60%</td><td>12.80%</td></tr></tbody></table>


The results above, removing PyBench and any benchmarks that stress C-language extension modules.<br>
<table><thead><th> </th><th> Training=SlowSpitfire </th><th> Training=2to3 </th><th> Training=PyBench </th><th> Training=2to3,Django,SlowFoo </th></thead><tbody>
<tr><td> 2to3 </td><td>4%</td><td>7%</td><td>5%</td><td>6%</td></tr>
<tr><td> Django </td><td>10%</td><td>10%</td><td>7%</td><td>13%</td></tr>
<tr><td>SlowPickle</td><td>9%</td><td>4%</td><td>9%</td><td>8%</td></tr>
<tr><td>SlowSpitfire</td><td>32%</td><td>23%</td><td>27%</td><td>25%</td></tr>
<tr><td>SlowUnpickle</td><td>10%</td><td>4%</td><td>15%</td><td>12%</td></tr>
<tr><td>  </td></tr>
<tr><td>GEOMEAN</td><td>10.29%</td><td>7.62%</td><td>10.50%</td><td>11.34%</td></tr>