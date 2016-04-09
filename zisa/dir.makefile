
$(call standard_dirs)

all-$c: $($c_BINDIR)/zisa

$c_CXX = x86_64-managarm-g++

$c_INCLUDES := -I$(TREE_PATH)/frigg/include

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++1y
$c_CXXFLAGS += -DFRIGG_HAVE_LIBC

$c_LIBS = -lprotobuf-lite

$(call make_exec,zisa,main.o)
$(call compile_cxx,$($c_SRCDIR),$($c_OBJDIR))

