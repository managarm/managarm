
CXX = x86_64-managarm-g++
CXX_INCLUDE =
CXX_FLAGS = -std=c++11 $(CXX_INCLUDE)

BINARY = bin/zisa
OBJECTS = obj/main.o obj/keyboard.pb.o

LIBS = -lprotobuf-lite

SRCDIR = src

.PHONY: all clean

all: $(BINARY)

clean:
	rm -f bin/* obj/*

$(BINARY): $(OBJECTS)
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

