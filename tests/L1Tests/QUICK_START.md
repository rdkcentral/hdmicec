# L1 Unit Test Framework - Quick Reference

## Directory Structure

```text
tests/
├── Makefile.am                   # Conditionally includes L1Tests and L2Tests
├── BasicTest.cpp                 # Existing integration tests
├── CECCmd.cpp
├── CECMonitor.cpp
├── CECCmdTest.cpp
└── L1Tests/                      # L1 Unit Tests
    ├── Makefile.am               # L1 test build configuration
    ├── README.md                 # L1 test documentation
    ├── QUICK_START.md            # This quick reference
    ├── test_main.cpp             # Test runner entry point
    ├── run_coverage.sh           # coverage runner with 80% line gate
    ├── .lcovrc_l1                # lcov configuration (branch collection enabled)
    ├── .gitignore                # Ignore build artifacts
    ├── ccec/                     # CCEC library tests (13 files, 580 tests)
    │   ├── test_CECFrame.cpp
    │   ├── test_Connection.cpp
    │   ├── test_Bus.cpp
    │   ├── test_LibCCEC.cpp
    │   ├── test_MessageEncoder.cpp
    │   ├── test_MessageDecoder.cpp
    │   ├── test_OpCode.cpp
    │   ├── test_Operands.cpp
    │   ├── test_Driver_Mock.cpp
    │   ├── test_Driver.cpp
    │   ├── test_DriverImpl_Async.cpp
    │   ├── test_Util.cpp
    │   └── test_DriverAidl.cpp    # AIDL back-end + runtime selection (124 tests)
    └── osal/                     # OSAL library tests (3 files, 27 tests)
        ├── test_ConditionVariable.cpp
        ├── test_Mutex.cpp
        └── test_Thread.cpp
```

`tests/L2Tests/` sits beside this directory and is the second tier: `run_L2Tests` plus the
out-of-process `fake_hdmi_cec_aidl_host`.  See [The L2 tier](#the-l2-tier) below.

The two test subdirectories list exactly the entries in `Makefile.am`'s
`run_L1Tests_SOURCES`, each subdirectory in that list's relative order.  A
test file missing from that list is silently not compiled — no warning, no
build error — and the order matters, because GoogleTest registers fixtures
during static initialisation in link order, so append new entries rather
than inserting them (see the comment in `Makefile.am`).

## Build System Changes

### 1. configure.ac
- Added `--enable-l1tests` option
- Added Google Test dependency check
- Added `tests/L1Tests/Makefile` and `tests/L2Tests/Makefile` to configuration
- Added the **unconditional** AIDL/Binder dependency checks, which fail `configure` outright when
  the staging roots do not resolve — see [The AIDL/Binder dependency](#the-aidlbinder-dependency)

### 2. Makefile.am (root)
```makefile
DIST_SUBDIRS = cfg osal ccec tests
```

### 3. tests/Makefile.am
```makefile
if ENABLE_L1TESTS
SUBDIRS = L1Tests L2Tests
endif
```

### 4. tests/L1Tests/Makefile.am
- Defines `run_L1Tests` executable
- Links test sources with libRCEC.la and libRCECOSHal.la, plus the four AIDL/Binder edges
- `AM_CXXFLAGS = -Wall -std=c++17 -g @AIDL_CXXFLAGS@`

### 5. tests/L2Tests/Makefile.am
- Defines **two** executables: `run_L2Tests` and `fake_hdmi_cec_aidl_host`

## Quick Start

### Build and run, in one command
```bash
cd tests/L1Tests
./run_coverage.sh --build --run     # build, run every invocation, capture, report, gate
```

That is the maintained recipe.  Everything below is the same sequence by hand.

### The AIDL/Binder dependency
`libRCEC` links the generated HDMI-CEC AIDL client stubs and the Binder client library
**unconditionally** — both HAL back-ends are compiled in and one is chosen at run time — so there
is no switch to turn this off and nothing here builds without it.  **Two staging prefixes are the
whole contract**, and `configure` derives every path from them:

| Input | Spelling | Notes |
|--------|-------------|---------|
| HALIF staging root | `--with-halif-prefix=DIR` or `HALIF_PREFIX=DIR` | Omit when the `rdk-halif-aidl` checkout sits beside this submodule; `configure` falls back to it |
| AIDL stub libraries | `HALIF_LIB_DIR=DIR` | Needed whenever the built stubs are not at `HALIF_PREFIX/lib/halif` |
| Binder SDK root | `--with-binder-sdk=DIR` or `BINDER_SDK_DIR=DIR` | Holds `lib/binder` and `include/binder_sdk` |
| Binder headers | `BINDER_SDK_INCLUDE_DIR=DIR` | Only when they are staged away from `BINDER_SDK_DIR` |

From those, `configure` derives **four include roots** — `hdmicec/0.1.0.0/include`,
`common/0.2.0.0/include`, `common/current` and the Binder SDK header root — and **four link edges,
in dependency order**:

```text
-lhdmicec-v0.1.0.0-cpp -lcommon-v0.2.0.0-cpp -lbinder -lutils
```

Four things about that set are worth knowing before you debug it, and `README.md` carries the rest:

- **`common/current` is the only root that supplies `halcompat.h`** — it is *not* inside
  `common/0.2.0.0/include`, so omitting it surfaces as `halcompat.h: No such file or directory`.
- **`common` must be `0.2.0.0`**, the ABI the released `hdmicec` snapshot was pinned against; a
  newer `common` is not a substitute.
- **`-std=c++17` is required**, not preferred: `halcompat.h:73` opens
  `namespace com::rdk::hal::halcompat {`, a nested-namespace definition that does not compile at
  `c++14` at all.  `configure` checks for it and `AM_CXXFLAGS` sets it.
- **`libbinder`'s closure — `liblog`, `libbase`, `libcutils` — must be staged and on the loader
  path**, at link time as well as at run time, but must never be added as direct edges.
- **`libcutils_sockets` is staged alongside that closure without belonging to it.**  Measured
  `DT_NEEDED`: it names `libc` alone, and nothing staged names it — not `libbinder`, not
  `libcutils`, not `servicemanager`.  Stage it and leave it loadable; do not treat it as a missing
  dependency when a link fails, because it never was one.

Do not spell the roots or the edges into `CPPFLAGS`/`LDFLAGS` by hand; pass the prefixes and let
`configure` derive them, so that this build and the hand-written `ccec/src/Makefile` cannot end up
configured differently.

### Build with L1 Tests, by hand
Five steps are prerequisites rather than optional extras: the stub headers, the HAL-mock symlink,
`CPPFLAGS`/`PKG_CONFIG_PATH`, the two AIDL/Binder staging prefixes, and a **separate** `make` pass
for `tests/L1Tests` — a third for `tests/L2Tests`.  Omit any one and the build fails or silently
produces no test binary.

```bash
# From the hdmicec submodule root.  $GTEST_PREFIX must hold lib/pkgconfig/gtest.pc:
# configure.ac finds GoogleTest with PKG_CHECK_MODULES, which ignores -I and -L.
export GTEST_PREFIX="$HOME/.local/gtest-1.15.0"

# The two AIDL/Binder staging prefixes.  Omit HALIF_PREFIX when the rdk-halif-aidl
# checkout sits beside this submodule -- configure falls back to ../rdk-halif-aidl.
export HALIF_PREFIX="${HALIF_PREFIX:-$PWD/../rdk-halif-aidl}"
: "${HALIF_LIB_DIR:?the directory holding libhdmicec-v0.1.0.0-cpp}"
: "${BINDER_SDK_DIR:?the staged Binder SDK root holding lib/binder}"
: "${BINDER_SDK_INCLUDE_DIR:=$BINDER_SDK_DIR}"

# The loader path, needed to LINK and not only to run: configure's link probe links a program
# against libbinder, which carries no RUNPATH, so its closure -- liblog, libbase, libcutils --
# must be findable before ./configure runs, not after.  (libcutils_sockets is staged in the same
# directory but is not in that closure; nothing staged has a DT_NEEDED entry for it.)
export LD_LIBRARY_PATH="$HALIF_LIB_DIR:$BINDER_SDK_DIR/lib/binder${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

mkdir -p stubs/rdk/iarmbus stubs/ccec/drivers/iarmbus
touch stubs/rdk/iarmbus/libIARM.h \
      stubs/rdk/iarmbus/libIBus.h \
      stubs/rdk/iarmbus/libIBusDaemon.h \
      stubs/ccec/drivers/iarmbus/CecIARMBusMgr.h
ln -sf ../../../mocks/hdmicec/hdmi_cec_driver.h stubs/ccec/drivers/hdmi_cec_driver.h

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
```

That recipe is deliberately **uninstrumented** — no `-fprofile-arcs`, no `-ftest-coverage`.  A
first build does not need them; `./run_coverage.sh --build` adds them when coverage is wanted.
Read the configure output back before trusting it: it prints one `AIDL HDMI CEC back-end:` line per
resolved path plus the C++17 flag it settled on, so a prefix that resolved to the wrong place is
visible there rather than in a link error much later.  Read the `C++17 dialect` line for the flag
configure **accepted**, not for a spelling you expect: it is `-std=c++17` where the compiler takes
that spelling, `-std=gnu++17` or `-std=c++1z` where it takes one of those instead, and your own
`-std=` when you passed a C++17-or-newer dialect in `CXXFLAGS`.  Some dialect is always named —
configure settles one rather than trusting the compiler default, and it stops with a diagnostic
when nothing it tries compiles the C++17 constructs the AIDL headers use — so "none required" is
not a state it can report.  The `LD_LIBRARY_PATH` export in the block above is part
of the recipe for the same reason the prefixes are — it is needed to *link*, not merely to run.

`libglib2.0-dev` is a hard requirement of `configure`.

#### Those six regenerated `Makefile` files
`autoreconf`/`configure` rewrite six git-tracked `Makefile` files in this submodule.  Five are pure
toolchain output; **`ccec/src/Makefile` is not** — it is hand-written RDK build source, carrying
this migration's `-std=c++17`, `DriverAidlImpl.o` and rewritten link command, and it is in
`AC_CONFIG_FILES` all the same, so `configure` clobbers it by design.  `--build` therefore
snapshots all six from the working tree immediately before `autoreconf` and restores them from that
snapshot on the way out of the **same** invocation, so a cancelled build restores too.

**`./run_coverage.sh --restore` as a standalone command ALWAYS exits non-zero** — measured exit 1
both directly after a successful `--build --run` and on a tree nobody had configured — because the
snapshot lives only inside the invocation that took it and it will not reach for git instead.  It
also cannot be combined with `--build` or `--run`.  So **verify the restore with git, not with
`--restore`**:

```bash
git -C . status --porcelain -- \
  Makefile ccec/Makefile ccec/src/Makefile osal/Makefile osal/src/Makefile tests/Makefile
```

Silence is the pass — that is what was measured after the run above.  If you configured **by hand**,
`cp -p` those six somewhere you own *before* `autoreconf` and `cmp`/copy them back afterwards;
there is no other source.  **Never revert those six with `git checkout`** — that restores whatever
the *index* holds and silently destroys the hand-written `ccec/src/Makefile`.  `README.md` has the
full explanation.

`cfg/` is the other place tracked and generated content sit together — `autoreconf` fills it with 19
helper files beside the **one tracked** file `cfg/Makefile.am`, which is all `git ls-files cfg/`
names — so sweep generated output with `git clean -xd`, which never removes a tracked file, rather
than by deleting a directory whole.

### Build without L1 Tests (default)
```bash
./configure
make
```

`--enable-l1tests` is what is optional here; the AIDL/Binder prefixes are not.  A plain
`./configure` still needs them to resolve, because `libRCEC` carries both back-ends in every build.

### Run Tests Manually
```bash
cd tests/L1Tests
# INVOCATION A -- the legacy back-end.  The mode is spelled out even though unset means
# `absent`, and the negative filter is the one part that is NOT optional.
# Measured: 568 tests from 23 suites ran (the elapsed time varies from run to run;
# the counts do not), 568 PASSED, exit 0.
CEC_TEST_AIDL_MODE=absent \
  ./run_L1Tests --gtest_filter='-DriverAidlSessionTest.*:DriverAidlTransmitTest.*' \
                --gtest_print_time=1
```

`./run_L1Tests` is the libtool wrapper in this directory, **not** `.libs/run_L1Tests`.  Run it bare
and it **exits 1** — measured: `607 tests from 25 test suites ran`, `568 PASSED`, **`39 FAILED`**.
The 39 are the two AIDL-only fixtures, which assert in `SetUp` that the AIDL back-end is the
resolved one and **fail rather than skip** when it is not, deliberately, because a skipped arm is
indistinguishable from a passing one in an aggregate count.  They are `DriverAidlSessionTest` 27 and
`DriverAidlTransmitTest` 12 as measured, so the failing figure is whatever those two fixtures hold
between them and it moves with them — re-measure with `--gtest_list_tests` after adding a case
rather than trusting the total here.  See
[Back-End Selection](#back-end-selection-five-invocations).

Root `make check` does **not** run this suite: the top-level `Makefile.am` declares
`SUBDIRS = osal ccec`, so `check` never descends into `tests/`.  **And it does not omit the
recursion quietly — there is no `check` target at the configured root, so the command fails:
`make: *** No rule to make target 'check'.  Stop.`, measured exit 2.**  That is `make` reporting a
target that does not exist, not a broken tree.  Use
`make -C tests/L1Tests check` from the submodule root when you want automake to drive it — and
supply the filter as an **environment variable** there, because `make check` runs the binary itself
and takes no gtest arguments:

```bash
export GTEST_FILTER='-DriverAidlSessionTest.*:DriverAidlTransmitTest.*'
make -C tests/L1Tests check     # measured: PASS: run_L1Tests, "# TOTAL: 1  # PASS: 1", exit 0
```

### Run Specific Tests
```bash
./run_L1Tests --gtest_filter="CECFrameTest.*"
./run_L1Tests --gtest_filter="*Mutex*"
```

The second filter matches the 11 `MutexTest` cases in `osal/test_Mutex.cpp`.  Use
`--gtest_list_tests` to see the fixture names a filter can select.

### Measure Coverage
```bash
cd tests/L1Tests
./run_coverage.sh --build --run     # build, run every invocation, capture, report, gate
```

Artifacts have **no fixed default path**.  With no `--output-dir` the script
mints a fresh directory per run with `mktemp -d` at mode 0700 (files 0600)
and prints it as the `artifacts:` line at startup — **read that line to find
the files.**  The **parent** is not `/tmp` by assumption: the script takes the
first of **`$TMPDIR`, `$XDG_RUNTIME_DIR`, `$HOME`** whose whole ancestry
passes a custody check, and **skips** a group- or world-writable parent that
has no sticky bit rather than warning about it.  On a container whose `/tmp`
is mode 2777 that means the run lands under **`$HOME`** — measured here as
`/root/hdmicec-l1-coverage.XXXXXXXX`, with the private lcov `HOME` and the
Makefile snapshot alongside it.  The same check applies to a directory **you**
name, and there it is **fatal**: `--output-dir` under such a `/tmp` dies with
`/tmp has mode 2777 -- writable by group or world, without the sticky bit`
and names the two remedies (`chmod +t /tmp`, or a `TMPDIR` you own at 0700).
An unpredictable name cannot be pre-created by another local account, and it
is outside the git tree because `.gitignore` does not cover these names.
Pass `--output-dir DIR` (or set `COVERAGE_OUTPUT_DIR`) when you need a stable
location; `--output-dir .` reproduces the CI layout in-tree, at which point
they are build artifacts that must not be committed.  **Reusing a directory
is safe** — the script purges its own enumerated artifact names under the run
lock, never the directory recursively, so it holds exactly one generation.

The names, so you can find one without listing the directory:
`run_status.txt` (the authoritative verdict, carrying the run id and a Phase
of `IN-PROGRESS` → `COMPLETE-PASS` / `COMPLETE-FAIL` / `CANCELLED`; it is
**purged and invalidated with the rest of the previous generation**, its
opening `IN-PROGRESS` publication is a **hard precondition** for the run, and
a failure to publish the **final** marker makes an otherwise-successful run
exit non-zero — read it before anything else),
`run_invocation_<A..E>.log` and `rdkTestResults_invocation_<A..E>.json` (one
pair per invocation that **ran** — a missing letter means that invocation did
not run in this generation, and `run_status.txt` says so per letter),
`coverage.info` (unfiltered, what the branch-arm gate reads),
`filtered_coverage.info` and `line_gate_coverage.info` (what the line gate
reads), `per_file_coverage.tsv`, `uncovered_lines.txt`, `provenance.txt`, the
per-step logs, and `coverage/index.html`.

The per-file gate is on by default, so a single file below 80% fails the run.
**Exit status: 0** is the only pass; **1** is a missing prerequisite, a failed
step or a failed gate; **3 is ADVISORY and is not a pass** — the figures are
real but this invocation did not establish them, and `--help` enumerates all
**nine** triggers, of which *"one or more invocations were DEFERRED"* is the
one a host with no Binder driver always hits.  `--threshold` takes digits or
digits.digits between 0 and 100, and anything other than 80 is itself
advisory.  **The range is enforced lexically, on the value's own digits, and it
is enforced BEFORE the run lock, the tooling pre-flight, the counter zeroing
and the artifact directory** — so a refused bar never disturbs the tree or an
existing measurement.  Arbitrarily large values are refused too, including ones
past the shell's own integer range: `--threshold=9223372036854775808` exits 1
with *"threshold must be between 0 and 100"*, the same as `101`.  That is worth
stating because it did not always hold — an arithmetic `[ -gt ]` returns status
**2**, not false, for a value the shell cannot hold, so such a bar used to pass
validation untouched and reach `lcov`.  `./run_coverage.sh --selftest` holds
the guard to both ends of that range as step 3 of 6 and runs no suite; steps 4
and 5 are the two guards added since -- the untrusted-value renderer and the
direct-object loader guard below.

**Every action of `run_coverage.sh`, `--help` and `--selftest` and `--restore`
included, is REFUSED when `LD_PRELOAD`, `LD_AUDIT`, `LD_PROFILE`,
`LD_DYNAMIC_WEAK` or `LD_ORIGIN_PATH` is set.**  Those name objects to load or
change how symbols bind, so the loader-path rebuild this script performs does
not reach them, and every child of a run -- `lcov`, `gcov`, `genhtml`, `awk`,
`git`, `make`, both test binaries, the fake host -- would have run someone
else's object before its own `main`.  The check runs before the script resolves
its own path, and the refusal prints its remedy:
`env -u LD_PRELOAD ./run_coverage.sh --build --run`.  The same five are removed
from every child environment as well.  A set-but-EMPTY value is warned about and
unset rather than accepted, because glibc activates `LD_DYNAMIC_WEAK` on
presence and ignores its value.
A missing or unusable `flock` and an unsafe artifact-directory
ancestry are **fatal**, not advisory — as is a suite process that **outlives**
its invocation, which on the L2 tier is the fake service host holding the
global `"HdmiCec"` registration: it fails that invocation and the run, and is
terminated and named rather than warned about.  See `./run_coverage.sh --help`
for the full option and environment reference.

On a host that can only run some of the invocations, expect a non-zero exit: the AIDL-path
production files are reachable only from the invocations being deferred, so they stay below the
per-file bar.  That is the gate working, not a threshold to lower — do not pass
`--no-per-file-gate` or exempt a file to make it green.

## Back-End Selection: Five Invocations

`Driver::getInstance()` chooses between the two HAL back-ends **once per process**, inside
`LibCCEC::init` — the first thing this harness does that forces the factory.  By the time any
`TEST_F` body runs the choice is made, so **each outcome needs its own process** and no filter can
make one run exercise both arms.  **Ordering is load-bearing:** on B and C the fake must be
registered, and on E the host must have signalled ready, **before** `init` — afterwards the
selection is already on legacy and a green run proves nothing.

`./run_coverage.sh --run` drives all five, one process each, each with its own
`run_invocation_<letter>.log` that a later invocation cannot overwrite:

| Inv | Binary | `CEC_TEST_AIDL_MODE` | Selection | Cases |
|--------|-------------|---------|---------|---------|
| A | `run_L1Tests` | `absent` | Legacy | 568, measured — the pre-existing 483 plus 85 contract cases |
| B | `run_L1Tests` | `compatible` | AIDL (local interface) | the AIDL contract arms plus the 308 back-end-neutral cases — selects 423 here; measured green at 422 of 422 on the tree that registered 606 |
| C | `run_L1Tests` | `incompatible` | Legacy, rejection logged | 76 contract cases plus the same 308 — 384, measured green |
| D | `run_L2Tests` | `absent` | Legacy | the `DualPath*` round trip through the in-process legacy mock |
| E | `run_L2Tests` | `remote` | AIDL (remote proxy) | the same round trip over real Binder IPC |

**483 is the pre-existing legacy-path baseline**, and the binary now registers 607 cases in 25
fixtures.  Every total above is measured: A and D on any host, and B, C and E on the Binder-capable
guest that `.github/workflows/aidl-path-tests.yml` provisions, where all five invocations ran green
on 2026-09-01, and again in the acceptance runs of 2026-09-02 (E: 15 registered, 9 passed, 6
skipped, which is the measurement that discharged the earlier 14-registered run's 8-passed figure
and the expectation recorded alongside it; B selected 422 of the 606 the tree registered then, and
selects 423 now that the over-length inbound case has been added).  A host without a Binder driver cannot
reproduce B, C or E at all — `run_coverage.sh` reports those three DEFERRED there, never as passes.  The counts are documentation, not the gate — `run_coverage.sh` asks the binary how many
cases each filter selects, every time.

The five commands, with the filter each invocation actually uses (`INVOCATION_MATRIX` field 5 in
`run_coverage.sh`).  **Every filter is written out in full and each command is copy-pasteable as it
stands** — a `--gtest_filter` value is quoted, so the shell substitutes nothing inside it and a
placeholder would reach GoogleTest as literal text.  The ten *back-end-neutral* suites B and C
both name are `CECFrameTest`, `MessageDecoderTest`, `MessageDecoderTrackingTest`,
`MessageEncoderTest`, `OpCodeTest`, `OperandsTest`, `UtilTest`, `ConditionVariableTest`,
`MutexTest` and `ThreadTest`:

```bash
cd tests/L1Tests
# A -- measured: 568 selected / 39 excluded / 607 registered; 568 passed, 0 skipped; exit 0.
CEC_TEST_AIDL_MODE=absent ./run_L1Tests \
  --gtest_filter='-DriverAidlSessionTest.*:DriverAidlTransmitTest.*'

# B -- DEFERRED here; needs a Binder driver.  No pass count is claimed.  The filter itself is
# valid and selects 423 cases from 15 suites here, measured with --gtest_list_tests.
CEC_TEST_AIDL_MODE=compatible ./run_L1Tests \
  --gtest_filter='DriverAidl*:CECFrameTest.*:MessageDecoderTest.*:MessageDecoderTrackingTest.*:MessageEncoderTest.*:OpCodeTest.*:OperandsTest.*:UtilTest.*:ConditionVariableTest.*:MutexTest.*:ThreadTest.*-DriverAidlSelectionTest.*:DriverAidlLegacyArmTest.*'

# C -- DEFERRED here; needs a Binder driver.  No pass count is claimed on this host.  It selects 384
# cases from 13 suites here, measured the same way.
CEC_TEST_AIDL_MODE=incompatible ./run_L1Tests \
  --gtest_filter='DriverAidlCompatibilityTest.*:DriverAidlPreflightTest.*:DriverAidlLocalInstanceTest.*:CECFrameTest.*:MessageDecoderTest.*:MessageDecoderTrackingTest.*:MessageEncoderTest.*:OpCodeTest.*:OperandsTest.*:UtilTest.*:ConditionVariableTest.*:MutexTest.*:ThreadTest.*'

cd ../L2Tests
# D -- measured: 14 executed, 10 PASSED, 4 SKIPPED, exit 0.  The 4 are the DualPathAidlFlowTest
# cases, which this invocation PERMITS to skip; a skip anywhere else fails it.
CEC_TEST_AIDL_MODE=absent ./run_L2Tests --gtest_filter='DualPath*'
# or through automake, which supplies CEC_FAKE_AIDL_HOST_PATH itself.  No -C here: this block is
# already in tests/L2Tests, so make -C tests/L2Tests check would look for a nested path that does
# not exist and stop with exit 2.
make check      # measured: PASS: run_L2Tests, "# TOTAL: 1  # PASS: 1", exit 0

# E -- DEFERRED here.  CEC_FAKE_AIDL_HOST_PATH is required by THIS mode only, and 8 of the 14
# are mandatory: only DualPathLegacyFlowTest.* may skip.
CEC_TEST_AIDL_MODE=remote CEC_FAKE_AIDL_HOST_PATH="$PWD/fake_hdmi_cec_aidl_host" \
  ./run_L2Tests --gtest_filter='DualPath*'
```

**Skips are named, not counted.** Each invocation declares the *only* fixtures it may report as
SKIPPED — nothing for A, B and C, `DualPathAidlFlowTest.*` for D, `DualPathLegacyFlowTest.*` for E —
and a skip outside that set fails the invocation **with its name**.  A tolerance of "up to four
skips" would have accepted four skipped *mandatory* cases while four permitted ones ran.

### `CEC_TEST_AIDL_MODE`
The only switch.  Read by `tests/L1Tests/test_main.cpp` and `tests/L2Tests/test_main.cpp` and **by
no production source**:

- **`absent`** — register nothing; the legacy back-end is selected and libbinder is never touched.
  **Unset and empty both mean this.**
- **`compatible`** — register an in-process fake reporting its real, frozen metadata; AIDL is
  selected.
- **`incompatible`** — register an in-process fake whose interface hash is `"-1"`; the service is
  *present* and rejected, so legacy is selected and the rejection is logged.
- **`remote`** — launch the out-of-process fake host.  `run_L2Tests` only; in `run_L1Tests` it is a
  hard failure naming the other runner.

An unrecognised value is a hard failure, never a quiet fall back to `absent`.

### The L2 tier
`tests/L2Tests` builds `run_L2Tests` and `fake_hdmi_cec_aidl_host`, and they **must** be separate
processes: an in-process registration yields no proxy, no driver transaction and no
client-threadpool involvement, so it cannot prove the transport works.  `CEC_FAKE_AIDL_HOST_PATH`
tells the harness where the host binary is; it is set by the build, is **required by `remote` only**
and is ignored under `absent`.  Two more variables name **inherited descriptor numbers**, are set by
the harness immediately before it execs the host, and **must never be set by a caller** — no
workflow, script or documented command supplies either:

- **`CEC_FAKE_HOST_READY_FD`** — the pipe write end the host writes one fixed readiness token to.
  The harness does a bounded blocking wait on the read end (30 s) and fails if it expires.
- **`CEC_FAKE_HOST_CONTROL_FD`** and **`CEC_FAKE_HOST_OBSERVE_FD`** — the control channel, and it is
  **two separate unidirectional pipes, not a `socketpair`**: `CONTROL_FD` is the **read** end the
  host takes commands from, `OBSERVE_FD` the **write** end it sends replies back on.  Eight
  lowercase verbs, one line each: `ping`, `deliver <hex>`, `sent-count`, `last-sent`, `open-count`,
  `close-count`, `listener` and `shutdown`.  **Every reply begins `OK ` or `ERR `** — `OK pong`,
  `OK delivered <n>`, `OK last-sent <hex>`, and named failures such as `ERR no-listener`,
  `ERR bad-hex`, `ERR bad-args <verb>`, `ERR no-controller`.  A reply is awaited at most **10 s**
  and capped at `PIPE_BUF` (4096) bytes, so it arrives whole or not at all.
  Unset simply means no parent wants to drive events (the standalone-run case); set-but-unusable is
  a hard failure with its own exit code (**8** for a mis-wired channel, **9** for one that breaks
  mid-run).  This channel is what lets invocation E assert the
  **inbound** leg, which is why **8 of E's 14 cases were mandatory** when E was last run -- the 6
  `DualPathLegacyFlowTest` cases are E's permitted skips, exactly as the 4 `DualPathAidlFlowTest`
  cases are D's.  The tier has since gained `DualPathHostLifecycleTest`, which is mandatory under
  both, so the mandatory counts are now 11 of 15 for D (measured) and 9 of 15 for E (expected on a
  re-run).  Neither number is written down anywhere the run consults: `run_coverage.sh` derives
  "the selection minus the permitted set" from the binary each time.

`README.md` has the detail.

### What this host can actually run
**Compiling and linking works anywhere** — that needs only headers and libraries.  *Executing* an
AIDL-path case needs three more things: a kernel with Binder support, a Binder protocol version
matching the one `libbinder` was built for, and a running `servicemanager`.  Measured on the host
this file was last revised on — **kernel 6.12.85+, `CONFIG_ANDROID_BINDER_IPC` not set, no
`/dev/binder*`** — so **A and D are the two invocations runnable here**, and **B, C and E cannot
run at all**: registering a name needs `defaultServiceManager()`, which opens the driver and
*aborts the process* when the node is missing.  `run_coverage.sh` reports those three as
**DEFERRED**, never attempts them and never counts them as passed.  **No result for B, C or E is
claimed anywhere in this file**; they belong to the Binder-capable CI job.

## Test Executable

- **Name**: `run_L1Tests`
- **Location**: `tests/L1Tests/`
- **Type**: Google Test executable
- **Sources**: 16 test translation units + test_main.cpp + the two mock sources (all listed in
  `run_L1Tests_SOURCES`, which is the only gate on what gets compiled — and the order in that
  list is significant, so append at the end of a group rather than inserting)
- **Total Tests**: 607 cases registered in 25 fixtures, none disabled — measured with
  `./run_L1Tests --gtest_list_tests`.  That is the pre-existing **483 in 18 fixtures**, still
  unmodified, plus the 124 contract cases in `ccec/test_DriverAidl.cpp`.  No single invocation runs
  all 607: see [Back-End Selection](#back-end-selection-five-invocations).  There are more fixtures
  than translation units because some files declare more than one — `ccec/test_LibCCEC.cpp`
  (`LibCCECTest` and `LibCCECUninitializedTest`) and `ccec/test_MessageDecoder.cpp`
  (`MessageDecoderTest` and `MessageDecoderTrackingTest`) among them

## Key Features

- ✅ **Conditional Build**: Only builds when `--enable-l1tests` is specified
- ✅ **Backward Compatible**: Existing tests (BasicTest, CECCmd, etc.) unchanged
- ✅ **Clean Separation**: L1 tests in dedicated subdirectory
- ✅ **Google Test Framework**: Industry-standard C++ testing
- ✅ **Automated Testing**: `TESTS = run_L1Tests` in this directory's `Makefile.am`, so
  `make -C tests/L1Tests check` drives the suite (root `make check` does not — see above)

## Configuration Options

| Option | Description | Default |
|--------|-------------|---------|
| `--enable-l1tests` | Enable L1 unit tests | `no` |
| `--disable-l1tests` | Disable L1 unit tests | N/A |
| `--with-halif-prefix=DIR` | Staged `rdk-halif-aidl` tree | sibling `../rdk-halif-aidl` |
| `--with-binder-sdk=DIR` | Staged Binder SDK root | none — must resolve |

`HALIF_PREFIX`, `HALIF_LIB_DIR`, `BINDER_SDK_DIR` and `BINDER_SDK_INCLUDE_DIR` are the equivalent
`configure` variables.  Neither `--with-` option is optional in effect: both back-ends are in every
build, so `configure` fails naming the roots it tried when a prefix does not resolve.

## Next Steps

1. Provide GoogleTest so that `pkg-config` can find a `gtest.pc` — either
   `sudo apt-get install libgtest-dev libgmock-dev`, or a source build into a private prefix
   exported as `GTEST_PREFIX` (see [Quick Start](#quick-start))
2. Stage `rdk-halif-aidl` and the Binder SDK, and know the two prefixes (see
   [The AIDL/Binder dependency](#the-aidlbinder-dependency))
3. Generate the stubs and the HAL-mock symlink, then `autoreconf -if` and
   `./configure --enable-l1tests` with `CPPFLAGS`/`PKG_CONFIG_PATH` and the two prefixes set
4. Build: `make all` **and then** `make -C tests/L1Tests all` **and then**
   `make -C tests/L2Tests all`
5. Test — **invocation A**, and both the mode and the filter are part of the command.  Two forms,
   and **each has its own working directory** — a `make -C tests/L1Tests` issued from inside
   `tests/L1Tests` looks for a nested `tests/L1Tests` and stops with exit 2:
   - **from `tests/L1Tests`**, the runner directly:
     `CEC_TEST_AIDL_MODE=absent ./run_L1Tests --gtest_filter='-DriverAidlSessionTest.*:DriverAidlTransmitTest.*'`
     — measured **568 of 568 passed, exit 0**, and the case counts are the runner's own
   - **from the submodule root**, through automake:
     `export GTEST_FILTER='-DriverAidlSessionTest.*:DriverAidlTransmitTest.*'` and then
     `CEC_TEST_AIDL_MODE=absent make -C tests/L1Tests check` — measured `PASS: run_L1Tests`,
     `# TOTAL: 1  # PASS: 1`, exit 0.  Automake counts **one test, the whole binary**; it does not
     report the 568, so do not expect the runner's figures from this form

   **A bare `./run_L1Tests` exits 1** — see [Run Tests Manually](#run-tests-manually)
6. Test the L2 tier — **invocation D**.  Same two forms, same working-directory rule:
   - **from `tests/L2Tests`**: `CEC_TEST_AIDL_MODE=absent ./run_L2Tests` — measured **15 executed,
     11 passed, 4 permitted skips, exit 0**.  A bare `make check` from this directory is the same
     route through automake and also exits 0
   - **from the submodule root**: `make -C tests/L2Tests check` — measured `PASS: run_L2Tests`,
     `# TOTAL: 1  # PASS: 1`, exit 0.  Again **one test, the whole binary**: the 15/11/4 belong to
     the runner form above, not to this one

Or let `./run_coverage.sh --build --run` drive the matrix: it runs A and D here and reports
**B, C and E as DEFERRED**.

See **UNIT_TEST_SETUP.md** for complete documentation.
