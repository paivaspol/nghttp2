#!/bin/bash
prefix=$1
CFLAGS=-I$prefix/include \
CXXFLAGS=-I$prefix/include \
LDFLAGS=-L$prefix/lib \
./configure --prefix=$prefix
