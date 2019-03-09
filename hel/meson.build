
helix = shared_library('helix', 'src/globals.cpp',
	cpp_args: ['-std=c++17', '-Wall'],
	install: true)

install_headers(
    'include/hel.h',
    'include/hel-stubs.h',
    'include/hel-syscalls.h')

install_headers(
	'include/helix/await.hpp',
	'include/helix/ipc.hpp',
	'include/helix/memory.hpp')

lib_helix_dep = declare_dependency(
	include_directories: include_directories('include/'),
	link_with: helix)

# TODO: Adapt the documentation build from the old makefile:
#
# $(BUILD_PATH)/$c/Doxyfile: $(TREE_PATH)/$c/Doxyfile.in
# 	sed 's|@ROOTDIR@|$(TREE_PATH)/$d|' $< > $@
# 
# .PHONY: $c-doc
# $(call decl_targets,$c-doc)
# $c-doc: $(BUILD_PATH)/$c/Doxyfile
# 	mkdir -p $(BUILD_PATH)/$d/doc
# 	doxygen $(BUILD_PATH)/$d/Doxyfile

