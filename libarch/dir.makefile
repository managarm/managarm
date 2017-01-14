
$(call standard_dirs)

$c_CXX = x86_64-managarm-g++

$c_INCLUDES := -I$($c_HEADERDIR)

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++14 -Wall -Wextra -O2 -fPIC

$(call make_so,libarch.so,dma_pool.o)
$(call install_header,arch/bits.hpp)
$(call install_header,arch/dma_pool.hpp)
$(call install_header,arch/dma_structs.hpp)
$(call install_header,arch/io_space.hpp)
$(call install_header,arch/mem_space.hpp)
$(call install_header,arch/os-managarm/dma_pool.hpp)
$(call install_header,arch/register.hpp)
$(call install_header,arch/variable.hpp)
$(call install_header,arch/x86/mem_space.hpp)
$(call install_header,arch/x86/io_space.hpp)
$(call compile_cxx,$($c_SRCDIR),$($c_OBJDIR))

