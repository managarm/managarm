#!/bin/bash

as -o obj/rt_entry.o src/rt_entry.asm
ld -nostdlib -z max-page-size=0x1000 -T src/link.ld -o bin/kernel.elf obj/rt_entry.o

