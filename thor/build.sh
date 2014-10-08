#!/bin/bash

as --64 -o obj/rt_entry.o src/rt_entry.asm
g++ -ffreestanding -m64 -mno-red-zone -mcmodel=large -c -o obj/main.o src/main.cpp
ld -nostdlib -z max-page-size=0x1000 -T src/link.ld -o bin/kernel.elf obj/rt_entry.o obj/main.o

