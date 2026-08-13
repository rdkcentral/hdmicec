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
│   └── test_Util.cpp             # Log-level configuration and buffer dump
└── osal/                         # OSAL library tests
    ├── test_ConditionVariable.cpp # Condition variable tests
    ├── test_Mutex.cpp             # Mutex lock/unlock and copy-semantics tests
    └── test_Thread.cpp            # Thread construction, dispatch and detach tests
```

Every file listed above exists on disk and is git-tracked: `git ls-files tests/L1Tests`
returns exactly these 22 files and nothing else (`ccec/` and `osal/` are the two
directories holding them).  Anything else you see in that directory after a build --
`*.o`, `*.gcno`, `*.gcda`, `.deps/`, `.libs/`, `Makefile`, `Makefile.in`, `run_L1Tests`,
`*.log`, `*.trs` -- is generated output matched by `.gitignore`.

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

## Installation Steps

### 1. Install the build tooling and Google Test

`configure` has two hard external dependencies and will not proceed without either:
`PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 0.10.28])` and
`PKG_CHECK_MODULES([GTEST], [gtest >= 1.10.0])`.  Note what the second one implies —
**`PKG_CHECK_MODULES` consults `pkg-config` only and ignores `-I`/`-L` entirely**, so GoogleTest
has to be reachable as a `gtest.pc`, not merely as headers and archives on a path.

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
This suite compiles at `-std=c++14` (`AM_CXXFLAGS` in `tests/L1Tests/Makefile.am`).  GoogleTest
1.17 and later require C++17, so on a recent distribution the packaged copy will not compile
against this project at all.  Build it **unprivileged, into a private prefix**, and expose that
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
- Added `tests/L1Tests/Makefile` to AC_CONFIG_FILES

#### Makefile.am (root)
- Added `tests` to DIST_SUBDIRS

#### tests/Makefile.am
- Conditionally includes L1Tests subdirectory when `--enable-l1tests` is used
- Maintains backward compatibility with existing test applications

#### tests/L1Tests/Makefile.am
- Defines `run_L1Tests` test executable
- Links against libRCEC.la and libRCECOSHal.la
- Includes all test source files

### 3. Configure and Build

The whole sequence is automated — `cd tests/L1Tests && ./run_coverage.sh --build --run` builds
instrumented, runs the suite, captures coverage and applies the 80% line gate.  What follows is
that same sequence by hand.

Three prerequisites precede `configure`, and each is fatal rather than degrading if skipped:

- **Stub headers.**  The middleware includes IARM bus headers it does not ship.
- **The HAL driver mock symlink.**  It substitutes the GoogleMock HDMI-CEC driver for the real
  driver header, and it is the entire mocking seam — no test in this suite needs hardware.
- **`CPPFLAGS` reaching `mocks/` and `stubs/`, and `PKG_CONFIG_PATH` reaching a `gtest.pc`.**

```bash
# From the hdmicec submodule root
export GTEST_PREFIX="$HOME/.local/gtest-1.15.0"     # or the distribution's prefix

# 3a. Stub headers, and the HAL driver mock injected over the driver header
mkdir -p stubs/rdk/iarmbus stubs/ccec/drivers/iarmbus
touch stubs/rdk/iarmbus/libIARM.h \
      stubs/rdk/iarmbus/libIBus.h \
      stubs/rdk/iarmbus/libIBusDaemon.h \
      stubs/ccec/drivers/iarmbus/CecIARMBusMgr.h
ln -sf ../../../mocks/hdmicec/hdmi_cec_driver.h stubs/ccec/drivers/hdmi_cec_driver.h

# 3b. Generate build scripts and configure with L1 tests enabled.
#     Add -fprofile-arcs -ftest-coverage to CXXFLAGS and LDFLAGS for an instrumented build.
autoreconf -fi
PKG_CONFIG_PATH="$GTEST_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
CPPFLAGS="-I$PWD/mocks -I$PWD/stubs -I$GTEST_PREFIX/include" \
LDFLAGS="-L$GTEST_PREFIX/lib" \
  ./configure --enable-l1tests

# 3c. Build the libraries, THEN the suite.  The second pass is required:
#     the top-level Makefile.am declares SUBDIRS = osal ccec, so `make` at the
#     root builds no test binary.
make -j"$(nproc)" all
make -C tests/L1Tests all
```

`autoreconf`/`configure` rewrite six git-tracked `Makefile` files in this submodule.  They are
local toolchain output and must never be committed; `tests/L1Tests/run_coverage.sh --restore`
puts them back.

## Running Tests

### Basic Usage

Root `make check` does **not** run this suite — `SUBDIRS = osal ccec` in the top-level
`Makefile.am` means `check` never descends into `tests/`.  Drive it either through this suite's own
`check` target (`tests/L1Tests/Makefile.am` declares `TESTS = run_L1Tests`) or by running the
libtool wrapper from its own directory.  Run the wrapper, not `.libs/run_L1Tests`: the wrapper sets
the library search path the real binary needs.

```bash
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

# Run specific test suite
./run_L1Tests --gtest_filter="CECFrameTest.*"

# Run multiple test patterns (this one selects the 22 OSAL Mutex and Thread cases)
./run_L1Tests --gtest_filter="*Mutex*:*Thread*"

# List all available tests
./run_L1Tests --gtest_list_tests

# Generate XML report (for CI/CD)
./run_L1Tests --gtest_output=xml:test_results.xml

# Repeat tests for flakiness detection
./run_L1Tests --gtest_repeat=100

# Shuffle test execution order (diagnostic only -- see "Order independence" below;
# --gtest_random_seed pins the order so a finding is reproducible)
./run_L1Tests --gtest_shuffle --gtest_random_seed=12345
```

`--gtest_shuffle` is a diagnostic, not a gate -- and it does **not** pass.  Default order is green
and stable (thirty consecutive runs, 483/483, no crashes), but shuffled order is not: seeds `1`,
`2`, `42`, `100`, `500`, `777` and `20260808` fail deterministically, five of five repeats at `1`,
`42` and `500`, and seed `54321` ends in a production SIGSEGV five of five.  Use `42` or `100` to
reproduce the ordering failures.  Do **not** cite seed `12345` as a reproducer: it exits 0 four
runs in five here, even though it is the seed the project's own notes recorded as failing.  The
recurring cases are `BusTest.ListenerFiltering`, `BusTest.UnregisteredConnectionReceivesAll`,
`ConnectionTest.MatchSourceMismatchedAddress` and `ConnectionTest.SendAsyncMatchSource`; the two
production defects behind the crashing seed are documented in `tests/L1Tests/README.md`.  The fixtures that own the shared library's lifecycle now wait for the bus reader to
publish RUNNING before terminating it, so that window is not opened.  One green seed is not a
proof of order independence: use the flag to check that a test you just wrote is self-sufficient,
and re-derive the outcome after adding tests rather than trusting the figure quoted here.

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

### GitHub Actions Example

```yaml
name: L1 Unit Tests

on: [push, pull_request]

permissions:
  contents: read

jobs:
  test:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v7
        with:
          path: hdmicec

      # libgtest-dev/libgmock-dev are what put gtest.pc and gmock.pc on
      # pkg-config's path, which is the only thing configure's PKG_CHECK_MODULES
      # consults.  GLIB is the other hard configure dependency.
      - name: Install packages
        run: |
          sudo apt-get update
          sudo apt-get install -y autoconf automake libtool pkg-config g++ make \
                                  libglib2.0-dev libgtest-dev libgmock-dev lcov

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

      # Two make passes.  The top-level Makefile.am declares SUBDIRS = osal ccec,
      # so `make` at the root builds the libraries but no test binary -- and root
      # `make check` never reaches this suite at all.
      - name: Build hdmicec and the L1 suite
        working-directory: hdmicec
        run: |
          autoreconf -if
          CPPFLAGS="-I$PWD/mocks -I$PWD/stubs -I$GITHUB_WORKSPACE/install/usr/include" \
          LDFLAGS="-L$GITHUB_WORKSPACE/install/usr/lib -fprofile-arcs -ftest-coverage" \
          CXXFLAGS="-fprofile-arcs -ftest-coverage" \
            ./configure --enable-l1tests
          make -j"$(nproc)" all
          make -C tests/L1Tests all

      # Run the libtool wrapper from its own directory, and ask GoogleTest for the
      # result file the upload step then publishes.  `make check` writes no result
      # file of any kind, so --gtest_output is what produces one.
      - name: Run L1 tests
        working-directory: hdmicec/tests/L1Tests
        run: |
          ./run_L1Tests --gtest_print_time=1 \
                        --gtest_output=json:"$GITHUB_WORKSPACE/rdkL1TestResults.json"

      - name: Upload test results
        if: always()
        uses: actions/upload-artifact@v7
        with:
          name: test-results
          path: rdkL1TestResults.json
          if-no-files-found: error
```

Three notes on that example:

- **The uploaded path is the file the suite actually writes.**  `--gtest_output=json:<file>` (or
  `xml:<file>`) is what produces it; nothing in `make check` does.  An upload step pointed at
  `tests/L1Tests/*.xml` after a bare `make check` therefore uploads nothing, and with the default
  `if-no-files-found: warn` it reports success while doing so -- which is why `error` is set here.
- **The action majors are the ones current when this was written** (`checkout@v7`,
  `upload-artifact@v7`).  Do not copy an older pin: the v1, v2 and v3 artifact actions have been
  retired, so a workflow still on them fails outright.  Check
  each action's releases page before copying this and treat the exact major here as informative
  rather than normative -- the repository's own workflow is on different majors again.
- **What the example leaves out.**  The real workflow additionally pins CMake to 3.16.x, runs a
  Valgrind memcheck pass, and captures lcov coverage.  For coverage specifically, prefer
  `tests/L1Tests/run_coverage.sh` -- it reproduces the workflow's capture, filter and report steps
  and adds the per-file 80% gate.

## Next Steps

1. **Integration tests**: Consider adding integration tests in a separate directory
2. **Order independence**: a `--gtest_shuffle --gtest_random_seed=12345` run of this suite
   passes 483 of 483 on this tree in four runs out of five, but one green seed is not a proof --
   the fifth run of that seed ends in the production SIGSEGV documented in
   `tests/L1Tests/README.md`, so re-derive the outcome after
   adding tests, and new translation units must still be APPENDED to
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

## Resources

- [Google Test Documentation](https://google.github.io/googletest/)
- [Google Mock Documentation](https://google.github.io/googletest/gmock_for_dummies.html)
- [Google Test Primer](https://google.github.io/googletest/primer.html)
- [Advanced Testing Topics](https://google.github.io/googletest/advanced.html)

## Summary

The L1 unit test framework provides:
- ✅ 18 test fixtures with 483 individual tests, all passing, none disabled
- ✅ Comprehensive coverage for both CCEC and OSAL libraries
  - Complete CEC opcode coverage (60+ opcodes tested)
  - All operand types tested (19 classes, 75 tests)
  - Message encoding/decoding thoroughly tested
  - All OpCode GetOpName() cases covered
- ✅ Easy to extend and maintain
- ✅ Integrated with build system via `--enable-l1tests`
- ✅ CI/CD ready with XML output support
- ✅ Production-grade testing infrastructure
- ✅ Singleton and global-state handling documented: `test_main.cpp` registers a single
  `CecTestEnvironment` (a `testing::Environment`) that installs the HAL driver mock and
  calls `LibCCEC::getInstance().init("CEC_TEST")` once for the whole program, so no fixture
  re-initialises the library. A test that must observe an uninitialised-state guard runs under
  `LibCCECUninitializedTest`, whose `SetUp()` reaches the uninitialised state and whose
  `TearDown()` restores the initialised `CEC_TEST` state, so the custody belongs to the
  fixture rather than to the test body and no sibling fixture can observe the gap

