# libblockfs

> This part of the Handbook is Work-In-Progress.

libblockfs is a support library in Managarm responsible for handling file I/O on block devices like hard drives and USB. It is a general interface to the underlying file systems that are available in Managarm. Userspace programs do not call libblockfs directly, but request functionality through the [posix subsystem](../../posix/index.md).

The code for libblockfs can be found at [https://github.com/managarm/managarm/tree/master/drivers/libblockfs](https://github.com/managarm/managarm/tree/master/drivers/libblockfs).