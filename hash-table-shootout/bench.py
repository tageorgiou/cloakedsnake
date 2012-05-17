import sys, os, subprocess, signal

programs = [
#    'glib_hash_table',
#    'stl_unordered_map',
#    'boost_unordered_map',
#    'google_sparse_hash_map',
#    'google_dense_hash_map',
#    'qt_qhash',
#    'python_dict.lp',
    'python_dict.lpthm',
    'python_dict.lpths',
    'python_dict.lpthspf',
#    'python_dict.lpthmpf',
#    'python_dict.thm',
    'python_dict.p0',
#    'ruby_hash',
#    'python_dict.lpthm1',
#    'python_dict.lpthm2',
#    'python_dict.lpthm4',
#    'python_dict.lpthm8',
#    'python_dict.lpthm16',
]

minkeys  =  1*1000*1000
#maxkeys  = 40*1000*1000
maxkeys  = 4*1000*1000
interval =  2*1000*1000
best_out_of = 3

# for the final run, use this:
#minkeys  =  2*1000*1000
#maxkeys  = 40*1000*1000
#interval =  2*1000*1000
#best_out_of = 3
# and use nice/ionice
# and shut down to the console
# and swapoff any swap files/partitions

outfile = open('output', 'w')

if len(sys.argv) > 1:
    benchtypes = sys.argv[1:]
else:
    benchtypes = ('sequential', 'random', 'delete', 'sequentialstring', 'randomstring', 'deletestring')

for benchtype in benchtypes:
    nkeys = minkeys
    while nkeys <= maxkeys:
        for program in programs:
            fastest_attempt = 1000000
            fastest_attempt_data = ''

            for attempt in range(best_out_of):
                proc = subprocess.Popen(['./benches/'+program, str(nkeys), benchtype], stdout=subprocess.PIPE)

                # wait for the program to fill up memory and spit out its "ready" message
                try:
                    runtime = float(proc.stdout.readline().strip())
                except:
                    runtime = 0

                ps_proc = subprocess.Popen(['ps up %d | tail -n1' % proc.pid], shell=True, stdout=subprocess.PIPE)
                nbytes = int(ps_proc.stdout.read().split()[4]) * 1024
                ps_proc.wait()

                os.kill(proc.pid, signal.SIGKILL)
                proc.wait()

                if nbytes and runtime: # otherwise it crashed
                    line = ','.join(map(str, [benchtype, nkeys, program, nbytes, "%0.6f" % runtime]))

                    if runtime < fastest_attempt:
                        fastest_attempt = runtime
                        fastest_attempt_data = line

            if fastest_attempt != 1000000:
                print >> outfile, fastest_attempt_data
                print fastest_attempt_data

        nkeys += interval
