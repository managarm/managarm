# Building With Docker

This section explains how to build Managarm in a Docker environment.

> Note: we recommend using `cbuildrt` instead of Docker as it is faster, more minimal and better tested
(we use `cbuildrt` on our continuous integration build server, so breakages are more noticeable).

# Creating Docker image and container

1.  Install [Docker](https://docs.docker.com/get-docker/).
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

Now proceed to the [Building](index.md#building) section.
