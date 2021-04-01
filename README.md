# The managarm Operating System

![CI](https://github.com/managarm/managarm/workflows/CI/badge.svg) ![Documentation](https://github.com/managarm/managarm/workflows/Documentation/badge.svg)

![Screenshot](../assets/screenshots/managarm-glxgears-xclock.png?raw=true)

## What is this about?

This is the main repository of managarm, a microkernel-based operating system.

**What is special about managarm?** Some notable properties of managarm are:
(i) managarm is based on a microkernel while common Desktop operating systems like Linux and Windows use monolithic kernels,
(ii) managarm uses a completely asynchronous API for I/O
and (iii) despite those internal differences, managarm provides good compatibility with Linux at the user space level.

**Aren't microkernels slow?** Microkernels do have some performance disadvantages over monolithic kernels.
managarm tries to mitigate some of those issues by providing good abstractions (at the driver and system call levels)
that allow efficient implementations of common user space functionality (like POSIX).

**Is this a Linux distribution?** No, managarm runs its own kernel that does not originate from Linux.
While the managarm user space API supports many Linux APIs (e.g. epoll, timerfd, signalfd or tmpfs),
managarm does not share any source code (or binaries) with the Linux kernel.

**Official Discord server:** https://discord.gg/7WB6Ur3

## Features

* 64-bit operating system with SMP (i.e., multicore) and ACPI support.
* Fully asynchronous microkernel and servers.
* Support for many modern hardware devices such as USB 3 controllers.
* Networking support.
* POSIX and Linux API compatibility.
* Support for Linux-style special files (epoll, signalfd, ...) and pseudo file systems (`/sys`, `/proc`, ...).

## Trying out managarm

If you want to try out managarm without building the whole OS, you can download a
xz-compressed [nightly image](https://ci.managarm.org/repos/files/managarm/rolling/image.xz);
these images are automatically compiled on [our build server](https://ci.managarm.org)
and reflect the current state of Managarm development.
To run the (uncompressed) image using qemu, we recommend the following flags:

`qemu-system-x86_64 -enable-kvm -m 1024 -device piix3-usb-uhci -device usb-kbd -device usb-tablet -drive id=hdd,file=image,format=raw,if=none -device virtio-blk-pci,drive=hdd -vga virtio -debugcon stdio`

## Supported Software

Programs supported on managarm include [Weston](https://gitlab.freedesktop.org/wayland/weston/) (the Wayland reference compositor), [kmscon](https://www.freedesktop.org/wiki/Software/kmscon/) (a system console), GNU Coreutils, Bash, nano and others.

## Supported Hardware
**General** USB (UHCI, EHCI)\
**Graphics** Generic VBE graphics, Intel G45, virtio GPU, Bochs VBE interface, VMWare SVGA\
**Input** USB human interface devices, PS/2 keyboard and mouse\
**Storage** USB mass storage devices, ATA, virtio block

## Building managarm

While this repository contains managarm's kernel, its drivers and other core functionality,
it is not enough to build a full managarm distribution. Instead, we refer to the
[bootstrap-managarm](https://github.com/managarm/bootstrap-managarm) repository for build instructions.
