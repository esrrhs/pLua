#! /bin/sh
dir=$(cd `dirname $0`;pwd)
projectdir=$dir
builddir="$projectdir/build"
rundir="$projectdir/bin"
luadir="$projectdir/dep/lua-5.3.6"
lua="$projectdir/dep/lua-5.3.6.tar.gz"

if [ -f "$lua" ] && [ ! -d "$luadir" ]; then
  cd $projectdir/dep && tar zxvf $lua
  cd $projectdir && cp $projectdir/dep/lua-5.3.6/src/*.h $projectdir/src
fi

if [ ! -d "$rundir" ]; then
  mkdir -p $rundir && cd $rundir
fi

if [ -d "$builddir" ]; then
  rm $builddir -rf
  mkdir -p $builddir && cd $builddir
else
  mkdir -p $builddir && cd $builddir
fi

cmake ../
make

cd ../tools
go build plua.go
go build png.go

chmod a+x pprof
chmod a+x *.pl
chmod a+x *.sh
