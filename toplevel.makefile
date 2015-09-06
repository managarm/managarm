
.DEFAULT_GOAL = all

include $(TREE_PATH)/rules.makefile
$(call include_dir,eir)
$(call include_dir,thor/kernel)
$(call include_dir,thor/user_boot)
$(call include_dir,thor/acpi)
$(call include_dir,ld-init/server)
$(call include_dir,ld-init/linker)
$(call include_dir,drivers/vga_terminal)
$(call include_dir,drivers/initrd_fs)
$(call include_dir,zisa)

$(call include_dir,hel)

.PHONY: all

all: $(addprefix all-,$(DIRECTORIES))

clean: $(addprefix clean-,$(DIRECTORIES))

