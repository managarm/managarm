
$(call standard_dirs)

$c_CXX = x86_64-managarm-g++

$c_INCLUDES := -I$($c_HEADERDIR)

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++14 -Wall -Wextra -fpic

$(call make_so,libcompose.so,compose.o)
$(call install_header,libcompose.hpp)
$(call compile_cxx,$($c_SRCDIR),$($c_OBJDIR))

