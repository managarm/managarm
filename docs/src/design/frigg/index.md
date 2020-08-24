# frigg

> This part of the handbook is Work-In-Progress.

frigg is a lightweight utility library for system programming written in C++.
frigg has implementations for many frequently used constructs in C++, like vectors, arrays and lists. It also implements memory allocators, utilities for error handling (`frg::expected`), and logging utilities. In contrast to the C++ standard library, it is entirely freestanding and does not depend on functionality provided by the host. Hence, frigg is used extensively in the Managarm kernel and in mlibc.

The code for frigg can found at [https://github.com/managarm/frigg](https://github.com/managarm/frigg).
