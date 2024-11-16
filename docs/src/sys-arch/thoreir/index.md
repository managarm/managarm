# thor and eir

> This part of the handbook is Work-In-Progress.

thor and eir are the kernel of Managarm.

## eir

eir is the platform-dependant part of Managarm's kernel which hands over control to thor after setup. This pre-kernel takes over from the boot manager/loader and manages the boot protocol specific aspects in order to allow for the thor handoff to be generic. Various setup functions are performed, like getting a memory map, setting up a framebuffer and loading the initrd. `thor` is loaded from the initrd, as some boot protocols do not support passing more than a single module. After some architecture specific setup, page table setup and similar steps, the handoff info structure `EirInfo` is filled with information. After this, eir jumps to thor.

## thor

thor then proceeds to initialize the memory subsystem, scans and initializes PCI devices, starts the ACPI subsystem and starts the base servers and supporting infrastructure needed to run Managarm's userland. This means that the following programs are started (in order).
- mbus, which is used to allow servers to find each other.
- kerncfg, which exposes kernel configuration to servers.
- srvctl, controls the starting and stopping of servers.
- kernletcc, can compile and upload small programs (written in lewis) that run in kernel space.
- clocktracker, provides timekeeping services to servers.
- posix-subsystem, which runs posix-init, which is Managarm's init, and provides the environment that is expected by POSIX programs.

After starting the initial userspace, thor enters normal operations and handles system calls.
The code for thor and eir can found at [https://github.com/managarm/managarm/tree/master/kernel](https://github.com/managarm/managarm/tree/master/kernel).
