
$(call standard_dirs)

$c_CXX = x86_64-managarm-g++

$c_INCLUDES := -I$($c_HEADERDIR)

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++14 -Wall -fPIC -O2

$c_LIBS :=

$(call make_so,libhelix.so,globals.o)
$(call install_header,hel.h)
$(call install_header,hel-syscalls.h)
$(call install_header,helx.hpp)
$(call install_header,helix/ipc.hpp)
$(call install_header,helix/await.hpp)
$(call compile_cxx,$($c_SRCDIR),$($c_OBJDIR))

