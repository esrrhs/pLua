#! /bin/sh
rm CMakeCache.txt -rf
cmake . -DWITH_GRAPHVIZ=ON
make clean
make 

