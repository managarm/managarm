# `xbstrap`-Workflow

After modifying the source code, `xbstrap` is usually used to recompile Managarm's packages
and to run the resulting image in a virtual machine (such as `qemu`).
This section gives a quick overview of useful `xbstrap` subcommands
that are utilized to develop Managarm.

## Rebuilding packages

Changes to Managarm almost always involve the packages `managarm-kernel`,
`managarm-system` (which contains drivers and servers),
`mlibc` (our C library), or `mlibc-headers` (which must be rebuilt after
changing public mlibc headers).
`xbstrap` can be used to rebuild these packages
using
```sh
xbstrap install --rebuild managarm-system
```
(and similarly for packages other than `managarm-system`).
After rebuilding a package, the `make-image` step needs to be invoked again
to update the disk image
(see the [section on building Managarm](../building/index.md]) of this handbook):
```sh
xbstrap run make-image
# Or `xbstrap run make-image qemu` to also run the image in qemu.
```

Sometimes, it can become necessary to re-run the `configure` scripts
of ports (or Meson/CMake). This is usually the case if build scripts
and/or configuration files have changed in ways that the build
system does not anticipate. Invoking the `configure` step again
is done via:
```sh
xbstrap install --reconfigure managarm-system
```
