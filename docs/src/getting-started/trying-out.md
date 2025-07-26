# Trying out managarm

The Managarm project provides nightly builds of managarm if you want to try out
Managarm without setting up the development environment and building the whole
OS yourself.

## Nightly builds
The latest nightly build can be downloaded [here](https://builds.managarm.org/projects/managarm/success/repo/files/x86_64/image.xz).

Our build server, named `xbbs`, the xbstrap build server, which builds all of our packages automatically can be viewed at [https://builds.managarm.org](https://builds.managarm.org).

### Running the nightly builds
Before running the downloaded nightly build you have to **uncompress** it.

To run the nightly builds we recommend using [QEMU](https://www.qemu.org/) with the following options:

```bash
$ qemu-system-x86_64 -enable-kvm -m 2048 -cpu host,migratable=off -device qemu-xhci -device usb-kbd -device usb-tablet -drive id=hdd,file=image,format=raw,if=none -device virtio-blk-pci,drive=hdd -vga vmware -debugcon stdio
```

Be aware that the `-enable-kvm` flags works only on systems that support KVM.

If you boot into `kmscon` (the non graphical environment), or if you issue `login` from any shell, use the following default credentials to log in.

For the root user, username: `root`, password: `toor`.
For the Managarm user, username: `managarm`. password: `managarm`.
