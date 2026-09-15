# HDMI-CEC L1 Tests

This directory contains the L1 unit tests for the hdmicec library using Google Test (gtest/gmock).

## Framework

- **Test Framework**: Google Test (gtest) v1.10.0+
- **Mocking Framework**: Google Mock (gmock)
- **Language**: C++17 — set by `AM_CXXFLAGS = -Wall -std=c++17 -g @AIDL_CXXFLAGS@` in
  `Makefile.am`.  Not a preference: `rdk-halif-aidl/common/current/halcompat.h:73` opens
  `namespace com::rdk::hal::halcompat {`, a nested-namespace definition that is C++17 syntax and
  does not compile at `c++14` at all, and the generated AIDL headers include `<optional>`
  (`IHdmiCec.h:12`, used by `getProperty` at `:30`) — see
  [Building Tests](#building-tests) for the rest of the AIDL/Binder build contract
- **Build System**: Autotools
- **HAL back-ends**: the middleware compiles **both** the legacy in-process HAL back-end and the
  AIDL/Binder one into `libRCEC` and chooses between them once per process, so this suite is run
  once per selection outcome — see
  [Back-End Selection and the Invocation Matrix](#back-end-selection-and-the-invocation-matrix)
- **Coverage**: gcov/lcov via `run_coverage.sh`, gated at 80% line coverage — see [Coverage](#coverage)

## Prerequisites

Install the Google Test development packages.  Elevated privilege is needed for the package
manager and for nothing else:

```bash
# Ubuntu/Debian
sudo apt-get install libgtest-dev libgmock-dev
```

If the distribution packages ship headers without the static libraries, build GoogleTest from
source **unprivileged** and install it into a prefix you own.  Compiling as root is unnecessary,
and writing archives straight into `/usr/lib` puts files the package manager does not know about
where they can shadow a later distribution update:

```bash
cmake -S /usr/src/googletest -B "$HOME/gtest-build" \
      -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build "$HOME/gtest-build"
cmake --install "$HOME/gtest-build"        # writes only under $HOME/.local
```

Point `configure` at that prefix so the build finds it without any system-wide change:

```bash
CPPFLAGS="-I$HOME/.local/include" LDFLAGS="-L$HOME/.local/lib" ./configure --enable-l1tests
```

and put `$HOME/.local/lib` on `LD_LIBRARY_PATH` when running `run_L1Tests` if the shared
libraries were built there.  That line covers GoogleTest and GoogleTest only; the complete
`configure` invocation, which also has to reach the AIDL and Binder staging roots below, is in
[Building Tests](#building-tests).

### The AIDL/Binder dependency is mandatory, for every host and every vendor

`libRCEC` links the generated HDMI-CEC AIDL client stubs and the Binder client library
**unconditionally**, so this suite cannot be built without them.  There is no configure switch to
turn the AIDL back-end off and no vendor conditional anywhere in the selection: both back-ends are
compiled in and one is chosen at run time.  Two staging roots supply everything:

- **The staged `rdk-halif-aidl` tree** — the generated `com/rdk/hal/hdmicec` stubs, the frozen
  `common/0.2.0.0` import they were generated against, and `common/current/halcompat.h`.
- **The staged `linux_binder_idl` Binder SDK** — `libbinder`, `libutils` and the
  `binder/`, `utils/` and `android-base/` headers the generated stubs include.

`configure.ac` declares the two prefixes with `AC_ARG_VAR` and **derives every path from them**, so
they are the only two inputs a build needs and there is one place that decides — which is what keeps
the Autotools build and the hand-written `ccec/src/Makefile` from being configured differently.  Two
further variables exist for split staging layouts, one more exists for the hand build alone, and one
more again — the Binder wire ABI, after the table — is not a path at all.
**The four staging-path variables `configure` accepts are the ones marked *both* below**
(`AC_ARG_VAR`, `configure.ac:92-99`); `BINDER_LIB_DIR` is not one of them:

| Input | Spelling | Accepted by | Notes |
| --- | --- | --- | --- |
| HALIF staging root | `--with-halif-prefix=DIR` or `HALIF_PREFIX=DIR` | both | Omit it when the `rdk-halif-aidl` checkout sits beside this submodule; `configure` falls back to it.  The hand build has no fallback and needs it exported |
| AIDL stub libraries | `HALIF_LIB_DIR=DIR` | both | For any layout where they are not at `HALIF_PREFIX/lib/halif` — which is every host that builds the snapshots outside the checkout, so treat it as required until you have checked |
| Binder SDK root | `--with-binder-sdk=DIR` or `BINDER_SDK_DIR=DIR` | both | Expected to hold `lib/binder` and `include/binder_sdk` |
| Binder headers | `BINDER_SDK_INCLUDE_DIR=DIR` | both | Only for a layout whose headers are not under `BINDER_SDK_DIR` |
| Binder libraries | `BINDER_LIB_DIR=DIR` | **`ccec/src/Makefile` only** | The hand build's override for `libbinder`/`libutils`, defaulting to `$(BINDER_SDK_DIR)/lib/binder` (`ccec/src/Makefile:47,:54`), used in its `-L` list and in its Binder-wire-ABI probe, which names it in the error it raises when the ABI cannot be determined (`:195`).  **Passing it to `configure` does nothing**: `configure` derives the same directory itself from `BINDER_SDK_DIR` (`configure.ac:223-227`) and overwrites whatever was passed |

Only the hand build reads `BINDER_LIB_DIR`, so a split layout that stages the Binder libraries away
from `BINDER_SDK_DIR/lib/binder` has to say so twice in different ways: `BINDER_LIB_DIR` for
`make -C ccec/src`, and — since `configure` will not take that variable — an `-L` in the ambient
`LDFLAGS` for the Autotools build, which is exactly the case its "not found; relying on the ambient
`LDFLAGS`" result is written for.

One further `configure` variable is not a staging path at all: **`BINDER_IPC_32BIT`** (`AC_ARG_VAR`,
`configure.ac:570`) settles the Binder **wire ABI** — `yes` for the 32-bit ABI and protocol 7, `no`
for the 64-bit ABI and protocol 8 — and a `yes` reaches the compile as `-DBINDER_IPC_32BIT` on every
target that sees a Binder header.  **Leaving it unset is the normal case**, because `configure`
derives it from the staged `libbinder` itself; an explicit value is checked against those two
spellings and `configure` stops rather than choosing when it contradicts the ABI it measured in the
staged library (`configure.ac:641-694`), and the settled decision, the protocol it implies and where
it came from are printed together in its `Binder wire ABI` notice line (`:763`).  Both build systems
accept it and both derive it from the staged `libbinder` when it is left unset; the full treatment is
in the submodule's `UNIT_TEST_SETUP.md`.

From those, `configure` derives **four include roots** — and all four are required:

1. `$HALIF_PREFIX/hdmicec/0.1.0.0/include` — the generated `com/rdk/hal/hdmicec/*.h` stubs.
2. `$HALIF_PREFIX/common/0.2.0.0/include` — `com/rdk/hal/PropertyValue.h`, the frozen import the
   `hdmicec` snapshot was generated against.  **0.2.0.0 deliberately, not the newer `common`
   release**, because a newer one would put a different ABI underneath pre-generated code compiled
   against the older headers.
3. `$HALIF_PREFIX/common/current` — **the only root that supplies `halcompat.h`.**  That header is
   *not* inside root 2, so the two snapshot roots alone do not reach it, and omitting this one
   surfaces as `halcompat.h: No such file or directory`.  It also lives outside the `hdmicec/`-only
   sparse checkout `rdk-halif-aidl` is documented with, so a sparse clone has to materialise
   `common/` as well.
4. **The Binder SDK header root** — `$BINDER_SDK_DIR/include/binder_sdk` in a development layout, a
   flat `include` directory under a sysroot.  Required by the generated headers themselves, which
   pull `<binder/IBinder.h>`, `<binder/IInterface.h>`, `<binder/Status.h>`, `<binder/Parcel.h>`,
   `<utils/String16.h>`, `<utils/StrongPointer.h>` and `<android-base/macros.h>`.

and **four link edges, in dependency order**:

```
-lhdmicec-v0.1.0.0-cpp -lcommon-v0.2.0.0-cpp -lbinder -lutils
```

`libbinder`'s own transitive closure — `liblog`, `libbase` and `libcutils` — is **not** named as
direct edges, because nothing in the middleware references it, but the whole set must be staged
and on the loader path all the same.  A link that fails with those libraries present is a staging
problem; do not "fix" it by adding them as direct edges.

**`libcutils_sockets` is staged beside them but is not part of that closure**, and the distinction
matters when you are diagnosing a link failure — chasing it as a missing dependency looks for a
fault that is not there.  Measured `DT_NEEDED` in the staged SDK: `libbinder.so` names
`libutils libcutils libbase liblog`; `libutils.so` names `libcutils liblog`; `libcutils.so` names
`libbase liblog`; and `libcutils_sockets.so` names **`libc` alone**.  Nothing in the staged tree —
not one of those five libraries, not `servicemanager`, and neither HALIF stub library — carries a
`DT_NEEDED` entry for it.  It is a **companion library the SDK build produces and stages**, whose
twelve exported `socket_*`/`android_get_control_socket` symbols no part of this middleware
resolves.  So what is asked of it is only that it be **present and loadable in its own right**,
which is what the `ldd` check verifies; it is never a link edge and never a reason a link failed.

**Building is not the same as running.**  Compiling and linking the AIDL back-end needs only the
headers and libraries above, so the build integration is verifiable on an ordinary host.
*Executing* an AIDL-path case additionally needs a kernel with Binder support, a Binder protocol
version matching the one `libbinder` was built for, and a running `servicemanager` — see
[Which invocations this host can run](#which-invocations-this-host-can-run).

## Building Tests

**The short answer:** `./run_coverage.sh --build --run` from this directory does the whole
sequence below — build, orchestrate invocations A through E while deferring the ones this host
cannot support, capture, report and gate — and is the recipe this suite is maintained against.

The long answer, for building by hand.  Five prerequisites are easy to miss, and every one of
them is fatal rather than degrading:

1. **`stubs/` must exist.**  The middleware includes IARM bus headers it does not ship.
2. **The HAL driver mock must be symlinked over the driver header.**  That symlink is the entire
   mocking seam; without it the build looks for a real HDMI-CEC driver.  It gained no new entry for
   the AIDL back-end, and that is not an omission: `stubs/` mutes headers this middleware includes
   but does not ship, whereas the AIDL headers are real and are found through the two staging
   prefixes.
3. **`CPPFLAGS` must carry `mocks/` and `stubs/`, and `PKG_CONFIG_PATH` must reach a `gtest.pc`.**
4. **The two AIDL/Binder staging prefixes must resolve.**  They are optional on the command line
   and mandatory in effect — see
   [The AIDL/Binder dependency](#the-aidlbinder-dependency-is-mandatory-for-every-host-and-every-vendor).
   Get them wrong and `configure` still fails, but **not where you would expect**: its directory
   discovery is deliberately *tolerant*, and the failure comes from a later probe that compiles and
   links.  Read
   [How configure fails when a prefix is wrong](#how-configure-fails-when-a-prefix-is-wrong) before
   you debug one, because a `not found` line early in the output is not itself the error.  **That
   same probe links, so the staged Binder libraries must be on `LD_LIBRARY_PATH` before `configure`
   runs** — the export is in the block below, and resolving the two prefixes without it fails the
   probe just as a wrong prefix does.
5. **`make` at the top level does not build this suite.**  `tests/L1Tests` needs its own
   `make` pass, and `tests/L2Tests` a third one — see [Running Tests](#running-tests) for why root
   `make check` does not help either.

```bash
# From the hdmicec submodule root
export GTEST_PREFIX="$HOME/.local/gtest-1.15.0"       # or wherever gtest.pc lives

# The staged Binder SDK.  The ':?' spelling fails loudly with its own message rather than
# letting the build carry on and produce a library that silently carries one back-end.
: "${BINDER_SDK_DIR:?export this first: the staged Binder SDK root holding lib/binder}"
: "${BINDER_SDK_INCLUDE_DIR:=$BINDER_SDK_DIR}"
# The staged rdk-halif-aidl tree.  Omit this export entirely when the checkout sits beside
# this submodule -- configure falls back to ../rdk-halif-aidl by itself.
export HALIF_PREFIX="${HALIF_PREFIX:-$PWD/../rdk-halif-aidl}"
# The AIDL stub libraries.  A source checkout of rdk-halif-aidl holds headers and no built
# libraries, so on any host that stages them elsewhere this is REQUIRED, not optional --
# measured: omitting it here fails configure at its link probe.  Drop the line only when
# they really do sit at $HALIF_PREFIX/lib/halif.
: "${HALIF_LIB_DIR:?export this too: the directory holding libhdmicec-v0.1.0.0-cpp}"

# The loader path, needed to LINK and not only to run -- this is the "later probe that compiles
# and links" prerequisite 4 above warns about.  That probe links a program against libbinder,
# which carries no RUNPATH, and GNU ld does not search -L for a shared library's own DT_NEEDED
# entries, so the staged closure has to be findable BEFORE ./configure runs rather than after.
# Measured, hand-linking that same probe with all four -l edges and both -L paths correct: with
# this line it links, without it ld says "libcutils.so, needed by .../libbinder.so, not found
# (try using -rpath or -rpath-link)", fails on an undefined __android_log_error_write, and
# configure reports "the AIDL stub and Binder client libraries could not be linked" -- which
# reads as a wrong prefix and is not one.  Append "$GTEST_PREFIX/lib" as well when the
# GoogleTest in that prefix is a shared build: the probe never links GoogleTest -- configure
# finds it with PKG_CHECK_MODULES, which reads gtest.pc and links nothing -- but the built
# runners load it (see Prerequisites).
export LD_LIBRARY_PATH="$HALIF_LIB_DIR:$BINDER_SDK_DIR/lib/binder${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# 1+2. Stub headers, and the HAL driver mock injected over the driver header
mkdir -p stubs/rdk/iarmbus stubs/ccec/drivers/iarmbus
touch stubs/rdk/iarmbus/libIARM.h \
      stubs/rdk/iarmbus/libIBus.h \
      stubs/rdk/iarmbus/libIBusDaemon.h \
      stubs/ccec/drivers/iarmbus/CecIARMBusMgr.h
ln -sf ../../../mocks/hdmicec/hdmi_cec_driver.h stubs/ccec/drivers/hdmi_cec_driver.h

# 3+4. Generate and configure.  Drop the two coverage flags for a plain build.  The four
#      AIDL include roots, the four -l edges and the C++17 flag are DERIVED by configure
#      from the staging prefixes set at the top -- do not spell them into CPPFLAGS by hand.
autoreconf -if
PKG_CONFIG_PATH="$GTEST_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
CPPFLAGS="-I$PWD/mocks -I$PWD/stubs -I$GTEST_PREFIX/include" \
LDFLAGS="-L$GTEST_PREFIX/lib -fprofile-arcs -ftest-coverage" \
CXXFLAGS="-fprofile-arcs -ftest-coverage" \
  ./configure --enable-l1tests \
      --with-halif-prefix="$HALIF_PREFIX" \
      --with-binder-sdk="$BINDER_SDK_DIR" \
      BINDER_SDK_INCLUDE_DIR="$BINDER_SDK_INCLUDE_DIR" \
      HALIF_LIB_DIR="$HALIF_LIB_DIR"

# 5. Build the libraries, then the L1 suite, then the L2 tier
make -j"$(nproc)" all
make -C tests/L1Tests all
make -C tests/L2Tests all
```

Read the configure output back before trusting it.  It prints one `AIDL HDMI CEC back-end:` line
per resolved path plus the C++17 flag it settled on — HALIF prefix, AIDL stub libraries, Binder SDK
prefix, Binder header root, Binder libraries — so a path that resolved to the wrong place is visible
there rather than in a link error much later.  `--with-binder-sdk` and `BINDER_SDK_INCLUDE_DIR` may
name the same directory; they are separate inputs only because some staging layouts put the Binder
headers and libraries in different trees.

The third `make` builds the L2 tier's two programs — the runner and the out-of-process fake service
host.  Without it invocations D and E have no binary at all; see
[The L2 tier](#the-l2-tier-two-programs-two-processes).

`autoreconf`/`configure` rewrite six git-tracked `Makefile` files in this submodule.  Five of them
are pure local toolchain output.  **The sixth is not**: `ccec/src/Makefile` is a hand-written RDK
build file with its own object list and link command — real, committed source content, which this
migration edited to add `DriverAidlImpl.o`, `-std=c++17`, the AIDL include roots and a rewritten
link command — and it is listed in `configure.ac`'s `AC_CONFIG_FILES` all the same, so `configure`
clobbers it by design.  Two consequences, and the first is the one that costs work if it is
learned the hard way:

- **Never revert those six with `git checkout`.**  A blanket checkout restores whatever the *index*
  holds, which is right only under an assumption about the index and silently destroys an
  uncommitted edit to `ccec/src/Makefile`.  `run_coverage.sh` runs no git command that writes, in
  any mode, and no recipe in this file asks you to.
- **`--build` snapshots all six and restores them itself, inside the same invocation.**  The
  snapshot is taken from the working tree immediately before `autoreconf` runs, into a run-scoped
  `mktemp -d`, and restored from an `EXIT`/`INT`/`TERM` trap at the end of *that* invocation — so a
  failed or Ctrl-C'd build restores too, and there is nothing left to paste.  A second build in the
  same tree is refused by a lock file, because two runs would interleave their snapshots and the
  loser would "restore" the winner's generated content.
- **A standalone `./run_coverage.sh --restore` always exits non-zero, whatever `git status` says,
  and that is the contract rather than a limitation.**  The snapshot directory is run-scoped: the
  invocation that took it also removed it, `--build` is the only thing that takes one, and the
  runner refuses the two flags together (*"--restore is a standalone action; do not combine it with
  --build or --run"*, exit `1`) — so a separate `--restore` process has nothing authoritative to
  restore from and says so instead of guessing.  What git can tell it about the six is printed as
  *information* and never as the verdict.  With them clean it warns that *"none of the allowlisted
  paths differs from the index right now"*, states in the same breath that this **is not the same as
  "nothing to restore"** because the index is not the reference — a tree whose Makefiles were
  committed in their generated form reads as perfectly clean while holding exactly the content that
  needs replacing — and then exits `1` on *"THIS INVOCATION HAS NO SNAPSHOT, so there is nothing it
  can authoritatively restore from, and nothing was changed"* (measured, with all six clean and
  immediately after a completed build).  With them dirty the same refusal additionally names what is
  dirty.  In neither case does it reach for `git checkout`, in this or any other mode, because that
  restores what the index holds and destroys an uncommitted edit to `ccec/src/Makefile`.  If you need
  the six put back, run `./run_coverage.sh --build --run` and let it snapshot and restore them
  itself — and verify the restoration from git and your own snapshot rather than from any exit status
  of `--restore`: `git status --porcelain -- Makefile ccec/Makefile ccec/src/Makefile osal/Makefile
  osal/src/Makefile tests/Makefile` printing nothing, and a `cmp` of each path against the snapshot
  copy the block below takes.

A hand build has no such trap, so take the snapshot yourself — before `autoreconf`, from the
working tree, and restore from the copy rather than from git.  Put the snapshot, the build and the
restore in **one** bash shell, as below: a trap lives only as long as the shell that set it, so a
snapshot taken in one command and a restore pasted as a second command protects nothing in between,
which is exactly the window `autoreconf` and `configure` run in.

```bash
# From the hdmicec submodule root.  Run the whole block, in bash, in one go.  The exports from
# the recipe above must already be in effect: under `set -u` a missing one stops the block by
# name, and the trap puts the six back before it does.
(
  set -euo pipefail

  SIX=(Makefile ccec/Makefile ccec/src/Makefile osal/Makefile osal/src/Makefile tests/Makefile)

  # Owner-only, and checked: mktemp -d creates 0700 and `set -e` stops here if it cannot
  # create the directory at all, so the loop below never copies into an unset path.
  SNAP="$(mktemp -d "${TMPDIR:-/tmp}/hdmicec-mk-snapshot.XXXXXX")"
  chmod 700 "$SNAP"

  restore_six() {
    local f
    for f in "${SIX[@]}"; do
      if [ -f "$SNAP/$f" ]; then cp -p "$SNAP/$f" "$f"; fi
    done
    rm -rf "$SNAP"
  }
  trap restore_six EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM

  for f in "${SIX[@]}"; do
    if [ ! -f "$f" ]; then
      echo "$f is missing: run this from the hdmicec submodule root" >&2
      exit 1
    fi
    mkdir -p "$SNAP/$(dirname "$f")"
    cp -p "$f" "$SNAP/$f"
  done

  autoreconf -if
  PKG_CONFIG_PATH="$GTEST_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
  CPPFLAGS="-I$PWD/mocks -I$PWD/stubs -I$GTEST_PREFIX/include" \
  LDFLAGS="-L$GTEST_PREFIX/lib" \
    ./configure --enable-l1tests \
        --with-halif-prefix="$HALIF_PREFIX" \
        --with-binder-sdk="$BINDER_SDK_DIR" \
        BINDER_SDK_INCLUDE_DIR="$BINDER_SDK_INCLUDE_DIR" \
        HALIF_LIB_DIR="$HALIF_LIB_DIR"

  make -j"$(nproc)" all
  make -C tests/L1Tests all
  make -C tests/L2Tests all
)
```

Those mechanics were exercised against a synthetic six-file tree, with a stand-in for
`autoreconf`/`configure`/`make` that rewrites the six the way they do: a clean run exits `0`, a
failing build exits `1`, `SIGINT` exits `130`, `SIGTERM` exits `143`, and an unset prefix under
`set -u` exits `1` — **and all five leave the six back at their tracked content with the snapshot
directory removed**.  Run from the wrong directory it stops at `Makefile is missing` with exit `1`
and creates nothing.  Add the two coverage flags from the recipe above if you want an instrumented
tree; the block is otherwise the same.

The six end up matching the index while the binaries the build produced stay where they are, which
is the same end state `--build` leaves.  A *further* `make` in that tree needs the `configure` step
again, because the generated `Makefile` files it would have used are the ones just put back.

#### How configure fails when a prefix is wrong

**Directory discovery is tolerant, and it is the probes that are hard.**  Each of the include roots
and library directories is *checked* first, and a check that does not resolve prints `not found`
(`configure.ac:159,:168,:177`) or `not found; relying on the ambient CPPFLAGS` / `…LDFLAGS`
(`:204,:218,:232`) — and **configure carries on**.  That tolerance is deliberate: a sysroot may stage
these headers and libraries on the default search path, where there is no directory for configure to
name and the build is still correct.  So a `not found` line is a hint, not the error, and grepping
the log for it will mislead you.

What actually stops configure is one of six probes that compile, link or parse real code, in this
order:

1. **The C++17 probe** (`configure.ac:337`, error at `:373`) — a nested-namespace definition and
   `std::optional`, tried with `-std=c++17`, `-std=gnu++17` and `-std=c++1z`.  Failing all three
   stops with *"a C++17 compiler is required…"*.
2. **The `halcompat.h` + Binder header compile probe** (`:413`) — one `#include <halcompat.h>`,
   which proves three of the four include roots at once because that header pulls
   `binder/IServiceManager.h`, `binder/IInterface.h` and `utils/String16.h` in with it.  Its error
   lists the include roots it tried *and* the roots that do not exist, which is where those earlier
   `not found` lines resurface as diagnosis.
3. **The generated-AIDL-header compile probe** (`:438`) — `#include <com/rdk/hal/hdmicec/IHdmiCec.h>`,
   which proves the remaining root and the frozen `common 0.2.0.0` import it pulls in as
   `com/rdk/hal/PropertyValue.h`.
4. **The stub-and-Binder link probe** (`:466`) — it resolves `IHdmiCec::serviceName` from the stub
   library, `defaultServiceManager` from `libbinder` and a `String16` constructor from `libutils`,
   with `libcommon-v0.2.0.0-cpp` named on the same command line so the linker has to locate that file
   too.  This is the one that catches an unreachable `HALIF_LIB_DIR`: it stops at *"the AIDL stub and
   Binder client libraries could not be linked"* and lists the directories it searched.  The probe is
   linked and never run, so it is safe on a host with no Binder driver.
5. **The Binder kernel UAPI header probe** (`:516`, error at `:531`) — `#include
   <linux/android/binder.h>`, which is what supplies `BINDER_VERSION` and
   `BINDER_CURRENT_PROTOCOL_VERSION` to the bounded preflight.  It proves the header is *present and
   parses*; it says nothing about whether the running kernel has the driver, and on this host it
   passes while `/dev/binder` does not exist.
6. **The Binder wire-ABI determination** (`:585` and `:606`, errors at `:644`, `:665` and `:676`) —
   two link probes that ask the staged `libbinder` which wire ABI it was built for, then the three
   arms that stop: the ABI could not be determined, an explicit `BINDER_IPC_32BIT` contradicts what
   was measured, or the value given is not one of the two recognised spellings.  This is the only one
   of the six that can be satisfied by *stating* the answer rather than by fixing the staging, and
   `BINDER_IPC_32BIT` above is how.

Four of the six are pure compile-or-link probes, which is why an earlier revision of this section
counted four; the UAPI header probe and the wire-ABI determination were added later and belong on the
same list, because a reader who greps for the cause of a stopped `configure` needs all six.

## Test Structure

```
tests/L1Tests/
├── Makefile.am           # Autotools build configuration; run_L1Tests_SOURCES is the ONLY
│                         #   gate on what gets compiled, and its order is significant
├── run_coverage.sh       # gcov/lcov runner, per-file table and >=80% line-coverage gate
├── .lcovrc_l1            # lcov configuration with branch collection enabled
├── test_main.cpp         # Test runner entry point; installs the single global
│                         #   testing::Environment that creates the driver mock
├── ccec/                 # CCEC library tests (580: 456 pre-existing + 124 new)
│   ├── test_CECFrame.cpp        # 31 tests - frame construction, accessors, boundary sizes
│   ├── test_Connection.cpp      # 66 tests - listener registration, address filtering,
│   │                            #            plus the 6 cross-layer flow cases
│   ├── test_Bus.cpp             # 37 tests - listener dispatch, filtering, send retries
│   ├── test_LibCCEC.cpp         # 14 tests - init/term, addresses, uninitialised-state guards
│   ├── test_MessageEncoder.cpp  # 27 tests - CEC message encoding
│   ├── test_MessageDecoder.cpp  # 54 tests - all CEC opcodes, edge cases, tracking
│   ├── test_OpCode.cpp          # 82 tests - all opcodes, GetOpName coverage
│   ├── test_Operands.cpp        # 75 tests - all operand types, methods
│   ├── test_Driver_Mock.cpp     # 10 tests - mock driver verification
│   ├── test_Driver.cpp          # 22 tests - open/close, write, address management
│   ├── test_DriverImpl_Async.cpp# 26 tests - async transmit, invalid state, error paths
│   ├── test_Util.cpp            # 12 tests - log-level configuration, buffer dump
│   └── test_DriverAidl.cpp      # 124 tests in 7 fixtures - back-end selection, the
│                                #            compatibility rule, the binder preflight,
│                                #            single-element array marshalling, status
│                                #            translation, the frame-size boundary and the
│                                #            back-end-agnostic contract arms
└── osal/                 # OSAL library tests (27 tests)
    ├── test_ConditionVariable.cpp  # 5 tests - wait/signal/timed wait, deadline
    │                                #           normalisation, native handle
    ├── test_Mutex.cpp              # 11 tests - lock/unlock, copy semantics
    └── test_Thread.cpp             # 11 tests - named construction, dispatch, detach
```

**483 test cases in 18 fixtures**, all passing, none disabled.  The counts above are
measured with `./run_L1Tests --gtest_list_tests`, not estimated; re-measure with that
command rather than trusting this list after adding tests.  Two fixtures come out of
`test_LibCCEC.cpp` (`LibCCECTest`, 8 cases, and `LibCCECUninitializedTest`, 6) and two out of
`test_MessageDecoder.cpp` (`MessageDecoderTest`, 44, and `MessageDecoderTrackingTest`, 10), and
two out of `test_Connection.cpp` (`ConnectionTest`, 60 cases, and `IntegrationFlowTest`, 6 cases
covering the two cross-layer flows COVERAGE_GAPS.md catalogues as `#gap-flow-a` and `#gap-flow-b` - see
the block comment at the end of that file for what they assert and for the plugin-leg boundary
they cannot cross), which is why 18 fixtures come from 15 translation units.

**That figure is the pre-existing inventory, and it is kept as it was measured.**  The 483 and the
18 are the suite as it stood before `ccec/test_DriverAidl.cpp` was added; every one of those cases
is still present, unmodified.  The binary now registers **607 cases in 25 fixtures — 483 + the
contract suite's 124 in 7 fixtures**, measured the same way, with
`./run_L1Tests --gtest_list_tests`.  Where a group total in the tree above has moved it names both
parts rather than presenting a new sum as though it had always been one, and nowhere below is an
older figure silently adjusted upwards to match: where this file quotes 483 it means the
pre-existing inventory, and every one of these numbers is re-measurable with that one command.

Two further translation units are compiled into `run_L1Tests` from outside this directory, and
neither defines a test case of its own, so neither contributes to any of those counts:

- `../../mocks/hdmicec/hdmi_cec_driver_mock.cpp` — the GoogleMock HDMI-CEC HAL driver mock, and the
  last entry in `run_L1Tests_SOURCES`.  That final position is reserved for it by name.
- `../../mocks/hdmicec/fake_hdmi_cec_aidl_service.cpp` — the test-scope fake AIDL service, added
  immediately *before* the mock so the mock keeps that position.  It implements the existing
  `.aidl` interface under the production service name, is built for test targets only, and is
  linked into no production library.  See
  [`CEC_TEST_AIDL_MODE`](#cec_test_aidl_mode-the-only-switch) for what registers it and when.

### Adding a test file

A new translation unit is compiled ONLY if it appears in `run_L1Tests_SOURCES`, so a file
added without amending `Makefile.am` changes nothing and produces no coverage movement.  That
list is the only compile gate: an unregistered test file is silently not compiled, with no
warning and no build error to tell you, which makes it indistinguishable from a test that was
never written.  Place the entry so that it does not reorder the order-fragile suites: GoogleTest
registers each `TEST_F` during static initialisation in link order and runs the suites in
registration order, so inserting a file *ahead* of an existing suite reorders that suite, and
this suite contains pre-existing order-fragile tests that pass only in the established order.
The four translation units added by this pass are grouped with their siblings but placed *after*
every pre-existing entry of their layer — the two `ccec/` files after `ccec/test_Driver.cpp` and
the two `osal/` files after `osal/test_ConditionVariable.cpp` — which keeps the list readable
while leaving the established registration order of every pre-existing suite intact.  Verify any
new placement with a full run before relying on it.

## Back-End Selection and the Invocation Matrix

The middleware compiles **both** HAL back-ends into `libRCEC` — the legacy in-process C API and the
AIDL/Binder one — and `Driver::getInstance()` chooses between them **once per process**, inside
`LibCCEC::init`, which is the first thing this harness does that forces the factory
(`test_main.cpp:298`).  By the time any `TEST_F` body runs the choice is made and cannot be changed.

Two consequences follow, and the whole test design rests on them:

- **One binary run yields exactly one selection outcome**, so every outcome that has to be covered
  needs its own process.  No filter can make a single run exercise both arms of the selection.
- **Anything that is to influence the selection must happen before `init`.**  Registering a fake
  service afterwards leaves the selection on the legacy back-end and produces a green run that
  proves nothing at all about the AIDL path.

**Ordering is therefore load-bearing, not merely tidy.**  On invocations B and C the fake must be
registered, and on invocation E the host must be launched *and* have signalled ready, **before**
`LibCCEC::init` runs.  Both harnesses are built so that this holds by construction rather than by
being "early in `SetUp`", and both fail the run outright if the step they were asked for could not
be completed.

### The five invocations

`./run_coverage.sh --run` drives all five, one process each, in this order.  Each writes its own
`run_invocation_<letter>.log` and `rdkTestResults_invocation_<letter>.json`, and a later invocation
can never overwrite an earlier one's artifact:

| Inv | Binary | `CEC_TEST_AIDL_MODE` | Expected selection | What it runs |
| --- | --- | --- | --- | --- |
| A | `run_L1Tests` | `absent` | Legacy | Everything in the binary except the two AIDL-only contract fixtures — the whole pre-existing 483 plus 85 contract cases, **measured green at 568 from 23 suites** |
| B | `run_L1Tests` | `compatible` | AIDL (local interface) | The contract suite less its legacy-only fixtures (115 cases), plus the 308 back-end-neutral cases — **selects 423 from 15 suites on this tree; measured green at 422 of 422 on the tree that registered 606, before the over-length inbound case was added** |
| C | `run_L1Tests` | `incompatible` | Legacy, rejection logged | The three back-end-independent contract fixtures (76 cases), plus the same 308 — **selects 384 from 13 suites on this tree, and measured green at 384 of 384** |
| D | `run_L2Tests` | `absent` | Legacy | The `DualPath*` tier, end to end through the legacy in-process mock — **measured green: 15 registered, 11 passed, 4 skipped, exit `0`** |
| E | `run_L2Tests` | `remote` | AIDL (remote proxy) | The same `DualPath*` tier over real Binder IPC, inbound delivery included.  Authored and compiled; **never executed on this host or on the committed CI target**, though it has run green in purpose-built Binder-capable guests that are neither — **15 registered, 9 passed, 6 skipped, exit `0`**, the six skips being the legacy-arm cases that skip when the resolved back-end is not theirs.  An earlier run reported 14 registered and 8 passed **before `DualPathHostLifecycleTest` was added**; that case is back-end-independent and mandatory under E as under D, and the 15/9/6 figures above are the measurement that confirmed it rather than an expectation.  See [What invocation E does and does not yet evidence](#what-invocation-e-does-and-does-not-yet-evidence) |

**A run total appears above only where a run produced it.**  Invocation A's is a green run of
exactly that filter — `568 tests from 23 test suites ran`, `568 passed`, exit `0` — cross-read
against `./run_L1Tests --gtest_list_tests` and the FIXTURE MANIFEST in `ccec/test_DriverAidl.cpp`.
Invocation D's is a green run of the L2 tier on this host: `15 tests from 4 test suites ran`,
`11 passed`, `4 skipped`, exit `0`, the four skips being the AIDL-arm cases that skip rather than fail
when the resolved back-end is not theirs.  **B, C and E now carry run totals too, and they were
produced on a Binder-capable guest rather than projected.**  On 2026-09-01 all five invocations ran
in one instrumented i386 guest under the QEMU provisioning that
`.github/workflows/aidl-path-tests.yml` owns — kernel 5.15.148 with
`CONFIG_ANDROID_BINDER_IPC_32BIT=y`, Binder protocol 7 agreeing between the kernel, the SDK build
and the `BINDER_VERSION` ioctl, and `servicemanager` answering — and each passed all four of its own
checks: B `422 passed` from 15 suites with the AIDL back-end resolved — measured on the tree that
registered 606, where B selected 422; this tree selects 423 — C `384 passed` from 13 suites
with the legacy back-end resolved after the incompatible service was rejected, and E `9 passed`,
`6 skipped` of 15 over real Binder IPC.  **No host without a Binder driver can reproduce those three**, so
on such a host `run_coverage.sh` reports them DEFERRED and never as passes; a registration count is
still not a result, and where this file quotes one it says so.  The 308, the 76 and the 85 are
measured suite-group figures and are attributed where they are listed below.

**Those counts are documentation, not the gate.**  `run_coverage.sh` asks the binary itself how many
cases each invocation's own filter selects, every time, and additionally reconciles
`selected + excluded == registered` — so adding a case never requires editing a number here, and a
suite added to the binary and classified into neither group fails the invocation instead of quietly
going unrun.  The **four** per-invocation checks are a zero exit status, that measured count, the
selected-path line in the log, and **an empty process group at close-out** — the L2 harness forks the
fake service host into the invocation's process group, and a survivor would keep the global
`"HdmiCec"` registration, so a later AIDL invocation would resolve against a process from a previous
run; a leaked descendant therefore **fails** the invocation rather than being cleaned up quietly.  A
passing invocation logs *"PASSED all four checks (exit status, case count, selected back-end, and an
empty process group at close-out)"*.

**Invocation A carries a negative filter and it is not optional.**  The two AIDL-only fixtures
assert in `SetUp` that the AIDL back-end is the resolved one and **fail rather than skip** when it
is not — deliberately, because a skipped arm is indistinguishable from a passing one in an
aggregate count.  So A excludes them by name and everything else in the binary runs.

Three further points are worth stating because five invocations look like more than are needed:

- **Invocation C is only feasible in-process.**  `libbinder` resolves a name registered in the
  calling process to the local `BBinder`, so `getInterfaceHash()` and `getInterfaceVersion()`
  dispatch virtually and can be overridden — which is the only way a service can report an
  interface hash of `"-1"`.  A *remote* `Bn*` service cannot report bad metadata at all, because its
  generated `onTransact` answers those transactions from compiled-in constants.  Hence
  "present but not usable falls back" is an L1 behaviour with no L2 counterpart.
- **B and E are both required and neither substitutes for the other.**  An in-process registration
  is not IPC: no `Bp*` proxy is created, no transaction crosses the driver, and the client
  threadpool is never involved.  B is therefore the arm that establishes that the **adapter
  translates** — array marshalling, status mapping, the frame-length guard, the state guards — and E
  is the arm that establishes that the **transport works** and that the client threadpool receives.
  Both are designs, not results: neither has executed, because both need a Binder driver.  What E
  asserts, what drives it and what has and has not been demonstrated are in
  [What invocation E does and does not yet evidence](#what-invocation-e-does-and-does-not-yet-evidence).
- **The tests observe the selected back-end without any production introspection API.**  Nothing
  was added to `Driver.hpp`.  A case asserts identity either by `dynamic_cast` against the concrete
  types — reachable because `ccec/src/DriverAidlImpl.hpp` and `ccec/src/DriverImpl.hpp` are not
  installed headers and are included by relative path, exactly as the existing `ccec/` units reach
  `DriverImpl.hpp` — or by matching the selected-path line the factory logs, which is what L2 and
  device-level validation match on.  That line is the one to grep for in any invocation's log:

```
Driver::getInstance : HDMI CEC HAL back-end selected : legacy
```

### Which cases run under which invocation

`--gtest_filter` **selects by suite name, not by file name**, and several files in this directory
declare two suites, so a filter derived from file names would be wrong in a way nothing would
catch.  The two groups below were read out of the built binary with `--gtest_list_tests`:

- **Back-end-neutral — 10 suites, 308 cases, selected by every applicable *L1* invocation — A, B
  and C — unchanged and unmodified.**  `CECFrameTest` 31, `MessageDecoderTest` 44,
  `MessageDecoderTrackingTest` 10, `MessageEncoderTest` 27, `OpCodeTest` 82, `OperandsTest` 75,
  `UtilTest` 12, `ConditionVariableTest` 5, `MutexTest` 11, `ThreadTest` 11.  They contain no
  reference to either back-end — no mock, no `EXPECT_CALL`, no `Driver::getInstance` — and that is
  the backward-compatibility evidence, because A, B and C between them cover **both** selection
  outcomes: the same cases and the same assertions with the legacy back-end resolved (A, and C after
  its rejection) and with the AIDL back-end resolved (B).
- **Legacy-bound — 8 suites, 175 cases, invocation A only.**  `BusTest` 37, `ConnectionTest` 60,
  `IntegrationFlowTest` 6, `DriverTest` 22, `DriverImplAsyncTest` 26, `HdmiCecDriverMockTest` 10,
  `LibCCECTest` 8, `LibCCECUninitializedTest` 6.

308 + 175 = 483 and 10 + 8 = 18, which is the same inventory the
[Test Structure](#test-structure) counts describe.  **D and E are not in this arithmetic at all**:
they are the L2 tier's invocations, they run a different binary (`run_L2Tests`), and they have their
own 15 cases in 4 fixtures — no case in this directory is registered in them.  So "runs under both
selections" is a claim about A, B and C, and the L2 tier makes its own end-to-end claim separately;
see [The L2 tier](#the-l2-tier-two-programs-two-processes).

**Why the 175 cannot simply be filtered into the AIDL arms**, since "run everything on both paths"
is the obvious thing to want:

- Their `EXPECT_CALL`s name `HdmiCec*` functions a Binder back-end never calls, so under AIDL
  selection they would fail on unmet expectations rather than on behaviour — a false negative, and
  evidence of nothing.
- `ccec/test_DriverImpl_Async.cpp` and `ccec/test_Driver.cpp` assert that asynchronous transmit
  *succeeds*, where the AIDL back-end is required not to support it at all.
- `ccec/test_Connection.cpp:74` and `ccec/test_LibCCEC.cpp:178` install and invoke
  `DriverImpl::DriverReceiveCallback`, whose `static_cast<DriverImpl &>` is **ill-typed against an
  AIDL singleton — undefined behaviour, not a failed assertion.**  Restricting those suites to
  invocation A makes that route structurally unreachable under AIDL selection rather than merely
  unused.

Their behavioural content is not dropped: it is re-asserted back-end-agnostically by
`ccec/test_DriverAidl.cpp`, whose 7 fixtures — 124 cases, measured with `--gtest_list_tests` — are
partitioned by the back-end each needs.  `DriverAidlCompatibilityTest` 23,
`DriverAidlPreflightTest` 28 and `DriverAidlLocalInstanceTest` 25 under A, B and C;
`DriverAidlSelectionTest` 4 and `DriverAidlLegacyArmTest` 5 under A only;
`DriverAidlSessionTest` 27 and `DriverAidlTransmitTest` 12 under B only.  So 85 of the 124 run under
invocation A — 76 + 9 — which with the 483 pre-existing cases is A's measured 568.  That file's
FIXTURE MANIFEST is the authority for its own case set.

**No existing test unit was modified to make any of this work.**  The split is expressed *only*
through `--gtest_filter` arguments held in `run_coverage.sh`, which is what leaves the
ORDER-IS-SIGNIFICANT contract in `Makefile.am:112-131` — and every established suite's
registration order — exactly as it was.

### `CEC_TEST_AIDL_MODE`: the only switch

One environment variable distinguishes one invocation from the next.  It is read by
`tests/L1Tests/test_main.cpp` and `tests/L2Tests/test_main.cpp` and **by no production source**:

| Value | Effect |
| --- | --- |
| `absent` | Register nothing.  The lookup finds no service, the legacy back-end is selected, and libbinder is not touched at all.  **Unset and empty both mean this**, so the *selection* a plain `./run_L1Tests` makes is exactly the one it made before the AIDL back-end existed — but the binary is not the same binary, and a plain run now fails on the 39 AIDL-only cases, which is why invocation A carries a filter |
| `compatible` | Register an in-process fake reporting its real, frozen metadata.  The AIDL back-end is selected |
| `incompatible` | Register an in-process fake whose interface hash is `"-1"`.  The service is *present* and rejected, so the legacy back-end is selected and the rejection is logged |
| `remote` | Launch the out-of-process fake service host.  `run_L2Tests` only — in `run_L1Tests` it is a hard failure naming the other runner |

Three rules go with it, and each exists to stop a green run that proves nothing:

- **An unrecognised value is a hard failure, never a quiet fall back to `absent`.**  A typo that
  downgraded the run to the legacy path would report a green result for an AIDL invocation that
  never happened.
- **A service already registered under the production service name when a run starts is a hard
  failure**, not a condition to work around: the outcome would then depend on a stale process.
  Nothing is ever deregistered either, because the pinned C++ `IServiceManager` exposes no
  service-removal API — so a leftover registration is reported, not cleaned up.
- **The harness does not start the Binder client threadpool.**  `DriverAidlImpl::open()` owns that,
  and it runs inside `init`.  Starting one here would duplicate an ownership production code
  already holds.
- **An unrecognised value is reported back ESCAPED, BOUNDED and never at the start of a line.**  The
  diagnostic renders the value rather than echoing it: a literal backslash becomes `\\`, a newline
  `\n`, a carriage return `\r`, a tab `\t`, and any other byte outside printable ASCII `\xNN`,
  with the whole thing cut to 200 rendered characters and `...[truncated, N bytes total]` appended.
  So `CEC_TEST_AIDL_MODE=$'bogus\n::error::FORGED'` produces one line naming
  `"bogus\n::error::FORGED"` inside the harness's own message instead of a standalone
  `::error::` line a CI log would read as an annotation of its own, and a 5000-byte value costs
  about 230 bytes of log rather than 5000.  The same renderer is applied to
  `CEC_FAKE_AIDL_HOST_PATH`, to the fake host's three descriptor variables, and to the equivalent
  values in `run_coverage.sh` and the rootfs image builder.

### The L2 tier: two programs, two processes

`tests/L2Tests` registers **four** fixtures.  Three are the tier's integration arms —
`DualPathSelectionTest`, `DualPathLegacyFlowTest` and `DualPathAidlFlowTest`.  The fourth,
**`DualPathHostLifecycleTest`**, exercises the harness itself rather than a back-end: it forks a
child that leads its own process group, gives it a grandchild that **ignores `SIGTERM`**, and
requires the harness's own terminate-and-reap to end the whole group and to have swept every
descriptor the child was never told about.  It needs no Binder driver, no service manager and no
host, so it runs and means the same thing under every invocation.  It carries the `DualPath` prefix
because `run_coverage.sh` drives this tier with the single filter `DualPath*` and reconciles
`selected + excluded == registered`; a fixture named anything else would be registered, selected by
nothing and excluded by nothing, and would fail that reconciliation.

`tests/L2Tests` builds two `noinst_PROGRAMS`, and they must be separate processes:

- **`run_L2Tests`** — the runner, with `TESTS = run_L2Tests`.  It also compiles the legacy HAL mock,
  because invocation D drives the legacy back-end through it and without it that arm has no HAL.
- **`fake_hdmi_cec_aidl_host`** — the fake service host.  It gets the fake and nothing else: no
  `test_main.cpp`, no integration case, no `libRCEC`, no GoogleTest.  It must not contain the
  middleware whose behaviour the runner is measuring, or the two processes would no longer be
  measuring anything across a boundary.

An in-process registration produces no proxy, no driver transaction and no client-threadpool
involvement, which is precisely why the host is a separate binary.  Two variables carry the launch
handoff, and two more carry the control channel described in
[What invocation E does and does not yet evidence](#what-invocation-e-does-and-does-not-yet-evidence):

- **`CEC_FAKE_AIDL_HOST_PATH`** — where the L2 harness finds the host binary.  Set by the build for
  both L2 invocations; only mode `remote` reads it.
- **`CEC_FAKE_HOST_READY_FD`** — the number of an **inherited** descriptor, the write end of a pipe
  whose read end the waiting parent holds.  The harness creates the pipe, forks, sets this in the
  child's environment and blocks on the read end under a bounded timeout.  Do **not** set it
  yourself: it would name a descriptor the setter never opened, and the host treats an unusable
  descriptor as a hard failure with an exit code of its own rather than answering on stdout.  Left
  unset, the host falls back to standard output, which is what makes it runnable by hand for
  debugging.

The host writes the exact line `FAKE_HDMI_CEC_AIDL_HOST_READY` once, after it has published the
service and started its own **service-side** threadpool — a separate obligation from the
client-side pool the middleware starts — and then waits for `SIGTERM` or `SIGINT`, both handled so
that the parent's teardown produces a clean exit rather than a default-disposition kill.  The
parent terminates and reaps it in `TearDown`.

### What invocation E does and does not yet evidence

Invocation E is **authored, compiled and — since 2026-09-01 — executed on a Binder-capable guest,
and it has never executed on this host or on the committed CI target**.  That qualification is the
whole of the claim rather than a hedge on it.  All five invocations ran green against a real Binder
transport in purpose-built Binder-capable QEMU guests, with B and E selecting the AIDL back-end, and
running them found and fixed three latent test defects that no static check could have surfaced.
One of those guests was **neither this host nor** the deliberately all-32-bit protocol-7 i386 guest
`.github/workflows/aidl-path-tests.yml` targets — it was protocol-8 x86-64 under TCG — and it was
built for functional execution rather than coverage capture, so it establishes that the transport
and the inbound chain work and establishes nothing about the deferred branch arms.  The committed
workflow itself has never run as a GitHub Actions job, so its published run count is zero.  What
invocation E evidences and what still depends on the runner are separate things, so this section
states each without leaning on the other.

**The two-process split is necessary and it is not sufficient.**  Hosting the fake in a separate
process is what makes the middleware hold a real `Bp*` proxy and what makes an event callback arrive
on a binder threadpool thread *at all*.  It does not by itself cause one: after the split, the only
object that can invoke this process's listener lives on the far side of the boundary, so nothing in
the runner can produce an inbound frame on its own.

**What drives one is a test-only control channel, and it is ordinary pipes rather than anything
AIDL.**  The host inherits two descriptors named by `CEC_FAKE_HOST_CONTROL_FD` — the **read** end
of the pipe the harness sends commands down — and `CEC_FAKE_HOST_OBSERVE_FD` — the **write** end of
a second pipe the host sends replies back up.  They are **two separate unidirectional pipes, not a
`socketpair`**; nothing in either the harness or the host creates a socket pair at all.  Both are
set by the harness immediately before it execs the host, exactly like `CEC_FAKE_HOST_READY_FD`, and
neither is something to set by hand.

**Eight verbs, all lowercase, one line each:**

| Verb | Reply on success | What it is for |
|---|---|---|
| `ping` | `OK pong` | Prove the channel is live before the run depends on it |
| `deliver <hex>` | `OK delivered <n>` | Invoke `onMessageReceived` on the listener captured during `open()` |
| `sent-count` | `OK sent-count <n>` | How many `sendMessage` transactions the fake controller has taken |
| `last-sent` | `OK last-sent <hex>` | The bytes of the most recent one |
| `open-count` | `OK open-count <n>` | How many times `IHdmiCec::open` has been served |
| `close-count` | `OK close-count <n>` | How many times `IHdmiCec::close` has been served |
| `listener` | `OK listener …` | Whether a listener is currently captured |
| `shutdown` | `OK shutdown` | Ask the host to exit |

`deliver` is the one that matters here: out of process that callback crosses the Binder driver in
the client's direction, so it runs on the runner's own binder threadpool.  **Every reply line begins
`OK ` or `ERR `** — there is no bare `PONG`, `DELIVERED` or `NOLISTENER` — and the `ERR` forms are
named rather than generic: `ERR no-listener` when the fake had none captured, `ERR bad-hex` for an
unparseable payload, `ERR bad-args <verb>` for the wrong arity, `ERR no-controller` when no session
is open.  So a refused transport cannot be read as a delivered frame.  A reply is awaited for at
most **10 s** and is at most `PIPE_BUF` (4096) bytes, which is the size up to which a pipe write is
atomic — the cap is what makes a reply arrive whole or not at all rather than interleaved.  A
mis-wired channel exits the host **8**, and a channel that breaks mid-run exits it **9**, each
distinct from the readiness failure so an exit status says which handoff was botched.  No AIDL
method and no binder object is added for any of this: it is two test processes talking over pipes.

**What is asserted.**  Two of the L2 tier's cases assert the inbound chain end to end — the
callback, the state-guarded receive queue, the `Bus` reader thread, `Connection`'s address filter,
the decoder and the typed `MessageProcessor::process` overload — and the closed-state rejection,
that a frame delivered while the driver is not `OPENED` is refused at the guard.  A `oneway`
callback returns as soon as the transaction is handed to the driver, so each case pairs the delivery
with a bounded predicate wait rather than assuming the frame has already arrived.

**What has been demonstrated, exactly.**  All of the above has now run.  On 2026-09-01 invocation E
executed on a Binder-capable i386 guest provisioned by `.github/workflows/aidl-path-tests.yml` and
reported `14 tests`, `8 passed`, `6 skipped`, exit `0`, and the acceptance runs of 2026-09-02
re-measured it at `15 tests`, `9 passed`, `6 skipped`, exit `0` once
`DualPathHostLifecycleTest` was registered — the six skips being the legacy-arm cases,
which skip rather than fail when the resolved back-end is not theirs, the mirror of what the AIDL
arm does under D.  All four `DualPathAidlFlowTest` cases passed:
`OutboundImageViewOnCrossesRealBinderIpcToTheFakeService` (13 ms),
`OutboundActiveSourceWithOperandsCrossesRealBinderIpc` (5 ms),
`InboundFrameFromTheFakeServiceArrivesOnABinderThreadAndReachesTheTypedProcessor` (10 ms) and
`AFrameDeliveredWhileTheDriverIsNotOpenedIsRejectedByTheStateGuard` (1320 ms, the bounded
non-delivery window).  So the inbound chain has been observed end to end with the callback arriving
on the runner's own binder threadpool thread, and the closed-state rejection has been observed
refusing a frame at the guard.

**What that does and does not license.**  It licenses "E has delivered callbacks over real Binder
IPC, measured".  It does not license running E on a host without a Binder driver: there the
invocation is not attempted at all and `run_coverage.sh` reports it DEFERRED, because
`defaultServiceManager()` aborts the process when the driver node is missing.  Under invocation D on
such a host those same four AIDL-arm cases report `SKIPPED` — also measured — because the resolved
back-end is not theirs.  Reproducing E is therefore a property of the runner, not of the tree.

### Which invocations this host can run

Compiling and linking the AIDL back-end needs only headers and libraries, so **the build
integration is verifiable anywhere**.  Executing an AIDL-path case needs three more things: a
kernel with Binder support, a Binder protocol version matching the one `libbinder` was built for,
and a running `servicemanager`.

Measured on the host this file was last revised on: **kernel 6.12.85+, `CONFIG_ANDROID_BINDER_IPC`
not set, `binder` absent from `/proc/filesystems`, no `/dev/binder*`.**  So on a host like it:

- **A and D are the two that need no Binder transport**, so those are the two a driverless host can
  run — A the whole L1 regression arm, D the L2 round trip through the in-process legacy mock.
- **B, C and E cannot run at all** there, and not merely because a lookup would fail.  Registering a
  name requires `defaultServiceManager()`, which opens the driver, and on the pinned Binder stack
  that call **aborts the process** when the node is missing.  So an in-process fake is as
  unreachable as a remote one, and an attempt would take the whole runner down with it — along with
  the counters the invocations that *can* run have already accumulated.
- `run_coverage.sh` therefore reports an invocation it cannot run as **DEFERRED** and makes the
  verdict advisory.  A deferred invocation is never attempted, never counted as passed and never
  allowed to stand in for evidence it did not produce, and a run that deferred any of them cannot
  produce an acceptance verdict.  The probe is the driver node itself, overridable with
  `BINDER_DRIVER_NODE` and defaulting to `/dev/binder`.

**No result for B, C or E is claimed in this file.**  They belong to the Binder-capable job,
`.github/workflows/aidl-path-tests.yml`, whose whole reason to exist is that it can run the
complete matrix on one instrumented build on one machine.  The hosted
`.github/workflows/L1-tests.yml` job builds everything — both back-ends, both test tiers and the
fake host — and runs **invocation A only**, because a hosted GitHub runner has no Binder driver
and no `servicemanager`; its green tick is therefore regression evidence and not dual-path
evidence, and it should not be read as the latter.  Invocation A's passing on a driverless runner
is itself the demonstration that a platform without the AIDL HAL reaches the legacy back-end
instead of aborting.

## Running Tests

Run from **this directory**, and run the `run_L1Tests` that sits here — it is the libtool wrapper
script, and it sets the library search path the real binary under `.libs/` needs.  Invoking
`.libs/run_L1Tests` directly is the usual cause of a loader error that looks like a build failure.

### The one command for running this suite

**Invocation A is how this suite is run by hand, and the negative filter in it is not
optional.**  Every recipe in this file, in `QUICK_START.md` and in `run_coverage.sh`'s matrix is
this same command:

```bash
# From tests/L1Tests -- THE invocation-A command
CEC_TEST_AIDL_MODE=absent \
GTEST_FILTER="-DriverAidlSessionTest.*:DriverAidlTransmitTest.*" \
  ./run_L1Tests
```

Measured on this tree: `568 tests from 23 test suites ran`, `568 passed`, exit `0`.

Two halves, and neither is decoration.  `CEC_TEST_AIDL_MODE=absent` is the *default* rather than a
change — unset and empty both mean `absent` — and it is spelled out so the command names the
invocation it is instead of depending on a variable being unset.  **The filter is the half that is
mandatory.**  The binary registers 607 cases, and the two AIDL-only fixtures among them —
`DriverAidlSessionTest` 27 and `DriverAidlTransmitTest` 12, 39 cases — assert in `SetUp` that the
AIDL back-end is the resolved one and **fail rather than skip** when it is not.  So a bare
`./run_L1Tests` runs all 607 and exits `1` with exactly those 39 failures; that is measured, and it
is deliberate, because a skipped arm is indistinguishable from a passing one in an aggregate count.
`--gtest_filter='-DriverAidlSessionTest.*:DriverAidlTransmitTest.*'` is the same filter spelled as a
flag and behaves identically (measured: 568 / 568, exit `0`); the environment spelling is the one
used throughout because it survives being handed through `make` and through the coverage runner.

Root `make check` does **not** run this suite.  The top-level `Makefile.am` declares
`SUBDIRS = osal ccec`, so a `check` at the top level never descends into `tests/`.

**It does not quietly omit the recursion either — it fails.**  There is no `check` target at the
configured root at all, so the command stops with an error rather than succeeding with nothing to
do.  Measured at the configured submodule root:

```text
$ make check
make: *** No rule to make target 'check'.  Stop.
$ echo $?
2
```

Exit **2** is `make`'s "I was asked for something that does not exist", and it is the correct
outcome rather than a broken build — the root recurses into `osal` and `ccec`, neither of which
declares a `TESTS`.  Drive each test tier from **its own** directory: `make -C tests/L1Tests check`
for the L1 suite and `make -C tests/L2Tests check` for the L2 tier, or `cd` into either and run a
bare `make check` there.
`make -C tests/L1Tests check` from the submodule root does drive it — this directory's `Makefile.am`
declares `TESTS = run_L1Tests` — but it runs the binary with whatever environment it inherits and
sets no filter of its own, so the invocation-A environment has to be exported first or the check
fails on the same 39 cases:

```bash
# From the hdmicec submodule root
export CEC_TEST_AIDL_MODE=absent
export GTEST_FILTER='-DriverAidlSessionTest.*:DriverAidlTransmitTest.*'
make -C tests/L1Tests check
```

Everything else worth driving by hand.  A command that runs the **whole** binary carries the
invocation-A environment; a command that selects no `DriverAidl*` fixture, or that executes nothing
at all, needs no filter and says so:

```bash
# Run specific tests.  Neither of these filters selects a DriverAidl* fixture, so neither
# needs an exclusion of its own -- measured 31 / 31 and 10 / 10 twice, exit 0
./run_L1Tests --gtest_filter="CECFrameTest.*"
./run_L1Tests --gtest_filter='HdmiCecDriverMockTest.*:YourFixture.*' --gtest_repeat=2
#   ^ the second is the deterministic, seed-free check that a new test does not depend on the
#     mock's leftover callback registration (see Test Order Dependence)

# List all tests -- registration only, nothing is executed, so no filter is needed
./run_L1Tests --gtest_list_tests

# Per-test output and timings (there is no --gtest_verbose flag; see the note below)
CEC_TEST_AIDL_MODE=absent GTEST_FILTER='-DriverAidlSessionTest.*:DriverAidlTransmitTest.*' \
  ./run_L1Tests --gtest_print_time=1 --gtest_brief=0

# XML report.  Measured: the root element reports tests="568" failures="0" disabled="0"
CEC_TEST_AIDL_MODE=absent GTEST_FILTER='-DriverAidlSessionTest.*:DriverAidlTransmitTest.*' \
  ./run_L1Tests --gtest_output=xml:test_results.xml

# Diagnostic only: expose inter-test order dependencies (see Known Issues).  Seed 2 reproduced
# the two known legacy failures in every run of the historical sweep, which is why it is the seed
# spelled here.  No per-seed figure survives an inventory change, so re-measure rather than quote
# one: seed 12345 was green 7 runs in 10 on the pre-contract-suite 483-case inventory, then exited 1
# on a fixed ConnectionTest pair 7 runs out of 7 on the 567-case selection that preceded the
# logical-address registry guard, and is green 7 runs out of 7 on the 568-case selection this
# command runs today (measured; see Test Order Dependence).  Without the filter the 39 AIDL-only
# failures bury anything you are looking for
CEC_TEST_AIDL_MODE=absent GTEST_FILTER='-DriverAidlSessionTest.*:DriverAidlTransmitTest.*' \
  ./run_L1Tests --gtest_shuffle --gtest_random_seed=2

# All five invocations, one process each, then ONE coverage capture over all of them.  It
# orchestrates A through E and DEFERS the ones this host cannot support -- on a host with no
# Binder driver that means A and D run and B, C and E are deferred
./run_coverage.sh --run
```

Invocation A is the only one of the five drivable by hand from *this* directory; D is the L2
runner's own legacy arm and is driven from `tests/L2Tests`, and B, C and E need a Binder-capable
host.  Every invocation's mode, filter, expected back-end and artifact names live in
`run_coverage.sh`'s invocation matrix rather than in a command you compose — see
[Back-End Selection and the Invocation Matrix](#back-end-selection-and-the-invocation-matrix).

> `--gtest_verbose` is **not** a GoogleTest flag.  Passing it makes `run_L1Tests` print its help
> text, run **zero** tests and still exit `0` — a silently green no-op.  The two real flags are
> `--gtest_print_time=0|1` and `--gtest_brief=0|1`; `./run_L1Tests --help` is the authority.

## Writing New Tests

1. Create test file in appropriate directory (ccec/ or osal/)
2. Add to `run_L1Tests_SOURCES` in Makefile.am — **mandatory, and the step that is easy to
   miss.**  See [Adding a test file](#adding-a-test-file) for why, and for why the entry must be
   appended rather than inserted.
3. Use Google Test macros:

```cpp
#include <gtest/gtest.h>
#include "your_header.hpp"

class YourTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup before each test
    }
    
    void TearDown() override {
        // Cleanup after each test
    }
};

TEST_F(YourTest, TestName) {
    EXPECT_EQ(actual, expected);
    ASSERT_TRUE(condition);
}
```

Then, before you call it done:

- **Confirm the file was actually picked up.**  `./run_L1Tests --gtest_list_tests` must show the
  new cases, and the total must rise by exactly the number you added.  That check is what catches
  a missed step 2.
- **Make the test self-sufficient.**  Establish its own preconditions and leave shared state — the
  LibCCEC singleton, the driver mock's expectations, any file it writes — exactly as it found it.
  GoogleTest isolates fixture state only, never process-global or filesystem state, and test
  execution order is undefined.  Verify the new case both in isolation
  (`--gtest_filter=YourTest.TestName`) and in the full suite — the full-suite run being the
  invocation-A command in
  [The one command for running this suite](#the-one-command-for-running-this-suite), not a bare
  `./run_L1Tests`.

## Test Categories

- **Unit Tests**: Test individual classes/functions in isolation
- **Driver-dependent tests**: exercised through the GoogleMock HDMI-CEC HAL driver mock in
  `../../mocks/hdmicec/`, which is registered in `run_L1Tests_SOURCES` and injected by
  symlinking `mocks/hdmicec/hdmi_cec_driver.h` over `stubs/ccec/drivers/hdmi_cec_driver.h`.
  No test requires real CEC hardware, and none is skipped for the want of it.
- **Disabled tests**: there are none.  No test name in `ccec/` or `osal/` carries GoogleTest's
  disable prefix, all 607 registered cases are enabled, and
  `./run_L1Tests --gtest_also_run_disabled_tests --gtest_list_tests` lists the same 607 as a
  plain listing.  See [Known Issues](#known-issues-and-notes).

## Common Assertions

```cpp
EXPECT_EQ(val1, val2)     // val1 == val2
EXPECT_NE(val1, val2)     // val1 != val2
EXPECT_LT(val1, val2)     // val1 < val2
EXPECT_GT(val1, val2)     // val1 > val2
EXPECT_TRUE(condition)    // condition is true
EXPECT_FALSE(condition)   // condition is false
EXPECT_NO_THROW({code})   // code doesn't throw
EXPECT_THROW({code}, ex)  // code throws exception ex
```

## Test Coverage Details

### CCEC Library Tests (580 tests — 456 pre-existing, plus the contract suite's 124)

- **test_CECFrame.cpp** (31 tests): Frame construction, copy operations, serialization, hex dump, boundary sizes
- **test_Connection.cpp** (66 tests): Connection lifecycle, open/close, listener registration, address filtering, broadcast handling, and the `IntegrationFlowTest` cross-layer flow cases (HAL Rx callback through to the typed `process()` overload, and a typed message through to the exact bytes the HAL is handed)
- **test_Bus.cpp** (37 tests): Listener dispatch and filtering, send retries, timeout behaviour
- **test_LibCCEC.cpp** (14 tests): Singleton pattern, initialization/termination, logical/physical addresses, and the guards that throw when the library is not initialised. Split across two fixtures: `LibCCECTest` (8 cases) runs against the initialised library and never terminates it, and `LibCCECUninitializedTest` (6 cases) owns the library's lifecycle per case - `SetUp()` brings it to the uninitialised state through `ensureTerminated()`, which waits for the bus reader to publish RUNNING before calling `term()`, and `TearDown()` restores the initialised `CEC_TEST` state through `ensureInitialized()` - so no other fixture ever observes an uninitialised library
- **test_MessageEncoder.cpp** (27 tests): Encoding CEC messages through MessageEncoder
- **test_MessageDecoder.cpp** (54 tests): Decoding all 60+ CEC opcodes, polling messages, edge cases, opcode tracking; split across the `MessageDecoderTest` and `MessageDecoderTrackingTest` fixtures
- **test_OpCode.cpp** (82 tests): Complete GetOpName() coverage for all CEC opcodes, OpCode class methods
- **test_Operands.cpp** (75 tests): All operand classes (PhysicalAddress, LogicalAddress, DeviceType, Version, PowerStatus, AbortReason, OSDString, OSDName, Language, VendorID, UICommand, SystemAudioStatus, AudioStatus, RequestAudioFormat, ShortAudioDescriptor, AllDeviceTypes, RcProfile, DeviceFeatures, LatencyInfo)
- **test_Driver.cpp** (22 tests) / **test_Driver_Mock.cpp** (10 tests): Open/close and reopen, synchronous write including NACK and transmit-failure returns, address management, frame-detail printing, asynchronous write with both its success and failure returns, and direct mock verification
- **test_DriverImpl_Async.cpp** (26 tests): Asynchronous transmit, the transmit-completion callback, invalid-state guards, error-injection paths
- **test_DriverAidl.cpp** (124 tests in 7 fixtures): The AIDL/Binder back-end, the runtime selection between it and the legacy back-end, and the compatibility rule the selection rests on.  Three routes, because no single route reaches all of it: the compatibility predicate and the Binder preflight by direct call against locally constructed doubles, which need no registered service and no Binder driver; the back-end's closed-state behaviour on a *local* `DriverAidlImpl` instance, whose constructor touches no Binder at all, which is what makes every `status != OPENED` guard and the `writeAsync` prelude ordering reachable without a HAL of any kind; and the back-end actually resolved for the process, through `Driver::getInstance()`.  Fixtures are partitioned by the back-end each requires and assert that precondition in `SetUp` rather than adapting to whatever they find, which is why the file is spread across the invocation matrix instead of running as one block — see [Which cases run under which invocation](#which-cases-run-under-which-invocation)
- **test_Util.cpp** (12 tests): Log-level configuration parsing and the debug buffer dump.  Production `check_cec_log_status()` hardcodes `fopen("/tmp/cec_log_enabled")`, so this suite cannot be pointed at a private temporary without a production change.  One guard therefore owns every access to that fixed path: it is classified with `lstat` and refused unless it is absent or a regular file this process owns, reads open `O_RDONLY|O_NOFOLLOW|O_CLOEXEC`, and a write goes to a fresh `O_EXCL|O_NOFOLLOW` temporary in the same directory that is `rename()`d over the path with the original mode, owner and group reapplied — so a planted link is replaced rather than written through.  Because the path is fixed, the guard also takes an advisory `flock` on `/tmp/cec_log_enabled.testlock` **before** it captures anything and releases it only after it has restored, which makes the capture-mutate-restore window exclusive against another copy of this suite on the same host; a lock it cannot obtain is a hard failure rather than a warning, because proceeding without it is precisely the race.  Restoration runs on ordinary control flow only — `TearDown()` plus one `atexit` backstop, and **no signal handlers**.  The case bodies run in this process: `cec_log_level` is a non-atomic file-static with internal linkage that `check_cec_log_status()` writes and `CCEC_LOG()`/`dump_buffer()` read, and that residual exposure is stated rather than engineered around, because the fix — make the level atomic, or expose a seam for setting and reading it — is a production change and is reported as a blocked gap.  The file header carries the full rationale, including why an earlier forked-child design was withdrawn: `fork()` in a process that already has the Bus reader and writer threads left the child holding their locks and then ran GoogleTest, stdio and libgcov inside it, which bought a formal data race and paid for it with a deadlock

### OSAL Library Tests (27 tests)

- **test_ConditionVariable.cpp** (5 tests): Notify/wait synchronization patterns, timed wait and
  signalling before the timeout, the absolute-deadline computation when the requested wait pushes
  the nanosecond field past one second (which must carry into `tv_sec` rather than be handed to
  `pthread_cond_timedwait` out of range), and the native-handle accessor
- **test_Mutex.cpp** (11 tests): Lock/unlock including the recursive case, native-handle retrieval and its usability checked with `pthread_mutex_trylock` through that handle, copy construction and copy assignment.  `Mutex` exposes no try-lock operation of its own — its API is `lock()`, `unlock()` and `getNativeHandle()` — so none is tested
- **test_Thread.cpp** (11 tests): Named construction, dispatch through Runnable, detach, destruction

## Coverage

Two files in this directory carry the coverage setup:

- **`run_coverage.sh`** — drives the invocation matrix, captures gcov data once over all of it,
  writes an HTML report, prints a per-file table of line, function and branch figures read out of
  the lcov trace, enumerates every file below the
  bar together with its uncovered line numbers, and **exits non-zero when line coverage misses
  the threshold**.  It reproduces the capture, filter and report recipe of
  `.github/workflows/L1-tests.yml` — including that workflow's exclusion globs verbatim, which is
  what keeps the coverage denominator production-source-only — and adds the two things the
  workflow lacks: branch data and a numeric gate.
**`run_coverage.sh` REFUSES TO RUN when a direct-object loader variable is set**, and that
includes `--help`, `--selftest` and `--restore`.  The named set is `LD_PRELOAD`, `LD_AUDIT`,
`LD_PROFILE`, `LD_DYNAMIC_WEAK` and `LD_ORIGIN_PATH`; the check runs at load time, before the script
resolves its own path, so nothing it launches can have loaded an object first.  The reason it is a
refusal and not a scrub: these do not name directories to search, so the runner's careful rebuild of
the loader search path has no effect on them, and every child of a coverage run — `lcov`, `genhtml`,
`gcov`, `awk`, `git`, `make`, both test binaries and the fake service host — would load and run the
named object before its own `main`.  The figures, the per-invocation pass counts and the gate verdict
would then have been produced by processes something unreviewed had already modified.  `--selftest`
is refused for the same reason its result is quoted as evidence, and `--restore` because it writes
six git-tracked Makefiles.  The remedy is printed with the refusal:

```bash
env -u LD_PRELOAD ./run_coverage.sh --build --run     # or -u LD_AUDIT, etc.
```

Note that **empty is not absent** for all of them: glibc activates `LD_DYNAMIC_WEAK` on presence and
ignores its value, so the guard treats a set-but-empty variable as a warning and `unset`s it rather
than accepting it as harmless.  The same five are `unset` from every child environment as well, at
each launch site, so a future helper that sets one cannot reach a launched binary.  What this does
**not** cover, stated so it is not assumed: a literal `DT_RPATH`/`DT_RUNPATH` inside an artifact,
`/etc/ld.so.conf`, `/etc/ld.so.preload` and the loader cache, and the contents of a file that
legitimately sits inside an admitted library root.

- **`.lcovrc_l1`** — the LCOV configuration for this suite, with `lcov_branch_coverage = 1` and
  `lcov_function_coverage = 1`.  Stated plainly: **CI does not read this file.**  The workflow
  passes neither `--config-file` nor `--rc`, so the CI coverage step inherits the system default
  and middleware branch visibility is developer-opt-in.  `run_coverage.sh` is the in-tree consumer;
  it passes this file to `lcov` and `genhtml` and additionally forces `--rc branch_coverage=1`.

### One build, one capture, two consumers

Three properties of the measurement follow from the back-end selection resolving once per process,
and they are worth having in front of you before reading a coverage figure from this suite:

- **All five invocations run against one build on one machine, and the counters are zeroed once
  before the first and captured once after the last.**  gcov counters accumulate, so accumulation
  across the invocations is the *only* way both arms of the selection branch are ever covered — a
  per-invocation capture would show each arm's own half and never the whole.  It also means a
  figure produced on a host that could only run some of the invocations is not comparable with one
  produced where all five ran.
- **The branch gate reads the UNFILTERED capture** (`coverage.info`).  Some of the branches that
  have to be accounted for live in header-only code outside this submodule — above all in
  `halcompat.h`'s `isCompatible<>()`, whose empty-hash, `"-1"`, `"notfrozen"`, era and major arms
  are exactly what the selection rests on.  Filter first and those records are gone, and a branch
  gate that cannot see its own subject passes silently and for ever.
- **The line gate reads a derivative** (`line_gate_coverage.info`) — `filtered_coverage.info` with
  the dependency headers removed, and only them.  Its threshold is an aggregate, and an aggregate
  means nothing without a stable denominator.  **`halcompat.h` is retained** in that derivative: it
  is a consumed HALIF header called from the selection path, not toolchain code.
  `ccec/src/Driver.cpp` and `ccec/src/DriverAidlImpl.cpp` are never filtered out of either form,
  and their presence is asserted after filtering rather than inferred from the globs not naming
  them.  CI's seven exclusion globs are untouched, so `filtered_coverage.info` still means exactly
  what it always meant.

The per-branch gate ships **fully populated — 92 records: 89 required arms, every one with a real
gcov coordinate read out of a real trace, plus 3 recorded unreachable by construction with their
proof.**
A coordinate is a property of the *compiled* source rather than of execution: gcov writes a
`BRDA:<line>,<block>,<branch>,<taken>` record for every branch in an instrumented file that was
loaded, with `0` for an arc its block reached and did not take and `-` for an arc whose block never
ran.  All three mapped files — `ccec/src/Driver.cpp`, `ccec/src/DriverAidlImpl.cpp` and
`halcompat.h` — are compiled into `libRCEC` and the L1 runner on any host, so every coordinate was
derivable here and was derived here.  An earlier revision shipped them empty on the reasoning that a
full five-invocation trace was needed first; that reasoning confused the coordinate with the count.

**A coordinate is a property of the compiled source, and that means the target it was compiled for.**
Two records carry per-arc-layout alternatives instead of a bare arc index, written
`<blockArcCount>:<branch>` and comma-separated — for example `2:0,3:2`, read as "arc 0 where the line
has two block arcs, arc 2 where it has three".  They exist because gcc expands the inlined
`std::string` comparison on `halcompat.h:168` into **three** block arcs on x86-64 and **two** on i386,
so a manifest holding one set reports a false `ABSENT` on the other architecture — and `ABSENT` is the
signal reserved for a branch deleted from the source.  Both spellings were observed failing that way,
one per architecture, before the alternates were introduced.  The gate resolves an alternate against
the arc count in the very trace it is judging, so the selector can never disagree with the trace and
nothing has to be passed in from outside; a layout no record anticipates matches no key and is
reported `ABSENT`, which asks for a measurement rather than crediting the nearest arc.  Measured
across both targets: **312 of the 316** branch-carrying lines in the three mapped files emit an
identical layout, and only `halcompat.h:168` both diverges *and* carries mapped records.

What execution *does* decide is **takenness**, so the gate separates the two and enforces each as
far as it is knowable:

- **Presence is enforced for all 89 required arms, on any host.**  A mapped arm missing from
  the trace is a failure — that is the condition that catches a refactor silently deleting an arm,
  or a coordinate gone stale because a line moved above it.  Measured here: **0 absent**.
- **Takenness is enforced for every arm whose reacher actually ran.**  On a driverless host that is
  **59** of the 89, derived per record rather than hardcoded, and all 59 come back taken — with 0
  never taken and 0 left unchecked for want of coordinates.
- **The remaining 30 are reported DEFERRED with the missing resource named per arm.**  Deferred is
  **not** a pass: each is listed inline with the invocation or the driver it needed, and each pushes
  an advisory reason, which is what stops the run producing an acceptance verdict.  It is not a
  failure of the test set either — the cases exist and were not run — and the gate says which.
- **The 3 unreachable-by-construction records are printed on every run with the proof that makes
  them unreachable**, and are scored on neither count.  They are the factory's NULL-reason defensive
  report, which no path that exists can produce because `isServiceAvailable()` assigns a reason on
  every one of its false exits; `halcompat.h`'s client-era arm, which `IHdmiCec::VERSION` being 1000
  falsifies for every server value; and the slow-call diagnostic's clock-unreadable arm, which needs
  `clock_gettime(CLOCK_MONOTONIC)` to fail on a stack `timespec` — something Linux does not do — and
  whose helper sits in an anonymous namespace with no dynamic symbol, so no test can substitute for
  it either.  All three are recorded rather than deleted, so a reader can see why they are not counted
  instead of finding three arms quietly missing.

**The 30 deferrals are 28 that need invocation B, one that needs invocation C, and one that needs a
usable binder driver with no service registered on it.**  The last is `availability.service-absent`,
the service-NOT-FOUND arm that invocation A reaches on a Binder-capable host and cannot reach at all
without one; it is the only record carrying the `binder` token, because the preflight's own
protocol-version and context-manager arms reach their decisions through the `BinderPreflightProbe`
seam rather than through a real node and are therefore enforced here, among the 59.  Every one of
the three kinds is satisfied by the Binder-capable job that runs the whole matrix on one host, so
**full branch coverage of the selection code is established by that job and by no driverless run**,
this one included.

**Coverage is subordinate to functional passes.**  A coverage number never substitutes for a
passing suite: `run_coverage.sh` captures nothing until every invocation it could run has passed
its exit status, its measured case count and its selected-path check.

```bash
# Build (instrumented), orchestrate invocations A through E -- deferring the ones this host
# cannot support -- then capture, report and gate
./run_coverage.sh --build --run

# Measure whatever profile data already exists
./run_coverage.sh

# Raise the bar for a diagnostic run
./run_coverage.sh --threshold 90

# Gate on the aggregate ONLY -- a diagnostic opt-out, because per-file gating is the default
./run_coverage.sh --no-per-file-gate

# Full option and environment reference
./run_coverage.sh --help
```

**Per-file gating is on by default** (`COVERAGE_PER_FILE_GATE` defaults to `1`), so any single file
below the bar fails the run even when the aggregate is healthy — which is what the requirement's
per-target wording asks for.  `--per-file-gate` therefore only restates the default and exists for
symmetry; `--no-per-file-gate` (or `COVERAGE_PER_FILE_GATE=0`) is a **diagnostic** opt-out, for a
deliberate run such as watching the aggregate while a new production file's tests are still being
written — the closing verdict is marked advisory when it is in force, because a run made in that
mode is not evidence that the per-target bar was met.  Files below the bar are enumerated either
way, so turning the gate off never hides a figure; it only stops it failing the run.

**No file is waived.**  `COVERAGE_GATE_EXEMPT_FILES` exists — one path per line suppresses that
file's per-file *vote*, and the rules for when that is admissible are stated at its definition in
the script — but it is **empty by default and it is meant to stay that way**, and nothing below is
an argument for putting a path in it.

**The current measurement, and what it does:** on a driverless host — where invocations B, C and E
are deferred — `./run_coverage.sh --build --run` after a full clean measures the line-gate trace at
**86.0% lines (2405/2795), 91.6% functions (468/511), 52.5% branches (1453/2768)
across 32 source files**.  The aggregate **passes** the 80% bar.  **Expect the aggregate to move by
one line and two branches between runs, and expect it in one file.**  Two consecutive runs of this
command on this tree, compared file by file, agreed on 31 of the 32 and differed only in
`ccec/src/DriverImpl.cpp`, the untouched legacy back-end: 193/198 lines and 160/242 branches in one
run, 192/198 and 158/242 in the other, which moves the aggregate between 2405/2795 and 2404/2795
lines and between 1453/2768 and 1451/2768 branches.  It is a timing-dependent path in pre-existing
code that this migration does not modify, `ccec/src/Driver.cpp`, `ccec/src/DriverAidlImpl.cpp` and
every other file measured byte-identically across both runs, and the figures above are the
authoritative acceptance run's.  So read a one-line or two-branch difference as this property
rather than as a regression, and read a difference anywhere else as something to investigate.  The
per-file half **fails**, on exactly one file:
`ccec/src/DriverAidlImpl.cpp` at **58.6% (379/647)**.  The script therefore **exits 1**, and that
exit is the gate working rather than a defect: that file holds the AIDL back-end, which is reached
only by the invocations being deferred, so on this host most of its lines cannot be covered at all.
The way to close it is to run the full matrix on the Binder-capable job — **not** to exempt a file,
not to lower the threshold, not to add an exclusion glob, and not to pass `--no-per-file-gate`.
`ccec/src/Driver.cpp`, the other file this migration adds lines to, **clears the bar at exactly
80.0% (12/15)** — on the bar rather than above it, so a single newly uncovered line there would put
it below, which is worth knowing before adding one.  `halcompat.h` is retained in that trace and
sits at 85.7% lines (18/21), 80.0% functions (4/5) and 60.0% branches (24/40).

Three files that *are* above the bar are worth naming, because they were not always: the
template-heavy headers under `ccec/include` that sat below it at the
engagement baseline — `ccec/include/ccec/Operand.hpp` at 0.0% (0/6),
`ccec/include/ccec/Exception.hpp` at 33.3% (4/12) and `ccec/include/ccec/Messages.hpp` at 76.5%
(228/298).  Each was closed the only way a coverage bar may be closed, **by adding tests**:
`Operand`'s three default virtuals and all seven `Exception::what()` overrides are exercised from
`ccec/test_Operands.cpp`, and the previously unserialised message types from
`ccec/test_MessageEncoder.cpp`.  None of them was exempted and none was excluded from the trace.
Those three percentages are therefore **historical baseline figures**, quoted so the movement is
attributable, not current ones — re-measure with `./run_coverage.sh` rather than quoting them.

Notes worth knowing before you reach for `lcov` directly:

- **The gate must be spelled with an operation.**  `lcov --summary <trace> --fail-under-lines 80`
  works — exit 0 at or above the bar, exit 1 below it.  The intuitive
  `lcov --fail-under-lines 80 <trace>` is rejected outright by lcov 2.x and exits 2, which looks
  like a working gate while never once consulting the coverage figure.
- **Branch coverage is evidence, not a gate.**  gcov models branches as control-flow-graph arcs,
  including compiler-generated exception and static-destruction arcs no test can reach, so a
  branch percentage is reported to show movement and never used as a pass/fail criterion.  Line
  coverage is the gate.
- **Coverage counters accumulate.**  A stale `*.gcda` keeps a line marked hit long after the test
  that hit it stopped running.  `--run` zeroes the counters before the suite for exactly that
  reason; without it the script reports how many counter files it found and when they were
  written.
- **The build is a prerequisite.**  `--enable-l1tests` plus `-fprofile-arcs -ftest-coverage`, and
  `libglib2.0-dev` is a hard requirement of `configure`; so are the two AIDL/Binder staging
  prefixes, which `--build` passes straight through and names no path of its own for.
  `autoreconf`/`configure` also rewrite six git-tracked `Makefile` files in this submodule.
  `--build` snapshots them beforehand and restores them itself on every exit path of that same
  invocation.  A standalone `./run_coverage.sh --restore` has no snapshot of its own — the snapshot
  directory is run-scoped and gone — so it **always exits non-zero, whatever the six look like**:
  it prints the index comparison as information, says that a clean comparison is not the same as
  nothing to restore because the index is not the reference, and refuses on *"THIS INVOCATION HAS
  NO SNAPSHOT"* (measured: exit `1` with all six clean, straight after a completed build).  It
  never reaches for `git checkout`, which would destroy the hand-written content of
  `ccec/src/Makefile`.  See [Building Tests](#building-tests).
- **Branch coverage of the selection is the exception to the rule below.**  The whole-file branch
  percentage remains evidence rather than a gate, but the named selection and array-adaptation arms
  are checked individually against the unfiltered trace, per
  [One build, one capture, two consumers](#one-build-one-capture-two-consumers).

### Coverage output is a build artifact — never commit it

`run_coverage.sh` writes `coverage.info`, `filtered_coverage.info`, `line_gate_coverage.info`,
`coverage/`, `per_file_coverage.tsv`, `uncovered_lines.txt`, one
`rdkTestResults_invocation_<letter>.json` and one `run_invocation_<letter>.log` per invocation, and
its per-step logs.  The per-invocation names carry a letter rather than a timestamp precisely so
that an empty or failed invocation cannot be erased by whichever green one came after it.
`.gitignore` in this directory covers object files, the `run_L1Tests` binary and `*.log`/`*.trs`/
`*.xml`, but **not** those coverage names — so with no `--output-dir` and no
`COVERAGE_OUTPUT_DIR`, the script **mints its own root** with `mktemp -d` under
`${TMPDIR:-/tmp}` and prints the path it chose (`artifacts: …`).  There is deliberately no fixed
default name: a guessable path under a world-writable `$TMPDIR` can be pre-created as a symlink or
as someone else's directory, and every trace, log and HTML page written under it would then be
collectable or tamperable.  The minted root is unpredictable, created atomically at mode 0700, and
outside the git tree, so nothing under it can be staged even by `git add -A`.  Name a directory
when you need a stable location — `--output-dir DIR` or `COVERAGE_OUTPUT_DIR=DIR`, which is also
how you reproduce the CI layout in-tree with `--output-dir .` — and it is then held to stricter
ancestry checks precisely because a name chosen in advance is guessable.  Keeping in-tree
artifacts out of a commit is yours to manage.  `git clean -xd` is the sweep to reach for, and the
reason to prefer it over removing directories by hand is `cfg/`: `git ls-files cfg/` names exactly
one file there, the **tracked** `cfg/Makefile.am`, among the 19 generated helpers `autoreconf` puts
beside it, and `git clean` never removes a tracked file.

## Known Issues and Notes

### No Disabled Tests

**This suite has no disabled tests**, and that has been true throughout: it was true of the
pre-migration legacy-path suite, which reported **483 tests run, 483 passed, 0 disabled** — a
*historical* figure, quoted here because older notes quote it — and it is true of the suite as it
stands, where invocation A reports **568 run, 568 passed, exit `0`** and its XML output carries
`disabled="0"`.  A bare `./run_L1Tests` is not the command to check this with: it runs all 607
registered cases and exits `1` on the 39 AIDL-only ones, which is a filter matter and not a
disabled-test matter — see
[The one command for running this suite](#the-one-command-for-running-this-suite).

Verify with the listing, which executes nothing and needs no filter:

```bash
./run_L1Tests --gtest_list_tests | grep DISABLED_    # expect no output
```

Measured: no output, `grep` exits 1.  For the count as well as the absence, the invocation-A run's
XML report states it in one line — `tests="568" failures="0" disabled="0"`.

Two things are worth knowing if you are hunting for disabled tests on the strength of older notes.
The three `LibCCECTest` cases those notes named — a `Term`-throws case, a `Term`-succeeds case and a
multiple-init/term-cycles case — were **never** in `ccec/test_LibCCEC.cpp` under those names,
disabled or otherwise; they left the file upstream in commit `0f00d4e`, before this engagement.  The
*behaviour* is nevertheless covered now, enabled and passing, in a different fixture that this
project added: `LibCCECUninitializedTest.TermThrowsWhenNotInitialized` and
`LibCCECUninitializedTest.MultipleInitTermCycles`, with `term()` succeeding on an initialised library
exercised by that fixture's `SetUp()` on every one of its six cases.  And the two asynchronous-write
cases in `ccec/test_Driver.cpp` that did carry GoogleTest's disable prefix are enabled and passing as
`DriverTest.WriteAsync` and `DriverTest.WriteAsyncWithFailure`.

The reason once given for disabling — that LibCCEC's Bus reader/writer threads race when repeatedly
started and stopped, so init/term cycling had to be avoided altogether — no longer justifies
disabling anything.  That cycling is covered and green: the four
`LibCCECUninitializedTest.*ThrowsWhenNotInitialized` guard cases — for `term()`,
`addLogicalAddress()`, `getLogicalAddress()` and `getPhysicalAddress()` — plus
`InitWithNullNameClearsLogPrefix` and `MultipleInitTermCycles`, six cases in all, assert against a
deliberately terminated library.

The underlying race is real; it is **structurally avoided rather than timed around**.  There is no
pause anywhere in this suite's new tests.  Two things replace it, and
both are the pattern to reuse if you add another case that terminates the library:

- **The uninitialised state is owned by the fixture, per case, through two idempotent helpers.**
  `LibCCECUninitializedTest` declares no `SetUpTestSuite()` and no `TearDownTestSuite()`.  Its
  `SetUp()` calls `ensureTerminated()` and its `TearDown()` calls `ensureInitialized()`, so each
  case is handed the uninitialised state and hands back the initialised `CEC_TEST` state the rest
  of the binary runs against — including the case that empties the log prefix on purpose, which is
  why `TearDown()` also restores the prefix.  Both helpers work from EITHER state: the state is
  read with a read-only `getPhysicalAddress()` probe, chosen because it answers the question
  without initialising anything, unlike using `init()` as the probe.
- **What removes the need for a pause is the readiness wait, not the absence of cycling.**
  `ensureTerminated()` never calls `term()` until `awaitBusReaderRunning()` has observed the Bus
  reader publish RUNNING, and terminating before that observation is exactly the ordering in which
  `Bus::Reader`'s RUNNING/STOPPING transition can be lost.  The window is therefore closed by
  ordering rather than waited out by a sleep, which is why this fixture contains no `sleep`,
  `usleep` or `sleep_for` at all.  `ensureTerminated()`'s already-terminated early return is a
  guard that keeps the helper total over both states; in the default order no case reaches it,
  because every `TearDown()` leaves the library initialised.  The wait injects a frame, so it also
  **re-states the driver's inbound registration on the process-global mock before injecting** — see
  [Test Order Dependence](#test-order-dependence-diagnostic) for why a wait that trusted the
  existing registration was order-fragile, and failed with *"could not bring LibCCEC to the
  uninitialised state this case requires"* whenever `HdmiCecDriverMockTest` had run first.
- **The close() sentinel CANNOT be drained from a test, and the suite no longer claims otherwise.**
  `DriverImpl::close()` offers a NULL sentinel to the receive queue and `DriverImpl::read()` is what
  consumes it, but `Bus::stop()` only waits for the reader to leave its loop - it can leave with the
  sentinel still queued, and a second `close()` then queues another one, which is what the flush-arm
  dereference below needs.  Three cases used to follow their `term()` with a `Driver::read()` on the
  closed driver, on the stated theory that the call would take the sentinel off the queue.  **It does
  not.**  `read()` opens with `if (status != OPENED) { throw InvalidStateException(); }`
  (`ccec/src/DriverImpl.cpp:158-162`) and therefore throws *before* it reaches `rQueue.poll()`, so on
  a closed driver the flush arm at `DriverImpl.cpp:178-186` is unreachable and the sentinel is
  untouched.  The two calls that existed only as that mitigation are gone; the one in
  `ccec/test_DriverImpl_Async.cpp` remains because it is a real assertion on `read()`'s
  invalid-state arm, and its comment now says only that.  The window itself is **BLOCKED, reported
  not fixed** - see the section below - and the suite bounds its exposure instead: only
  `DriverTest.WriteAsync`, `DriverTest.WriteAsyncWithFailure` and `LibCCECUninitializedTest` cycle
  the shared library at all, and the last of those only ever calls `term()` behind the readiness
  wait described in the bullet above.

### Intermittent SIGSEGV in the Bus reader thread — a PRODUCTION defect, reported not worked around

`./run_L1Tests` can exit 139 (SIGSEGV).  The fault is a null dereference in PRODUCTION source that
the tests merely reach, so the defect is reported rather than fixed, and an exit 139 should be read
as this window reopening rather than as a test regression.

Re-measured on this tree after the order-independence fix described in
[Test Order Dependence](#test-order-dependence-diagnostic), on the pre-contract-suite inventory of
483 cases: **14 consecutive full-suite runs in the default order, 14 green (483 / 483),
0 exit-139** — and the crash still fires as soon as the order
changes, at **11 exit-139 in 81 shuffled full-suite runs (13.6%)**, spread across seeds 7, 15 and
12345.

**No seed is a reliable reproducer, and no seed is safe.**  Seed 12345 crashed 3 runs in 10, seed 15
4 in 16 and seed 7 3 in 16, while seed 5 (6 runs), seed 2 (10 runs) and seed 54321 (10 runs) did not
crash once — the remaining seeds in the tables below had one run each, which settles nothing about
their rate either way.  Seed 54321 is the reason this paragraph is worded the way it is: an earlier
revision of this file quoted it as crashing **5 runs out of 5**, and it is now green **10 runs out of
10**.  A per-seed crash figure is a snapshot of a race, not a property of the seed, so re-measure it
rather than quoting one; the per-seed numbers behind these totals are in
[Test Order Dependence](#test-order-dependence-diagnostic).  Every figure in this paragraph belongs
to that 483-case inventory, and seed 12345 has since been re-measured twice — at **7 exit-1 in 7
runs, no crash** on the 567-case selection that preceded the logical-address registry guard, and at
**7 exit-0 in 7 runs** on the 568-case selection today — see
[Current-inventory re-measurement of seed 12345](#current-inventory-re-measurement-of-seed-12345),
which is this paragraph's point made twice over.  The default order keeps the window
narrow; it does not close it.

Every crash observed in this pass fired in the same place, and the signature is worth knowing when
triaging one.  The last case GoogleTest started was `DriverTest.ZZZ_OpenWithFailure` in **11 of the
11** crashing runs, and all **five** crashes that were symbolised — four core files plus one live
`gdb` capture — put frame `#0` in `DriverImpl::read` and frame `#1` in `Bus::Reader::run`, which is
the arm described immediately below.  In each of them the main thread was inside `DriverImpl::open`
(`ccec/src/DriverImpl.cpp:110`, reached from `ccec/test_Driver.cpp:561`) blocked on the driver mutex
while the Bus reader dereferenced the sentinel — the race stated in one line.

**IT IS NOT MITIGATED IN THE TESTS, AND MUST NOT BE "MITIGATED" WITH A `Driver::read()` ON A CLOSED
DRIVER.**  That call looks like it drains the `close()` sentinel off the receive queue and cannot:
`read()` throws its invalid-state exception before it ever reaches `rQueue.poll()` (see the bullet
in [No Disabled Tests](#no-disabled-tests)), so the sentinel stays queued and a green run following
such a call was green for some other reason.  What the suite does instead is bound its exposure:
only `DriverTest.WriteAsync`, `DriverTest.WriteAsyncWithFailure` and `LibCCECUninitializedTest`
cycle the shared library at all, and the last of those calls `term()` only behind the Bus-reader
readiness wait.

Core dump, symbolised (`gdb ./.libs/run_L1Tests core.<pid>`):

```
#0  DriverImpl::read (...) at DriverImpl.cpp:182     ->  frame = *inFrame;
#1  Bus::Reader::run ()   at Bus.cpp:158
#2  CCEC_OSAL::Thread::CEntry at Thread.cpp:41
```

`DriverImpl::read()`'s flush-and-throw arm (`ccec/src/DriverImpl.cpp:180-184`) runs when the
driver is closed while a frame is still queued:

```cpp
while (rQueue.size() > 0) {
    inFrame = rQueue.poll();
    frame = *inFrame;          // line 182: inFrame may be NULL
    delete inFrame;
}
```

`EventQueue::poll()` is DOCUMENTED to return a default-constructed value -- for
`CECFrame *`, a null pointer -- when it is woken with the condition unset, expressly so a
waiting thread can close the queue out (`osal/include/osal/EventQueue.hpp:101-115`).  The
happy path at `DriverImpl.cpp:172` null-checks the same call; the flush arm does not, and
trusts `size()` instead.  Between the `size()` check and the `poll()` the condition can be
reset, so `poll()` legitimately hands back null and line 182 dereferences it.

**Required production change (NOT made here: production source is out of scope for this
pass, so this is reported rather than fixed):** null-check the flush arm exactly as the
happy path does, for example

```cpp
while (rQueue.size() > 0) {
    inFrame = rQueue.poll();
    if (inFrame == 0) { break; }   // woken with the condition unset
    frame = *inFrame;
    delete inFrame;
}
```

Two further production changes would each close the same window on their own, and either is
preferable to the test-side workaround that does not exist: have `CCEC_OSAL::EventQueue` publish its
element count and its condition variable under ONE lock, so `size()` and `poll()` cannot disagree;
or have `DriverImpl::close()` consume the sentinel it queues instead of leaving it for a reader that
may already have gone.

Until one of them lands, treat a lone exit-139 with no `[  FAILED  ]` line as this defect and
re-run; `run_coverage.sh` fails closed on it and reports no coverage, which is the correct
behaviour.  Note that this arm was UNCOVERED before this project's tests reached it, which is why
the latent dereference had never been observed.

#### A SECOND, DISTINCT SIGSEGV IN THE SAME THREAD — dangling `Connection` listener

An exit 139 does not always mean the arm above.  A second, distinct null dereference lives in the
same thread: an earlier pass reproduced it on this tree under
`--gtest_shuffle --gtest_random_seed=54321` in **5 runs out of 5**, and `gdb` put it somewhere else
entirely:

```
Thread 2 "run_L1Tests" received signal SIGSEGV
#0  Connection::DefaultFrameListener::notify  at Connection.cpp:335
#1  Bus::Reader::run                          at Bus.cpp:165
#2  CCEC_OSAL::Thread::CEntry                 at Thread.cpp:41
```

`Connection::~Connection(void)` is EMPTY (`ccec/src/Connection.cpp:49-51`): it neither calls
`close()` nor removes the connection's listener from `Bus::listeners`.  A `Connection` that is
opened and then destroyed without an explicit `close()` therefore leaves a dangling
`FrameListener *` on the bus, and the next frame the reader dispatches calls `notify()` through
freed storage at `Bus.cpp:165`.

**Required production change (NOT made here — production source is out of scope):**
`Connection::~Connection()` must detach, either by calling `close()` or by removing its bus
listener directly.  The test side cannot close this: a test can only avoid provoking it, which is
why `ConnectionTest.CloseDetachesTheConnectionAndDestroyingAClosedOneLeavesTheBusUsable` destroys a
connection it has already closed and asserts detachment through `close()` rather than through the
destructor.

**This arm was NOT reproduced in this pass.**  Seed 54321 — the seed that produced the backtrace
above, 5 runs out of 5 — is now green 10 runs out of 10, and none of the 11 crashes measured across
81 shuffled runs was this arm: all five that were symbolised were the `DriverImpl::read` arm of the
section above.  That is a change in *exposure*, not a fix: `Connection::~Connection()` is still
empty, the dangling-listener window is still open, and the analysis below still stands.  It is also
why an exit 139 must be triaged from its backtrace rather than from its seed.

So when triaging an exit 139, read the backtrace before assuming which defect you have: frame `#0`
in `DriverImpl::read` is the sentinel dereference, frame `#0` in
`Connection::DefaultFrameListener::notify` is this one.

### Test Order Dependence (diagnostic)

The suite is green in its default order — invocation A, measured at 568 of 568, exit `0`.  **Under a
randomised order it is not**, and a single green seed proves nothing either way, so run more than
one.  A shuffle is a full run, so it carries the invocation-A environment; without the filter the 39
AIDL-only failures bury the order-dependent ones this is looking for:

```bash
CEC_TEST_AIDL_MODE=absent GTEST_FILTER='-DriverAidlSessionTest.*:DriverAidlTransmitTest.*' \
  ./run_L1Tests --gtest_shuffle --gtest_random_seed=<seed>
```

**Every figure in the two tables below is HISTORICAL: it was measured on the pre-contract-suite
inventory** — 483 cases in 18 fixtures, before `ccec/test_DriverAidl.cpp` was added — and they are
kept exactly as they were measured, because their value is provenance.  **None of them describes the
568-case selection a shuffle runs today**, and the seeds have since been re-measured against that
selection and moved outright — see
[Current-inventory re-measurement of seed 12345](#current-inventory-re-measurement-of-seed-12345).
Read `483 / 483` in them as "the whole suite as it then
was", and `483 / 481` as "that suite less the fixed pair".  The same sweep under invocation A
reports that invocation's whole selection in place of each 483 — 568 cases today, and 539 when the
seed-2 sweep was measured: `539 tests ran`, `537 passed`, exit `1`, the two failures being the same
fixed pair named below.  Re-measure the total rather than adjusting it, because the selection grows
with every case added to the contract suite and the fixed pair does not.

Two independent problems are in play under a shuffle, and every figure below separates them:

- **Order-dependent ASSERTION failures** (exit `1`).  Deterministic for a given seed: the same seed
  fails the same cases every time.
- **An intermittent SIGSEGV** (exit `139`).  A race in production source, described in
  [Intermittent SIGSEGV in the Bus reader thread](#intermittent-sigsegv-in-the-bus-reader-thread--a-production-defect-reported-not-worked-around);
  it is NOT tied to a seed, and when it fires it pre-empts the summary so the run prints no totals
  at all.  That is why the same seed can read as green in one table and as a crash in another.

#### What was fixed, and what remains

Nine cases added by this project — the six in `LibCCECUninitializedTest`,
`IntegrationFlowTest.InboundImageViewOnReachesTypedProcessorThroughTheWholeStack`,
`IntegrationFlowTest.InboundBroadcastActiveSourceReachesTheProcessor` and
`ConnectionTest.CloseDetachesTheConnectionAndDestroyingAClosedOneLeavesTheBusUsable` — were
themselves order-fragile when they were written, and they are not any more.  **One cause explained
all nine.**

`HdmiCecDriverMockTest.ReceiveMessageCallback` (`ccec/test_Driver_Mock.cpp`) calls the real
`HdmiCecSetRxCallback()` with a test lambda and a pointer to a local `bool`.  The mock's default
action *stores* that pair on the process-global mock object, so it outlives the case that set it.
`DriverImpl::open()` early-returns while the driver is already `OPENED`
(`ccec/src/DriverImpl.cpp:110-113`) and therefore never re-registers its own receive callback, so
after that one case every `injectReceivedMessage()` in the program invoked a dead lambda through a
freed stack address and no injected frame reached `DriverImpl::DriverReceiveCallback` — and from
there nothing reached `Bus`, `Connection` or any listener.  In the default order that case runs
after its victims and nothing shows; under a shuffle it can run before them, and then any case that
depends on an injected inbound frame fails.  The tell was
`ccec/test_LibCCEC.cpp` reporting *"could not bring LibCCEC to the uninitialised state this case
requires"*, because its readiness wait injects a frame to observe the Bus reader publish RUNNING.

The remedy is in three parts, all of them test-side:

- `HdmiCecDriverMockTest::SetUp()`/`TearDown()` now capture and restore the mock's `rxCallback`,
  `rxCallbackData`, `txCallback`, `txCallbackData` and `currentHandle` — fixture state management
  only; no `TEST_F` body was touched.
- `awaitBusReaderRunning()` in `ccec/test_LibCCEC.cpp` re-states the driver's inbound registration
  (`DriverImpl::DriverReceiveCallback`) before it injects, so it does not depend on who ran before.
- `ccec/test_Connection.cpp` does the same through a file-local `restoreDriverInboundRoute()`,
  called from `IntegrationFlowTest::SetUp()` and from the one `ConnectionTest` case that injects.
  It is deliberately **not** called from `ConnectionTest::SetUp()`, which is shared with 60
  pre-existing passing cases.

**What remained after that was the pre-existing fragility, and it was exactly two cases**:
`ConnectionTest.MatchSourceMismatchedAddress` and `ConnectionTest.SendAsyncMatchSource`.  They pass
in the default order, they are not this project's tests, and they are deliberately left unedited.
**They no longer fail under a shuffle either, and no case was edited to achieve it:** they were
inheriting the process-global logical-address registry from whichever case ran before them, and
`LogicalAddressRegistryGuard` in `test_main.cpp` now restores that registry after every case, so
each case sees the baseline the default order gives it. Re-measured across every seed the tables
below name as a reliable reproducer of the pair — 2, 42, 100, 15 and 12345 — each is now
`568 passed`, exit `0`; see
[Current-inventory re-measurement of seed 12345](#current-inventory-re-measurement-of-seed-12345).
Six further legacy cases that used to fail under a shuffle — `BusTest.ListenerFiltering`,
`BusTest.UnregisteredConnectionReceivesAll`, `ConnectionTest.MultipleListenersNotification`,
`ConnectionTest.DefaultFilterUnregistered`, `ConnectionTest.DefaultFilterSpecificAddress` and
`ConnectionTest.DefaultFilterBroadcast` — stopped failing without being edited, because they were
victims of the same hijacked callback registration.

#### Assertion outcome, per seed

Fifteen seeds, measured on this tree after the fix, on the historical 483-case inventory.  Every run
that reached the summary reported 483 tests run and zero skipped:

| Seeds | Exit | Ran / passed | Cases that failed |
| --- | --- | --- | --- |
| 5, 7, 15, 500, 12345, 54321 | `0` | 483 / 483 | none — green, like the default order |
| 1, 2, 11, 12, 14, 42, 100, 777, 999 | `1` | 483 / 481 | `ConnectionTest.MatchSourceMismatchedAddress`, `ConnectionTest.SendAsyncMatchSource` |

The failing set is now a **fixed pair** wherever it appears, which is itself the evidence that the
nine cases above were fixed at their cause rather than reordered around: before the fix the same
sweep produced a different subset at almost every seed (a `BusTest` pair at seed 500, three cases at
seed 2, five `ConnectionTest` cases at seed 54321, and six to fifteen unique failures at seeds 5, 7,
11, 12, 14 and 15).  Seeds 7, 15 and 12345 appear in the green row because that is what they report
**when they reach the summary** — they are also the three seeds that crash, which the next table
separates out.

#### Crash rate, per seed, by repetition

Historical, on the same 483-case inventory as the table above, and superseded for seed 12345 by the
current measurement below:

| Seed | Runs | exit `0` | exit `1` | exit `139` |
| --- | --- | --- | --- | --- |
| 5 | 6 | 6 | 0 | 0 |
| 7 | 16 | 13 | 0 | 3 |
| 15 | 16 | 12 | 0 | 4 |
| 12345 | 10 | 7 | 0 | 3 |
| 54321 | 10 | 10 | 0 | 0 |
| 2 | 10 | 0 | 10 | 0 |

Four further runs of seed 12345 under `gdb` crashed once more, for 4 in 14 at that seed.  Across
every shuffled full-suite run in this pass — 81 of them, 77 plain and 4 under `gdb`, over the fifteen
seeds above — the totals are **11 exit-139, 0 unexpected assertion failures beyond the fixed pair**.
The default order over the same period was **14 runs, 14 green, 0 exit-139**.

#### Independent re-measurement, and two seeds that moved

The tables above are kept exactly as they were measured, because their value is provenance.  A later
QA-remediation pass re-ran the sweep on the same tree — 16 shuffled runs plus one default-order run,
each bounded at 600 s — and the outcome both confirms the split and moves two seeds, which is the
point of recording it separately rather than editing the numbers above:

| Seed | Runs | exit `0` | exit `1` (fixed pair) | exit `139` |
| --- | --- | --- | --- | --- |
| default order | 1 | 1 | 0 | 0 |
| 1, 2, 11, 12, 14, 42, 999 | 1 each | 0 | 7 | 0 |
| 7, 15 | 1 each | 2 | 0 | 0 |
| 100 | 3 | 0 | 3 | 0 |
| 777 | 1 | 0 | 0 | **1** |
| 12345 | 3 | 2 | 0 | **1** |

Every exit-1 run reported 483 ran / 481 passed with the same fixed pair —
`ConnectionTest.MatchSourceMismatchedAddress` and `ConnectionTest.SendAsyncMatchSource` — and the
default order was green at 483 / 483, so nothing in the assertion picture has changed.

- **Seed 777 crashed.**  It sits in the exit-1 row of the assertion table above and had never been
  observed crashing.  Its crashing run had already printed both `ConnectionTest` failures before the
  fault pre-empted the summary, so it read as "exit 139 with two named failures and no totals" —
  which is exactly the interleaving the two tables above warn about, now observed rather than
  predicted.
- **Seed 100 did not crash in three runs**, although the QA checkpoint that prompted this
  re-measurement did observe it crashing.  Two independent measurements of the same seed therefore
  disagree, which is the strongest form of the point this section already makes.

Totals for the re-measurement: **2 exit-139 in 16 shuffled runs (12.5%)**, against 11 in 81 (13.6%)
above — the same rate within the noise of a race — and **0 exit-139 in the default order**.  Neither
required production change has been made, so nothing here is a fix: the exposure moved seeds, and the
two defects are still open exactly as described above and in
[Intermittent SIGSEGV in the Bus reader thread](#intermittent-sigsegv-in-the-bus-reader-thread--a-production-defect-reported-not-worked-around).

Read those two tables together and the split is unambiguous: seed 2 fails its two assertions ten
times out of ten and never crashes, while seed 54321 crashes zero times out of ten and passes all
483.  **Do not quote a single seed as "the" reproducer of either problem.**  For the assertion pair
use seed 2, 42 or 100, which reproduce it every time; for the crash, budget for repetition rather
than a seed — seed 15 is the highest observed rate at 4 in 16, and the seed an earlier revision of
this file named for it (54321) no longer reproduces it at all.

#### Current-inventory re-measurement of seed 12345

Both tables above are historical.  One seed has since been re-measured against the selection a
shuffle actually runs today — seven consecutive runs of the invocation-A filter at seed 12345 on the
568-case selection, each bounded at 300 s and pinned to two cores so a parallel checkout could not
distort the timing:

```bash
# From tests/L1Tests
for i in $(seq 7); do
  CEC_TEST_AIDL_MODE=absent GTEST_FILTER='-DriverAidlSessionTest.*:DriverAidlTransmitTest.*' \
    taskset -c 4,5 timeout 300 ./run_L1Tests --gtest_shuffle --gtest_random_seed=12345
  echo "run $i rc=$?"
done
```

**Measured: 7 runs, 7 exit `0`, 0 exit `1`, 0 exit `139`.**  Every one of the seven reported
`568 tests from 23 test suites ran`, `568 passed`, in 8078 to 8442 ms.  **The assertion pair no
longer fails under shuffle, and that is a change in the suite rather than in the seed.**  An earlier
revision of this section measured the same seed on the 567-case selection at 7 exit-`1` in 7 runs,
every one of them failing the fixed pair `ConnectionTest.SendAsyncMatchSource` and
`ConnectionTest.MatchSourceMismatchedAddress`; the difference between then and now is
`LogicalAddressRegistryGuard` in `test_main.cpp`, a global test-event listener that restores the
process-global logical-address registry after every case, which is exactly the state those two cases
were inheriting from whichever case ran before them.  **Re-measured across every seed the historical
tables name as a reliable reproducer of that pair — 2, 42, 100 and 15 — each is now `568 passed`,
exit `0`.**  So the assertion half of the order dependence is addressed at its cause rather than
worked around by ordering, and the per-seed *figures* below still do not survive an inventory change
— which is why every one carries its era, why no rate is quoted without one, and why the paragraph
above naming seeds 2, 42 and 100 as assertion reproducers is now historical rather than current.

The failing set is specific to the seed *and* to the test inventory: re-derive it after adding
tests rather than trusting the tables above, because shuffling reorders the whole program and a
different seed exposes a different subset of the same underlying fragility.  Corroborating evidence
sits in the driver file:
`DriverTest.AAA_DriverSingletonAccess` is commented "runs first alphabetically", and
`DriverTest.ZZZ_OpenWithFailure` and `DriverTest.ZZZ_CloseWithFailure` are commented "runs late
to avoid breaking other tests" — deliberate alphabetical-ordering prefixes, which only make
sense as a workaround for this fragility.  `ZZZ_OpenWithFailure` is also the case that was running
in all 11 crashing runs, so the prefix bounds the assertion problem and not the race.

Order dependence in this suite is therefore **substantially reduced and still not formally
resolved**, and **randomised-order runs remain diagnostic here, not an acceptance gate**.  The
assertion half is addressed at its cause — the registry guard above — and every seed measured is
green; what keeps this from being a resolution is the crash half, which is the `Bus` worker
lifecycle defect recorded as PERF-01 in `blitzy/documentation/Project Guide.md` §4.1, is not
seed-bound, and is not fixed here.  Three consequences for anyone adding tests:

- Place new translation units in `run_L1Tests_SOURCES` *after* every pre-existing entry of their
  layer, so no established suite is reordered.  GoogleTest registers each `TEST_F` during static
  initialisation, in link order, and runs the suites in registration order, so a file inserted
  ahead of an existing suite reorders that suite.
- A new test must establish its own preconditions and leave shared state untouched, and must be
  verified both in isolation (`--gtest_filter=YourFixture.*`) and in the full suite, the latter
  through the invocation-A command in
  [The one command for running this suite](#the-one-command-for-running-this-suite).
- **The HAL driver mock is process-global, and its registered callbacks are part of the state you
  can break.**  GoogleTest isolates fixture members, not the mock singleton: `VerifyAndClear` and
  re-stated `ON_CALL` defaults restore *expectations*, and restore nothing about which receive or
  transmit callback the driver last registered.  A test that calls `HdmiCecSetRxCallback()` or
  `HdmiCecSetTxCallback()` — directly or through production code — must capture and restore the
  previous registration, and a test that injects an inbound frame must not assume the route is
  still intact.  Verify a new test with
  `--gtest_filter='HdmiCecDriverMockTest.*:YourFixture.*' --gtest_repeat=2`, which exposes this
  class of defect deterministically and without a seed.

### Dead Skip Guards

`ccec/test_Driver.cpp` contains 14 `GTEST_SKIP()` guards — 13 spelled
`"Mock is nullptr - test environment not initialized"` and one
`"Driver not in valid state for this test"`.  Measurement shows **none of them ever fires**: the
default run, and every shuffled run in the tables above that reached its summary, reported the whole
inventory run and zero skipped — 483 of 483 when those tables were measured, and 568 of 568 under
invocation A today — green seeds and failing seeds alike.  They are dead defensive code
rather than silent skips, so the suite's pass count is its real pass count.  New tests deliberately
do not copy that idiom — if a precondition genuinely cannot be met, the test is not written and
the gap is reported instead.

### LibCCEC Singleton Behavior

LibCCEC is a singleton shared across all test suites. The test fixture SetUp() catches `InvalidStateException` to handle cases where LibCCEC has already been initialized by other test suites (e.g., DriverTest).

### Thread Timing

- Thread-related tests may need timing adjustments on slow systems
- Bus thread cleanup can take time.  Init/term cycling is nonetheless covered — see
  [No Disabled Tests](#no-disabled-tests) — and it is covered **without any wall-clock pause**:
  `LibCCECUninitializedTest` owns the uninitialised state per case, and its `term()` runs only
  after `awaitBusReaderRunning()` has observed the reader publish RUNNING, so the dropped-stop
  window is closed by ordering instead of waited out.  The `close()` sentinel is a separate matter
  and is **not** drained by anything in this suite — it cannot be, because `read()` throws at its
  state guard before reaching the queue.  There is no `sleep`, `usleep` or `sleep_for` anywhere in
  the tests this project added

### Hardware Dependencies

- **None.** No test in this suite requires real CEC hardware or a real driver.  A complete
  GoogleMock HDMI-CEC HAL driver mock already exists at `../../mocks/hdmicec/`
  (`hdmi_cec_driver_mock.h` / `.cpp`), is registered in `run_L1Tests_SOURCES`, and is injected by
  symlinking `mocks/hdmicec/hdmi_cec_driver.h` over `stubs/ccec/drivers/hdmi_cec_driver.h` during
  the build.  In the default order all 60 `ConnectionTest` and all 22 `DriverTest` cases pass
  against the mock with no hardware present
- Failure and error paths are provoked by making the mock return non-success codes, and the
  asynchronous completion path by invoking the callback the driver registered during open —
  never by waiting on real hardware
- **A kernel capability is not hardware, but it does bound where some cases can execute.**  The
  AIDL-selected invocations need a Binder driver, a matching protocol version and a running
  `servicemanager`; the legacy ones need none of that and run anywhere this suite builds.  See
  [Which invocations this host can run](#which-invocations-this-host-can-run)
