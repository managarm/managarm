
CXX = x86_64-managarm-g++
CXX_INCLUDE =
CXX_FLAGS = -fpic -ffreestanding -fno-exceptions -fno-rtti \
	-std=c++0x -fvisibility=hidden $(CXX_INCLUDE)

AS = x86_64-managarm-as
AS_FLAGS =

LD = x86_64-managarm-ld
LD_FLAGS = -shared

LIBRARY = bin/ld-init.so
OBJECTS = main.o runtime.o frigg-debug.o frigg-initializer.o frigg-libc.o
OBJECT_PATHS = $(addprefix obj/,$(OBJECTS))

SRCDIR = src
FRIGG_SRCDIR = ../frigg/src

.PHONY: all clean

all: $(LIBRARY)

clean:
	rm -f $(LIBRARY) $(OBJECT_PATHS)

$(LIBRARY): $(OBJECT_PATHS)
	$(LD) $(LD_FLAGS) -o $(LIBRARY) $(OBJECT_PATHS)

obj/%.o: src/%.cpp
	$(CXX) $(CXX_FLAGS) -c -o $@ $(SRCDIR)/$(@F:%.o=%.cpp)
	$(CXX) $(CXX_FLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" \
		$(SRCDIR)/$(@F:%.o=%.cpp)

obj/%.o: src/%.s
	$(AS) $(AS_FLAGS) -o $@ $(SRCDIR)/$(@F:%.o=%.s)

obj/frigg-%.o: $(FRIGG_SRCDIR)/%.cpp
	$(CXX) $(CXX_FLAGS) -c -o $@ $(FRIGG_SRCDIR)/$(@F:frigg-%.o=%.cpp)
	$(CXX) $(CXX_FLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" \
		$(FRIGG_SRCDIR)/$(@F:frigg-%.o=%.cpp)

-include $(OBJECT_PATHS:%.o=%.d)

