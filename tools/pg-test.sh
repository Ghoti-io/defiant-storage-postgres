#!/bin/sh
#
# SPDX-License-Identifier: LGPL-3.0-only
#
# Run a command against a throwaway Postgres server.
#
# Starts docker.io/library/postgres by the digest the spine pins (18.6,
# linux/amd64) in podman or docker, on 127.0.0.1 and a random port, with a
# random password and a 512 MB memory cap; waits until it accepts
# connections; runs COMMAND in the build image (the one this repository's
# Containerfile defines, which has libpq) on the host's network with
# GDEFSPG_TEST_CONNINFO naming the server; and removes the server on every
# exit path, a failure or a signal included. The exit status is COMMAND's.
#
#   tools/pg-test.sh [options] -- COMMAND
#
#   --engine E        podman or docker (default: $GHOTI_CONTAINER_ENGINE,
#                     else podman)
#   --image IMG       the build image (default ghoti-build-pg:gcc16-libpq17.11)
#   --mount SRC:DST   a volume for COMMAND's container (repeatable; default
#                     the current directory at the same path)
#   --workdir DIR     COMMAND's working directory (default the current one)
#   --env NAME=VALUE  set in COMMAND's container (repeatable)
#   --memory SIZE     COMMAND's container's memory cap (default 4g)
#   --pull-and-remove pull the server image if it is missing and remove it
#                     again at exit if this run pulled it (the EVO-X2)
#
# On the EVO-X2 it runs as the host phase of a job, where
# GHOTI_CONTAINER_ENGINE is the job's capped docker (tools/evo help,
# --host-after); documentation/postgres.md has the commands.

set -u

PG_IMAGE=docker.io/library/postgres@sha256:885953109528ad3dfc90362b1a6f50a78620b5315be19f187753d267e484dc5b
ENGINE=${GHOTI_CONTAINER_ENGINE:-podman}
IMAGE=ghoti-build-pg:gcc16-libpq17.11
MOUNTS=
WORKDIR=$(pwd)
ENVS=
MEMORY=4g
PULL=0
while [ $# -gt 0 ]; do
  case $1 in
  --engine) ENGINE=$2; shift 2 ;;
  --image) IMAGE=$2; shift 2 ;;
  --mount) MOUNTS="$MOUNTS -v $2"; shift 2 ;;
  --workdir) WORKDIR=$2; shift 2 ;;
  --env) ENVS="$ENVS -e $2"; shift 2 ;;
  --memory) MEMORY=$2; shift 2 ;;
  --pull-and-remove) PULL=1; shift ;;
  --) shift; break ;;
  *) printf 'pg-test: unknown option %s\n' "$1" >&2; exit 2 ;;
  esac
done
[ $# -gt 0 ] || { printf 'usage: tools/pg-test.sh [options] -- COMMAND\n' >&2; exit 2; }
[ -n "$MOUNTS" ] || MOUNTS="-v $WORKDIR:$WORKDIR"

engine_bin=$ENGINE
if ! command -v "$engine_bin" >/dev/null 2>&1; then
  printf 'pg-test: %s is not installed. The Postgres tests start their server in a container: install podman (or set --engine docker), or set GDEFSPG_TEST_SKIP_SERVER=1 to skip the Postgres arm (it is then reported as SKIPPED: postgres, never as passed).\n' "$ENGINE" >&2
  exit 1
fi

NAME=gdefspg-test-$$-$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')
PW=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
PULLED=0
cleanup() {
  "$ENGINE" rm -f "$NAME" >/dev/null 2>&1 || true
  if [ "$PULLED" = 1 ]; then
    "$ENGINE" rmi "$PG_IMAGE" >/dev/null 2>&1 || true
  fi
}
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

if ! "$ENGINE" image inspect "$PG_IMAGE" >/dev/null 2>&1; then
  if [ "$PULL" = 1 ]; then
    "$ENGINE" pull "$PG_IMAGE" >/dev/null || { printf 'pg-test: could not pull %s\n' "$PG_IMAGE" >&2; exit 1; }
    PULLED=1
  else
    printf 'pg-test: the server image is missing: %s pull %s\n' "$ENGINE" "$PG_IMAGE" >&2
    exit 1
  fi
fi

# A random port; another one if it is taken. The server is thrown away after
# the run, so it does not wait for the disk: the conformance suite's
# allocation-failure tests make a schema per allocation, and on SQLite the
# first of them took 1,149 s on disk and 35 s on tmpfs (EVO-X2, 2026-10-19).
started=0
for try in 1 2 3 4 5; do
  PORT=$(( 20000 + $(od -An -N2 -tu2 /dev/urandom | tr -d ' ') % 40000 ))
  if "$ENGINE" run -d --rm --name "$NAME" --memory 512m --memory-swap 512m \
      -p "127.0.0.1:$PORT:5432" -e POSTGRES_PASSWORD="$PW" \
      "$PG_IMAGE" -c fsync=off -c synchronous_commit=off \
      -c full_page_writes=off >/dev/null 2>&1; then
    started=1
    break
  fi
  "$ENGINE" rm -f "$NAME" >/dev/null 2>&1 || true
done
[ "$started" = 1 ] || { printf 'pg-test: the Postgres server could not start (%s run failed five times)\n' "$ENGINE" >&2; exit 1; }

# Ready when it answers on TCP: the image's first-run setup listens on its
# socket only, so a TCP answer is the real server.
ready=0
for i in $(seq 1 120); do
  if "$ENGINE" exec "$NAME" pg_isready -q -h 127.0.0.1 -U postgres >/dev/null 2>&1; then
    ready=1
    break
  fi
  if [ -z "$("$ENGINE" ps -q --filter "name=$NAME")" ]; then
    break
  fi
  sleep 1
done
if [ "$ready" != 1 ]; then
  printf 'pg-test: the Postgres server did not become ready; its log:\n' >&2
  "$ENGINE" logs "$NAME" 2>&1 | tail -20 >&2
  exit 1
fi
printf 'pg-test: %s on 127.0.0.1:%s (%s)\n' "$PG_IMAGE" "$PORT" "$NAME"

CONNINFO="host=127.0.0.1 port=$PORT user=postgres password=$PW dbname=postgres connect_timeout=10"
# Files COMMAND writes belong to the caller: rootless podman maps the
# caller's id into the container, docker runs as it.
case $(basename "$ENGINE") in
podman) USERFLAG=--userns=keep-id ;;
*) USERFLAG="--user $(id -u):$(id -g)" ;;
esac
# shellcheck disable=SC2086
"$ENGINE" run --rm --network host --memory "$MEMORY" --memory-swap "$MEMORY" \
  $USERFLAG $MOUNTS -w "$WORKDIR" -e HOME=/tmp \
  -e GDEFSPG_TEST_CONNINFO="$CONNINFO" $ENVS "$IMAGE" sh -c "$*"
rc=$?
exit $rc
