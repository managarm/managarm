
CXX = x86_64-elf-g++
CXX_INCLUDE =
CXX_FLAGS = -ffreestanding -m32 -fno-exceptions -fno-rtti -std=c++0x $(CXX_INCLUDE)

AS = x86_64-elf-as
AS_FLAGS = --32

LD = x86_64-elf-ld
LD_FLAGS = -m elf_i386 -nostdlib -T src/link.ld

KERNEL = bin/eir.elf
OBJECTS = obj/frigg-arch-gdt.o obj/frigg-initializer.o obj/frigg-debug.o obj/main.o obj/multiboot.o
LIBS = libgcc.a

SRCDIR = src
FRIGG_SRCDIR = ../frigg/src
FRIGG_ARCHDIR = ../frigg/src/arch_x86

.PHONY: all clean

all: $(KERNEL)

clean:
	rm -f $(KERNEL) $(OBJECTS)

$(KERNEL): $(OBJECTS)
	$(LD) $(LD_FLAGS) -o $(KERNEL) $(OBJECTS) $(LIBS)

obj/%.o: src/%.cpp
	$(CXX) $(CXX_FLAGS) -c -o $@ $(SRCDIR)/$(@:obj/%.o=%.cpp)
	$(CXX) $(CXX_FLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" \
		$(SRCDIR)/$(@:obj/%.o=%.cpp)

obj/%.o: src/%.asm
	$(AS) $(AS_FLAGS) -o $@ $(SRCDIR)/$(@:obj/%.o=%.asm)

obj/frigg-%.o: $(FRIGG_SRCDIR)/%.cpp
	$(CXX) $(CXX_FLAGS) -c -o $@ $(FRIGG_SRCDIR)/$(patsubst obj/frigg-%.o,%.cpp,$@)
	$(CXX) $(CXX_FLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" \
		$(FRIGG_SRCDIR)/$(@:obj/frigg-%.o=%.cpp)

obj/frigg-arch-%.o: $(FRIGG_ARCHDIR)/%.cpp
	$(CXX) $(CXX_FLAGS) -c -o $@ $(FRIGG_ARCHDIR)/$(@F:frigg-arch-%.o=%.cpp)
	$(CXX) $(CXX_FLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" \
		$(FRIGG_ARCHDIR)/$(@F:frigg-arch-%.o=%.cpp)

-include $(OBJECTS:%.o=%.d)

