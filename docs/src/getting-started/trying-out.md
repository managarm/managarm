# Trying out managarm

The Managarm project provides nightly builds of managarm if you want to try out
Managarm without setting up the development environment and building the whole
OS yourself.

## Nightly builds
The latest nightly build can be downloaded [here](https://ci.managarm.org/repos/files/managarm/rolling/image.xz).

Our build server, named `xbbs`, the xbstrap build server, which builds all of our packages automatically can be viewed at [https://ci.managarm.org](https://ci.managarm.org).

### Running the nightly builds
Before running the downloaded nightly build you have to **uncompress** it.

To run the nightly builds we recommend using [QEMU](https://www.qemu.org/) with the following options:

```bash
$ qemu-system-x86_64 -enable-kvm -m 1024 -device piix3-usb-uhci -device usb-kbd -device usb-tablet -drive id=hdd,file=image,format=raw,if=none -device virtio-blk-pci,drive=hdd -vga virtio -debugcon stdio
```

Be aware that the `-enable-kvm` flags works only on systems that support KVM.
