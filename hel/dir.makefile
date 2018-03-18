
$(call standard_dirs)

$c_CXX = x86_64-managarm-g++

$c_INCLUDES := -I$($c_HEADERDIR)

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++17 -Wall -fPIC -O2

$c_LIBS :=

$(call make_so,libhelix.so,globals.o)
$(call install_header,hel.h)
$(call install_header,hel-stubs.h)
$(call install_header,hel-syscalls.h)
$(call install_header,helix/ipc.hpp)
$(call install_header,helix/await.hpp)
$(call compile_cxx,$($c_SRCDIR),$($c_OBJDIR))

$(BUILD_PATH)/$c/Doxyfile: $(TREE_PATH)/$c/Doxyfile.in
	sed 's|@ROOTDIR@|$(TREE_PATH)/$d|' $< > $@

.PHONY: $c-doc
$(call decl_targets,$c-doc)
$c-doc: $(BUILD_PATH)/$c/Doxyfile
	mkdir -p $(BUILD_PATH)/$d/doc
	doxygen $(BUILD_PATH)/$d/Doxyfile

