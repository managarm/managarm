
.DEFAULT_GOAL = all

include $(TREE_PATH)/rules.makefile
$(call include_dir,eir)
$(call include_dir,thor)
$(call include_dir,ld-init/server)
$(call include_dir,ld-init/linker)
$(call include_dir,zisa)

.PHONY: all

all: $(addprefix all-,$(DIRECTORIES))

clean: $(addprefix clean-,$(DIRECTORIES))

