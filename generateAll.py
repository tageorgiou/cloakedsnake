import os

versions = {
        'p0': '-DINSTRUMENT_DICT',
        'lp': '-DINSTRUMENT_DICT -DLINEAR_PROBING',
        #'thm': '-DINSTRUMENT_DICT -DTABULATION_MAIN',
        #'lpthm': '-DINSTRUMENT_DICT -DLINEAR_PROBING -DTABULATION_MAIN',
        #'lpths': '-DINSTRUMENT_DICT -DLINEAR_PROBING -DTABULATION_SHORT',
        #'lpthspf': '-DINSTRUMENT_DICT -DLINEAR_PROBING -DTABULATION_SHORT -DTABULATION_PREFETCH',
#        'lpthmpf': '-DINSTRUMENT_DICT -DLINEAR_PROBING -DTABULATION_MAIN -fprofile-use',
        #'lpthm1': '-DINSTRUMENT_DICT -DLINEAR_PROBING -DTABULATION_MAIN -DTABLE_MASK=0',
#        'lpthm2': '-DINSTRUMENT_DICT -DLINEAR_PROBING -DTABULATION_MAIN -DTABLE_MASK=1',
#        'lpthm4': '-DINSTRUMENT_DICT -DLINEAR_PROBING -DTABULATION_MAIN -DTABLE_MASK=3',
        'lpthm8': '-DINSTRUMENT_DICT -DLINEAR_PROBING -DTABULATION_MAIN -DTABLE_MASK=7 -DTABULATION_PREFETCH',
#        'lpthm16': '-DINSTRUMENT_DICT -DLINEAR_PROBING -DTABULATION_MAIN -DTABLE_MASK=15',
#        'lpths2': '-DINSTRUMENT_DICT -DLINEAR_PROBING -DTABULATION_SHORT -DTABLE_MASK=1',
#        'lpths4': '-DINSTRUMENT_DICT -DLINEAR_PROBING -DTABULATION_SHORT -DTABLE_MASK=3',
#        'lpths8': '-DINSTRUMENT_DICT -DLINEAR_PROBING -DTABULATION_SHORT -DTABLE_MASK=7',
#        'lpthmint': '-DINSTRUMENT_DICT -DLINEAR_PROBING -DTABULATION_MAIN -DRAND_TABLE_NAME=randinttable',
#        'lpthmpf': '-DINSTRUMENT_DICT -DLINEAR_PROBING -DTABULATION_MAIN -fprofile-use',
}

def generateVersion(version):
    os.system('./touchall')
    os.system('EXTRA_CFLAGS="%s" make -j2' % versions[version])
    os.chdir('hash-table-shootout')
    os.system('rm build/*')
    os.system('EXTRA_CFLAGS="%s" make -j2' % versions[version])
    os.system('cp build/python_dict benches/python_dict.%s' % version)
    os.chdir('..')

for version in versions.keys():
    generateVersion(version)
    
