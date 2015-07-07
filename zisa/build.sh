#!/bin/bash

x86_64-managarm-g++ -std=c++0x -c -o obj/main.o src/main.cpp
x86_64-managarm-g++ -std=c++0x -c -o obj/hel.o src/hel.cpp
x86_64-managarm-g++ -z max-page-size=0x1000 -o bin/init.elf obj/main.o obj/hel.o

