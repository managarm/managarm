
CXX = x86_64-managarm-g++
CXX_INCLUDE =
CXX_FLAGS = -std=c++11 $(CXX_INCLUDE)

PROGRAM = bin/zisa.elf
OBJECTS = obj/main.o obj/keyboard.pb.o

LIBS = -lprotobuf-lite -ltest

STRIP = x86_64-managarm-strip

SRCDIR = src

.PHONY: all clean

PROGRAM_ORIGINAL = $(PROGRAM:%.elf=%.original.elf)

all: $(PROGRAM)

clean:
	rm -f $(PROGRAM) $(PROGRAM_ORIGINAL) $(OBJECTS)

$(PROGRAM): $(PROGRAM_ORIGINAL)
	$(STRIP) -s -o $@ $<

$(PROGRAM_ORIGINAL): $(OBJECTS)
	$(CXX) -o $@ $(OBJECTS) $(LIBS)

obj/%.o: src/%.cpp
	$(CXX) $(CXX_FLAGS) -c -o $@ $(SRCDIR)/$(@:obj/%.o=%.cpp)
	$(CXX) $(CXX_FLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" \
		$(SRCDIR)/$(@:obj/%.o=%.cpp)
obj/%.pb.o: src/%.pb.cc
	$(CXX) $(CXX_FLAGS) -c -o $@ $(SRCDIR)/$(@F:%.pb.o=%.pb.cc)
	$(CXX) $(CXX_FLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" \
		$(SRCDIR)/$(@F:%.pb.o=%.pb.cc)

-include $(OBJECTS:%.o=%.d)

