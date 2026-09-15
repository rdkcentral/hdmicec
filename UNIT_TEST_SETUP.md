# Unit Test Framework Setup Guide

## Overview

A comprehensive L1 unit test framework has been created for the hdmicec library using **Google Test (gtest/gmock)**.

## Why Google Test?

**Google Test** was selected as the best framework for this project because:

1. ✅ **C++ Native**: Perfect for your C++ codebase (ccec/*.cpp, osal/*.cpp)
2. ✅ **Industry Standard**: Widely adopted, excellent documentation and community support
3. ✅ **Rich Features**: Built-in mocking (gmock), fixtures, parameterized tests, death tests
4. ✅ **RDK Ecosystem**: Commonly used in RDK projects
5. ✅ **Easy Integration**: Works seamlessly with autotools build system
6. ✅ **Modern**: Supports C++11/14/17 features used in your code

### Alternatives Considered

- **CUnit**: C-only framework, awkward for C++ classes and namespaces ❌
- **CppUnit**: Older, less maintained, verbose syntax ❌
- **Catch2**: Good but header-only increases compile times ⚠️
- **Boost.Test**: Heavy dependency, overkill for this project ⚠️

## Framework Structure

```
tests/L1Tests/
├── README.md                     # Documentation
├── QUICK_START.md                # Build-and-run quick reference
├── Makefile.am                   # Build configuration; run_L1Tests_SOURCES is the only
│                                 #   gate on what is compiled, and its order matters
├── run_coverage.sh               # gcov/lcov runner with the >=80% line-coverage gate
├── .lcovrc_l1                    # lcov configuration, branch collection enabled
├── .gitignore                    # Ignores the build output: *.o, .deps/, .libs/, Makefile,
│                                 #   run_L1Tests and the *.log/*.trs/*.xml result files
├── test_main.cpp                 # Test runner entry point
├── ccec/                         # CCEC library tests
│   ├── test_CECFrame.cpp         # CECFrame class tests
│   ├── test_Connection.cpp       # Connection class tests
│   ├── test_Bus.cpp              # Bus dispatch, filtering and send-retry tests
│   ├── test_LibCCEC.cpp          # LibCCEC singleton tests
│   ├── test_MessageEncoder.cpp   # Message encoding tests
│   ├── test_MessageDecoder.cpp   # Message decoding tests
│   ├── test_OpCode.cpp           # OpCode enum/class tests
│   ├── test_Operands.cpp         # PhysicalAddress/LogicalAddress tests
│   ├── test_Driver_Mock.cpp      # Mock driver verification
│   ├── test_Driver.cpp           # Driver open/close/write/address tests
│   ├── test_DriverImpl_Async.cpp # Async transmit, invalid state, error paths
│   ├── test_Util.cpp             # Log-level configuration and buffer dump
│   └── test_DriverAidl.cpp       # AIDL back-end contract suite: back-end selection,
│                                 #   version compatibility, the bounded preflight and
│                                 #   the adapter's translation arms
└── osal/                         # OSAL library tests
    ├── test_ConditionVariable.cpp # Condition variable tests
    ├── test_Mutex.cpp             # Mutex lock/unlock and copy-semantics tests
    └── test_Thread.cpp            # Thread construction, dispatch and detach tests

tests/L2Tests/
├── Makefile.am                   # Declares TWO noinst_PROGRAMS -- see section 2
├── .gitignore                    # Ignores this tier's build output, run_L2Tests and
│                                 #   fake_hdmi_cec_aidl_host included
├── test_main.cpp                 # Runner entry point; owns the fake service host's
│                                 #   lifecycle -- launch in its own process group, readiness
│                                 #   wait, group-wide bounded teardown, reap.  Also carries
│                                 #   DualPathHostLifecycleTest, the one case that proves it
└── ccec/
    └── test_DualPathIntegration.cpp # One CCEC round trip, asserted against each
                                     #   HAL back-end over its real transport.  14 cases,
                                     #   the tier's 15th being DualPathHostLifecycleTest in
                                     #   test_main.cpp;
                                     #   the legacy arm runs anywhere, the AIDL arm needs
                                     #   a Binder-capable host and skips without one

mocks/hdmicec/                    # Not a test directory: the seams both tiers build against
├── hdmi_cec_driver.h             # Symlinked over the driver header (see step 3a)
├── hdmi_cec_driver_mock.{h,cpp}  # The legacy in-process HAL mock
├── fake_hdmi_cec_aidl_service.{h,cpp} # Test-scope fake com.rdk.hal.hdmicec service
└── fake_hdmi_cec_aidl_service_host.cpp # Hosts that fake in a process of its own
```

Every file listed above exists on disk and is git-tracked: `git ls-files tests/L1Tests`
returns exactly those 23 files and nothing else, and `git ls-files tests/L2Tests` the four
above (`ccec/` and `osal/` are the directories holding them).  Anything else you see in
either directory after a build -- `*.o`, `*.gcno`, `*.gcda`, `.deps/`, `.libs/`, `Makefile`,
`Makefile.in`, `run_L1Tests`, `run_L2Tests`, `fake_hdmi_cec_aidl_host`, `*.log`, `*.trs` --
is generated output matched by `.gitignore`.  That reading is per directory rather than
tree-wide -- `cfg/` holds 19 generated helper files beside the one **tracked** file
`cfg/Makefile.am`, which is all `git ls-files cfg/` names -- so sweep generated output
with `git clean -xd`, which never removes a tracked file, rather than by deleting a
directory whole.  Neither production library source list references `mocks/`, which is
why nothing under it can reach a shipped binary.

## Test Coverage

### CCEC Library Tests (12 test translation units, 456 tests)
Counts below are measured with `./run_L1Tests --gtest_list_tests`; re-measure with that
command after adding tests rather than trusting this list.
- **CECFrame** (31 tests): Constructor, copy operations, serialization, buffer management, hex dump, boundary sizes
- **Connection** (66 tests): Object creation, lifecycle management, open/close, listener registration, address filtering, and the `IntegrationFlowTest` cross-layer cases (6) declared in the same translation unit
- **Bus** (37 tests): Listener dispatch and filtering, send retries and timeout behaviour
- **LibCCEC** (14 tests): Singleton pattern, initialization/termination, logical/physical address management, and the uninitialised-state guards that throw.  Split across two fixtures: `LibCCECTest` (8 cases) never terminates the library, and `LibCCECUninitializedTest` (6 cases) owns the library's lifecycle per case -- `SetUp()` reaches the uninitialised state through `ensureTerminated()`, which waits for the bus reader to publish RUNNING before calling `term()`, and `TearDown()` restores the initialised `CEC_TEST` state through `ensureInitialized()` -- so no other fixture ever observes an uninitialised library
- **MessageEncoder** (27 tests): Encoding CEC messages through MessageEncoder
- **MessageDecoder** (54 tests): Comprehensive decoding of all 60+ CEC opcodes, polling messages, error handling, opcode tracking
- **OpCode** (82 tests): Complete GetOpName() coverage for all CEC opcodes, OpCode class methods (constructor, serialize, print)
- **Operands** (75 tests): All operand types including PhysicalAddress, LogicalAddress, DeviceType, Version, PowerStatus, AbortReason, OSDString, OSDName, Language, VendorID, UICommand, SystemAudioStatus, AudioStatus, RequestAudioFormat, ShortAudioDescriptor, AllDeviceTypes, RcProfile, DeviceFeatures, LatencyInfo
- **Driver** (22 tests) and **mock driver** (10 tests): Open/close, synchronous write, address management, mock verification
- **DriverImpl async** (26 tests): Asynchronous transmit, the transmit-completion callback, invalid-state guards and error-injection paths
- **Util** (12 tests): The log-level configuration read path -- every recognised level mapped to its numeric setting, plus the missing-file, empty-file, unrecognised-key and short-prefix arms that leave it unchanged -- and the hex buffer dump: emitted at debug and trace, silent below them, with zero-length and maximum-length buffers

The counts above sum to the 456 in the heading -- 12 figures across 11 bullets, because
**Driver** and **mock driver** are separate translation units counted on one line, and
**LibCCEC** and **MessageDecoder** each declare two fixtures inside one translation unit.

### OSAL Library Tests (3 test translation units, 27 tests)
- **ConditionVariable** (5 tests): Notify/wait synchronization patterns, signalling, timed wait and timeout behavior, the absolute-deadline computation when the requested wait carries the nanosecond field past one second (it must roll into `tv_sec` rather than reach `pthread_cond_timedwait` out of range), and the native-handle accessor
- **Mutex** (11 tests): Default construction, lock/unlock including the recursive case where each `lock()` needs a matching `unlock()`, native-handle retrieval, copy construction, copy assignment, chained and self-assignment, and a copy owning a distinct handle usable independently of the original. `Mutex` has no try-lock operation, so none is tested
- **Thread** (11 tests): Both constructor overloads (unnamed, and named with empty and long names), `run()` dispatching to the `Runnable`, `start()` dispatching it on another thread, `detach()`, and destruction -- at scope exit, after `detach()`, and without a `start()`. `stop()` and `getNativeHandle()` are declared in `Thread.hpp` but never defined, so neither links; there is no join operation

**483 test cases in 18 fixtures, all passing, none disabled.**  There are more fixtures
than translation units because `ccec/test_LibCCEC.cpp` declares `LibCCECTest` and
`LibCCECUninitializedTest`, and `ccec/test_MessageDecoder.cpp` declares `MessageDecoderTest`
and `MessageDecoderTrackingTest`.

Everything above is the **legacy-path baseline**: the units enumerated in this section, unmodified.
The binary also registers the AIDL contract suite from `ccec/test_DriverAidl.cpp` -- **124 cases in
7 fixtures**, so `run_L1Tests` now registers **607 cases in 25 fixtures** -- and the L2 tier is a
separate binary again, with **15 cases in 4 fixtures** -- the three integration fixtures in
`ccec/test_DualPathIntegration.cpp` and `DualPathHostLifecycleTest` in `test_main.cpp`.

**Those 124 are counted here and nowhere else in this guide**, so that no second copy of the total
can drift away from this one: a passage below that splits them names this same total and points
back here, and carries no count of its own.  Measured with `--gtest_list_tests`, the seven fixtures
group by the back-end each one needs resolved -- `DriverAidlCompatibilityTest` (23),
`DriverAidlPreflightTest` (28) and `DriverAidlLocalInstanceTest` (25) are back-end independent and
run under every L1 invocation, which is 76 cases; `DriverAidlSelectionTest` (4) and
`DriverAidlLegacyArmTest` (5) need the legacy back-end resolved, 9; `DriverAidlSessionTest` (27)
and `DriverAidlTransmitTest` (12) need the AIDL back-end resolved, 39.  Those sum to the 124 above,
and the 85 the default invocation selects is the first two groups: 76 + 9.

**Registered is not run, and this guide keeps the two apart everywhere.**  No single invocation
runs the whole set, because a fixture cannot opt into a different back-end from its own `SetUp`
-- see [Back-End Selection](#back-end-selection-the-invocation-matrix).  The default invocation
selects **568 of the 607** and a measured run passes all 568; the other 39 are the two AIDL-only
fixtures.  Re-measure with `--gtest_list_tests` on your own build rather than trusting any figure
here, per the note above.

## Installation Steps

### 1. Install the build tooling and Google Test

`configure` has hard external dependencies and will not proceed without them:
`PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 0.10.28])`,
`PKG_CHECK_MODULES([GTEST], [gtest >= 1.10.0])`, and the AIDL/Binder checks covered under
[Configure and Build](#3-configure-and-build) — those last ones unconditional rather than gated
on `--enable-l1tests`, because `libRCEC` carries both HAL back-ends in every build.  Note what
the first two imply — **`PKG_CHECK_MODULES` consults `pkg-config` only and ignores `-I`/`-L`
entirely**, so GoogleTest has to be reachable as a `gtest.pc`, not merely as headers and archives
on a path.

#### Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install libgtest-dev libgmock-dev cmake
```

#### Build from source (if the packages ship headers without libraries):

Build **unprivileged** and install into a prefix you own.  Root is not needed to compile, and
copying archives straight into `/usr/lib` leaves files the package manager does not track in a
directory it does manage, where they can shadow a later distribution update.  GoogleTest and
GoogleMock build together from one source tree:

```bash
cmake -S /usr/src/googletest -B "$HOME/gtest-build" \
      -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build "$HOME/gtest-build"
cmake --install "$HOME/gtest-build"        # writes only under $HOME/.local
```

Then point `configure` at that prefix, leaving the system directories untouched:

```bash
CPPFLAGS="-I$HOME/.local/include" LDFLAGS="-L$HOME/.local/lib" ./configure --enable-l1tests
```

Add `$HOME/.local/lib` to `LD_LIBRARY_PATH` when running `run_L1Tests` if the shared libraries
were built there.  Elevated privilege belongs to the package-manager step above and nowhere
else in this procedure.

#### RHEL/CentOS:
```bash
sudo yum install autoconf automake libtool pkgconfig gcc-c++ make cmake \
                 glib2-devel gtest-devel gmock-devel
```

#### Build GoogleTest from source when the packaged copy is too new
This suite compiles at `-std=c++17` (`AM_CXXFLAGS` in `tests/L1Tests/Makefile.am`), raised from
`-std=c++14` by the AIDL/Binder dependency.  GoogleTest 1.15.0 is the version this project is
built and validated against, so a distribution copy that has moved past it is still worth
replacing with a pinned one.  Build it **unprivileged, into a private prefix**, and expose that
prefix to `configure`:

```bash
export GTEST_PREFIX="$HOME/.local/gtest-1.15.0"      # any writable directory

git clone --depth 1 --branch v1.15.0 https://github.com/google/googletest.git /tmp/googletest
cmake -S /tmp/googletest -B /tmp/googletest/build \
      -DCMAKE_INSTALL_PREFIX="$GTEST_PREFIX" \
      -DBUILD_GMOCK=ON -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build /tmp/googletest/build -j"$(nproc)"
cmake --install /tmp/googletest/build                # no sudo: the prefix is yours

# How configure then finds it (see step 3)
export PKG_CONFIG_PATH="$GTEST_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
```

> **Do not build in `/usr/src/gtest` as root and copy archives into `/usr/lib`.**  Earlier
> revisions of this document gave exactly that recipe.  It overwrites libraries the package
> manager owns, cannot be cleanly undone, and does not even solve the problem — nothing is
> written to `pkg-config`'s path, so `PKG_CHECK_MODULES([GTEST], ...)` still fails.  The private
> prefix above is the supported route and the one `tests/L1Tests/run_coverage.sh` uses.

### 2. Build System Configuration

The build system has been configured with the following changes:

#### configure.ac
- Added `--enable-l1tests` configure option
- Added Google Test dependency check
- Added `tests/L1Tests/Makefile` and `tests/L2Tests/Makefile` to AC_CONFIG_FILES
- Added the AIDL/Binder availability checks, which are **hard and unconditional**.  They are
  modelled on the mandatory `PKG_CHECK_MODULES([GLIB], ...)` above them: an absent Binder SDK or
  absent AIDL stub library **fails `configure`**, naming the roots it tried, rather than
  degrading to a build with one back-end.  There is deliberately no `--enable-aidl` and no
  Automake conditional — `libRCEC` carries both HAL back-ends in every build for every SOC
  vendor and chooses between them at run time
- Added two staging-prefix arguments, `--with-halif-prefix=DIR` and `--with-binder-sdk=DIR`, and
  their `configure`-variable equivalents `HALIF_PREFIX`, `BINDER_SDK_DIR`, plus `HALIF_LIB_DIR`
  and `BINDER_SDK_INCLUDE_DIR` for layouts that stage libraries or headers away from the prefix.
  Every include root and library directory is derived from those, so this build and the
  hand-written `ccec/src/Makefile` cannot end up configured with different sets.  `HALIF_PREFIX`
  falls back to a sibling `../rdk-halif-aidl` checkout when it is not given
- Added an explicit **C++17** check, so a toolchain that cannot compile the constructs the AIDL
  headers use, or that rejects `-std=c++17`, fails here rather than part-way through compiling
  `ccec/src/DriverAidlImpl.cpp`.  A toolchain whose *default* dialect is older but which accepts
  the flag is pinned to C++17 rather than rejected.  The flag it settles on is substituted for the
  consuming `Makefile.am` files — each of which pins the same flag with a literal of its own — and
  is not injected into the build's `CXXFLAGS`

#### Makefile.am (root)
- Added `tests` to DIST_SUBDIRS

#### tests/Makefile.am
- Conditionally includes the L1Tests **and L2Tests** subdirectories when `--enable-l1tests` is
  used — `SUBDIRS = L1Tests L2Tests` inside the existing `ENABLE_L1TESTS` conditional, so the
  L2 tier is reached by the same switch and needs no second one
- Maintains backward compatibility with existing test applications

#### tests/L1Tests/Makefile.am
- Defines `run_L1Tests` test executable
- Links against libRCEC.la and libRCECOSHal.la, plus the four AIDL/Binder link edges
- Includes all test source files
- Compiles at **`-std=c++17`**, raised from `-std=c++14`.  That is a requirement of the
  dependency and not a preference: `halcompat.h` opens a nested-namespace definition, which is
  C++17 syntax that does not compile at `c++14` at all, and the generated AIDL headers include
  `<optional>`
- Adds one translation unit, `ccec/test_DriverAidl.cpp`, **appended immediately after
  `ccec/test_Util.cpp`** so that it closes the `ccec/` group, with the HAL driver mock source
  still last in the list.  Order is load-bearing here: GoogleTest registers each `TEST_F` during
  static initialisation, that order follows the link order of the objects, which follows this
  list — so a new unit is APPENDED and never inserted (see `## Next Steps` below)

#### tests/L2Tests/Makefile.am
- Declares **two** `noinst_PROGRAMS`: `run_L2Tests`, the integration runner, and
  `fake_hdmi_cec_aidl_host`, the test-scope fake AIDL service in a process of its own.  Neither
  is installed, which is the structural half of the guarantee that the fake never reaches a
  shipped binary
- `TESTS = run_L2Tests` names the **runner only**.  The host is a helper the runner launches,
  waits on and reaps; listing it would make `make check` execute a service process that runs
  until it is signalled, hanging the build rather than failing it
- The legacy HAL mock source belongs to the **runner** — the legacy arm drives the legacy
  back-end through it, and without it that arm has no HAL at all
- The fake AIDL service source belongs to the **host** and never to the runner, because the two
  must be separate processes.  Compiling it into the runner would silently turn this tier back
  into the in-process case: libbinder resolves a name registered in the calling process to the
  local `BBinder`, so no proxy is created and no transaction crosses the driver (see
  [Back-End Selection](#back-end-selection-the-invocation-matrix))

### 3. Configure and Build

The whole sequence is automated — `cd tests/L1Tests && ./run_coverage.sh --build --run` builds
instrumented, runs every invocation it can, captures coverage and applies the 80% line gate.  What
follows is that same sequence by hand.

**Clear the direct-object loader variables from your shell before any of it.**  `run_coverage.sh`
refuses every action — `--build`, `--run`, `--help`, `--selftest` and `--restore` alike — when
`LD_PRELOAD`, `LD_AUDIT`, `LD_PROFILE`, `LD_DYNAMIC_WEAK` or `LD_ORIGIN_PATH` is set, and it also
removes those five from every child environment it launches.  They name objects to LOAD, or change
how symbols bind, so the loader **search path** the script rebuilds from validated roots does not
reach them: with one set, `lcov`, `gcov`, `genhtml`, `awk`, `git`, `make`, both test binaries and the
fake service host would each have run an unreviewed object before their own `main`, and the coverage
figures, the pass counts and the gate verdict would be that object's output as much as the code's.
The refusal happens at load time, before the script resolves its own path, and prints the remedy:

```bash
env -u LD_PRELOAD ./run_coverage.sh --build --run
```

A set-but-empty value is warned about and unset rather than accepted, because glibc activates
`LD_DYNAMIC_WEAK` on presence and ignores its value.  `LD_LIBRARY_PATH` is **not** in that set: it
names directories, the script rebuilds it deliberately from roots it has validated, and that rebuild
is unchanged.  Note also that the in-guest init the rootfs builder generates unsets the same five
before it exports the build environment, so the guest run starts from the same guarantee.

**On a host with no Binder driver, expect that command to exit `1`, and expect exactly one file to
be named.**  Measured on this tree, where only invocations A and D could run: **32 source files,
86.0% line coverage (2405/2795), 91.6% function, 52.5% branch** — the aggregate clears the 80% bar
— while the **per-file half fails** on `ccec/src/DriverAidlImpl.cpp` at **58.6%** (379/647).  That
figure is a property of the host rather than of the test set: the file is reached largely from the
deferred invocations, and the cases that reach it exist and are compiled in, but their invocations
are **deferred** — no binary is launched for them, so GoogleTest never sees them and they move no
counter.  That exit `1` is the expected outcome here and is **not a threshold to lower** — do not
pass `--no-per-file-gate`, do not exempt a file and do not add an exclusion glob.  Re-run the whole
matrix on the binder-capable job before treating a figure from a partial matrix as this suite's
coverage.

`ccec/src/Driver.cpp` is not named alongside it: that file measures **80.0% (12/15)** and passes.
It carries no unexecutable pair to depress the figure, because the selection helper reads the
reason for a fallback back from the single availability query it already made rather than calling
the Binder preflight a second time to decide what to report.  Read the 80.0% as *exactly* on the
bar and not as headroom — one further line unreachable without a driver puts the file below it.

Four prerequisites precede `configure`, and each is fatal rather than degrading if skipped:

- **Stub headers.**  The middleware includes IARM bus headers it does not ship.
- **The HAL driver mock symlink.**  It substitutes the GoogleMock HDMI-CEC driver for the real
  driver header, and it is the entire mocking seam — no test in this suite needs hardware.
- **`CPPFLAGS` reaching `mocks/` and `stubs/`, and `PKG_CONFIG_PATH` reaching a `gtest.pc`.**
- **The staged Binder SDK and AIDL stub libraries.**  `libRCEC` links the generated HDMI-CEC
  AIDL client stubs and the Binder client library unconditionally, so nothing here builds
  without them and there is no switch to turn them off.  Two staging prefixes are the whole of
  the contract; `configure` derives every path below from them, which is what keeps this build
  and the hand-written `ccec/src/Makefile` from being configured differently.

From those two prefixes come **four include roots**.  All four are required, and a root present
in one consumer and missing from another is the exact failure the single derived set exists to
prevent:

1. `<halif-prefix>/hdmicec/0.1.0.0/include` — the generated `com/rdk/hal/hdmicec/*.h` stubs.
2. `<halif-prefix>/common/0.2.0.0/include` — `com/rdk/hal/PropertyValue.h`, the frozen import the
   snapshot was generated against.
3. `<halif-prefix>/common/current` — the root that supplies **`halcompat.h`**.  That header is
   **not** inside `common/0.2.0.0/include`, so the two snapshot roots alone do not reach it.
   This is the single most commonly missed root; omitting it surfaces as
   `halcompat.h: No such file or directory`.
4. `<binder-sdk-prefix>/include/binder_sdk` — required by the generated headers themselves,
   which pull `<binder/IBinder.h>`, `<binder/IInterface.h>`, `<binder/Status.h>`,
   `<binder/Parcel.h>`, `<utils/String16.h>`, `<utils/StrongPointer.h>` and
   `<android-base/macros.h>`.

And **four link libraries, in dependency order**, from `<halif-prefix>/lib/halif` and
`<binder-sdk-prefix>/lib/binder`:

```text
-lhdmicec-v0.1.0.0-cpp -lcommon-v0.2.0.0-cpp -lbinder -lutils
```

Those four are the direct edges and the only ones to name.  `libbinder`'s own closure —
`liblog`, `libbase`, `libcutils` — must be **staged and on the loader path**, but must never be
added as direct edges.

`libcutils_sockets` is staged in the same directory and is **not** in that closure.  Measured
`DT_NEEDED` across the staged SDK: `libbinder.so` → `libutils libcutils libbase liblog`;
`libutils.so` → `libcutils liblog`; `libcutils.so` → `libbase liblog`; `libcutils_sockets.so` →
`libc` alone.  No staged object names it, `servicemanager` included, and none of its twelve
exported `socket_*` symbols is resolved by anything here.  It is a companion library the SDK build
stages, required only to be present and loadable in its own right — so it cannot be the cause of a
link failure, and looking for it as one wastes the diagnosis.  Put the Binder library directory on
`LD_LIBRARY_PATH` for the **link** as well as the run: `libbinder.so` carries no `RUNPATH` and
GNU ld does not search `-L` for a shared library's own dependencies, so an absent closure fails
`configure`'s link probe long before anything is executed.

> **Two things a reader will otherwise get wrong.**  **`common` is linked at 0.2.0.0, not
> 0.2.1.0** — that is the ABI the released `hdmicec@0.1.0.0` snapshot was generated and pinned
> against (`rdk-halif-aidl/hdmicec/0.1.0.0/CMakeLists.txt` links `common-v0.2.0.0-cpp` and
> includes `common/0.2.0.0/include`), while `rdk-halif-aidl/common/metadata.yaml` has moved on;
> linking the newer library puts a different ABI underneath pre-generated code.  And
> **`rdk-halif-aidl` is documented as sparse-checked-out to `hdmicec/` only** (see the workspace
> `README.md`), yet roots 2 and 3 live under `common/`, outside that path — so a fresh sparse
> clone fails on a missing `halcompat.h`.  Fix it with `git sparse-checkout add common` inside
> that checkout, or take a full one.  A tree whose sparse set has already been expanded will
> never show the symptom, which is exactly why it is written down here.

```bash
# From the hdmicec submodule root
export GTEST_PREFIX="$HOME/.local/gtest-1.15.0"     # or the distribution's prefix

# The two AIDL/Binder staging prefixes.  Omit HALIF_PREFIX when the rdk-halif-aidl
# checkout sits beside this submodule -- configure falls back to ../rdk-halif-aidl.
# HALIF_LIB_DIR and BINDER_SDK_INCLUDE_DIR are the split-staging overrides, needed
# whenever the built stubs or the Binder headers are not under their own prefix.
export HALIF_PREFIX="${HALIF_PREFIX:-$PWD/../rdk-halif-aidl}"
: "${HALIF_LIB_DIR:?the directory holding libhdmicec-v0.1.0.0-cpp}"
: "${BINDER_SDK_DIR:?the staged Binder SDK root holding lib/binder}"
: "${BINDER_SDK_INCLUDE_DIR:=$BINDER_SDK_DIR}"

# libbinder.so carries no RUNPATH, so its closure has to be findable to LINK against
# libRCEC, not merely to run it.
export LD_LIBRARY_PATH="$HALIF_LIB_DIR:$BINDER_SDK_DIR/lib/binder${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# 3a. Stub headers, and the HAL driver mock injected over the driver header
mkdir -p stubs/rdk/iarmbus stubs/ccec/drivers/iarmbus
touch stubs/rdk/iarmbus/libIARM.h \
      stubs/rdk/iarmbus/libIBus.h \
      stubs/rdk/iarmbus/libIBusDaemon.h \
      stubs/ccec/drivers/iarmbus/CecIARMBusMgr.h
ln -sf ../../../mocks/hdmicec/hdmi_cec_driver.h stubs/ccec/drivers/hdmi_cec_driver.h

# 3b. Generate build scripts and configure with L1 tests enabled.
#     Add -fprofile-arcs -ftest-coverage to CXXFLAGS and LDFLAGS for an instrumented build.
#     The four AIDL include roots, the four -l edges and the C++17 flag are DERIVED by
#     configure from the prefixes passed below -- do not spell them into CPPFLAGS by hand.
autoreconf -fi
PKG_CONFIG_PATH="$GTEST_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
CPPFLAGS="-I$PWD/mocks -I$PWD/stubs -I$GTEST_PREFIX/include" \
LDFLAGS="-L$GTEST_PREFIX/lib" \
  ./configure --enable-l1tests \
      --with-halif-prefix="$HALIF_PREFIX" \
      --with-binder-sdk="$BINDER_SDK_DIR" \
      HALIF_LIB_DIR="$HALIF_LIB_DIR" \
      BINDER_SDK_INCLUDE_DIR="$BINDER_SDK_INCLUDE_DIR"

# 3c. Build the libraries, THEN each test tier.  The extra passes are required:
#     the top-level Makefile.am declares SUBDIRS = osal ccec, so `make` at the
#     root builds no test binary at all.  The third pass is what produces the L2
#     runner and the out-of-process fake service host.
make -j"$(nproc)" all
make -C tests/L1Tests all
make -C tests/L2Tests all
```

Read the `configure` output back before trusting it.  It prints one `AIDL HDMI CEC back-end:`
line per resolved path plus the C++17 flag it settled on, so a prefix that resolved to the wrong
place is visible there rather than in a link error much later.

`autoreconf`/`configure` rewrite six git-tracked `Makefile` files in this submodule.  Five are
pure local toolchain output.  **`ccec/src/Makefile` is not** — it is hand-written RDK build
source, and it now carries this migration's own changes, yet it is listed in `AC_CONFIG_FILES`
all the same, so `configure` clobbers it by design.  `run_coverage.sh --build` therefore
snapshots all six from the working tree immediately **before** it invokes `autoreconf`, and
restores them from that snapshot on the way out of the **same** invocation — off the same trap
that handles cancellation, so an interrupted build restores too.  A standalone
`tests/L1Tests/run_coverage.sh --restore` restores from that invocation's snapshot; finding the
six dirty with no snapshot of its own, it reports that it cannot safely restore and **exits
non-zero** rather than falling back to `git checkout`.

> **Never revert those six with `git checkout`.**  A checkout restores whatever the *index*
> holds, which is a different claim from "what was there before `configure` ran" — and for
> `ccec/src/Makefile` it would silently revert the hand-written build's half of this migration.
> That is why `run_coverage.sh` executes no `git checkout` in any mode, and why it refuses
> instead of guessing.

## Running Tests

### Basic Usage

Root `make check` does **not** run this suite — `SUBDIRS = osal ccec` in the top-level
`Makefile.am` means `check` never descends into `tests/`.  **It fails rather than skipping quietly:
no `check` target exists at the configured root, so `make check` there prints
`make: *** No rule to make target 'check'.  Stop.` and exits 2 (measured).**  Read that as `make`
answering for a target nobody declared, not as a configuration fault.  Drive it either through this suite's own
`check` target (`tests/L1Tests/Makefile.am` declares `TESTS = run_L1Tests`) or by running the
libtool wrapper from its own directory.  Run the wrapper, not `.libs/run_L1Tests`: the wrapper sets
the library search path the real binary needs.

Either way, **export `GTEST_FILTER` first**, and treat it as part of the command rather than as
tuning.  The binary now also registers the AIDL contract suite, two of whose fixtures require the
AIDL back-end to be the *resolved* one; the selection is made once per process before any test
body runs, so under the default legacy selection those two **fail rather than skip** — by design,
so that a skipped arm cannot be mistaken for a passing one.  The negative filter below is what
makes the default run green.  It is spelled as an environment variable, not as a `--gtest_filter`
argument, for one reason: it then reaches *every* way the binary is started, including through
`make check`.

```bash
# The default invocation's filter.  Not optional -- see above, and see
# "Back-End Selection" below for what the other invocations are.  With it set,
# expect 568 tests from 23 test suites, 568 passed, 0 skipped, exit 0.
export GTEST_FILTER='-DriverAidlSessionTest.*:DriverAidlTransmitTest.*'

# Run all tests through automake, from the hdmicec submodule root
make -C tests/L1Tests check

# ...or run the suite directly, from tests/L1Tests
cd tests/L1Tests && ./run_L1Tests

# Turn the output UP: print each test's elapsed time
./run_L1Tests --gtest_print_time=1

# Turn the output DOWN: report failures only, suppressing the per-test progress lines
./run_L1Tests --gtest_brief=1
```

> **Always take flag names from `./run_L1Tests --help`.**  GoogleTest has no general
> "verbose" switch -- the two flags above are how you raise and lower its output -- and it
> handles an unrecognised `--gtest_*` argument by printing its help text, running **no tests
> at all**, and still exiting `0`.  A misspelled flag therefore produces a run that looks
> successful but tested nothing, which no exit-status check will catch.

### Advanced Usage

```bash
# Navigate to test directory
cd tests/L1Tests

# The default invocation's filter applies here too -- export it as in Basic Usage above.
# A --gtest_filter argument on the command line overrides it, which is what the next
# two commands rely on; the ones after them inherit it.
export GTEST_FILTER='-DriverAidlSessionTest.*:DriverAidlTransmitTest.*'

# Run specific test suite
./run_L1Tests --gtest_filter="CECFrameTest.*"

# Run multiple test patterns.  ANCHOR EACH PATTERN TO ITS SUITE: this selects exactly the
# 22 OSAL Mutex and Thread cases, whereas the unanchored "*Mutex*:*Thread*" also matches
# DriverAidlSessionTest.OpenSucceedsAndDeliversWhenAThreadPoolWasAlreadyStarted -- 23 cases
# from 3 suites, of which that one FAILS by design under the legacy selection.
./run_L1Tests --gtest_filter="MutexTest.*:ThreadTest.*"

# List all available tests
./run_L1Tests --gtest_list_tests

# Generate XML report (for CI/CD)
./run_L1Tests --gtest_output=xml:test_results.xml

# Repeat tests for flakiness detection -- SCOPE THE REPEAT.  The global environment is set
# up once per process and NOT per iteration, so whatever state one iteration leaves in the
# process-wide Driver singleton is still there for the next; the whole suite does not
# survive that.  See the note below before widening this filter.  These two suites are pure
# value-type cases -- neither names Driver, LibCCEC or any getInstance at all -- so they
# carry nothing across an iteration.  Measured: exit 0, 100 clean iterations.
CEC_TEST_AIDL_MODE=absent \
  ./run_L1Tests --gtest_filter='CECFrameTest.*:OpCodeTest.*' --gtest_repeat=100

# Shuffle test execution order (diagnostic only -- see "Order independence" below;
# --gtest_random_seed pins the order so a finding is reproducible)
./run_L1Tests --gtest_shuffle --gtest_random_seed=12345
```

**Why the repeat above is scoped, stated so a reader who widens it anyway knows what they are
looking at.**  `--gtest_repeat` re-runs the registered test cases and **not** the global
environment: `CecTestEnvironment::SetUp` and its `TearDown` bracket the whole process, so one
`LibCCEC::init` and one `term` serve every iteration.  **Measured** on a three-iteration run of the
default invocation's filter: `grep -c 'Global test environment' <log>` returns `2` -- one set-up
line and one tear-down line for three iterations.  Every iteration after the first therefore starts
against whatever state its predecessor left in the process, and the suite does not survive that.
Two different problems follow, and they must not be conflated: only one of the two is a recorded
product defect.

**Deterministic assertion failures from iteration 2 on -- shared-state carry-over between the
suite's own cases.**  **Measured**, bounded to three iterations: iteration 1 green, then iterations
2 and 3 each failing exactly `ConnectionTest.MatchSourceMismatchedAddress`
(`ccec/test_Connection.cpp:1149`) and `ConnectionTest.SendAsyncMatchSource` (`:1302`), exit `1`,
with the middleware logging `Altering Initiator to match connection source` and the captured first
byte reading `0x4F` where both cases expect `0x0F`.  The chain sits entirely inside the suite's own
premises.  Both cases are written on the premise that `PLAYBACK_DEVICE_1` is *not* in the driver's
registered-address list, so `matchSource`'s `isValidLogicalAddress` guard reads false and the
frame's source nibble is left alone.  `LibCCECTest.AddLogicalAddressAfterInit`
(`tests/L1Tests/ccec/test_LibCCEC.cpp:302`) registers exactly that address on the process-wide
singleton and never removes it, and `DriverImpl::close()` deliberately does not clear
`logicalAddresses` (`ccec/src/DriverImpl.cpp:130-153`) -- legacy behaviour this migration preserves
rather than improves.  In default order the registrar runs *after* both consumers, which is why the
first pass is green and no later one is.  Isolated proof, two commands: repeating
`ConnectionTest.MatchSourceMismatchedAddress` alone with `--gtest_repeat=3` exits `0` with no
failures, and repeating it together with `LibCCECTest.AddLogicalAddressAfterInit` exits `1`, failing
on iterations 2 and 3 only.  That is the same class as the seed-dependent failures under **Order
independence** below -- a case's assumption about shared state, exposed by a different execution
order -- and not the lifecycle defect described next.

**Terminations mid-run, which are that lifecycle defect and should not be filed as a new bug.**  A
longer unscoped repeat may not finish at all: a 100-iteration attempt stopped during iteration 8
with a SIGSEGV (exit `139`), and a serial retest terminated during iteration 23 with exit `125`.
Those two outcomes are the recorded legacy lifecycle defect **PERF-01**, in
`blitzy/documentation/Project Guide.md` §4.1: `Bus::start()` spawns each worker generation through
a detached thread nothing can join, a `term` arriving before that generation reaches `RUNNING`
leaves it unstoppable, and the accumulated NULL sentinels then reach the unguarded flush arm at
`ccec/src/DriverImpl.cpp:182` (`frame = *inFrame;`).  It reproduces on the pre-migration sources at
a **higher** incidence, so it is neither introduced nor aggravated by
the AIDL back-end, and every root-cause file is out of scope for this migration.  Read §4.1 before
spending time on a repeat run: it carries the thread-count and `term`-duration signatures, the
third failure mode (`pure virtual method called` during shutdown), and the reason a quiet run is not
evidence of repair.  For flakiness detection meanwhile, repeat a filter whose cases neither drive
the library's lifecycle nor read the driver singleton's registered-address state -- which is what
the example above does.

`--gtest_shuffle` is a diagnostic, not a gate -- and the suite does **not** pass under every
shuffled order.  Default order is green and stable; shuffled order is not, and two distinct
problems are in play which behave differently.  Order-dependent **assertion** failures (exit `1`)
are deterministic for a given seed: the same seed fails the same cases every run.  The
intermittent **production SIGSEGV** (exit `139`) is **not tied to any seed** -- it fires at a
measured rate across repeated runs, so the same seed can read as green in one pass and as a crash
in another.  Two consequences: **no seed is a reliable crash reproducer**, so budget for repetition
rather than for a seed; and one green seed is not a proof of order independence.

**Every measured per-seed figure lives in one place and is deliberately not copied here:** the
**Test Order Dependence (diagnostic)** section of `tests/L1Tests/README.md`, which is the
authoritative shuffle-order record and carries the crash rate, the assertion outcome per seed, the
two production defects behind the crashes, and the toolchain and suite size the sweeps were
measured on.  Read that section before quoting any seed, and quote it rather than keeping a figure
here: a per-seed result is a snapshot of one tree, one toolchain and one test inventory, so a
second copy of it is a second thing to go stale.

The fixtures that own the shared library's lifecycle wait for the bus reader to publish RUNNING
before terminating it, so that window is not opened.  The sanctioned use of the flag is narrow:
pin the order with `--gtest_random_seed=<seed>` to check that a test you just wrote is
self-sufficient, and re-derive any figure on your own build.  Read a seed that reproduces a
failure for which of the two problems it reproduced -- an **assertion** failure under a pinned seed
is a real order dependence and worth reporting with that seed attached, because it repeats for the
same seed on the same tree and inventory, while a **crash** under a pinned seed says only that the
crash fired on that run, since it is tied to no seed and a rerun of the same seed can come back
green.

### Back-End Selection: the Invocation Matrix

`libRCEC` compiles **both** HDMI-CEC HAL back-ends in — the legacy in-process C API and the
out-of-process `com.rdk.hal.hdmicec` AIDL/Binder HAL — and chooses between them at run time.  That
choice constrains how the suites are run, so read this before running anything but the default.

**The selection resolves once per process.**  `test_main.cpp` calls
`LibCCEC::getInstance().init("CEC_TEST")` inside a global `::testing::Environment::SetUp`, and that
`init` is the first thing which forces `Driver::getInstance()`.  By the time any `TEST_F` body runs
the choice is already made and cannot be changed, so **one binary run yields exactly one selection
outcome** and every outcome needs its own process.  No filter can make a single run exercise both
arms of the selection — which is why there is a matrix rather than a single run.

There are **five selection outcomes, A through E**, and **five execution records** -- one per
outcome, and no others.  `tests/L1Tests/run_coverage.sh --run` drives every record the host it is
running on can reach, one process each, and writes a separate result and log artifact per record so
that a later one cannot overwrite an earlier one's evidence:

| Inv | Binary | Fake service | Expected selection | Cases | Has it run? |
|--------|-------------|---------|---------|---------|---------|
| A | `run_L1Tests` | not reachable | Legacy | 568 selected, 39 excluded | **Yes, on any host: measured 568 of 568 from 23 suites, exit `0`** |
| B | `run_L1Tests` | in-process, compatible | AIDL, via the local interface | 423 selected, 184 excluded | **Deferred on a driverless host and on the committed CI runner; measured green on a Binder-capable guest — 422 of 422 from 15 suites, exit `0`, on the tree that registered 606; this tree selects 423** |
| C | `run_L1Tests` | in-process, **incompatible** — reports interface hash `"-1"` | Legacy, with the incompatibility logged | 384 selected, 223 excluded | **Deferred on a driverless host and on the committed CI runner; measured green on a Binder-capable guest — 384 of 384 from 13 suites, exit `0`** |
| D | `run_L2Tests` | host not launched | Legacy | 15 registered | **Yes, on any host: measured 11 passed, 4 skipped, exit `0`** |
| E | `run_L2Tests` | host launched and ready | AIDL, over a real remote proxy | 15 registered | **Deferred on a driverless host and on the committed CI runner; measured green on a Binder-capable guest — 9 passed, 6 skipped of 15, exit `0`.  An earlier run reported 8 passed of the 14 then registered, before `DualPathHostLifecycleTest` was added; that case is mandatory here as under D, and the 9/6 figures are the measurement that confirmed it rather than an expectation** |

**Where each of the five has and has not run, stated once and exactly, because it is the fact most
easily blurred.**  On **this** host, and on the **committed** CI runner, invocations **B, C and E
are deferred** for want of a binder kernel driver: `run_coverage.sh` reports them as deferred,
never attempts them, and never lets them stand in for evidence they did not produce.  **All five
invocations have been executed green once against a real binder transport**, in a purpose-built
binder-capable QEMU guest, and that run is recorded in `blitzy/documentation/Project Guide.md`
§4.2 -- with the guest's kernel and SDK configuration, the per-invocation results, and the three
test-code defects that only executing them could have found.  That guest is **not** the committed
target of `.github/workflows/aidl-path-tests.yml`, which is a deliberately all-32-bit
**protocol-7 i386** job, and **that committed workflow has never run** -- so nothing anywhere,
including §4.2, may be read as a result on it.  Read §4.2 for the numbers rather than copying them
here: a second copy of a measured figure is a second thing to go stale.

Every Cases figure in the table above is measured, because each comes from listing that
invocation's own filter against the built binary — but a *selection* total is not a result, which
is what the right-hand column is for.  The L2 tier's 15 registered cases divide into 4 that belong
to neither arm, 6 legacy-arm, 4 AIDL-arm and 1 that tests the harness itself and is mandatory under
both, which is why **D reports 11 passed and 4 skipped**: a case whose arm is not the resolved
back-end skips rather than fails.  E's recorded 9 passed and 6 skipped are of the 15 registered
before the harness case existed.  For why B, C and E cannot run here at all, see
[Runtime prerequisites for the AIDL path](#runtime-prerequisites-for-the-aidl-path); for the job
that is meant to produce them in CI, and its unrun state, see
[CI/CD Integration](#cicd-integration).

**One branch arm has no reacher on a binder-capable host, and that is recorded where it belongs
rather than as a sixth record here.**  `resolveBackEnd` logs two distinct legacy fallbacks, and the
one that says the transport is unavailable -- the **preflight-FALSE** arm -- is taken by invocation
A on a *driverless* host and not on a host whose preflight succeeds.  `run_coverage.sh`'s own
branch-arm manifest says so at the arm's own record (`availability.transport-declined`, the entry
naming `ccec/src/DriverAidlImpl.cpp`), and the alternative reacher it names there is a note about
how the arm *could* be reached on a binder-capable host -- **not a mode, not an invocation, and not
an artifact the runner writes**.  `CEC_TEST_AIDL_MODE` has four values and the matrix has five
records; there is no sixth of either.

Some cases in the contract suite belong to one arm rather than both, and that is by design rather
than a coverage gap -- a fixture cannot opt into a different back-end from its own `SetUp`, because
the selection is already resolved by then.  One such case is worth naming, so it is not read as a
defect: **physical-address retrieval is unavailable on the AIDL back-end**, pending the
device-settings HAL contract that governs the read (tracked as blocked item **B1**).  No workaround
is delivered and none should be added; the assertion for it therefore runs on the legacy arm only.

**Of the contract suite's 124 cases, 85 run under the default invocation and 39 do not** -- the
same 124, partitioned the same way, as [Test Coverage](#test-coverage) above rather than a second
count of them -- and the difference is worth holding on to when reading anything below:

- **Established, inside the default invocation's measured 568.**  All 28 `DriverAidlPreflightTest`
  cases, covering *every* decision arm of the bounded preflight -- the protocol-version mismatch
  and the matching driver whose context manager never answers included, plus a **positive**
  verdict, all reached through a probe seam the predicate takes as a defaulted parameter, which is
  what puts them within reach of a host with no Binder driver at all.  All 23 compatibility arms,
  accept and reject.  All 25 local-instance arms, on a `DriverAidlImpl` whose constructor touches
  no Binder -- every `status != OPENED` guard, the `writeAsync` prelude ordering, and
  `getPhysicalAddress`'s B1 block.  The 4 selection cases -- the absent-service fallback, the
  selected-path log line, the factory returning the same object on every call, and a mid-process
  registration that leaves the resolved back-end alone.  The 5 legacy arms, including a **measured
  receive-path delivery case** that drives a frame from the HAL mock through to an application
  listener, which is what makes the observation machinery the AIDL arms depend on something
  demonstrated rather than assumed.  And, one of those 25 local-instance arms rather than a case
  beyond them, a structural guard that reads the **real** HDMI CEC Sink plugin source and checks
  that the call-path shapes the AIDL session fixture models still match it.  That source is a
  **required acceptance input**, read at a reviewed revision: the case **fails** when it cannot be
  found, naming `CEC_SINK_CALLER_SOURCE` and every path it tried, and both CI workflows check the
  Sink component out as a **required step** — no `continue-on-error` — pinned to the reviewed
  immutable commit `2ad7e1a4712908a4d0fb838caebcbfcaabba55d8`, with the resolve step exiting
  non-zero when the file is absent.  A failure therefore means one of two things and not a third:
  the input was not supplied, or the plugin's call paths really moved.
- **Asserted, compiled in, and invocation-B-only.**  Everything in `DriverAidlSessionTest` and
  `DriverAidlTransmitTest`: the adapter's status translation, the single-element array marshalling,
  the frame-length guard, the session lifecycle, the listener's accept-and-reject behaviour,
  `onMessageSent`'s diagnostic carrying its status, its length **and** its message bytes — all
  three of which production logs at `LOG_DEBUG`, the bytes as bounded lowercase hex, and all three
  of which the case asserts **unconditionally** after raising the level through production's own
  `check_cec_log_status()` — under a locked, `O_NOFOLLOW`, atomically-replacing custody of
  `/tmp/cec_log_enabled`, since that path is production's own choice and is shared by every process
  on the host — failing loudly if the level could not be raised; the synchronous listener detach
  that makes a callback after a failed close or after owner destruction a dropped callback rather
  than a dereference of freed state, and the caller-visible half of the
  `addLogicalAddress` failure mapping through both modelled Sink call paths.  **None of it executes
  here**, because B is deferred on this host and on the committed runner.  All of it **has**
  executed, once, in the binder-capable guest of `blitzy/documentation/Project Guide.md` §4.2 --
  which is where one of that run's three test-code defects was found, in `DriverAidlSessionTest`
  itself, so treat a green reading of these cases as owed to that guest and not to any run
  performed here.

#### `CEC_TEST_AIDL_MODE`

One environment variable selects the invocation.  It takes four values and is read **only** by
`tests/L1Tests/test_main.cpp` and `tests/L2Tests/test_main.cpp` — **no production source reads it,
and none may**:

- **`absent`** — register nothing and launch nothing; the lookup finds no service, the legacy
  back-end is selected, and libbinder is never touched.  **Unset and empty both mean this**, so a
  default run is invocation A or D.
- **`compatible`** — register an in-process fake reporting its real, frozen metadata; the AIDL
  back-end is selected.  `run_L1Tests` only.
- **`incompatible`** — register an in-process fake whose interface hash is `"-1"`; the service is
  *present* and rejected as incompatible, so legacy is selected and the rejection is logged.
  `run_L1Tests` only.
- **`remote`** — launch the out-of-process fake service host and wait for it to signal ready.
  `run_L2Tests` only.

A value handed to the wrong runner is a hard failure naming the right one, and an unrecognised
value is a hard failure too — never a quiet fall back to `absent`, because a typo that silently
downgraded a run to the legacy back-end and reported it green is the defect this whole matrix
exists to prevent.

> **Ordering is load-bearing.**  On B and C the fake must be registered, and on E the host must
> have been launched *and* signalled ready, **before** `LibCCEC::init`.  Reaching the service
> afterwards leaves the already-resolved selection on legacy and produces a green run that proves
> nothing at all.  For the same reason, a service already registered under `"HdmiCec"` when a run
> starts is a **hard failure and not a condition to work around**: it would make this run's outcome
> depend on a stale process.  Nothing unregisters the fake either — the pinned C++
> `IServiceManager` exposes no service-removal API.

For the L2 tier, `CEC_FAKE_AIDL_HOST_PATH` tells the harness where the host binary is and is set by
the build.  The host then inherits **two separate channels**, both set by the harness — do not set
either yourself:

- **The readiness pipe**, named by `CEC_FAKE_HOST_READY_FD`.  It carries the host's **one readiness
  token, once**, and nothing else.  Readiness is a pipe and a bounded wait, not a sleep, and the
  host writes no readiness line on any failure path.
- **The control and observation channel**, a *different* inherited pipe pair named by
  `CEC_FAKE_HOST_CONTROL_FD` (the read end the host takes commands from) and
  `CEC_FAKE_HOST_OBSERVE_FD` (the write end it answers on).  The two travel together: both unset
  means no channel, and one without the other is a hard failure that writes no readiness token at
  all.  Traffic is line-oriented — one `\n`-terminated command per line, exactly one reply line per
  command, every reply beginning `OK ` or `ERR ` — and the vocabulary is `ping`,
  `deliver <lowercase-hex>`, `sent-count`, `last-sent`, `open-count`, `close-count`, `listener` and
  `shutdown`.  This is how a case observes what the host actually saw; it is **not** the readiness
  pipe.

#### Why an out-of-process host exists at all

libbinder resolves a name registered in the *calling* process to the local `BBinder`, so
`interface_cast` hands back that very object: no `Bp*` proxy, no transaction across the binder
driver, and no client threadpool involvement.  That cuts both ways, and it is why B and E are both
needed and neither substitutes for the other:

- The **in-process** fake is honest coverage of the adapter's translation logic, and it is the
  *only* way to reach the compatibility-rejection branches — local dispatch honours a virtual
  override of the interface hash and version, whereas a remote service cannot report bad metadata
  at all, because its generated `onTransact` answers from compiled-in constants.
- Only the **out-of-process** host can make the middleware hold a real proxy, cross the driver, and
  receive its listener callback on a binder threadpool thread.  That is what invocation E is for,
  and **it is what invocation E did on 2026-09-01 — on a Binder-capable guest, and on no other
  host.**  E is deferred on this host and on the committed CI runner alike; the runs that exist are
  the ones recorded in `blitzy/documentation/Project Guide.md` §4.2, where 14 were registered, 8
  passed, 6 skipped, exit `0`, with all four `DualPathAidlFlowTest` cases green.

**Invocation E is structurally complete, its assertions are real, and a green run of it exists —
but only where a Binder driver does, and never on this host or on the committed runner.**  The
distinction is the whole point of this section, so it is spelled out rather than left to inference.
Its four `DualPathAidlFlowTest` cases assert that the fake service is holding the very listener the
middleware passed to `open()`; that a frame the host is told to deliver arrives decoded at a typed
processor; that **the thread that delivered it is not the test thread**, which is the observable
form of "a binder threadpool thread received it"; that the outbound cases reach the host with an
**exact byte sequence and an exact call count**, read back over the host's control and observation
channel — the inherited pipe *pair*, not the readiness pipe, which carries only the one readiness
token — rather than inferred from the absence of an exception; and that a frame delivered while the
driver is not opened is rejected by the state guard.  Those four cases **have** been exercised
against a real transport once, in the §4.2 guest, where two of them failed on their first execution
and were corrected -- each had derived its expected wire image from the caller's pre-header frame --
so read them as assertions that one purpose-built guest has run, **never as evidence that this host,
or the committed binder job, has observed delivery over a real proxy or receipt on a binder
threadpool thread.**  Under invocation D those four cases skip, which is why D reports 11 passed and
4 skipped rather than 15 passed; under E it is the six legacy-arm cases that skip, which is why E
reported 9 passed and 6 skipped of the 15 registered when it last ran.

#### Runtime prerequisites for the AIDL path

These are separate from the build prerequisites in [Configure and Build](#3-configure-and-build),
and the distinction matters: **compiling and linking** the AIDL back-end needs only headers and
libraries and is fully verifiable anywhere, while **executing** an AIDL-path case needs three more
things — a kernel with binder support, a binder protocol version matching the one `libbinder` was
built for, and a running `servicemanager`.

A hosted GitHub runner has none of them, and neither does a typical development host: measured on
the host this was last revised on, kernel **6.12.85+**, `CONFIG_ANDROID_BINDER_IPC` not set,
`binder` absent from `/proc/filesystems`, no `/dev/binder*`, and no `/dev/kvm` either — so not even
a nested binder-capable guest can be booted there.  There, **invocations B, C and E cannot run at
all** — and not merely because a lookup would fail: registering a name requires
`defaultServiceManager()`, which opens the driver node, and on the pinned Binder stack that call
*aborts the process* when the node is missing.  So an in-process fake is as unreachable as a remote
one.  `run_coverage.sh` reports those invocations as **deferred**, never attempts them and never
lets them stand in for evidence they did not produce.  The legacy path — A and D — remains fully
executable.

**No result for B, C or E is claimed on this host or on the committed CI runner, and none exists to
claim: all three are deferred on both**, for exactly the reason above.  Everything they assert —
the adapter's translation arms, the in-process compatibility fallback, a real `Bp*` proxy carrying
a transaction across the driver, and a listener callback arriving on a thread that is not the test
thread — has been executed green exactly once, in the purpose-built binder-capable QEMU guest
recorded in `blitzy/documentation/Project Guide.md` §4.2, and **that guest is not the committed
target of the binder job described under [CI/CD Integration](#cicd-integration), which has never
run.**  So the transport has been observed working, in one guest, once: treat a claim that *this*
host, *this* runner or *that committed job* observed it as a documentation defect and correct it,
and cite §4.2 for anything stronger.

> **A driverless host is itself evidence.**  Such a host must reach the legacy back-end *without
> aborting and without hanging*, which is the entire point of the middleware's bounded preflight:
> it declines, and the factory falls back.  That is the one requirement a machine with no binder
> driver is uniquely placed to demonstrate, and the hosted CI job gates on it.

There is **no production API for querying the selected back-end**, deliberately.  The active path is
observable through the line the factory logs once at initialisation, at `LOG_INFO`, which is the
default level.  **Read it out of an invocation log rather than out of a live pipeline**, because a
pipeline is where this evidence goes wrong: a trailing `grep` replaces the suite's exit status with
its own, so a red run whose log happens to carry the line reports success.  **Measured:** the
unfiltered suite is `607 tests from 25 test suites ran`, 568 passed, **39 failed, exit `1`** — while
`./run_L1Tests 2>&1 | grep 'HDMI CEC HAL back-end selected'` in a default shell prints the legacy
line and **exits `0`**.  Every invocation log `run_coverage.sh` writes belongs to a run whose exit
status, test count and selected-path line were all checked before the run was allowed to proceed,
which is exactly what a live `grep` throws away:

```bash
# THE FORM TO PREFER.  One line per record, out of the artifacts of a run that was gated.
# COVERAGE_DIR is whatever run_coverage.sh was given as --output-dir; the run_status.txt in
# it names the run those logs belong to, which is why that file is read first.
COVERAGE_DIR=/path/to/the/coverage/output/directory
grep 'HDMI CEC HAL back-end selected' "$COVERAGE_DIR"/run_invocation_*.log

# A LIVE RUN, when there is no such directory yet.  Three things are not optional here.
# `set -o pipefail` keeps the suite's status instead of grep's; CEC_TEST_AIDL_MODE and the
# invocation-A filter are what make the suite green in the first place (without the filter
# this run is the red 607-case one above); and `tee` writes the log while leaving the
# status to be checked, which is the whole point.
set -o pipefail
CEC_TEST_AIDL_MODE=absent \
  ./run_L1Tests --gtest_filter='-DriverAidlSessionTest.*:DriverAidlTransmitTest.*' \
  2>&1 | tee run_invocation_A.log
echo "suite exit: $?"        # 0, and only then is the next line evidence
grep 'HDMI CEC HAL back-end selected' run_invocation_A.log
```

Never collapse those two commands back into one pipeline ending in `grep`.  The selected-path line
is a statement about *which back-end a passing run selected*; taken from a run whose status was
discarded it is a statement about nothing.

Tests additionally assert identity with a `dynamic_cast` against the concrete back-end types, which
a test translation unit can name because neither `ccec/src/DriverImpl.hpp` nor
`ccec/src/DriverAidlImpl.hpp` is an installed header.

## Writing New Tests

### Example Test File

Create `tests/L1Tests/ccec/test_NewClass.cpp`:

```cpp
#include <gtest/gtest.h>
#include "ccec/NewClass.hpp"



class NewClassTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup before each test
        obj = new NewClass();
    }
    
    void TearDown() override {
        // Cleanup after each test
        delete obj;
    }
    
    NewClass* obj;
};

TEST_F(NewClassTest, BasicFunctionality) {
    EXPECT_EQ(obj->getValue(), 42);
    EXPECT_TRUE(obj->isValid());
}

TEST_F(NewClassTest, EdgeCase) {
    obj->setValue(-1);
    EXPECT_THROW(obj->process(), std::invalid_argument);
}
```

Add to `tests/L1Tests/Makefile.am`:
```makefile
run_L1Tests_SOURCES = \
    ... \
    ccec/test_NewClass.cpp
```

## CI/CD Integration

`.github/workflows/L1-tests.yml` in this repository is the authoritative pipeline -- read it first,
and prefer changing it over copying the example below.  The example is a single-job reduction of
that workflow, carrying the same build sequence as
[Configure and Build](#3-configure-and-build) so the two cannot drift apart.

There are now **two** workflows, and the division of labour between them is the whole point:

- **`.github/workflows/L1-tests.yml`** builds everything -- the Binder SDK, both AIDL client stub
  snapshots, `libRCEC` with both HAL back-ends compiled in, and all three test binaries -- and then
  runs **invocation A only**.
- **`.github/workflows/aidl-path-tests.yml`** is defined to run **the complete matrix -- all five
  selection outcomes, A through E, and no other record -- on one instrumented build on one
  machine**, followed by a single coverage capture and the per-branch gate.  It provisions a
  guest that can actually run binder, because a hosted runner cannot (see
  [Back-End Selection](#back-end-selection-the-invocation-matrix)).  **This committed workflow has
  never run.**  The one execution of the matrix that exists was performed in a *different*,
  purpose-built binder-capable QEMU guest, recorded in
  `blitzy/documentation/Project Guide.md` §4.2; this workflow is the delivered mechanism for
  reproducing that in CI, not a record that it has done so, and none of §4.2's results is a result
  on its target.

  The guest is a **protocol-7, all-32-bit** one end to end, and the way it reaches that is the part
  worth reading in the workflow itself rather than paraphrasing: its kernel pin is
  `GUEST_KERNEL_VERSION: '5.15.148'`, built `ARCH=i386`, with **both** `CONFIG_ANDROID_BINDERFS`
  and `CONFIG_ANDROID_BINDER_IPC_32BIT` enabled.  Those two do not coexist on a stock tree --
  binderfs starts at Linux 5.0 and mainline deleted the 32-bit option after 4.17 -- so the job
  **restores the option**: a minimal patch carried inline in the workflow re-adds the Kconfig
  entry, the matching `#define BINDER_IPC_32BIT 1` and the per-directory
  `ccflags-$(CONFIG_ANDROID_BINDER_IPC_32BIT) += -DBINDER_IPC_32BIT=1`, is dry-run first and
  **fails the job outright if it does not apply cleanly**, and is followed by an assertion that
  `CONFIG_ANDROID_BINDER_IPC_32BIT=y` in the **built** `.config` -- absent, not merely "is not
  set", being the failure that catches a patch that silently did nothing.  The staged SDK is built
  with `BINDER_IPC_32BIT=ON` to match, asserted from its own CMake cache entry.  Device nodes come
  from **binderfs**, not statically: the in-guest init mounts `-t binder` on `/dev/binderfs`,
  requires the three nodes the initial instance takes from
  `CONFIG_ANDROID_BINDER_DEVICES="binder,hwbinder,vndbinder"`, and symlinks each to the `/dev/<node>`
  path libbinder resolves by name.  The protocol is then asserted in four places rather than
  assumed -- the built `.config`, the SDK cache entry, the image manifest's derivation, and the
  driver's own `BINDER_VERSION` ioctl read inside the running guest -- and none of them may be
  relaxed to make the job green.  **That pairing of a restored option with a 5.15 driver is
  unproven until this job first runs**, which is why every layer asserts it.

  One provisioning detail is easy to miss and fails the guest run when it is: only the `hdmicec`
  payload is copied into the image, so the **real HDMI CEC Sink plugin source** the L1 drift guard
  reads has to be staged deliberately.  The guard treats that source as an acceptance input and
  **fails** when it cannot read it, so the image builder **always stages that file into the guest**.
  `--sink-caller-source` and a sibling checkout detected beside `--payload` are two ways of naming
  the **same input**: whichever the builder found is canonicalised into one staged path, so a
  detected sibling is copied in exactly as an explicitly passed flag is, and the builder **refuses
  to build when neither is available** rather than producing an image the guard cannot use.  The
  copy lands at a fixed guest path — `/etc/cec-l2/sink-caller-source.cpp`, mode `0644`, beside the
  image's other build-time evidence — is recorded in **both** the in-image build record and the
  **published** manifest, and the in-guest init exports `CEC_SINK_CALLER_SOURCE` to it -- that
  export and the guard's own path search are **belt and braces over the staging, not substitutes
  for it**, since neither can find a file that was never copied in.  The workflow passes what its
  own required resolve step produced and then checks the manifest recorded it.

  Two details make that last check worth more than a restatement of the flag the workflow passed.
  The builder writes the manifest key only **after** the staged copy is in place, so reading it
  back is evidence about the **image** rather than about the instruction; and the manifest carries
  the staged file's **sha256** beside its path, which the workflow compares against the pinned
  checkout it passed in.  The digest is what separates the two failures that otherwise look
  identical: a **stale or wrong revision** staged into the image fails the guard in exactly the way
  a **genuine plugin drift** does.  Without it, the first symptom of a staging fault would be a red
  `TheModelledSinkCallPathsStillMatchTheRealSinkSource` an hour into a guest boot, reported as a
  drift when nothing had drifted.  The init adds one more check at boot for the case neither can
  cover — a manifest that names the file while the image does not carry it — and refuses to
  continue rather than letting the suites run and the guard take the blame.

Invocation A is **repeated** in the second job rather than imported from the first, and that is
deliberate: gcov counters accumulate across repeated runs of the same instrumented binary but
cannot accumulate across machines, so running every record against one build on one host before a
single capture is the only way both arms of the selection branch land in one trace.  Do not
"optimise" it by reusing the hosted job's counters.

> **A green tick on the hosted job is not dual-path evidence.**  It is build-plus-legacy: it proves
> the pre-existing suite still passes with both back-ends compiled in and the Binder libraries
> linked, and it proves that a host with no binder driver reaches the legacy back-end without
> aborting -- which is the one requirement a driverless machine is uniquely placed to demonstrate.
> It establishes nothing about the AIDL path executing: that job runs **invocation A only**, and of
> the rest, B, C and E cannot run there at all for want of a binder driver, while D is simply not
> run there.  Read the binder-capable job for that, and only that — and note that **the committed
> binder job has never run**, so **no invocation in which a binder driver is required — B and E,
> where the AIDL back-end is resolved, and C, whose own outcome is a legacy fallback — has produced
> a result in CI at all.**  The one place all three have produced a result is the guest recorded in
> `blitzy/documentation/Project Guide.md` §4.2, which is not this job's target.

### Artifacts, and the one list that decides their names

The names of everything `tests/L1Tests/run_coverage.sh` writes are decided in **exactly one place**:
the block in that script's header titled `ARTIFACTS (fixed names, written under the output
directory)`.  Only the *directory* is overridable -- the file names are fixed, so a local run's
output is interchangeable with a CI artifact bundle.  Everything else -- this guide included --
**defers to that block rather than restating it**, because an artifact no document names is an
artifact a reader concludes was never produced.  `tests/L1Tests/README.md`, under "Coverage output
is a build artifact", carries the full per-entry description; what follows is what a CI reader needs
from it.

**Who uploads which is a separate question, and the two workflows answer it differently**, so it is
answered per job below.  Do not read "both workflows upload what the runner writes": the hosted job
never runs the runner at all.

What the runner writes into its output directory:

- **`run_status.txt` -- read this one FIRST.**  The script's header calls it THE AUTHORITATIVE
  MARKER: which run id the directory holds, which invocations ran, which deferred, and the outcome.
  Every other name in the directory is shared by every run that ever writes there, so this is the
  file that ties the rest to one generation.  A run claims it **before** it removes or writes
  anything else and stops if it cannot, and it exits non-zero if it cannot record its own verdict --
  so it never carries one run's verdict over another run's artifacts.
- **The trace triple and the report.**  `coverage.info` (deliberately **unfiltered** -- the branch
  gate's input), `filtered_coverage.info` (production-source-only), `line_gate_coverage.info` (the
  line gate's input) and `coverage/index.html`, plus `per_file_coverage.tsv` and
  `uncovered_lines.txt`.
- **One `rdkTestResults_invocation_<L>.json` and one `run_invocation_<L>.log` per matrix record**,
  `A` through `E` and no other letter -- five records, one per selection outcome, and written only
  for the invocations the run actually performed: a deferred one writes neither file.  The letter
  rather than a timestamp is what stops a failed or empty record being erased by whichever green one
  ran after it.
- **`provenance.txt`** -- how the measurement was produced: the toolchain, the gcov version, which
  invocations ran and which were deferred, and the counter state the capture read.  It is what
  makes a figure attributable months later, so it is evidence rather than a log.
- **`filter_dependencies.log`** -- which dependency-header paths the filter dropped from
  `filtered_coverage.info` to produce `line_gate_coverage.info`, and which it deliberately kept
  (`halcompat.h` is kept, as a consumed HALIF header).  Without it a change in the line gate's
  denominator cannot be explained.
- **The per-step logs.**  `build.log`, `capture.log`, `filter.log`, `genhtml.log`.

Those are thirteen fixed names plus two files per record actually performed, so a driverless host's
directory -- where B, C and E defer -- holds **seventeen** entries: the thirteen, plus a JSON and a
log each for `A` and `D`.  A directory with more letters than the run performed, or with no
`run_status.txt`, is not this script's output and should not be read as it.

**The hosted job, `.github/workflows/L1-tests.yml`, uploads none of the above** -- it never calls
`run_coverage.sh`.  It runs invocation A directly and keeps its own `lcov -c` / `lcov -r` /
`genhtml` shape, and its two upload steps carry exactly:

- **`selected_back_end_invocation_A.log`**, its own capture of the selected-path line, on an upload
  step of its own with `if-no-files-found: error`.
- **`coverage/`, `tools/supply-chain-pins.txt`, `valgrind_log`,
  `rdkL1TestResultsWithoutValgrind.json` and `rdkL1TestResultsWithValgrind.json`**, with
  `if-no-files-found: warn` -- and that `warn` is load-bearing rather than lazy, because
  `valgrind_log` is written only in the memcheck step's fallback arm and does not exist on a healthy
  run.

So do not go looking for `run_invocation_*.log`, `rdkTestResults_invocation_*.json`,
`provenance.txt` or `run_status.txt` in a hosted-job bundle: that job produces none of them.
`coverage/` is the one name the two sets share, and in the hosted bundle it is that job's own
`genhtml` output over its own `lcov -r` filter, not the runner's.

**The binder-capable job, `.github/workflows/aidl-path-tests.yml`, uploads the runner's directory
whole.**  It runs `run_coverage.sh --build --run --output-dir` against the guest's artifact
directory, copies that directory out of the image and uploads it as one artifact rather than naming
files -- precisely because the names are fixed and only the directory is the override point, so
enumerating them here would invent paths and risk uploading nothing.  Two further artifacts come
with it: the **guest console log**, and the **provisioning records** (the image manifest, the kernel
configuration that was actually built, the resolved supply-chain pins, the i386 toolchain file and
CMake cache, and the binder restoration patch).

**Three files in that bundle come from the guest and not from the runner**, and they are the first
things to read when a run in that job fails: **`run_coverage.status`**, the exact exit status
`run_coverage.sh` returned, written by the in-guest init and the file the workflow reads to decide
the job; **`servicemanager.log`**, the binder name service's own output and the only evidence of why
readiness failed; and **`servicemanager_readiness.log`**, every readiness attempt, which is what
distinguishes a probe that blocked from one that could not run.  Their names live with their
producer, `.github/workflows/aidl-path-tests-rootfs.sh`, which writes them into the very directory
it then hands the runner as `--output-dir`, and `run_coverage.sh` neither writes nor reads them.

**There is no `binder-platform-record.txt`, and nothing in this repository produces one.**  The
BINDER PLATFORM RECORD is a **console block**, not a file: the in-guest init prints the kernel
binder configuration it read from the running kernel, the SDK build flags it read from the staging
tree and the protocol version it read from `/dev/binder` between two banner lines, because those
three are meaningful only together.  It reaches a reader through the **guest console log** upload,
which is also where the runner's per-invocation output and the branch-arm gate's own result appear,
the gate writing to stdout rather than to a file.  Look for the record there; a file of that name
does not exist to be looked for.

### GitHub Actions Example

```yaml
name: L1 Unit Tests

on: [push, pull_request]

permissions:
  contents: read

jobs:
  test:
    # ubuntu-24.04, NOT 22.04, and the reason is the compiler and nothing else.
    # The acceptance toolchain is the GCC 13 family (see below); jammy carries no
    # gcc-13 package at all, while noble carries gcc-13/g++-13.  Everything else
    # about this job would work on either label.
    runs-on: ubuntu-24.04

    # The two AIDL/Binder staging prefixes, plus the two split-staging overrides:
    # build_binder.sh stages its runtime under out/target and its headers under
    # out/build, so neither is derivable from the other.  LD_LIBRARY_PATH is not
    # only for the run -- libbinder.so carries no RUNPATH, so its closure has to
    # be findable to LINK against libRCEC.  GTEST_FILTER is the invocation-A
    # filter; as an environment variable it reaches every way this job starts the
    # binary, which a --gtest_filter argument would not.
    #
    # CEC_TOOLCHAIN_GCC_MAJOR is the acceptance toolchain, named ONCE so it cannot
    # drift between the steps that consume it.  It pins the MAJOR, not the patch:
    # gcov refuses a notes file written by a different compiler family, so build
    # and capture must agree -- but no Ubuntu archive supplies the 13.4.0 the
    # workspace container happens to have (noble ships 13.3.0), and reaching it
    # would take an unpinned third-party PPA.  So the major is gated and the exact
    # patch is RECORDED with the results.  Do not write a gate here that claims to
    # enforce 13.4.0 while accepting 13.3.0.
    env:
      CEC_TOOLCHAIN_GCC_MAJOR: '13'
      HALIF_PREFIX: ${{ github.workspace }}/rdk-halif-aidl
      HALIF_LIB_DIR: ${{ github.workspace }}/rdk-halif-aidl/out/target/lib/halif
      BINDER_SDK_DIR: ${{ github.workspace }}/rdk-halif-aidl/out/target
      BINDER_SDK_INCLUDE_DIR: ${{ github.workspace }}/rdk-halif-aidl/out/build
      LD_LIBRARY_PATH: ${{ github.workspace }}/install/usr/lib:${{ github.workspace }}/rdk-halif-aidl/out/target/lib/halif:${{ github.workspace }}/rdk-halif-aidl/out/target/lib/binder
      GTEST_FILTER: "-DriverAidlSessionTest.*:DriverAidlTransmitTest.*"

    steps:
      - uses: actions/checkout@v7
        with:
          path: hdmicec

      # TWO CMAKE INSTALLATIONS, AND BOTH HAVE TO STAY.  The pinned 3.16.x must
      # remain what a bare `cmake` resolves to for the plugin and test chain,
      # which 3.20 and later break.  The Binder SDK needs the opposite: its
      # documented floor is 3.22.1.  Reaching the newer one by absolute path is
      # not possible, because build_binder.sh and build_modules.sh invoke a bare
      # `cmake` and offer no override -- so it is reached by a STEP-SCOPED PATH
      # prepend instead, leaving the job-wide pin intact.  Simplify this and one
      # of the two chains breaks.
      - name: Set up CMake
        uses: jwlawson/actions-setup-cmake@v1.13
        with:
          cmake-version: '3.16.x'

      # libgtest-dev/libgmock-dev are what put gtest.pc and gmock.pc on
      # pkg-config's path, which is the only thing configure's PKG_CHECK_MODULES
      # consults.  GLIB is the other hard configure dependency.  flex and bison
      # are Binder SDK prerequisites, not extras: build_binder.sh configures a
      # host AIDL compiler that needs both.
      #
      # gcc-13/g++-13 are named VERSIONED rather than as a bare `g++`, which would
      # install whatever the image defaults to.  gcc-13 is pulled in as well as
      # g++-13 because it is what carries gcov-13, and gcov is the half of the
      # toolchain the coverage evidence depends on.
      - name: Install packages
        run: |
          sudo apt-get update
          sudo apt-get install -y autoconf automake libtool pkg-config make \
                                  gcc-13 g++-13 \
                                  libglib2.0-dev libgtest-dev libgmock-dev lcov \
                                  cmake ninja-build flex bison

      # PROVE THE TOOLCHAIN BEFORE BUILDING ANYTHING, and fail the job rather than
      # producing a trace whose provenance nobody can state.  Three things are
      # asserted: all three tools are present, all three report the pinned major,
      # and all three report the SAME FULL VERSION -- the last because gcov's
      # notes-file stamp is derived from the major AND minor, and all three come
      # from one package pair so agreement is achievable.
      #
      # The gcov shim is not decoration.  run_coverage.sh invokes a BARE `gcov`
      # and has no override knob, so the only way to select gcov-13 for it is to
      # put a `gcov` that resolves to gcov-13 first on PATH.  An exported CC/CXX
      # reaches the compiler and never reaches gcov.
      #
      # `.github/workflows/L1-tests.yml` is the authority for this step and carries
      # the full version of it, including a second step that re-checks the exports
      # survived into later steps.  Take it from there rather than from here.
      - name: Select and prove the acceptance toolchain
        run: |
          set -euo pipefail
          major="${CEC_TOOLCHAIN_GCC_MAJOR:?pin missing from the job env block}"
          cc="/usr/bin/gcc-$major"; cxx="/usr/bin/g++-$major"; gcov="/usr/bin/gcov-$major"
          for t in "$cc" "$cxx" "$gcov"; do
            test -x "$t" || { echo "::error::$t absent - install gcc-$major g++-$major"; exit 1; }
          done
          # -dumpfullversion, not -dumpversion: since GCC 7 the latter prints the
          # major alone, so comparing it against gcov's full version fails on every
          # correct toolchain.
          v_cc="$("$cc"  -dumpfullversion 2>/dev/null || "$cc"  -dumpversion)"
          v_cxx="$("$cxx" -dumpfullversion 2>/dev/null || "$cxx" -dumpversion)"
          v_gcov="$("$gcov" --version | head -n 1 | awk '{print $NF}')"
          printf 'gcc %s / g++ %s / gcov %s (pinned major %s)\n' \
                 "$v_cc" "$v_cxx" "$v_gcov" "$major"
          for v in "$v_cc" "$v_cxx" "$v_gcov"; do
            case "$v" in "$major".*) ;; *) echo "::error::$v is not major $major"; exit 1 ;; esac
          done
          [ "$v_cc" = "$v_cxx" ] && [ "$v_cc" = "$v_gcov" ] || {
            echo "::error::gcc/g++/gcov disagree: $v_cc / $v_cxx / $v_gcov"; exit 1; }
          shim="${RUNNER_TEMP:-/tmp}/toolchain-shim"; mkdir -p "$shim"
          ln -sfn "$gcov" "$shim/gcov"
          { echo "CC=$cc"; echo "CXX=$cxx"; } >> "$GITHUB_ENV"
          echo "$shim" >> "$GITHUB_PATH"
          # The exact patch is RECORDED with the results, which is the honest half
          # of a major-only pin.
          printf 'toolchain: gcc %s, g++ %s, gcov %s\n' "$v_cc" "$v_cxx" "$v_gcov" \
            >> "$GITHUB_STEP_SUMMARY"

      # The suite is compiled against GoogleTest v1.15.0, matching the CI pin.
      # CPPFLAGS/LDFLAGS below point the compiler and linker at this prefix.
      - uses: actions/checkout@v7
        with:
          repository: google/googletest
          ref: v1.15.0
          path: googletest
      - name: Build googletest
        run: |
          cmake -S "$GITHUB_WORKSPACE/googletest" -B build/googletest \
                -DCMAKE_INSTALL_PREFIX="$GITHUB_WORKSPACE/install/usr" \
                -DBUILD_GMOCK=ON -DBUILD_SHARED_LIBS=OFF \
                -DCMAKE_POSITION_INDEPENDENT_CODE=ON
          cmake --build build/googletest -j"$(nproc)"
          cmake --install build/googletest

      # rdk-halif-aidl supplies the com.rdk.hal.hdmicec AIDL/Binder HAL that
      # libRCEC now carries a second back-end for.  It is a sibling submodule of
      # the SUPERPROJECT, not of hdmicec, so nothing brings it in implicitly.
      #
      # The ref is a COMMIT SHA on purpose: it keeps the generated stubs, the
      # frozen common 0.2.0.0 import and halcompat.h on the one combination that
      # was validated together, which a branch name would not.  Both values below
      # mirror `.github/workflows/L1-tests.yml`, which is the authority for the
      # pin -- take them from there rather than from here.
      - uses: actions/checkout@v7
        with:
          repository: Blitzy-Sky/rdk-halif-aidl
          ref: 3ff17d171a46805decc7408fa69f545c80226d4b
          path: rdk-halif-aidl

      # THE SPARSE-CHECKOUT HAZARD, caught here rather than 150 lines later.
      # rdk-halif-aidl is documented as sparse-checked-out to hdmicec/ only, and
      # two of the four include roots live outside that path under common/ --
      # including common/current, the only place halcompat.h exists anywhere in
      # the tree.  actions/checkout is full by default, so all four should be
      # present; this proves it instead of assuming it.  On a sparse clone the
      # fix is `git sparse-checkout add common`.
      - name: Verify the rdk-halif-aidl include roots
        run: |
          set -euo pipefail
          for root in common/current/halcompat.h \
                      common/0.2.0.0/include \
                      hdmicec/0.1.0.0/include; do
            if [ ! -e "$GITHUB_WORKSPACE/rdk-halif-aidl/$root" ]; then
              echo "::error::missing $root - materialise common/ or take a full checkout"
              exit 1
            fi
          done

      # BINDER_VERSION is deliberately NOT set: build_binder.sh reads the
      # committed binder_sdk.version pin, and overriding it would decouple the
      # generated C++ from the generator that produced it.  .sdk_ready is the
      # script's completion marker, so asserting it catches a half-built SDK.
      - name: Build the Binder SDK
        run: |
          set -euo pipefail
          cd "$GITHUB_WORKSPACE/rdk-halif-aidl"
          PATH="/usr/bin:$PATH" ./build_binder.sh       # the >=3.22.1 cmake, step-scoped
          test -f "$BINDER_SDK_DIR/.sdk_ready"

      # common is built FIRST because build_modules.sh pre-checks that each
      # dependency's shared object already exists, and hdmicec 0.1.0.0 depends on
      # common.  common stays at 0.2.0.0 -- it is the ABI the released hdmicec
      # snapshot was generated and pinned against, and a newer release is not a
      # substitute.  No AIDL compiler is involved: both snapshots ship committed
      # pre-generated C++.
      - name: Build the AIDL client stub snapshots
        run: |
          set -euo pipefail
          cd "$GITHUB_WORKSPACE/rdk-halif-aidl"
          PATH="/usr/bin:$PATH" ./build_modules.sh common --version 0.2.0.0
          PATH="/usr/bin:$PATH" ./build_modules.sh hdmicec --version 0.1.0.0

      # Stub the IARM bus headers the middleware includes but does not ship, and
      # symlink the GoogleMock HDMI-CEC driver over the driver header.  Both are
      # build prerequisites, not optional extras: without the symlink the suite
      # compiles against the real HAL and does not link.
      - name: Generate stub headers and inject the HAL driver mock
        working-directory: hdmicec
        run: |
          mkdir -p stubs/rdk/iarmbus stubs/ccec/drivers/iarmbus
          touch stubs/rdk/iarmbus/libIARM.h \
                stubs/rdk/iarmbus/libIBus.h \
                stubs/rdk/iarmbus/libIBusDaemon.h \
                stubs/ccec/drivers/iarmbus/CecIARMBusMgr.h
          ln -sf ../../../mocks/hdmicec/hdmi_cec_driver.h \
                 stubs/ccec/drivers/hdmi_cec_driver.h

      # THE SECOND BUILD SYSTEM, AND THIS IS THE ONLY PLACE IT CAN BE CHECKED.
      # ccec/src/Makefile is the HAND-WRITTEN RDK build of the same library -- a bare
      # `make` from rdk_build.sh reaches it on every SOC branch -- and it carries its
      # own -std=c++17, DriverAidlImpl.o and the four AIDL/Binder link edges.  It is
      # listed in configure.ac's AC_CONFIG_FILES, so the ./configure in the next step
      # OVERWRITES it: this step has to run BEFORE that autoreconf, or it would build
      # Automake output through libtool and assert nothing at all about the RDK path.
      #
      # Two details here are not stylistic.  The mock and stub roots reach the
      # compiler through CPATH and must NOT be passed as `make CFLAGS=...`: that file
      # assigns CFLAGS with `:=` and then appends its own INCLUDE list, and a
      # command-line variable assignment overrides both -- silently dropping
      # -std=c++17 and all four AIDL include roots, leaving this step verifying a
      # build nobody performs.  And there is NO -j: `all: clean library` declares no
      # ordering between its two prerequisites, so a parallel make runs the link
      # concurrently with the clean and fails on a missing object.
      #
      # ALL FOUR DIRECT EDGES ARE ASSERTED, libcommon-v0.2.0.0-cpp INCLUDED, and the
      # fourth is the one that needs explaining.  No middleware symbol references it --
      # its PropertyValue is reached only through the hdmicec stub library's own
      # interface -- so --as-needed, the effective default in these toolchains, would
      # drop it.  Both build systems therefore scope -Wl,--no-as-needed to exactly
      # these four libraries and restore --as-needed after them: the hand-written
      # ccec/src/Makefile in its link command, and ccec/src/Makefile.am as a
      # comma-joined -Wl,--no-as-needed,<the four>,--as-needed group, because libtool
      # moves a bare -Wl,--no-as-needed to after the whole deplibs list where it would
      # have no effect.  That wrapping is the ONLY thing keeping this edge in the ELF,
      # which is precisely why it is asserted rather than left to the closure check:
      # delete -lcommon-v0.2.0.0-cpp from AIDL_LIBS and nothing else would notice.
      # Measured on the hand-built library: DT_NEEDED carries
      # libhdmicec-v0.1.0.0-cpp.so, libcommon-v0.2.0.0-cpp.so, libbinder.so and
      # libutils.so, and `ldd` reports nothing "not found".  Both real workflows assert
      # the same four -- L1-tests.yml for each build system, aidl-path-tests.yml for
      # each of its two -- so a three-entry loop here would contradict them.
      #
      # The clean at the end is part of the check rather than tidiness: these objects
      # are compiled WITHOUT -fprofile-arcs -ftest-coverage, and the libtool build
      # below would otherwise be free to reuse them.  The real workflow runs it
      # unconditionally, before its verdict, because the residue has to go whether
      # the checks passed or not.
      - name: Build and verify the hand-written RDK library (second build system)
        working-directory: hdmicec
        run: |
          set -euo pipefail
          export CPATH="$GITHUB_WORKSPACE/hdmicec/mocks:$GITHUB_WORKSPACE/hdmicec/stubs"
          make -C ccec/src all          # no -j; see the note above
          lib=ccec/src/install/lib/libRCEC.so
          test -f "$lib"
          readelf -d "$lib" | grep NEEDED
          ldd "$lib"
          needed="$(mktemp)"          # outside the checkout, so nothing is left behind
          readelf -d "$lib" | awk -F'[][]' '/NEEDED/ { print $2 }' > "$needed"
          for soname in libhdmicec-v0.1.0.0-cpp.so libcommon-v0.2.0.0-cpp.so \
                        libbinder.so libutils.so; do
            grep -qxF -- "$soname" "$needed" \
              || { echo "::error::$soname is not a DT_NEEDED entry of the hand-built $lib"; exit 1; }
          done
          rm -f "$needed"
          ! ldd "$lib" | grep -q 'not found'
          make -C ccec/src clean

      # Three make passes.  The top-level Makefile.am declares SUBDIRS = osal ccec,
      # so `make` at the root builds the libraries but no test binary -- and root
      # `make check` never reaches this suite at all.  It does not skip quietly
      # either: no `check` target exists at the configured root, so `make check`
      # there stops with "No rule to make target 'check'" and exit 2.  That is why
      # each tier below is driven with an explicit -C rather than from the root.
      # The third pass is what produces the L2 runner and the out-of-process fake
      # service host.
      #
      # The four AIDL include roots join CPPFLAGS and the two library directories
      # join LDFLAGS, and the four staging prefixes are passed to ./configure by
      # the names configure.ac declares.  That is not duplication: configure falls
      # back to the ambient flags for several of them, which is right for a sysroot
      # build and wrong to depend on here.  Nothing already in these variables is
      # removed -- the mocks, stubs and GoogleTest roots stay, and so do
      # -fprofile-arcs -ftest-coverage, without which coverage has no counters.
      #
      # There is no --enable-aidl to match --enable-l1tests, and there is no vendor
      # conditional: both HAL back-ends are compiled into libRCEC for every SOC and
      # the choice between them is made at run time.  The dialect is not set here
      # either -- it belongs to the three Makefile.am files, and configure probes
      # for it so an older toolchain fails at configure time.
      - name: Build hdmicec and both test tiers
        working-directory: hdmicec
        run: |
          autoreconf -if
          CPPFLAGS="-I$PWD/mocks -I$PWD/stubs -I$GITHUB_WORKSPACE/install/usr/include \
                    -I$HALIF_PREFIX/hdmicec/0.1.0.0/include \
                    -I$HALIF_PREFIX/common/0.2.0.0/include \
                    -I$HALIF_PREFIX/common/current \
                    -I$BINDER_SDK_INCLUDE_DIR/include/binder_sdk" \
          LDFLAGS="-L$GITHUB_WORKSPACE/install/usr/lib -L$HALIF_LIB_DIR \
                   -L$BINDER_SDK_DIR/lib/binder -fprofile-arcs -ftest-coverage" \
          CXXFLAGS="-fprofile-arcs -ftest-coverage" \
            ./configure --enable-l1tests \
              HALIF_PREFIX="$HALIF_PREFIX" \
              HALIF_LIB_DIR="$HALIF_LIB_DIR" \
              BINDER_SDK_DIR="$BINDER_SDK_DIR" \
              BINDER_SDK_INCLUDE_DIR="$BINDER_SDK_INCLUDE_DIR"
          make -j"$(nproc)" all
          make -C tests/L1Tests all
          make -C tests/L2Tests all

      # The loader closure, measured rather than inferred.  `readelf -d` alone
      # would report a healthy-looking library whose closure does not resolve, so
      # both are run: the direct edges libRCEC declares, and everything the loader
      # actually has to find -- which includes libbinder's own closure.
      - name: Verify the AIDL and Binder loader closure
        working-directory: hdmicec
        run: |
          set -euo pipefail
          readelf -d ccec/src/.libs/libRCEC.so | grep NEEDED
          ldd ccec/src/.libs/libRCEC.so
          ! ldd ccec/src/.libs/libRCEC.so | grep -q 'not found'

      # Run the libtool wrapper from its own directory, and ask GoogleTest for the
      # result file the upload step then publishes.  `make check` writes no result
      # file of any kind, so --gtest_output is what produces one.
      #
      # THIS IS INVOCATION A, and only A: the job-level GTEST_FILTER above is what
      # makes it green.  A hosted runner cannot run B, C or E at all -- they need a
      # binder driver, B and E to resolve the AIDL back-end and C to register the
      # incompatible service whose rejection is the fallback it establishes -- while
      # D needs nothing this runner lacks and is simply not scheduled by this job.  The
      # grep is what turns the run into a statement about the SELECTION rather than
      # just another green test run -- there is no introspection API, so the log
      # line the factory emits is the observable.
      - name: Run L1 tests (invocation A, legacy back-end)
        working-directory: hdmicec/tests/L1Tests
        run: |
          set -o pipefail
          ./run_L1Tests --gtest_print_time=1 \
                        --gtest_output=json:"$GITHUB_WORKSPACE/rdkL1TestResults.json" \
                        2>&1 | tee run_invocation_A.log
          grep -q 'HDMI CEC HAL back-end selected : legacy' run_invocation_A.log

      - name: Upload test results
        if: always()
        uses: actions/upload-artifact@v7
        with:
          name: test-results
          path: rdkL1TestResults.json
          if-no-files-found: error
```

Four notes on that example:

- **The uploaded path is the file the suite actually writes.**  `--gtest_output=json:<file>` (or
  `xml:<file>`) is what produces it; nothing in `make check` does.  An upload step pointed at
  `tests/L1Tests/*.xml` after a bare `make check` therefore uploads nothing, and with the default
  `if-no-files-found: warn` it reports success while doing so -- which is why `error` is set here.
- **The action majors are the ones current when this was written** (`checkout@v7`,
  `upload-artifact@v7`).  Do not copy an older pin: the v1, v2 and v3 artifact actions have been
  retired, so a workflow still on them fails outright.  Check
  each action's releases page before copying this and treat the exact major here as informative
  rather than normative -- the repository's own workflow is on different majors again.
- **What the example leaves out.**  The real workflow additionally runs a Valgrind memcheck pass,
  captures lcov coverage, and asserts each staged include root and library by name before using it.
  For coverage specifically, prefer `tests/L1Tests/run_coverage.sh` -- it reproduces the workflow's
  capture, filter and report steps and adds the per-file 80% gate, and the artifacts it leaves are
  the set described under
  [Artifacts, and the one list that decides their names](#artifacts-and-the-one-list-that-decides-their-names).
  The hand-written RDK step above is a reduction too: the real one also requires each expected
  library to resolve **inside the staged SDK tree**, so that an unrelated system copy of
  `libbinder.so` cannot leave the job green, and it names the loader closure it checks rather than
  only the direct edges.
- **What it leaves out for the AIDL path, and cannot supply.**  A hosted runner has no binder
  kernel driver and no `servicemanager`, so the example builds the AIDL back-end and runs
  **invocation A only** -- it never selects it.  Invocations B, C and E need the driver, a matching
  binder protocol version and that daemon -- C included, because registering its incompatible
  service opens the driver even though the back-end it then resolves is the legacy one -- which is
  why they live in `.github/workflows/aidl-path-tests.yml` on a guest provisioned for it.
  Nothing in the example should be read as evidence that the AIDL path executes.

## Next Steps

1. **Integration tests**: done -- `tests/L2Tests/` is that directory.  It holds its own runner and
   an out-of-process fake AIDL service host, and it asserts one CCEC round trip against each HAL
   back-end over that back-end's real transport.  **The legacy half runs here** -- invocation D,
   measured 11 passed and 4 skipped of 15, exit `0` -- and **the AIDL half does not**: its four
   cases are the ones that skip, and they need the binder-capable host described under
   [Runtime prerequisites for the AIDL path](#runtime-prerequisites-for-the-aidl-path), where they
   have run once (`blitzy/documentation/Project Guide.md` §4.2, invocation E at 8 passed and 6
   skipped) and not since.  Extending
   it means appending to
   `run_L2Tests_SOURCES` under the same group-end rule as item 2, and reading
   [Back-End Selection](#back-end-selection-the-invocation-matrix) first, because a case there
   belongs to one arm of the selection or to neither
2. **Order independence**: shuffled ordering remains a diagnostic and it does not pass.  The
   order-dependent assertion failures are deterministic per seed, while the production SIGSEGV
   documented in `tests/L1Tests/README.md` is intermittent and tied to no seed at all -- so **no
   seed is a reliable crash reproducer and a green seed proves nothing**.  The measured per-seed
   figures are deliberately not repeated here: read the **Test Order Dependence (diagnostic)**
   section of that README, which owns them, and re-derive the outcome on your own build after
   adding tests -- and note that new translation units must still be APPENDED to
   `run_L1Tests_SOURCES` rather than inserted, because static-initialisation order decides
   the order the suites run in
3. **Continuous testing**: the L1 workflow already runs this suite; `run_coverage.sh`
   reproduces its capture, filter and report steps locally with branch data enabled and a
   machine-checked >=80% line-coverage gate

Hardware mocking is already in place (`mocks/hdmicec/hdmi_cec_driver_mock.h`, injected by
symlinking it over the driver header the production code includes) and no test in this suite
is disabled.  gcov/lcov coverage reporting is wired: see `run_coverage.sh`.

## Troubleshooting

### gtest not found
```bash
# Check if gtest is installed
pkg-config --modversion gtest

# If not found, install or build from source
```

### Link errors
```bash
# Ensure libraries are in library path
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH

# Or add to configure
./configure --enable-l1tests LDFLAGS="-L/usr/local/lib"
```

### Test failures
```bash
# Run with per-test timings
./run_L1Tests --gtest_print_time=1

# Stop at the first failure instead of running the whole suite
./run_L1Tests --gtest_break_on_failure

# Capture a machine-readable result file to inspect the failure detail
./run_L1Tests --gtest_output=json:l1_results.json

# Debug specific test
gdb --args ./run_L1Tests --gtest_filter="FailingTest.*"
```

### `halcompat.h: No such file or directory`
The `common/current` include root is missing.  It is the only place that header exists, it is
**not** inside `common/0.2.0.0/include`, and it sits outside the `hdmicec/`-only sparse checkout
`rdk-halif-aidl` is documented with.
```bash
# Is the root there at all?
ls "$HALIF_PREFIX/common/current/halcompat.h"

# Absent because the checkout is sparse?  Materialise common/ (or take a full checkout)
git -C "$HALIF_PREFIX" sparse-checkout list
git -C "$HALIF_PREFIX" sparse-checkout add common

# Present, but configure did not find it: read back which roots it actually resolved
grep 'AIDL HDMI CEC back-end' config.log
```

### `cannot find -lbinder` / `-lutils` / `-lhdmicec-v0.1.0.0-cpp`
The staging prefixes did not resolve, or `libbinder`'s own closure is not reachable.  Remember that
the closure -- `liblog`, `libbase`, `libcutils` -- has to be findable for the **link**, not merely
for the run, because `libbinder.so` carries no `RUNPATH`.  `libcutils_sockets` is staged beside
them and is **not** in the closure -- nothing staged names it -- so its presence or absence is not
what this error is about.
```bash
# The two library directories, and the pinned common ABI by name
ls "$HALIF_LIB_DIR"/libhdmicec-v0.1.0.0-cpp.so "$HALIF_LIB_DIR"/libcommon-v0.2.0.0-cpp.so
ls "$BINDER_SDK_DIR"/lib/binder

# The closure has to be on the loader path for the link probe to pass
export LD_LIBRARY_PATH="$HALIF_LIB_DIR:$BINDER_SDK_DIR/lib/binder${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# What configure tried, verbatim
grep -A5 'AIDL stub and Binder client libraries' config.log
```

### `<optional>: No such file` or `expected '{' before '::' token`
`-std=c++17` did not reach that translation unit.  Both are the same fault seen from two
directions: the generated AIDL headers include `<optional>`, and `halcompat.h` opens a
nested-namespace definition that is C++17 syntax.
```bash
# What configure settled on.  It reports -std=c++17 on every path where the compiler accepts the
# flag, including the path where the ambient dialect already covered C++17, so this line reading
# anything else means configure did not get that far
grep 'C++17 flag' config.log

# What the failing directory actually compiles with
grep -n 'std=c++' tests/L1Tests/Makefile tests/L2Tests/Makefile ccec/src/Makefile
```

### `error while loading shared libraries: libbinder.so`
`LD_LIBRARY_PATH` does not cover the staged SDK.  The direct edges are not the whole story: the
loader also has to resolve `libbinder`'s closure, which nothing in the middleware names.
```bash
# Anything reported "not found" here is the answer
ldd ccec/src/.libs/libRCEC.so | grep 'not found'
ldd tests/L1Tests/.libs/run_L1Tests | grep 'not found'

export LD_LIBRARY_PATH="$HALIF_LIB_DIR:$BINDER_SDK_DIR/lib/binder${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

### An AIDL-path run selected the legacy back-end instead
Either the service was published *after* `LibCCEC::init` -- by then the selection has already
resolved and cannot change -- or this host has no binder driver, in which case the preflight
declines by design and the fallback is correct rather than faulty.  A run that looks green here
proves nothing about the AIDL path, so check the log line rather than the exit status.
```bash
# Which back-end was actually selected -- exactly one line per process
grep 'HDMI CEC HAL back-end selected' run_invocation_*.log

# Can this host run a binder-dependent invocation (B, C or E) at all?
ls -l /dev/binder* ; grep -w binder /proc/filesystems

# Let the runner decide, and report the ones it cannot attempt as deferred
./run_coverage.sh --run
```

## Resources

- [Google Test Documentation](https://google.github.io/googletest/)
- [Google Mock Documentation](https://google.github.io/googletest/gmock_for_dummies.html)
- [Google Test Primer](https://google.github.io/googletest/primer.html)
- [Advanced Testing Topics](https://google.github.io/googletest/advanced.html)
- [AIDL backends](https://source.android.com/docs/core/architecture/aidl/aidl-backends) -- the C++
  libbinder backend this middleware uses, and its threadpool requirement
- [rdkcentral/linux\_binder\_idl](https://github.com/rdkcentral/linux_binder_idl) -- the Binder SDK,
  including the kernel configuration an AIDL-path run needs.  Read its device-node guidance against
  the kernel you are actually provisioning: it documents the **binderfs** route, which exists only
  from Linux 5.0, and that is the route `.github/workflows/aidl-path-tests.yml` takes -- a
  `5.15.148` i386 kernel with `CONFIG_ANDROID_BINDERFS` enabled, whose in-guest init mounts
  binderfs and symlinks the nodes the initial instance takes from `CONFIG_ANDROID_BINDER_DEVICES`.
  Being a **protocol-7** guest as well does not make it a pre-binderfs one: mainline deleted
  `CONFIG_ANDROID_BINDER_IPC_32BIT` after 4.17, so the workflow restores that one option to the
  pinned 5.15 tree with an inline patch instead of dropping to a kernel too old for binderfs
- `tests/L1Tests/README.md` and `tests/L1Tests/QUICK_START.md` -- the same build and invocation
  contract at suite level, in more detail
- `mocks/README.md` -- what each seam under `mocks/hdmicec` is, including the test-scope fake AIDL
  service and its host

## Summary

The L1 unit test framework provides:
- ✅ 18 test fixtures with 483 individual tests, all passing, none disabled -- the legacy-path
  baseline, carried forward unmodified and now joined by the AIDL contract suite in
  `ccec/test_DriverAidl.cpp` and by the L2 integration tier
- ✅ Comprehensive coverage for both CCEC and OSAL libraries
  - Complete CEC opcode coverage (60+ opcodes tested)
  - All operand types tested (19 classes, 75 tests)
  - Message encoding/decoding thoroughly tested
  - All OpCode GetOpName() cases covered
- ✅ Easy to extend and maintain
- ✅ Integrated with build system via `--enable-l1tests`, which now brings in both the L1 suite and
  the L2 integration tier
- ✅ Both HAL back-ends **compiled in** from one build: `libRCEC` carries the legacy and the
  AIDL/Binder back-end in every build for every SOC vendor, with **no build-time switch and no
  vendor conditional** -- the choice is made at run time, so the suites are run once per selection
  outcome (see [Back-End Selection](#back-end-selection-the-invocation-matrix))
- ⚠️ **Both back-ends have been *executed*, each under its own selection -- but not both here.**  The
  legacy selection runs on any host and is measured green -- invocation A at 568 of 568, invocation D
  at 11 passed and 4 skipped of 15.  The three binder-dependent invocations (B, C, E) are **deferred
  on this host and on the committed CI runner**, for want of a Binder kernel driver, and have been
  executed green against a real binder transport in purpose-built binder-capable QEMU guests,
  recorded in `blitzy/documentation/Project Guide.md` §4.2: B 422 of 422 on the tree that then
  registered 606 (this tree selects 423), C 384 of 384, E 9 passed with 6 skipped of 15.  Of those three only **B and E** resolve the AIDL back-end: **C resolves the legacy
  one**, because it registers an in-process service whose interface hash is `"-1"` and asserts that
  the factory falls back -- and it is deferred here because a registration needs the driver, not
  because it selects the AIDL path.  So the AIDL transport **has** been observed working, over real
  Binder IPC, with an inbound callback arriving on a binder threadpool thread
- ⚠️ **That evidence is a guest's, not this runner's, and the difference is load-bearing.**  The
  **committed GitHub Actions job has never run**, so its published run count is zero, and the guest
  that produced the numbers above is not automatically the deliberately all-32-bit protocol-7 i386
  target that job provisions.  Nothing here should be read as evidence that this host, the hosted-CI
  runner or that committed job has observed the AIDL transport working
- ⚠️ **B, C and E cannot be reproduced on a host without a Binder driver**, which is the ordinary
  development host and the hosted-CI runner both.  There they are reported DEFERRED -- never
  attempted and never counted as passes -- because registering a service name calls
  `defaultServiceManager()`, which aborts the process when the driver node is missing.  A green
  driverless run is evidence about the legacy selection and the build integration, and nothing
  about the transport
- ✅ CI/CD ready with XML output support
- ✅ Production-grade testing infrastructure
- ✅ Singleton and global-state handling documented: `test_main.cpp` registers a single
  `CecTestEnvironment` (a `testing::Environment`) that installs the HAL driver mock, acts on
  `CEC_TEST_AIDL_MODE`, and then calls `LibCCEC::getInstance().init("CEC_TEST")` once for the whole
  program -- in that order, because `init` is what resolves the back-end selection -- so no fixture
  re-initialises the library. A test that must observe an uninitialised-state guard runs under
  `LibCCECUninitializedTest`, whose `SetUp()` reaches the uninitialised state and whose
  `TearDown()` restores the initialised `CEC_TEST` state, so the custody belongs to the
  fixture rather than to the test body and no sibling fixture can observe the gap

