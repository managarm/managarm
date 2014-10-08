#!/bin/bash

gcc -ffreestanding -m32 -c -o obj/main.o -std=c99 src/main.c
as --32 -o obj/multiboot.o src/multiboot.asm
ld -m elf_i386 -nostdlib -T src/link.ld -o bin/kernel.elf obj/multiboot.o obj/main.o

