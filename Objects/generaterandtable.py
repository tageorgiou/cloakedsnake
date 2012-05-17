randnums = open('randnums','r').read().split()
randgenerator = randnums.__iter__()
tables = 16
tablesize = 2**8
tableentrylength = 8
print "unsigned char randchartable[%d] = {" % (tables * tablesize * tableentrylength)

for i in range(0,tables):
    print "\t//table %d" % i
    #8 tables
    for j in range(0,tablesize):
        #256 entries per table
        print "\t",
        for k in range(0,tableentrylength):
           print "%3d," % int(randgenerator.next()),
        print ""
print "};"
print "long *randlongtable = (long*) randchartable;"
print "int *randinttable = (int*) randchartable;"
