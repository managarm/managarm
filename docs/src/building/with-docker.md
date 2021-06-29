# Building with Docker

This section explains how to build Managarm in a Docker environment.

> Note: we recommend using `cbuildrt` instead of Docker as it is faster, requires less privileges and is better tested
(we use `cbuildrt` on our continuous integration build server, so breakages are more noticeable).

1.  Complete the [Preparations](index.md#preparations) section.
1.  Install [Docker](https://docs.docker.com/get-docker/).
1.  Build a Docker image from the provided Dockerfile:
    ```bash
    docker build -t managarm-buildenv --build-arg=USER=$(id -u) src/docker
    ```
1.  Create a `bootstrap-site.yml` file inside the `build` directory containing:
    ```yml
    container:
      runtime: docker
      image: managarm-buildenv
      src_mount: /var/bootstrap-managarm/src
      build_mount: /var/bootstrap-managarm/build
      allow_containerless: true
    ```
    This `bootstrap-site.yml` will instruct our build system to invoke the build scripts within your container image.

Now proceed to the [Building](index.md#building) section.
