#!/bin/bash

#set -x # print commands
PYTHON=python3

THIS_SCRIPT=$0
if [ -z "$ROOT_DIR" ]; then ROOT_DIR=$(cd `dirname $THIS_SCRIPT`; pwd); fi
echo "ROOT_DIR == " $ROOT_DIR
BUILDPATH=$(dirname `find $ROOT_DIR -name halide.*so | tail -n1`)
echo "BUILDPATH ==" $BUILDPATH
export PYTHONPATH="$BUILDPATH:$PYTHONPATH"
echo "PYTHONPATH ==" $PYTHONPATH

# Operate in the build directory, so that output files don't pollute the top-level directory.
cd build

FAILED=0

# separator
S=" --------- "
Sa=" >>>>>>>> "
Sb=" <<<<<<<< "


for i in ${ROOT_DIR}/apps/*.py
do
  echo $S $PYTHON $i $S
  $PYTHON $i
  if [[ "$?" -ne "0" ]]; then
        echo "$Sa App failed $Sb"
	let FAILED=1
	break
  fi
done

if [[ "$FAILED" -ne "0" ]]; then
  exit -1
else
  echo "$S (all applications ran) $S"
fi
