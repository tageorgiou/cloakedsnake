import random
res = []
for i in range(0, 256, 1):
    res.append(random.randint(-2**63+1, 2**63-1))
for j in range(8):
    print "long table" + str(j) + "[256] = {"
    for i in range(0, len(res), 8):
        str_q = str(res[i:i+8])[1 : -1]
        print str_q + ", \\"
    print "};"
