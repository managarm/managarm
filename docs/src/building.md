# Building Managarm

This guide shows how to build a managarm distribution from source utilising the [bootstrap-managarm](https://github.com/managarm/bootstrap-managarm) patches and build scripts.

## Building a managarm distribution from source

### Build environment
To make sure that all build environments work properly, it is recommended to
setup a build environment with [Docker](https://www.docker.com/) using the
provided [Dockerfile](https://github.com/managarm/bootstrap-managarm/blob/master/docker/Dockerfile).

Make sure that you have atleast 20 - 30 GiB of free disk space.

#### Preperations
First and foremost we will create a directory (`$MANAGARM_DIR`) which will be
used for storing the source and build directories.

```sh
export MANAGARM_DIR="$HOME/managarm" # In this example we are using $HOME/managarm, but it can be any directory
mkdir -p "$MANAGARM_DIR" && cd "$MANAGARM_DIR"
```

Then clone this directoy into a `src` directory:
```sh
git clone https://github.com/managarm/bootstrap-managarm.git src
```

#### Creating Docker image and container
> Note: this step is not needed if you don't want to use a Docker container, if so skip to the next paragraph.
1. Install Docker (duh) and make sure it is working properly by running `docker run hello-world` and making sure the output shows:
```
Hello from Docker!
This message shows that your installation appears to be working correctly.
```
2. Creae a Docker image from the provided Dockerfile:
```sh
docker build -t managarm_buildenv src/docker
```
3. Start a container:
```sh
docker run -v $(realpath "$MANAGARM_DIR"):/home/managarm_buildenv/managarm -it managarm_buildenv
```

You are now running a `bash` shell within a Docker container with all the build
dependencies already installed. Inside the home directory (`ls`) there shiuld be
a `managarm` directory shared with the host containing a `src` directory (this repo).
If this is not the case go back and make sure you followed the steps properly.

Switch to the `managarm` directory:
```sh
cd managarm
```
Now proceed to the Building paragraph.

#### Installing dependecies manually
> Note: if you created a Docker image in the previous step, skip this paragraph.
1.  Certain programs are required to build managarm;
    here we list the corresponding Debian packages:
    `build-essential`, `pkg-config`, `autopoint`, `bison`, `curl`, `flex`, `gettext`, `git`, `gperf`, `help2man`, `m4`, `mercurial`, `ninja-build`, `python3-mako`, `python3-protobuf`, `python3-yaml`, `texinfo`, `unzip`, `wget`, `xsltproc`, `xz-utils`, `libexpat1-dev`, `rsync`, `python3-pip`, `python3-libxml2`, `netpbm`, `itstool`, `zlib1g-dev`, `libgmp-dev`, `libmpfr-dev`, `libmpc-dev`.
1.  `meson` is required. There is a Debian package, but as of Debian Stretch, a newer version is required.
    Install it from pip: `pip3 install meson`.
1.  The [xbstrap](https://github.com/managarm/xbstrap) tool is required to build managarm. Install it from pip: `pip3 install xbstrap`.

### Building
1. Create and change into a `build` directory
```sh
mkdir build && cd build
```
2. Initialize the build directory with
```sh
xbstrap init ../src
```
3. Start the build using
```sh
xbstrap install --all
```
Note that this command can take **multiple hours**, depending on your machine.

### Creating Images
> Note: if using a Docker container the following command are meant to be ran **outside** the Docker container, in the `build` directory on the host. Adding to the aforementioned commands, one would `exit` from the container once the build finishes, enter the `build` directory, and proceed as follows.

After managarm's packages have been built, building a HDD image of the system is straightforward. The [`image_create.sh`](https://gitlab.com/qookei/image_create) script can be used tp create an empty HDD imsage.

Download the `image_create.sh` script and mark it executable:
```sh
wget 'https://gitlab.com/qookei/image_create/raw/master/image_create.sh'
chmod +x image_create.sh
```

This repository contains a `mkimage` script to copy the system onto the image. Note that `mkimage` requires `libguestfs` and `rsync` to be able to create an image without requiring root access and synchronise it.

After installing `libguestfs` it might be necessary to run the following:
```sh
sudo install -d /usr/lib/guestfs
sudo update-libguestfs-appliance
```

Hence, running the following commands in the build directory should produce a working image and launch it using [QEMU](https://qemu.org/):
```sh
# Copy some files (e.g., the timezone configuration) from the host to system-root/.
# It is sufficient to run this once.
../src/scripts/prepare-sysroot

# Create a HDD image file called 'image' and copy the system onto it.
./image_create.sh image 4GiB ext2 gpt
../src/scripts/mkimage

# To launch the image using QEMU, you can run:
../src/scripts/run-qemu
```
