#!/bin/bash

as --64 -o obj/runtime0.o src/runtime0.asm
g++ -ffreestanding -m64 -mno-red-zone -mcmodel=large -fno-exceptions -fno-rtti -std=c++0x -c -o obj/runtime1.o src/runtime1.cpp
g++ -ffreestanding -m64 -mno-red-zone -mcmodel=large -fno-exceptions -fno-rtti -std=c++0x -c -o obj/main.o src/main.cpp
g++ -ffreestanding -m64 -mno-red-zone -mcmodel=large -fno-exceptions -fno-rtti -std=c++0x -c -o obj/debug.o src/debug.cpp
ld -nostdlib -z max-page-size=0x1000 -T src/link.ld -o bin/kernel.elf obj/runtime0.o obj/runtime1.o obj/main.o obj/debug.o

