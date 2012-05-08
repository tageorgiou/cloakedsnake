#!/bin/bash
#finds all the file source files that contain the relevant ifdef
make > clean-bout
echo "built clean version"
mv python python-clean

if [$# = 1]
then
  echo "No args.  Assuming running with all flags in IFDEF_LIST"
  ifdef_flags=`grep -o -E '^.*?:' IFDEF_LIST | sed 's/://'`
else
  echo "Pulling flags from args"
  for arg in "$@"
  do 
    ifdef_flags="$arg $ifdef_flags"
  done
fi


flaglist=''
for flag in $ifdef_flags
do
  echo "processing $flag"
  flaglist="-D$flag $flaglist"
  involvedfiles=`find -name *.c | xargs grep "$flag" | grep -o -E '^.*?:'| uniq | sed 's/://'`
  for f in $involvedfiles
  do
    touch $f
    echo "touched $f"
  done
done

echo "Building optimized binary.  Flags are: $flaglist"
EXTRA_CFLAGS="$flaglist" make > profile-out/opt-bout
echo "built optimized version"
mv python python-opt

echo "starting first benmark run"
./python-clean Tools/pybench/pybench.py | tee profile-out/outputNotOptimized

echo "starting second benmark run"
./python-opt Tools/pybench/pybench.py | tee profile-out/outputOptimized

python comp_per.py profile-out/outputNotOptimized profile-out/outputOptimized
