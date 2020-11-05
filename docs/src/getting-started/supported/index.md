# Supported Hardware and Software

## Software
Programs supported on managarm include [Weston](https://gitlab.freedesktop.org/wayland/weston/) (the Wayland reference compositor), [kmscon](https://www.freedesktop.org/wiki/Software/kmscon/) (a system console), GNU coreutils, bash, nano and others.

A list of packages is available on our [repo](https://ci.managarm.org/project/managarm/packages). A quick overview of various categories can be found at [package list](package-index.md).

## Hardware
### Real Hardware
- General: USB (UHCI, EHCI, XHCI)
- Graphics: Generic VBE graphics, Intel G45
- Input: USB human interface devices, PS/2 keyboard and mouse
- Storage: USB mass storage devices, AHCI, ATA

### Virtual Hardware
Includes all of the hardware listed above
- Graphics: virtio GPU, Bochs VBE interface, VMWare SVGA
- Storage: virtio block
- Networking: virtio net
