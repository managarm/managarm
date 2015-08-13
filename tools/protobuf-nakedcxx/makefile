
BINARY = bin/protobuf-nakedcxx

bin/protobuf-nakedcxx: obj/main.o
	g++ -o $@ $< -lprotobuf -lprotoc

obj/%.o: src/%.cpp
	g++ -c -o $@ -std=c++11 $<

