# posix subsystem

> This part of the Handbook is Work-In-Progress.

The posix subsystem is the core of Managarm's userspace. It is started by [thor](../thoreir/index.md) and handles all posix requests made by userspace programs, like file I/O, memory allocation and sockets. It also implements various Linux API's like `epollfd`, `signalfd`, `timerfd` and `inotify`. For file I/O on block devices, it communicates with [libblockfs](../drivers/libblockfs/index.md), which is responsible for the actual file I/O on ext2 file systems.

On startup, the subsystem runs `posix-init`, which is a two stage init responsible for bringing up the userland. Thus, `posix-init` does the following operations:

- Starting of several servers for storage, this includes the USB host controller drivers (`ehci`, `uhci` and `xhci`) and the block devices (`virtio-block` and `ata`).
- Mounting of the root (`/`) file system and the various pseudo file systems (`procfs`, `sysfs`, `devtmpfs`, `tmpfs` and `devpts`) and entering it via `chroot`.
- Executing stage 2, which brings up the rest of the userspace

In stage 2, the following operations take place

- Starting of `udevd`, which is responsible for populating `/dev` and starting of other servers via udev rules.
- Starting of drivers that are not yet integrated in udev rules.
- Ensuring that the `Direct Rendering Manager (drm)`, keyboard and mouse are available.
- Parsing the kernel command line for hints on what userspace program to run.
- Populating the environment with essential variables.
- Either launching `weston` with `XWayland` support, `sway` or `kmscon`, as requested.

Afterwards, init goes to sleep and the userspace is operational.

The code for the posix subsystem can be found at [https://github.com/managarm/managarm/tree/master/posix/subsystem](https://github.com/managarm/managarm/tree/master/posix/subsystem), while the code for `posix-init` can be found at [https://github.com/managarm/managarm/tree/master/posix/init](https://github.com/managarm/managarm/tree/master/posix/init).
