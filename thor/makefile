
CXX = x86_64-managarm-g++
CXX_INCLUDE = -I../frigg/include -I../eir/include
CXX_FLAGS = -ffreestanding -m64 -mno-red-zone -mcmodel=kernel \
	-fno-exceptions -fno-rtti -std=c++0x -Wall $(CXX_INCLUDE)

AS = x86_64-managarm-as
AS_FLAGS = --64

LD = x86_64-managarm-ld
LD_FLAGS = -nostdlib -z max-page-size=0x1000 -T src/link.ld

KERNEL = bin/thor
OBJECTS = obj/frigg-debug.o obj/frigg-initializer.o obj/frigg-libc.o \
	obj/frigg-arch-gdt.o obj/frigg-arch-idt.o obj/frigg-arch-tss.o \
	obj/runtime0.o obj/runtime1.o \
	obj/physical.o obj/paging.o \
	obj/main.o obj/hel.o \
	obj/core.o obj/descriptor.o obj/usermem.o obj/schedule.o obj/ipc.o \
	obj/event.o obj/thread.o obj/rd.o obj/k_init.o

SRCDIR = src
FRIGG_SRCDIR = ../frigg/src
FRIGG_ARCH_SRCDIR = ../frigg/src/arch_x86

.PHONY: all clean

all: $(KERNEL)

clean:
	rm -f bin/* obj/*

$(KERNEL): $(OBJECTS)
	$(LD) $(LD_FLAGS) -o $(KERNEL) $(OBJECTS)

obj/%.o: src/%.cpp
	$(CXX) $(CXX_FLAGS) -c -o $@ $(SRCDIR)/$(@:obj/%.o=%.cpp)
	$(CXX) $(CXX_FLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" \
		$(SRCDIR)/$(@:obj/%.o=%.cpp)

obj/%.o: src/%.asm
	$(AS) $(AS_FLAGS) -o $@ $(SRCDIR)/$(@:obj/%.o=%.asm)

obj/frigg-%.o: $(FRIGG_SRCDIR)/%.cpp
	$(CXX) $(CXX_FLAGS) -c -o $@ $(FRIGG_SRCDIR)/$(@F:frigg-%.o=%.cpp)
	$(CXX) $(CXX_FLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" \
		$(FRIGG_SRCDIR)/$(@F:frigg-%.o=%.cpp)

obj/frigg-arch-%.o: $(FRIGG_ARCH_SRCDIR)/%.cpp
	$(CXX) $(CXX_FLAGS) -c -o $@ $(FRIGG_ARCH_SRCDIR)/$(@F:frigg-arch-%.o=%.cpp)
	$(CXX) $(CXX_FLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" \
		$(FRIGG_ARCH_SRCDIR)/$(@F:frigg-arch-%.o=%.cpp)

-include $(OBJECTS:%.o=%.d)

