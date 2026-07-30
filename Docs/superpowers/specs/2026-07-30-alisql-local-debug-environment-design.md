# AliSQL Local Debug Environment Design

## Goal

Create a self-contained local AliSQL Debug build and validation environment at
`/Users/zhuqingping/Work/Database/MySQL/alisql-install`. It must not share a
build, install tree, data directory, socket, port, log, or password mechanism
with the existing `mysql-8.0.41` instance.

## Layout

```text
alisql-install/
  build-debug/       CMake/Ninja Debug build tree
  install/           CMAKE_INSTALL_PREFIX and installed binaries
  data/              initialized MySQL data directory
  log/               server error log
  run/               pid file and Unix socket
  tmp/               server temporary files
  my.cnf             instance configuration
  build-debug.sh     configure, build and install
  initialize.sh      initialize an empty local data directory
  start.sh           start the configured server
  stop.sh            stop the configured server
  connect.sh         connect through the private Unix socket
```

Only scripts, `my.cnf`, and empty runtime directories are created initially.
`build-debug/`, `install/`, and `data/` are produced by explicit build or
initialization commands; no existing directory is deleted or reinitialized.

## Build

`build-debug.sh` derives the AliSQL source root from its own path and configures
`build-debug/` with Ninja, `CMAKE_BUILD_TYPE=Debug`, the repository's bundled
`extra/boost`, and `CMAKE_INSTALL_PREFIX=alisql-install/install`. It uses the
Homebrew Bison and OpenSSL locations already available on this machine.

The script accepts `JOBS` as an optional environment override and otherwise
chooses a conservative CPU-based parallelism level. It always performs the
build and install step, leaving incremental CMake/Ninja state intact. A source
or dependency preflight failure exits before configuration or installation.

## Instance lifecycle

`my.cnf` binds the server to `127.0.0.1:3344` and places every mutable path
under `alisql-install`. It disables binary logging for local validation, enables
`local_infile`, permits local file loading, and keeps buffer-pool dump/load
disabled. The Unix socket is `run/mysql.sock`; the pid file is `run/mysqld.pid`.

`initialize.sh` creates required directories and invokes installed `mysqld`
with `--initialize-insecure` only when `data/mysql` is absent. It refuses to
touch a nonempty/initialized data directory. Root has no password in this local
Debug-only instance and is reachable by the private socket, not a password
embedded in source, scripts, shell history, or `my.cnf`.

`start.sh` checks that the installed server exists and that the instance is not
already live, then launches `mysqld --defaults-file=...` in the background.
It waits for `mysqladmin ping` over the configured socket and returns the error
log location on failure. `stop.sh` uses `mysqladmin shutdown` over that socket;
if unavailable it reports the pid and does not send a blind kill signal.

`connect.sh` invokes the installed client as `root` through the socket and
passes all supplied arguments through unchanged, so callers can select a
database or execute a statement without exposing credentials.

## Safety and verification

- Scripts use `set -euo pipefail`, resolve their paths without relying on the
  calling directory, and quote all paths.
- No script performs `rm -rf`, deletes data, or silently reinitializes an
  existing instance.
- Scripts do not run a server, build the complete source tree, or initialize
  data as part of their creation test.
- Verification after creation: `bash -n` for every script; `my_print_defaults`
  if an installed binary exists; and a read-only path/configuration check.
- The user starts the potentially long build explicitly with `build-debug.sh`.
