
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

