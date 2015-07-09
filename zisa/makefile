
CXX = x86_64-managarm-g++
CXX_INCLUDE =

PROGRAM = bin/init.elf
OBJECTS = obj/main.o

SRCDIR = src

.PHONY: all clean

all: $(PROGRAM)

clean:
	rm -f $(PROGRAM) $(OBJECTS)

$(PROGRAM): $(OBJECTS)
	$(CXX) -o $@ $(OBJECTS)

obj/%.o: src/%.cpp
	$(CXX) $(CXX_INCLUDE) -c -o $@ $(SRCDIR)/$(@:obj/%.o=%.cpp)

