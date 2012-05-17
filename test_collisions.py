dict_size = 10000
load_factor = .01 
def test_ht(size, load_factor):
  perturbing = False
  vals = [-1] * dict_size
  collisions = {}
  total_probes = 0
  total_collisions = 0
  for i in range(int(dict_size * load_factor)):
    collisions[i] = 0

  for i in range(int(dict_size * load_factor)):
    slot = hash(str(i))
    collisions[i] += 1
    total_probes += 1
    while vals[slot % dict_size] != -1:
      slot += 1
      total_probes += 1
    vals[slot % dict_size] = i 

  collisions = sorted(collisions.values())
  #print collisions[-50:]

  print "total_probes", total_probes

  #print "max bins: ", collisions[-50:]
  #print "max is", max(collisions)

  longest_chain = 0
  current_chain = 0
  longest_chain_set = []
  current_chain_set = []
  average_chain_len = 0
  num_chains = 0
  print len(vals)
  for i in range(dict_size):
    if vals[i] != -1:
      current_chain += 1
      average_chain_len += 1
      current_chain_set.append(vals[i])
    else:
      if current_chain > 0:
        num_chains += 1
      if current_chain > longest_chain:
        longest_chain = current_chain
        longest_chain_set = current_chain_set[:]
      current_chain = 0
      current_chain_set = []


  if current_chain > longest_chain:
    longest_chain = current_chain

  print "longest chain", longest_chain
  print "average chain", float(average_chain_len) / num_chains
  return longest_chain, float(average_chain_len) / num_chains

def linprob_test():
  table_size = 1000000
  load_factor = [.01, .05, .1, .2, .5, .75, .99,.999]
  print [(l, test_ht(table_size, l)[0]) for l in load_factor]
  print [(l, test_ht(table_size, l)[1]) for l in load_factor]

if __name__ == "__main__":
  linprob_test()
