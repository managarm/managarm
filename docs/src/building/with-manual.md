# Building With Manual Installation

This section explains how to build Managarm with manual installation of the build dependencies.

> Note: we recommend using `cbuildrt` as it provides a clean build environment
separate from your host machine, so issues are less frequent and easier to reproduce. It also allows you
to use pre-built tools from our build server rather than compiling everything from source.

1.  Certain programs are required to build managarm; To get a list of the corresponding Debian packages we refer you to the [Dockerfile](https://github.com/managarm/bootstrap-managarm/blob/master/docker/Dockerfile)
1.  `meson` is required. There is a Debian package, but as of Debian Stretch, a newer version is required.
    Install it from pip: `pip3 install meson`.
1.  `protobuf` is also required. There is a Debian package, but a newer version is required.
    Install it from pip: `pip3 install protobuf`
1.  For managarm kernel documentation you may also want `mdbook`.
    You can install it from your distribution repositories, or using cargo: `cargo install --git https://github.com/rust-lang/mdBook.git mdbook`.

Now proceed to the [Building](index.md#building) paragraph.
