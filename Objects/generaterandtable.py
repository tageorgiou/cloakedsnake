randnums = open('randnums','r').read().split()
randgenerator = randnums.__iter__()
print "unsigned char randchartable[%d] = {" % (8 * 256 * 8)

for i in range(0,8):
    print "\t//table %d" % i
    #8 tables
    for j in range(0,256):
        #256 entries per table
        print "\t",
        for k in range(0,8):
           print "%3d," % int(randgenerator.next()),
        print ""
print "};"
print "long *randtable = (long*) randchartable;"
