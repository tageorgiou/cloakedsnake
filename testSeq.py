import time
def baseconvert(n, base):
    digits = "0123456789abcdefghijklmnopqrstuvwxyz"
    try:
        n = int(n)
        base = int(base)
    except:
        return ""
    if n < 0 or base < 2 or base > 36:
        return ""
    s = ""
    while 1:
        r = n % base
        s = digits[r] + s
        n = n / base
        if n == 0:
            break
    return s

def getString(fr, to):
	for i in range(fr, to):
		yield baseconvert(i, 36)


from timeit import default_timer as clock

def main():
   strings = ["key_" + str(i) for i in xrange(1000000)]

   t = clock()
   d = dict.fromkeys(strings, 0)
   print round(clock() - t, 2)
   if len(d) < 200: print d

# import psyco; psyco.bind(main)

if __name__ == "__main__":
    # start = time.clock()
    # test = {}
    # for i in  getString(0, 1000000):
    #     test[i] = "Test"
    # print i
    # print (time.clock() - start) 
    main()
