
.DEFAULT_GOAL = all

include $(TREE_PATH)/rules.makefile
$(call include_dir,frigg)
$(call include_dir,libarch)
$(call include_dir,eir)
$(call include_dir,thor/kernel)
$(call include_dir,mbus)
$(call include_dir,posix/subsystem)
$(call include_dir,posix/init)
$(call include_dir,ld-init/linker)
#$(call include_dir,libnet)
$(call include_dir,drivers/gfx/intel)
$(call include_dir,drivers/libblockfs)
$(call include_dir,drivers/libcompose)
$(call include_dir,drivers/libterminal)
$(call include_dir,drivers/usb/devices/hid)
$(call include_dir,drivers/usb/devices/storage)
$(call include_dir,drivers/usb/hcds/ehci)
$(call include_dir,drivers/usb/hcds/uhci)
$(call include_dir,drivers/vga_terminal)
$(call include_dir,drivers/virtio)
#$(call include_dir,drivers/kbd)
#$(call include_dir,drivers/bochs_vga)
#$(call include_dir,drivers/ata)
$(call include_dir,protocols/fs)
$(call include_dir,protocols/hw)
$(call include_dir,protocols/mbus)
$(call include_dir,drivers/uart)
$(call include_dir,protocols/usb)

$(call include_dir,tools/frigg_pb)
$(call include_dir,hel)

.PHONY: all clean gen

