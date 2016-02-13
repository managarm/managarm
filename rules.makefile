
c :=

define include_dir
$(eval 
  c := $1

  .PHONY: all-$1 gen-$1 clean-$1 install-$1
  $$(call decl_targets,all-$1 gen-$1 clean-$1 install-$1)

  include $(TREE_PATH)/$1/dir.makefile

  ifeq ($c,)
    all: all-$1
    gen: gen-$1
    clean: clean-$1
    install: install-$1
  else
    all-$c: all-$1
    gen-$c: gen-$1
    clean-$c: clean-$1
    install-$c: install-$1
  endif

  c := $c
)
endef

define decl_targets
$(eval
  $1: d := $$c
)
endef

define standard_dirs
$(eval
  $$c_SRCDIR := $(TREE_PATH)/$$c/src
  $$c_HEADERDIR := $(TREE_PATH)/$$c/include
  $$(call define_gendir,GEN,$(BUILD_PATH)/$$c/gen)
  $$(call define_objdir,OBJ,$(BUILD_PATH)/$$c/obj)
  $$(call define_bindir,BIN,$(BUILD_PATH)/$$c/bin)
)
endef

# automates directory handling
# * defines a variable for the directory
# * calls decl_targets
# * defines a rule to mkdir the directory
# * includes all *.d files in the directory
define define_gendir
$(eval
  $$c_$1DIR := $2

  $(call decl_targets,$2 $2/%)

  $2:
	mkdir -p $$@

  -include $2/*.d
)
endef

# see define_gendir
# automates object directory handling
# * makes sure the directory is cleaned on 'make clean'
define define_objdir
$(eval
  $$c_$1DIR := $2

  $(call decl_targets,$2 $2/%)

  $2:
	mkdir -p $$@

  clean-$$c: clean-$$c-$1DIR

  .PHONY: clean-$$c-$1DIR
  clean-$$c-$1DIR:
	rm -rf $2

  -include $2/*.d
)
endef

# see define_gendir
# * does not include *.d files
# * makes sure the directory is cleaned on 'make clean'
define define_bindir
$(eval
  $$c_$1DIR := $2

  $(call decl_targets,$2 $2/%)

  $2:
	mkdir -p $$@

  clean-$$c: clean-$$c-$1DIR

  .PHONY: clean-$$c-$1DIR
  clean-$$c-$1DIR:
	rm -rf $2
)
endef

# adds rules to compile and install an executable
define make_exec
$(eval
  all-$$c: $$($$c_BINDIR)/$1

  install-$c: install-$$c-$1

  .PHONY: install-$$c-$1
  install-$$c-$1: $$($$c_BINDIR)/$1
	install $$($$d_BINDIR)/$1 $(SYSROOT_PATH)/usr/bin

  $$($$c_BINDIR)/$1: $$(addprefix $$($$c_OBJDIR)/,$2) | $$($$c_BINDIR)
	$$($$d_CXX) -o $$@ $($$d_LDFLAGS) $$(addprefix $$($$d_OBJDIR)/,$2) $$($$d_$3LIBS)
)
endef

# adds rules to compile and install a shared library
define make_so
$(eval
  all-$$c: $$($$c_BINDIR)/$1

  install-$c: install-$$c-$1

  .PHONY: install-$$c-$1
  install-$$c-$1: $$($$c_BINDIR)/$1
	install $$($$d_BINDIR)/$1 $(SYSROOT_PATH)/usr/lib

  $$($$c_BINDIR)/$1: $$(addprefix $$($$c_OBJDIR)/,$2) | $$($$c_BINDIR)
	$$($$d_CXX) -shared -o $$@ $($$d_LDFLAGS) $$(addprefix $$($$d_OBJDIR)/,$2) $$($$d_LIBS)
)
endef

define install_header
$(eval
  install-$c: install-$$c-$1

  .PHONY: install-$$c-$1
  install-$$c-$1:
	install $$($$d_HEADERDIR)/$1 $(SYSROOT_PATH)/usr/include
)
endef

define gen_protobuf_cpp
$(eval
  $2/%.pb.tag: $1/%.proto | $2
	$(PROTOC) --cpp_out=$2 --proto_path=$1 $$<
	touch $$@
)
endef

define compile_cxx
$(eval
  $2/%.o: $1/%.cpp | $2
	$$($$d_CXX) -c -o $$@ $$($$d_CXXFLAGS) $$<
	$$($$d_CXX) $$($$d_CXXFLAGS) -MM -MP -MF $$(@:%.o=%.d) -MT "$$@" -MT "$$(@:%.o=%.d)" $$<

  $2/%.o: $1/%.cc | $2
	$$($$d_CXX) -c -o $$@ $$($$d_CXXFLAGS) $$<
	$$($$d_CXX) $$($$d_CXXFLAGS) -MM -MP -MF $$(@:%.o=%.d) -MT "$$@" -MT "$$(@:%.o=%.d)" $$<
)
endef

