#!/bin/bash

as --64 -o obj/runtime0.o src/runtime0.asm
g++ -ffreestanding -m64 -fno-exceptions -fno-rtti -std=c++0x -c -o obj/main.o src/main.cpp
g++ -ffreestanding -m64 -fno-exceptions -fno-rtti -std=c++0x -c -o obj/hel.o src/hel.cpp
ld -nostdlib -z max-page-size=0x1000 -T src/link.ld -o bin/init.elf obj/runtime0.o obj/main.o obj/hel.o

