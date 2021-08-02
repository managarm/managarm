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
    curl https://repos.managarm.org/buildenv/managarm-buildenv.tar.gz -o managarm-rootfs.tar.gz
    tar xvf managarm-rootfs.tar.gz
    ```
1.  **Inside the `build` directory**, create a file named `bootstrap-site.yml` with the following contents:
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
1.  In the `build/bootstrap-site.yml` file you just created, replace the `container.rootfs` key with the path to your `rootfs`.


### Building
1.  Initialize the build directory with
	```bash
	cd build
	xbstrap init ../src
	```

1.  Decide which software you want to include in the image you create. There are several meta-packages available which help you decide this:
    * `base`: just enough to boot into `kmscon` plus some functional commands to play with (such as `less` and `grep`)
    * `base-devel`: same as `base` plus some extra development tools (such as `gcc` and `binutils`)
    * `weston-desktop`: full managarm experience with a selection of terminal and GUI software

1.  To actually execute the build, we recommend that you install the necessary tools and packages as binaries from our build server.
	This can save you multiple hours of compilation, depending on your machine.

	> Note: **this only works if you build in a container** (though this is untested with Docker). Containerless builds must do a full build from source (see below).

	```bash
	xbstrap pull-pack --deps-of <meta-package> mlibc mlibc-headers # e.g xbstrap pull-pack --deps-of base mlibc mlibc-headers
	xbstrap install --deps-of <meta-package>

	# The following two commands fetch managarm and mlibc (plus their required tools) to enable local development:
	xbstrap download-tool-archive --build-deps-of managarm-system --build-deps-of managarm-kernel --build-deps-of mlibc
	xbstrap install --rebuild managarm-system managarm-kernel mlibc mlibc-headers
	```

	If instead you want to build everything from source, simply run `xbstrap install <group>`, e.g `xbstrap install weston-desktop`.
	Note that this can take multiple hours, depending on your machine.

### Creating Images
After managarm's packages have been built, building a HDD image of the system is straightforward. 

For all methods, the image creation and updation commands shown below require the following programs to be installed on the host:
`rsync`, `losetup`, `sfdisk`, `mkfs.ext2`, `mkfs.vfat`. Refer to [image_create](https://github.com/qookei/image_create#requirements) for more details on this.

1.  Decide what method you want to use to copy files to the image:
	1. **Using libguestfs** (the default method). We recommend this when root access is not possible or desirable. However, it is much slower than method 2 and requires some setup on the host (see below).
	1. **Using a classic loopback and mount** (requires root privileges). We recommend this when root access is acceptable because it is the fastest method and most guaranteed to work.
	1. **Via Docker container** (only works with the Docker build method). This is handy in case the user is in the `docker` group since it does not require additional root authentication. We discourage this because it uses `docker run --privileged` (which is not safer than giving root access) and [currently has some bugs](https://github.com/managarm/bootstrap-managarm/issues/103).

	Going with method 1 will require `libguestfs` to be installed on the host.
	After installing `libguestfs`, if you encounter errors while making the image it might be necessary to run the following:
	```bash
	sudo install -d /usr/lib/guestfs
	sudo update-libguestfs-appliance
	```
1.  Add the following to `build/bootstrap-site.yml`, depending on what mount method you have chosen:
	```yaml
	define_options:
	  mount-using: 'loopback' # or guestfs/docker
	```
1.  Create the image:
	```bash
	xbstrap run initialize-empty-image
	```
1.  Copy the system onto it:
	```bash
	xbstrap run make-image
	```
1.  Launch the image using QEMU:
	```bash
	xbstrap run qemu # Note that you can combine the last two operations: xbstrap run make-image qemu
	```

Alternatively, you can call the necessary scripts manually (check their help messages for more information):
* `../src/managarm/tools/gen-initrd.py` and `../src/scripts/update-image.py` for copying the files onto the image (the mount method can be selected with the `--mount-using` argument)
* `../src/scripts/vm-util.py qemu` for launching QEMU
