
CXX = x86_64-managarm-g++
CXX_INCLUDE = -I../eir/include
CXX_FLAGS = -ffreestanding -m64 -mno-red-zone -mcmodel=large \
	-fno-exceptions -fno-rtti -std=c++0x $(CXX_INCLUDE)

AS = x86_64-managarm-as
AS_FLAGS = --64

LD = x86_64-managarm-ld
LD_FLAGS = -nostdlib -z max-page-size=0x1000 -T src/link.ld

KERNEL = bin/thor.elf
OBJECTS = obj/frigg-gdt.o obj/frigg-idt.o obj/frigg-tss.o \
	obj/runtime0.o obj/runtime1.o \
	obj/main.o obj/hel.o \
	obj/core.o obj/descriptor.o obj/usermem.o obj/schedule.o obj/ipc.o \
	obj/event.o obj/thread.o obj/rd.o \
	obj/debug.o \
	obj/memory/physical-alloc.o obj/memory/paging.o \
	obj/memory/kernel-alloc.o

SRCDIR = src
FRIGG_SRCDIR = ../frigg/src/arch_x86

.PHONY: all clean

all: $(KERNEL)

clean:
	rm -f $(KERNEL) $(OBJECTS)

$(KERNEL): $(OBJECTS)
	$(LD) $(LD_FLAGS) -o $(KERNEL) $(OBJECTS)

obj/%.o: src/%.cpp
	$(CXX) $(CXX_FLAGS) -c -o $@ $(SRCDIR)/$(@:obj/%.o=%.cpp)
	$(CXX) $(CXX_FLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" \
		$(SRCDIR)/$(@:obj/%.o=%.cpp)

obj/%.o: src/%.asm
	$(AS) $(AS_FLAGS) -o $@ $(SRCDIR)/$(@:obj/%.o=%.asm)

obj/frigg-%.o: $(FRIGG_SRCDIR)/%.cpp
	$(CXX) $(CXX_FLAGS) -c -o $@ $(FRIGG_SRCDIR)/$(@:obj/frigg-%.o=%.cpp)
	$(CXX) $(CXX_FLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" \
		$(FRIGG_SRCDIR)/$(@:obj/frigg-%.o=%.cpp)

-include $(OBJECTS:%.o=%.d)

