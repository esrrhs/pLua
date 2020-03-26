#! /bin/sh
cmake . -DWITH_GRAPHVIZ=ON
make clean
make 

rm CMakeCache.txt -rf
cmake .
make clean
make
