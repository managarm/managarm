# Managarm Handbook

<center>
<img src="img/logo.png" width="25%" height="25%">
</center>

Managarm is a pragmatic microkernel-based operating system with fully asynchronous I/O.

This documentation contains conceptual and technical details for various
parts of the system and different guides.

The Official Discord server can be found here: [https://discord.gg/7WB6Ur3](https://discord.gg/7WB6Ur3).

## What is this about? - from the managarm README.

This is the main repository of managarm, a microkernel-based operating system.

What is special about managarm? Some notable properties of managarm are: (i) managarm is based on a microkernel while common Desktop operating systems like Linux and Windows use monolithic kernels, (ii) managarm uses a completely asynchronous API for I/O and (iii) despite those internal differences, managarm provides good compatibility with Linux at the user space level.

Aren't microkernels slow? Microkernels do have some performance disadvantages over monolithic kernels. managarm tries to mitigate some of those issues by providing good abstractions (at the driver and system call levels) that allow efficient implementations of common user space functionality (like POSIX).

Is this a Linux distribution? No, managarm runs its own kernel that does not originate from Linux. While the managarm user space API supports many Linux APIs (e.g. epoll, timerfd, signalfd or tmpfs), managarm does not share any source code (or binaries) with the Linux kernel.

