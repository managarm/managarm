
CXX = x86_64-managarm-g++
CXXINCLUDE = -I../frigg/include
CXXFLAGS = -std=c++11 -ffreestanding -fno-exceptions -fno-rtti $(CXXINCLUDE)

AS = x86_64-managarm-as
AS_FLAGS =

LD = x86_64-managarm-ld

BINARIES = bin/ld-server bin/ld-init.so

server_OBJECTS = main.o frigg-glue-hel.o frigg-debug.o frigg-initializer.o
server_OBJECT_PATHS = $(addprefix obj/server/,$(server_OBJECTS))

linker_OBJECTS = main.o frigg-glue-hel.o linker.o runtime.o \
	frigg-debug.o frigg-initializer.o frigg-libc.o
linker_OBJECT_PATHS = $(addprefix obj/linker/,$(linker_OBJECTS))

SRCDIR = src
FRIGG_SRCDIR = ../frigg/src

.PHONY: all clean

all: $(BINARIES)

clean:
	rm -f bin/* obj/linker/* obj/server/*

bin/ld-server: $(server_OBJECT_PATHS)
	$(CXX) -o $@ -nodefaultlibs $+

bin/ld-init.so: $(linker_OBJECT_PATHS)
	$(LD) -shared -o $@ $+

obj/server/%.o: L_CXXFLAGS = $(CXXFLAGS)
obj/linker/%.o: L_CXXFLAGS = $(CXXFLAGS) -fpic -fvisibility=hidden

$(SRCDIR)/linker/frigg-%.cpp: $(FRIGG_SRCDIR)/%.cpp
	install -p $< $@

$(SRCDIR)/server/frigg-%.cpp: $(FRIGG_SRCDIR)/%.cpp
	install -p $< $@

obj/%.o: $(SRCDIR)/%.cpp
	$(CXX) -c -o $@ $(L_CXXFLAGS) $<
	$(CXX) $(CXXINCLUDE) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

obj/%.o: $(SRCDIR)/%.s
	$(AS) $(AS_FLAGS) -o $@ $<

-include $(OBJECT_PATHS:%.o=%.d)

