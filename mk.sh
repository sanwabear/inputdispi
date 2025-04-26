#!/bin/sh
mkdir -p build
cd build
cmake ..
make

cd ..
mkdir -p bin
mv build/input_dispi bin
cp -Rp fonts bin
