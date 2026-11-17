#!/bin/bash
# usage: matrix.sh BIN OUTDIR
bin=$1 out=$2; mkdir -p $out
cd /home/user/shadps4-open-test
for t in $(ls out); do
  [ -f out/$t/$t.elf ] || continue
  for cfg in "mirror precise" "shared precise" "shared disabled"; do
    set -- $cfg; sb=""; [ $1 = shared ] && sb=--shared-backing
    log=$out/$t.$1-$2.log
    SHADPS4=$bin timeout 300 bash scripts/run-test-lavapipe.sh $t --readbacks $2 $sb > $log 2>&1
    echo "$t $1-$2 exit=$? $(grep -hoE 'status=[A-Z]+( reason=[a-z_]+)?' $log | head -1)"
  done
done
