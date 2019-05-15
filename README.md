# The managarm Operating System

![Screenshot](../assets/screenshots/echo-hello-managarm.png?raw=true)

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

## Supported Software

Currently, Weston (the Wayland reference compositor) and GNU Bash run on managarm.

## Supported Hardware
**General** USB (UHCI, EHCI)\
**Graphics** Generic VBE graphics, Intel G45, virtio GPU, Bochs VBE interface, VMWare SVGA\
**Input** USB human interface devices, PS/2 keyboard and mouse\
**Storage** USB mass storage devices, ATA, virtio block

## Building managarm

While this repository contains managarm's kernel, its drivers and other core functionality,
it is not enough to build a full managarm distribution. Instead, we refer to the
[bootstrap-managarm](https://github.com/managarm/bootstrap-managarm) repository for build instructions.
