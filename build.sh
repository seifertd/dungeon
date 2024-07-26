#!/bin/bash

export LD_LIBRARY_PATH=~/External/raylib-5.0_linux_i386/lib
gcc -I ~/External/raylib-5.0_linux_i386/include dungeon.c -lraylib -lm -o dungeon
