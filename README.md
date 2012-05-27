cloakedsnake
============

Modified Python files:
Objects/dictobject.c: dictionary implementation
Objects/stringobject.c: changed string hash function
Objects/unicodeobject.c: changed unicode hash function
Include/Python.h: add various things to includes
Python/pythonrun.h: add instrumentation
A couple more miscellaneous intrumentation and code changes

We organized the code we modified as a bunch of ifdefs so that different parts
of our implementation may be turned on and off.  If you complete without
specifying compiler flags, standard python is built.  We have flags for 1 or 2
byte indices, various table sizes, turning prefetching on and off, selecting
the random number tables, linear probing, instrumentation, and a few more.

Our Benchmarks:
benchmark/: a few of the real world benchmarks
Tools/pybench: pybench, implementation specific benchmarks
docs/: graph generation and results files
hash-table-shootout: synthetic C benchmarks for python's dictionary implementation

In the root directory there are a couple other analysis programs and scripts we
wrote.
