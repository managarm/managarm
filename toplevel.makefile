
.DEFAULT_GOAL = all

include $(TREE_PATH)/rules.makefile
$(call include_dir,frigg)
$(call include_dir,eir)
$(call include_dir,thor/kernel)
$(call include_dir,thor/user_boot)
$(call include_dir,thor/acpi)
$(call include_dir,thor/initrd)
$(call include_dir,mbus)
$(call include_dir,posix/subsystem)
$(call include_dir,posix/init)
$(call include_dir,ld-init/linker)
$(call include_dir,bragi)
$(call include_dir,libcompose)
$(call include_dir,drivers/vga_terminal)
$(call include_dir,drivers/kbd)
$(call include_dir,drivers/bochs_vga)
$(call include_dir,drivers/ata)
$(call include_dir,zisa)

$(call include_dir,tools/frigg_pb)
$(call include_dir,hel)

.PHONY: all clean gen

all: $(addprefix all-,$(DIRECTORIES))

clean: $(addprefix clean-,$(DIRECTORIES))

