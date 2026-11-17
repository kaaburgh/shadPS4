#!/bin/bash
# usage: matrix2.sh BIN OUTDIR TEST...
bin=$1 out=$2; shift 2; mkdir -p $out
cd /home/user/shadps4-open-test
for t in "$@"; do
  for cfg in "mirror precise" "shared precise" "shared disabled"; do
    set -- $cfg; sb=""; [ $1 = shared ] && sb=--shared-backing
    log=$out/$t.$1-$2.log
    SHADPS4=$bin timeout 300 bash scripts/run-test-lavapipe.sh $t --readbacks $2 $sb > $log 2>&1
    echo "$t $1-$2 $(grep -hoE 'status=[A-Z]+( reason=[a-z_]+)?|HOST_RESULT FAIL.*' $log | head -1 | cut -c1-80)"
  done
done
