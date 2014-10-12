#!/bin/bash

g++ -ffreestanding -m32 -fno-exceptions -fno-rtti -std=c++0x -c -o obj/main.o src/main.cpp
as --32 -o obj/multiboot.o src/multiboot.asm
ld -m elf_i386 -nostdlib -T src/link.ld -o bin/kernel.elf obj/multiboot.o obj/main.o

