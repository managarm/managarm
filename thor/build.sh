#!/bin/bash

as --64 -o obj/runtime0.o src/runtime0.asm
g++ -ffreestanding -m64 -mno-red-zone -mcmodel=large -fno-exceptions -fno-rtti -std=c++0x -c -o obj/frigg-gdt.o ../frigg/src/arch_x86/gdt.cpp
g++ -ffreestanding -m64 -mno-red-zone -mcmodel=large -fno-exceptions -fno-rtti -std=c++0x -c -o obj/frigg-idt.o ../frigg/src/arch_x86/idt.cpp
g++ -ffreestanding -m64 -mno-red-zone -mcmodel=large -fno-exceptions -fno-rtti -std=c++0x -c -o obj/runtime1.o src/runtime1.cpp
g++ -ffreestanding -m64 -mno-red-zone -mcmodel=large -fno-exceptions -fno-rtti -std=c++0x -c -o obj/main.o src/main.cpp
g++ -ffreestanding -m64 -mno-red-zone -mcmodel=large -fno-exceptions -fno-rtti -std=c++0x -c -o obj/core.o src/core.cpp
g++ -ffreestanding -m64 -mno-red-zone -mcmodel=large -fno-exceptions -fno-rtti -std=c++0x -c -o obj/schedule.o src/schedule.cpp
g++ -ffreestanding -m64 -mno-red-zone -mcmodel=large -fno-exceptions -fno-rtti -std=c++0x -c -o obj/hel.o src/hel.cpp
g++ -ffreestanding -m64 -mno-red-zone -mcmodel=large -fno-exceptions -fno-rtti -std=c++0x -c -o obj/debug.o src/debug.cpp
g++ -ffreestanding -m64 -mno-red-zone -mcmodel=large -fno-exceptions -fno-rtti -std=c++0x -c -o obj/memory-physical-alloc.o src/memory/physical-alloc.cpp
g++ -ffreestanding -m64 -mno-red-zone -mcmodel=large -fno-exceptions -fno-rtti -std=c++0x -c -o obj/memory-paging.o src/memory/paging.cpp
g++ -ffreestanding -m64 -mno-red-zone -mcmodel=large -fno-exceptions -fno-rtti -std=c++0x -c -o obj/memory-kernel-alloc.o src/memory/kernel-alloc.cpp
ld -nostdlib -z max-page-size=0x1000 -T src/link.ld -o bin/kernel.elf obj/frigg-gdt.o obj/frigg-idt.o \
	obj/runtime0.o obj/runtime1.o \
	obj/main.o \
	obj/core.o \
	obj/schedule.o \
	obj/hel.o \
	obj/debug.o obj/memory-physical-alloc.o obj/memory-paging.o obj/memory-kernel-alloc.o

