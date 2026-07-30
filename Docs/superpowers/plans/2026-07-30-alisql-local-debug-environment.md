# AliSQL Local Debug Environment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create a self-contained Debug build, installation, local database lifecycle, and connection environment in `/Users/zhuqingping/Work/Database/MySQL/alisql-install`.

**Architecture:** The target directory owns scripts, configuration, and all mutable instance paths. Scripts derive both the target directory and the sibling `AliSQL` source root from their own location, then use the installed client/server for lifecycle actions over a private Unix socket. The environment uses Debug binaries and localhost port `3344`, without modifying the source-tree build directory or the existing MySQL 8.0.41 instance.

**Tech Stack:** Bash with `set -euo pipefail`, CMake, Ninja, Homebrew Bison/OpenSSL, AliSQL Debug binaries.

## Global Constraints

- Target root is exactly `/Users/zhuqingping/Work/Database/MySQL/alisql-install`.
- Source root is exactly `/Users/zhuqingping/Work/Database/MySQL/AliSQL`.
- Build type is `Debug`; build tree is `build-debug`; install prefix is `install`.
- The local server binds only `127.0.0.1`, listens on `3344`, and uses `run/mysql.sock`.
- Runtime directories are `data`, `log`, `run`, and `tmp` below the target root.
- The scripts contain no password and `my.cnf` contains no password.
- Initialization uses `--initialize-insecure` only when `data/mysql` does not exist; no script deletes or reinitializes existing data.
- Preserve user-owned source worktree files and target contents outside the files/directories named by this plan.

---

### Task 1: Create configuration and build script

**Files:**
- Create: `/Users/zhuqingping/Work/Database/MySQL/alisql-install/my.cnf`
- Create: `/Users/zhuqingping/Work/Database/MySQL/alisql-install/build-debug.sh`
- Create directories: `/Users/zhuqingping/Work/Database/MySQL/alisql-install/{log,run,tmp}`

**Interfaces:**
- Consumes: sibling source root `../AliSQL`, Homebrew Bison at `/opt/homebrew/opt/bison/bin/bison`, Homebrew OpenSSL at `/opt/homebrew/opt/openssl@3`.
- Produces: `build-debug/` CMake/Ninja tree and `install/` Debug installation tree.

- [ ] **Step 1: Define a configuration acceptance check**

The configuration must resolve only below the target root, use the fixed local
network/socket values, and include no authentication value:

```bash
grep -Fx 'port=3344' my.cnf
grep -Fx 'bind_address=127.0.0.1' my.cnf
grep -Fx 'socket=/Users/zhuqingping/Work/Database/MySQL/alisql-install/run/mysql.sock' my.cnf
! grep -Eqi '(password|api[_-]?key|authorization)' my.cnf
```

- [ ] **Step 2: Create `my.cnf` and `build-debug.sh`**

`my.cnf` must set these exact server paths and local validation settings:

```ini
[mysqld]
basedir=/Users/zhuqingping/Work/Database/MySQL/alisql-install/install
datadir=/Users/zhuqingping/Work/Database/MySQL/alisql-install/data
tmpdir=/Users/zhuqingping/Work/Database/MySQL/alisql-install/tmp
socket=/Users/zhuqingping/Work/Database/MySQL/alisql-install/run/mysql.sock
pid_file=/Users/zhuqingping/Work/Database/MySQL/alisql-install/run/mysqld.pid
log_error=/Users/zhuqingping/Work/Database/MySQL/alisql-install/log/mysqld.err
port=3344
bind_address=127.0.0.1
skip-log-bin
local_infile=ON
secure-file-priv=""
innodb_buffer_pool_dump_at_shutdown=OFF
innodb_buffer_pool_load_at_startup=OFF
log_error_verbosity=3
```

`build-debug.sh` must resolve `ROOT_DIR` and `SOURCE_DIR`, check for the
source directory, CMake, Ninja, Bison and OpenSSL, create `build-debug`, then
configure, build and install:

```bash
cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  -DWITH_BOOST="$SOURCE_DIR/extra/boost" \
  -DBISON_EXECUTABLE=/opt/homebrew/opt/bison/bin/bison \
  -DWITH_SSL=/opt/homebrew/opt/openssl@3 \
  -DWITH_UNIT_TESTS=ON
cmake --build "$BUILD_DIR" --parallel "$JOBS"
cmake --install "$BUILD_DIR"
```

- [ ] **Step 3: Verify configuration and script syntax**

Run:

```bash
bash -n /Users/zhuqingping/Work/Database/MySQL/alisql-install/build-debug.sh
grep -Fx 'port=3344' /Users/zhuqingping/Work/Database/MySQL/alisql-install/my.cnf
grep -Fx 'bind_address=127.0.0.1' /Users/zhuqingping/Work/Database/MySQL/alisql-install/my.cnf
```

Expected: every command exits zero. Do not run the full build during this task.

### Task 2: Create safe instance lifecycle scripts

**Files:**
- Create: `/Users/zhuqingping/Work/Database/MySQL/alisql-install/initialize.sh`
- Create: `/Users/zhuqingping/Work/Database/MySQL/alisql-install/start.sh`
- Create: `/Users/zhuqingping/Work/Database/MySQL/alisql-install/stop.sh`

**Interfaces:**
- Consumes: `my.cnf`, `install/bin/mysqld`, `install/bin/mysqladmin`, and directories from Task 1.
- Produces: an initialized `data/` only when explicitly requested, a running local instance, and a graceful socket-based shutdown.

- [ ] **Step 1: Define lifecycle failure checks**

Before an installed server exists, each lifecycle script must fail clearly and
must not create an initialized data directory or a pid file:

```bash
! /Users/zhuqingping/Work/Database/MySQL/alisql-install/initialize.sh
! /Users/zhuqingping/Work/Database/MySQL/alisql-install/start.sh
! test -e /Users/zhuqingping/Work/Database/MySQL/alisql-install/data/mysql
! test -e /Users/zhuqingping/Work/Database/MySQL/alisql-install/run/mysqld.pid
```

- [ ] **Step 2: Create initialization, startup, and shutdown behavior**

Each script starts with `set -euo pipefail` and resolves its root with:

```bash
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="$ROOT_DIR/install"
CONFIG_FILE="$ROOT_DIR/my.cnf"
```

`initialize.sh` creates `data`, `log`, `run`, and `tmp`, refuses when
`data/mysql` exists, checks `install/bin/mysqld`, and runs:

```bash
"$INSTALL_DIR/bin/mysqld" --defaults-file="$CONFIG_FILE" --initialize-insecure
```

`start.sh` refuses if `mysqladmin --socket="$ROOT_DIR/run/mysql.sock" ping`
succeeds, removes only a stale socket/pid after confirming no live server owns
the configured pid, starts the server with `nohup`, and polls socket ping up to
30 times. It prints `log/mysqld.err` on startup failure.

`stop.sh` invokes only:

```bash
"$INSTALL_DIR/bin/mysqladmin" --socket="$ROOT_DIR/run/mysql.sock" -uroot shutdown
```

It reports a missing server/socket or failed graceful shutdown and never uses
`kill`, `pkill`, or a destructive data operation.

- [ ] **Step 3: Verify lifecycle script syntax and no-build failure behavior**

Run:

```bash
bash -n /Users/zhuqingping/Work/Database/MySQL/alisql-install/initialize.sh
bash -n /Users/zhuqingping/Work/Database/MySQL/alisql-install/start.sh
bash -n /Users/zhuqingping/Work/Database/MySQL/alisql-install/stop.sh
```

Expected: all scripts parse. If `install/bin/mysqld` is absent, invoking
`initialize.sh` and `start.sh` must exit nonzero without creating `data/mysql`.

### Task 3: Create connection script and final checks

**Files:**
- Create: `/Users/zhuqingping/Work/Database/MySQL/alisql-install/connect.sh`
- Verify: all scripts and `my.cnf` from Tasks 1-2

**Interfaces:**
- Consumes: `install/bin/mysql` and `run/mysql.sock`.
- Produces: a root socket client command that accepts all normal mysql CLI arguments unchanged.

- [ ] **Step 1: Define connection-script acceptance check**

The script must use the installed client, socket, and root account; it must not
include `-p`, `--password`, TCP host, or a hard-coded database:

```bash
! grep -E -- '(^|[[:space:]])(-p|--password|--host|-h)' connect.sh
grep -F '"$INSTALL_DIR/bin/mysql" --socket="$SOCKET_FILE" -uroot "$@"' connect.sh
```

- [ ] **Step 2: Create `connect.sh`**

The script resolves `ROOT_DIR`, checks `install/bin/mysql`, then launches:

```bash
SOCKET_FILE="$ROOT_DIR/run/mysql.sock"
exec "$INSTALL_DIR/bin/mysql" --socket="$SOCKET_FILE" -uroot "$@"
```

The script does not require a running instance during syntax validation.

- [ ] **Step 3: Run final static verification**

Run:

```bash
for script in build-debug.sh initialize.sh start.sh stop.sh connect.sh; do
  bash -n "/Users/zhuqingping/Work/Database/MySQL/alisql-install/$script"
done
grep -Fx 'port=3344' /Users/zhuqingping/Work/Database/MySQL/alisql-install/my.cnf
grep -Fx 'bind_address=127.0.0.1' /Users/zhuqingping/Work/Database/MySQL/alisql-install/my.cnf
! rg -n --pcre2 -i "(api[_-]?key\\s*[:=]\\s*[^[:space:]]{8,}|authorization\\s*[:=]\\s*bearer\\s+[A-Za-z0-9._-]{8,}|--password(?:=|[[:space:]])|[[:space:]]-p[^[:space:]]+)" /Users/zhuqingping/Work/Database/MySQL/alisql-install
```

Expected: all checks pass. Full build, initialization, and startup are explicit
operator actions after review because they can take time and create a database
instance.

- [ ] **Step 4: Commit the source-repository design/plan record only**

Run:

```bash
git add Docs/superpowers/plans/2026-07-30-alisql-local-debug-environment.md
git commit -m "docs: plan local AliSQL debug environment"
```

The target environment is outside this Git worktree and is intentionally not
added to the AliSQL repository.
