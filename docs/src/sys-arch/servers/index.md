# servers

> This part of the Handbook is Work-In-Progress.

Servers are how most drivers running on a managarm system should be implemented. Exceptions to that are drivers that are required for kernel functionality, for instance interrupt controllers, which are part of the kernel. e9 debug logging is also implemented in-kernel. Notably, ACPI handling is also done in-kernel - having this run in a server has previously proved overly complex, and some functionality is needed by the kernel to set up timers and interrupts anyways.

As (filesystem) servers are used to access file systems, we cannot rely on loading servers from a disk - they are loaded from the initrd for bootstrapping. This is necessary for at least all servers that can be used for file access, and their dependencies.

Other servers can be loaded from regular file systems, but they will run in the same initrd environment, which notably does not execute under the control of posix-subsystem. As these servers may rely on shared objects that are not part of the initrd, they are uploaded by `runsvr` as needed. The YAML recipes used by `bakesvr` provide the file list.

## Launching

Servers needed for bootstrapping are launched from stage1 (`posix/init/src/stage1.cpp`). After this, posix-subsystem sets up the environment by mounting the rootfs, setting up mounted filesystems like procfs, sysfs, and others. Once udev is launched, our udev rules are evaluated. These use `runsvr` to bind devices to servers (via the `svrctl` protocol) or launch servers. USB udev rules are located under `drivers/usb/90-managarm-usb.rules`, and similar rules exist for NICs and graphics.
