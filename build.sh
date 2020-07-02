#! /bin/sh
rm CMakeCache.txt -rf
cmake .
make clean
make
