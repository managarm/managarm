# Fuzzing

Managarm has a [syzkaller port](https://github.com/managarm/syzkaller) that can fuzz Thor and the posix subsystem.

You will need Go installed to set it up, and QEMU with KVM to run it.

1. Build managarm with coverage enabled. Set up the build directory as in the [build guide](../building/cbuildrt.md), up to `xbstrap init`.
    Set `MANAGARM_WORKDIR` to the directory of your managarm installation (`~/managarm` in the build guide).

    ```
    export MANAGARM_WORKDIR=~/managarm
    ```

    In `$MANAGARM_WORKDIR/src/bootstrap.d/managarm-system.y4.yml`:

    - managarm-kernel configure step: add ``-Dkernel_frame_pointers=true`` and ``-Dkcov=true``
    - managarm-system configure step: add ``-Dkcov=true``

    Then,

    ```
    cd $MANAGARM_WORKDIR/build
    xbstrap install --rebuild managarm-kernel managarm-system
    ```

2. Install enough packages to ssh into managarm:

    ```
    xbstrap install core-files mlibc mlibc-headers libgcc libstdc++ \
        managarm-kernel managarm-kernel-uefi managarm-system managarm-system-rs \
        managarm-scripts limine kmscon bash coreutils shadow util-linux \
        dhcpcd iproute2 iana-etc ca-certificates openssh
    ```

3. Create the managarm image. Run in the `build` directory:

    ```
    xbstrap run initialize-empty-image

    # Log to serial so syzkaller can read the logs.
    mkdir -p system-root/etc/managarm
    echo 'cmdline = "serial"' > system-root/etc/managarm/boot.toml

    # Generate the SSH key that syzkaller logs in with.
    mkdir -p $MANAGARM_WORKDIR/keys
    ssh-keygen -t ed25519 -N "" -f $MANAGARM_WORKDIR/keys/id_ed25519
    install -d -m 700 system-root/root/.ssh
    install -m 600 $MANAGARM_WORKDIR/keys/id_ed25519.pub system-root/root/.ssh/authorized_keys

    # Enable root login in SSH.
    mkdir -p system-root/etc/ssh/sshd_config.d
    echo 'PermitRootLogin yes' > system-root/etc/ssh/sshd_config.d/60-syzkaller.conf

    xbstrap run make-image
    ```

4. Build syzkaller:

    ```
    xbstrap install-tool host-syzkaller
    ```

5. Configure syz-manager:

    ```
    # Use unstripped thor/posix-subsystem for crash reports/coverage.
    mkdir -p $MANAGARM_WORKDIR/kernel-obj
    ln -s ../build/pkg-builds/managarm-kernel/kernel/thor/thor $MANAGARM_WORKDIR/kernel-obj/
    ln -s ../build/pkg-builds/managarm-system/posix/subsystem/posix-subsystem $MANAGARM_WORKDIR/kernel-obj/

    # Commit hash
    COMMIT="managarm $(git -C $MANAGARM_WORKDIR/src/managarm rev-parse --short HEAD)"

    # Config for Thor fuzzing.
    cat > $MANAGARM_WORKDIR/managarm.cfg <<EOF
    {
        "target": "managarm/amd64",
        "http": "127.0.0.1:56741",
        "workdir": "$MANAGARM_WORKDIR/workdir",
        "image": "$MANAGARM_WORKDIR/build/image",
        "sshkey": "$MANAGARM_WORKDIR/keys/id_ed25519",
        "ssh_user": "root",
        "syzkaller": "$MANAGARM_WORKDIR/build/tools/host-syzkaller",
        "kernel_obj": "$MANAGARM_WORKDIR/kernel-obj",
        "kernel_src": "$MANAGARM_WORKDIR",
        "kernel_build_src": "/var/lib/managarm-buildenv",
        "tag": "$COMMIT",
        "type": "qemu",
        "procs": 2,
        "cover": true,
        "sandbox": "none",
        "enable_syscalls": ["hel*", "syz_hel_*", "syz_mmap"],
        "vm": {"count": 2, "cpu": 2, "mem": 2048}
    }
    EOF
    ```

    To fuzz the posix subsystem, make a second config with `"disable_syscalls": ["hel*", "syz_hel_*"]` instead of
    `enable_syscalls`.

    Each config must have its own `workdir`.

6. Run syz-manager:

    ```
    # syz-manager uses the managarm toolchain for crash reports, coverage and C reproducers.
    BUILD_DIR=$MANAGARM_WORKDIR/build
    export PATH=$BUILD_DIR/tools/system-gcc/bin:$PATH
    export PATH=$BUILD_DIR/tools/cross-binutils/bin:$PATH
    export PATH=$PATH:$BUILD_DIR/tools/host-llvm-toolchain/bin
    export SYZ_CC_managarm_amd64="x86_64-managarm-gcc --sysroot=$BUILD_DIR/system-root"

    $BUILD_DIR/tools/host-syzkaller/bin/syz-manager -config $MANAGARM_WORKDIR/managarm.cfg
    ```

    The web interface will be up at <http://127.0.0.1:56741>. From there, you will be able to see the current fuzzing status and crashes it found.

    Do not rebuild the image, the kernel or host-syzkaller while syz-manager is running.

7. Report bugs on Discord:

    Report crashes on the [Managarm Discord server](https://discord.gg/7WB6Ur3)'s #log-triage forum. As syzkaller will attempt to reproduce bugs and create a syz/C reproducer, please prefer sending a reproducer instead of just the crash.
