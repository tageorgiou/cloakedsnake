from collections import defaultdict
from matplotlib.pyplot import *

lines = open('output','r').read().split()
benches = set(map(lambda l: l.split(',')[0], lines))
print benches

def plot_bench(benchname):
    filtered_lines = [l for l in lines if l.split(',')[0] == benchname]
    results = defaultdict(list)
    for line in filtered_lines:
        benchname,size,impl_name,um,time = line.split(',')
        results[impl_name].append(time)
    print results
    for key in results.keys():
        plot(results[key], label=key)
    legend()
    show()

plot_bench('sequentialstring')
