# managarm's port of FreeBSD's e1000 driver

## Structure

`nic/freebsd-e1000/import/` contains the files taken from FreeBSD, separated into source and header files (in `src` and `include` specifically).

`nic/freebsd-e1000/src` contains the managarm parts of the code. Of particular interest are `src/osdep.cpp` and `include/e1000_osdep.h`, which include the OS-specific components that are used by FreeBSD's e1000 driver internally.

## Updating the imported driver

Copy the `.c` files from FreeBSD's `sys/dev/e1000` directory into our `nic/freebsd-e1000/import/src`.
Copy the `.h` files from FreeBSD's `sys/dev/e1000` directory into our `nic/freebsd-e1000/import/include`.
Adapt `nic/freebsd-e1000/import/meson.build` to include any new or removed `.c` files.
