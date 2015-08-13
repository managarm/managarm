
NAKED_DIR = include/bragi-naked

NAKED_HEADERS = $(NAKED_DIR)/ld-server.nakedpb.hpp

.PHONY: all

all: $(NAKED_HEADERS)

$(NAKED_DIR)/%.nakedpb.hpp: proto/%.proto
	protoc --proto_path=proto --plugin=protoc-gen-nakedcxx=../tools/protobuf-nakedcxx/bin/protobuf-nakedcxx --nakedcxx_out=include/bragi-naked $<

