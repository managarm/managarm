# posix subsystem

> This part of the Handbook is Work-In-Progress.

The posix subsystem is the core of Managarm's userspace. It is started by [thor](../thoreir/index.md) and handles all posix requests made by userspace programs, like file I/O, memory allocation and sockets. It also implements various Linux API's like `epollfd`, `signalfd`, `timerfd` and `inotify`. It has a lot of communication with [libblockfs](../libblockfs/index.md), which is responsible for the actual file I/O on ext2 file systems.

On startup, the subsystem runs `posix-init`, which is a two stage init responsible for bringing up the userland. Thus, `posix-init` does the following operations:
- Starting of several servers for storage, this includes the USB servers (`ehci`, `uhci` and `xhci`) and the block devices (`virtio-block` and `ata`).
- Mounting of the root (`/`) file system and the various pseudo file systems (`procfs`, `sysfs`, `devtmpfs`, `tmpfs` and `devpts`) and changing to it.
- Executing stage 2, which brings up the rest of the userspace

In stage 2, the following operations take place
- Starting of `udevd`, which is responsible for populating `/dev`
- Starting of drivers that are not yet integrated in udev rules.
- Ensuring that the `drm`, keyboard and mouse are available.
- Parsing the kernel command line for hints on what userspace program to run.
- Populating the environment with essential variables.
- Either launching `weston` with `XWayland` support or `kmscon`, as requested.

Afterwards, init goes to sleep and the userspace is operational.

The code for the posix subsystem can be found at [https://github.com/managarm/managarm/tree/master/posix/subsystem](https://github.com/managarm/managarm/tree/master/posix/subsystem), while the code for `posix-init` can be found at [https://github.com/managarm/managarm/tree/master/posix/init](https://github.com/managarm/managarm/tree/master/posix/init).