#read 2 streams from [tmp, stdin?] -- make tmp files in repo to track?

#regex to find per groups.  Put in 2 dicts keyed on the name

#compare categories by percent difference x-y/x

#output (possibly sorted)

import sys
import re
pat = r"(\w*):\s*([0-9\.]*(m|u)s)\s*([0-9\.]*(m|u)s)\s*([0-9\.]*(m|u)s)\s*([0-9\.]*(m|u)s)"
tot = r"Totals:\s*([0-9\.]*(m|u)s)\s*([0-9\.]*(m|u)s)"
def diff_profile(f1, f2):
  dat1 = file(f1).read()
  dat2 = file(f2).read()

  fields1 = re.findall(pat, dat1)
  fields2 = re.findall(pat, dat2)

  tot1 = re.findall(tot, dat1)
  tot2 = re.findall(tot, dat2)

  results1 = dict([(field[0], [field[1], field[3], field[5]]) for field in fields1])
  results2 = dict([(field[0], [field[1], field[3], field[5]]) for field in fields2])

  final_results = {}
  for key in results1:
    if key in results2:
      r1Val = [parse_time(x) for x in results1[key]]
      r2Val = [parse_time(x) for x in results2[key]]
      final_results[key] = sum([percent_diff(r1, r2) for (r1, r2) in zip(r1Val,
        r2Val)]) / len(r1Val)

      final_results[key] = percent_diff(r1Val[0], r2Val[0])
      print "%s:  %f%%" % (key, final_results[key])

  print "-----------"
  tot1 = tot1[0]
  tot2 = tot2[0]
  print "Totals: %f%%" % percent_diff(parse_time(tot1[2]), parse_time(tot2[2]))



  
def parse_time(t):
  if 'ms' in t:
    return float(t.replace('ms', ''))*.001
  elif 'us' in t:
    return float(t.replace('us', ''))*.000001
  else:
    return t

def percent_diff(x1, x2):
  return (x1-x2)/x1

if __name__ == "__main__":
  if len(sys.argv) == 3:
    diff_profile(sys.argv[1], sys.argv[2])
