# thor and eir

> This part of the handbook is Work-In-Progress.

thor and eir are the kernel of managarm.

eir is the platform-dependant part of managarm's kernel which hands over control to thor after setup.

thor then proceeds to initialize the memory subsystem, scans and initializes pci devices, starts the ACPI subsystem and starts the base servers and supporting infrastructure needed to run Managarm's userland. This means that the following programs are started (in order).
- mbus, which is used to allow servers to find eachother.
- kerncfg, which exposes kernel configuration to servers.
- srvctl, controls the starting and stopping of servers.
- kernletcc, can compile and upload small programs (written in lewis) that run in kernel space.
- clocktracker, provides timekeeping services to servers.
- posix-subsystem, which runs posix-init, which is Managarm's init.

After starting the initial userspace, thor enters normal operations and handles kernel requests
The code for thor and eir can found at [https://github.com/managarm/managarm/tree/master/kernel](https://github.com/managarm/managarm/tree/master/kernel).
