from collections import defaultdict
from matplotlib.pyplot import *

lines = open('output','r').read().split()
benches = set(map(lambda l: l.split(',')[0], lines))
dictsizes = sorted(list(set(map(lambda l: int(l.split(',')[1]), lines))))
#print benches

def plot_bench(benchname):
    filtered_lines = [l for l in lines if l.split(',')[0] == benchname]
    results = defaultdict(list)
    for line in filtered_lines:
        benchname,size,impl_name,um,time = line.split(',')
        results[impl_name].append(time)
    print results
    ax = figure().add_subplot(111)

    for key in results.keys():
        ax.plot(dictsizes, results[key], label=key)
    ax.set_xlabel('keys')
    ax.set_ylabel('time (s)')
    ax.legend()
    return ax

#plot_bench('randomstring')
#title('Random String Keys')
plot_bench('sequentialstring')
title('Sequential String Keys')
savefig('out.png')
show()
