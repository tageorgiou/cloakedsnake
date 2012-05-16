import os

versions = {
        'p0': '-DINSTRUMENT_DICT',
        'lp': '-DINSTRUMENT_DICT -DLINEAR_PROBING',
        'thm': '-DINSTRUMENT_DICT -DTABULATION_MAIN',
        'lpthm': '-DINSTRUMENT_DICT -DLINEAR_PROBING -DTABULATION_MAIN',
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
    
