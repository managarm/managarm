
.DEFAULT_GOAL = all

include $(TREE_PATH)/rules.makefile
$(call include_dir,drivers/libcompose)
$(call include_dir,drivers/libterminal)
$(call include_dir,drivers/vga_terminal)

$(call include_dir,hel)

.PHONY: all clean gen

