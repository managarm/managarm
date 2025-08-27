# Hardware CI

Managarm has some infrastructure to automate testing of Managarm builds.

## Using `hwci`

> **⚠️ Note that access to Managarm's `hwci` requires your SSH public key to be added to our list of trusted users.**

To use the hardware CI, you need:
- Access permissions (see above).
- A Managarm system root (see [the build instructions](building/index.md)).
- The base system installed to the system root (`xbstrap install base`).
- Packages installed that are required the desired device.
    This may include `boot-artifacts` and `vendor-devicetrees` (`xbstrap install boot-artifacts vendor-devicetree`).
    The Raspberry Pi also requires `raspberrypi-firmware`.

Setting up `hwci` is done as follows:

1. Install `hwci` into a Python virtual environment. A convenient location for this may be the Managarm build tree.
    ```
    python3 -m venv venv
    ./venv/bin/pip3 install git+https://github.com/managarm/managarm-hwci.git
    ```

    Alternatively, `hwci` may be installed via `pipx` or other Python package managers.
    Note that updating `hwci` from Git may require `--force-reinstall` for `pip3` if the package version did not change
    since the last previous update.
2. `hwci` requires a `hwci.toml` configuration file in the directory that you run it from.
    The following configuration file gives access to the `bpi-f3` and `rpi4` devices.
    ```toml
    # The SSH key listed here must correspond to the public key that you use for authentication:
    ssh_identity_file = "~/.ssh/id_rsa"

    [repositories]
    aarch64-latest = "https://builds.managarm.org/projects/managarm_aarch64/success/repo/packages/aarch64/"
    riscv64-latest = "https://builds.managarm.org/projects/managarm_riscv64/success/repo/packages/riscv64/"

    [presets.rpi4]
    arch = "aarch64"
    repository = "aarch64-latest"
    packages = [ ] # We do not want to build from repositories.
    profile = "raspi4" # Profile for gen-boot-artifacts.py.

    [presets.bpi-f3]
    arch = "riscv64"
    repository = "riscv64-latest"
    packages = [ ] # We do not want to build from repositories.
    profile = "bpi-f3" # Profile for gen-boot-artifacts.py.
    ```
3. Finally, you can launch a run on one of the machines by running

    ```
    ./venv/bin/python3 -m hwci --relay https://hwci.managarm.org/relay -d rpi4 --sysroot system-root/ --timeout 300
    ```

    Here `system-root/` is the path to your Managarm system root (for the same architecture as the device).

## Setting up a hardware CI target

This section documents how to attach new devices to the hardware CI.

### Requirements

**Hardware requirements**.

- One or more devices that should be made available via hardware CI. In the following we call these _devices_ as opposed to the _target server_ (see below).
- A Linux machine that acts as the _target server_.
- Some way to turn the devices on and off. For example, this can be achieved via smart sockets, a NanoKVM connected to the device's ATX power button and LED, etc.
- Serial connections between the target server and the devices (e.g., a USB-to-serial adapter connected to the device).
- Network connectivity between the target server and devices to enable PXE or TFTP boot.

**Software requirements**.

- A WireGuard connection between the relay (`https://hwci.managarm.org/`) and the target server.
    Setting this up requires coordination with our infrastructure.
- A TFTP daemon on the target server (e.g., `tftp-hpa`).
- Enough disk space on the target server to store the disk image of all devices. Around 10 GiB per device should cover all use cases.
- Optionally, for faster snapshots of image files: a reflink-capable partition (e.g., `btrfs` or `xfs`) for the TFTP and image files.

**Device installation**.

1. For each device, create a directory for the TFTP files. This should be a subdirectory of the
    TFTP daemon's directory (e.g., if the TFTP daemon serves `/srv/tftp/`,
    each device should have a subdirectory in `/srv/tftp/`).
    * Note that some devices have specific requirements on what their
        TFTP directories should be named like. For example: by default, the Raspberry Pi 4 searches
        for a TFTP directory that matches its serial number.
    * Devices that use proper PXE should not have such requirements.
2. When using DHCP, set up your DHCP server for PXE or TFTP boot of each device.
3. Ensure that PXE or TFTP boot works for each device.
    How exactly this is done varies by device.
    * For example U-Boot can usually just boot using the `dhcp` or `tftp` commands.

**Installation on the target server**.

1. The target server needs a user and group (e.g., called `hwci`) to run the target server daemon.
    * The `hwci` user must have access to the `dialout` group (otherwise the daemon cannot access
        the serial device that connects the target server and devices).
    * Additionally, the `hwci` user must have write access to the per-device TFTP directories
        (but not to the TFTP daemon's root directory).
2. Install `hwci` into a virtual environment on the target server:
    ```bash
    sudo python3 -m venv /usr/local/lib/hwci/venv
    sudo /usr/local/lib/hwci/venv/pip3 install git+https://github.com/managarm/managarm-hwci.git
    ```
3. Create a state directory `/var/local/hwci` that is owned by the `hwci` user and group.
4. Create a configuration file `/var/local/hwci/target.toml` for the target server daemon.
    ```toml
    [devices.rpi4]
    tftp = "/srv/tftp/2026eece"
    uart = "/dev/ttyUSB0"
    switch = {"shelly" = "http://shelly-switch-a.local"}

    [devices.bpi-f3]
    tftp = "/srv/tftp/bpi-f3"
    uart = "/dev/ttyUSB1"
    switch = {"shelly" = "http://shelly-switch-b.local"}
    ```
5. Create a systemd service file `/usr/local/lib/systemd/system/hwci-target.service`
    (with `10.0.70.2` replaced by the actual WireGuard IP)
    ```
    [Service]
    Type=simple
    ExecStart=/usr/local/lib/hwci/venv/bin/python3 -m hwci.target --address 10.0.70.2
    WorkingDirectory=/var/local/hwci
    User=hwci
    Group=hwci
    ```
6. Enable and start `hwci-target.service` via `systemctl`.
