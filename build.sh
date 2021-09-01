#! /bin/sh
dir=$(cd `dirname $0`;pwd)
projectdir=$dir/
builddir="$projectdir/build"
rundir="$projectdir/bin"

if [ ! -d "$rundir" ]; then
  mkdir -p $rundir && cd $rundir
fi

if [ -d "$builddir" ]; then
  rm $builddir -rf
  mkdir -p $builddir && cd $builddir
  cd $builddir
else
  mkdir -p $builddir && cd $builddir
fi

cmake ../
make
