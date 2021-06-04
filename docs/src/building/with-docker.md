# Building With Docker

This section explains how to build Managarm in a Docker environment.

> Note: not recommend TODO

# Creating Docker image and container

1.  A working `docker` installation is required to perform a containerized build.
2.  Build a Docker image from the provided Dockerfile:
    ```bash
    docker build -t managarm-buildenv --build-arg=USER=$(id -u) src/docker
    ```
3.  Create a `bootstrap-site.yml` file inside the `build` directory containing:
    ```yml
    container:
      runtime: docker
      image: managarm-buildenv
      src_mount: /var/bootstrap-managarm/src
      build_mount: /var/bootstrap-managarm/build
      allow_containerless: true
    ```
    This `bootstrap-site.yml` will instruct our build system to invoke the build scripts within your container image.

Now proceed to the [Building](index.md#building) paragraph.
