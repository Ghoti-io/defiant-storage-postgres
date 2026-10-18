# The build image for defiant-storage-postgres: ghoti-build:gcc16 and libpq.
#
# libpq is built against only inside this image, never on a developer host
# (spine, version table: PostgreSQL). Build it after ghoti-build:gcc16
# (suite/Containerfile.build) exists in the same engine:
#
#   podman build -t ghoti-build-pg:gcc16-libpq17.11 -f Containerfile .
#
# On the EVO-X2: tools/evo image build localhost/ghoti-build-pg:gcc16-libpq17.11
# <dir holding this file> <this file>.
#
# The base is the local ghoti-build:gcc16 by its tag: that image is built
# where it runs and has no registry digest, and its own Containerfile pins
# what is in it (gcc:16.2.0 by digest, every package by version). The check
# below refuses any other compiler. The client library is pinned to the
# exact Debian trixie version; a rebuild that cannot get it fails rather than
# taking another. (The gcc base image already carries this libpq-dev, from
# buildpack-deps; naming it here makes the pin this image's own and fails the
# build if the base ever stops carrying it.)
ARG GHOTI_BUILD_IMAGE=ghoti-build:gcc16
FROM ${GHOTI_BUILD_IMAGE}

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libpq-dev=17.11-0+deb13u1 \
      libpq5=17.11-0+deb13u1 \
 && rm -rf /var/lib/apt/lists/* \
 && gcc --version | head -1 | grep -q ' 16\.2\.0$' \
 && pkg-config --exact-version=17.11 libpq \
 && test -f "$(pkg-config --variable=includedir libpq)/libpq-fe.h"
