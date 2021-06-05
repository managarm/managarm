# Building Managarm

This guide shows how to build a managarm distribution from source utilising the [bootstrap-managarm](https://github.com/managarm/bootstrap-managarm) patches and build scripts.

## Building a managarm distribution from source

### Build environment
To make sure that all build environments work properly, it is recommended to
setup a build environment with our lightweight containerized build runtime [cbuildrt](https://github.com/managarm/cbuildrt) (see below for instructions).
It is also possible to build with [Docker](https://www.docker.com/) 
(see [here](with-docker.md)), or by installing the dependencies manually (see [here](with-manual.md)), but these methods are no longer recommended.

Make sure that you have at least 20 - 30 GiB of free disk space.

#### Preparations

1.  Create a directory which will be used for storing the
source and build directories. Here we use `~/managarm`, but it can be any directory.
    ```sh
    mkdir ~/managarm && cd ~/managarm
    ```
1.  Install the `xbstrap` build system via `pip3`:
    ```bash
    pip3 install xbstrap
    ```
1.  The `git`, `subversion` and `mercurial` tools are required on the host (whether you build in a container or not). Install these via your package manager.
1.  Clone this repository into a `src` directory and create a `build` directory:
    ```bash
    git clone https://github.com/managarm/bootstrap-managarm.git src
    mkdir build
    ```

### Creating a `cbuildrt` environment

1.  Download and install the latest `cbuildrt` release by running:
    ```bash
    xbstrap prereqs cbuildrt xbps
    ```
    > Note: If you choose to build `cbuildrt` from source, make sure to place the resulting binary either in `$PATH` or in `~/.xbstrap/bin`.

1.  Download and unpack the latest Managarm `rootfs` somewhere:
    ```bash
    curl https://mirrors.managarm.org/cbuildrt/rootfs/managarm-buildenv.tar.gz -o managarm-rootfs.tar.gz
    tar xvf managarm-rootfs.tar.gz
    ```
1.  **Inside the `build` directory**, create a file named `bootstrap-site.yml` with the following contents (replace the `container.rootfs` key as appropriate):
    ```yml
    pkg_management:
      format: xbps

    container:
      runtime: cbuildrt
      rootfs:  /path/to/your/rootfs
      uid: 1000
      gid: 1000
      src_mount: /var/lib/managarm-buildenv/src
      build_mount: /var/lib/managarm-buildenv/build
      allow_containerless: true
    ```
    > Note: you must keep the `src_mount` and `build_mount` values as shown above if you want to be able to use pre-built tools from our build server. These paths refer to locations on the *container*, not on your host machine. Also note that these paths cannot be changed after starting the build; doing so will likely result in a broken directory tree.


### Building
1.  Initialize the build directory with
    ```bash
    cd build
    xbstrap init ../src
    ```
1.  There are several meta-packages available which control what software is built, this means that the build can be started using one of the following commands:

    If you want the full managarm experience with a selection of terminal and gui software available to try out use
    ```bash
    xbstrap install weston-desktop
    ```
    If you want to boot into `kmscon` and have some functional commands to play around with use
    ```bash
    xbstrap install base
    ```
    If you want some development tools in addition to the functionality of `base` use
    ```bash
    xbstrap install base-devel
    ```
    If you only want to build the bare minimum to boot into `kmscon` use
    ```bash
    xbstrap install --all
    ```
    Note that this command can take multiple hours, depending on your machine.


### Creating Images
> Note: if using a Docker container the following command are meant to be ran **outside** the Docker container, in the `build` directory on the host. Adding to the aforementioned commands, one would `exit` from the container once the build finishes, enter the `build` directory, and proceed as follows.

After managarm's packages have been built, building a HDD image of the system
is straightforward. The [image_create.sh](https://github.com/qookei/image_create) script
can be used to create an empty HDD image.

Download the `image_create.sh` script and mark it executable:
```bash
wget 'https://raw.githubusercontent.com/qookei/image_create/master/image_create.sh'
chmod +x image_create.sh
```

This repository contains an `update-image.py` script to mount the image and copy the system onto it.

This script can use 3 different methods to copy files onto the image:
1. **Using libguestfs** (the default method). We recommend this when root access is not possible or desirable. However, it is much slower than method 2 and requires some setup on the host (see below).
1. **Using a classic loopback and mount** (requires root privileges). We recommend this when root access is acceptable because it is the fastest method and most guaranteed to work.
1. **Via Docker container** (only works with the Docker build method). This is handy in case the user is in the `docker` group since it does not require additional root authentication. We discourage this because it uses `docker run --privileged` (which is not safer than giving root access) and [currently has some bugs](https://github.com/managarm/bootstrap-managarm/issues/103).

Going with method 1 will require `libguestfs` to be installed on the host.
After installing `libguestfs` it might be necessary to run the following:
```bash
sudo install -d /usr/lib/guestfs
sudo update-libguestfs-appliance
```

If you're using xbstrap to invoke the script, selecting the desired method can be done by adding the following to the `bootstrap-site.yml` file:
```
define_options:
    mount-using: 'loopback' # or guestfs/docker
```

When invoking the script directly, selecting the method can be done with the `--mount-using` argument.

For all methods, the `image_create.sh` and `make-image` invocations shown below require the following programs to be installed:
`rsync`, `losetup`, `sfdisk`, `mkfs.ext2`, `mkfs.vfat`. Refer to [image_create](https://github.com/qookei/image_create#requirements) for more details on this.

Hence, running the following commands in the build directory
should produce a working image and launch it using QEMU:
```bash
# Create a HDD image file called 'image'.
# Check "./image_create.sh -h" for more options.
./image_create.sh -o image -s 4GiB -t ext2 -p gpt -l limine -be

# Copy the system onto it.
xbstrap run make-image

# Launch the image using QEMU.
xbstrap run qemu

# Or as a one-liner:
xbstrap run make-image qemu

# Alternatively, you can call the necessary scripts manually:
# Check their help messages for more information.
../src/managarm/tools/gen-initrd.py
../src/scripts/update-image.py
../src/scripts/vm-util.py qemu
```
