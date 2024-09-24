#!/usr/bin/env bash

mpicc -g -O0 -o mm25d.dbg MM25D.c utils.c -lm -L$MKLROOT/lib/intel64 -lmkl_rt
mpicc -O3 -march=native -mtune=native -o mm25d MM25D.c utils.c -lm -L$MKLROOT/lib/intel64 -lmkl_rt
