from collections import defaultdict
from matplotlib.pyplot import *
import json
import matplotlib
matplotlib.rcParams['lines.linewidth'] = 2

datapoints = []
for line in open('output','r').read().split('\n'):
    if len(line) > 0:
        datapoints.append(json.loads(line))
programs = set(map(lambda l: l['program'], datapoints))
#benches = set(map(lambda l: l.split(',')[0], lines))
#dictsizes = sorted(list(set(map(lambda l: int(l.split(',')[1]), lines))))
#print benches
#dictsizes = [2,4,6]

lines = set(['python_dict.lpthm8','python_dict.lpthm8pf', 'python_dict.thm8', 'python_dict.lp',
'python_dict.p0'])

def plot_bench(benchname):
    filtered_points = filter(lambda l: l['benchtype'] == benchname, datapoints)
    results = defaultdict(list)
    for point in filtered_points:
#        if point['program'] == 'python_dict.lpthm2':
#            continue
        if point['program'] not in lines:
            continue
        #results[point['program']].append(point['chain-length'])
        #results[point['program']].append(point['runtime'] / int(point['nkeys']))
        #results[point['program']].append(point['scollisioncount'] / int(point['nkeys']))
        results[point['program']].append(point['runtime'])
    print results
    ax = figure().add_subplot(111)

    for key in results.keys():
        ax.plot(results[key], label=(key.replace('python_dict.','')))
    ax.set_xlabel('# keys')
    ax.set_ylabel('time (s)')
    ax.legend(loc=0)
    return ax

plot_bench('sequentialstring')
title('Sequential Integer Keys')
#title('Random String Keys')
#plot_bench('sequential')
#title('Sequential String Keys')
savefig('out.png')
show()
