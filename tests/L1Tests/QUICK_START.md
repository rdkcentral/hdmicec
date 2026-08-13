# L1 Unit Test Framework - Quick Reference

## Directory Structure

```text
tests/
├── Makefile.am                   # Conditionally includes L1Tests
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
    ├── ccec/                     # CCEC library tests (12 files, 456 tests)
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
    │   └── test_Util.cpp
    └── osal/                     # OSAL library tests (3 files, 27 tests)
        ├── test_ConditionVariable.cpp
        ├── test_Mutex.cpp
        └── test_Thread.cpp
```

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
- Added `tests/L1Tests/Makefile` to configuration

### 2. Makefile.am (root)
```makefile
DIST_SUBDIRS = cfg osal ccec tests
```

### 3. tests/Makefile.am
```makefile
if ENABLE_L1TESTS
SUBDIRS = L1Tests
endif
```

### 4. tests/L1Tests/Makefile.am
- Defines `run_L1Tests` executable
- Links test sources with libRCEC.la and libRCECOSHal.la

## Quick Start

### Build and run, in one command
```bash
cd tests/L1Tests
./run_coverage.sh --build --run     # build, run, capture, report, gate at 80% lines
```

That is the maintained recipe.  Everything below is the same sequence by hand.

### Build with L1 Tests, by hand
Four steps are prerequisites rather than optional extras: the stub headers, the HAL-mock symlink,
`CPPFLAGS`/`PKG_CONFIG_PATH`, and a **separate** `make` pass for `tests/L1Tests`.  Omit any one and
the build fails or silently produces no test binary.

```bash
# From the hdmicec submodule root.  $GTEST_PREFIX must hold lib/pkgconfig/gtest.pc:
# configure.ac finds GoogleTest with PKG_CHECK_MODULES, which ignores -I and -L.
export GTEST_PREFIX="$HOME/.local/gtest-1.15.0"

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
  ./configure --enable-l1tests

make -j"$(nproc)" all
make -C tests/L1Tests all
```

`libglib2.0-dev` is a hard requirement of `configure`.  `autoreconf`/`configure` also rewrite six
git-tracked `Makefile` files in this submodule; they are local toolchain output, must never be
committed, and `./run_coverage.sh --restore` puts them back.

### Build without L1 Tests (default)
```bash
./configure
make
```

### Run Tests Manually
```bash
cd tests/L1Tests
./run_L1Tests          # the libtool wrapper in this directory, NOT .libs/run_L1Tests
```

Root `make check` does **not** run this suite: the top-level `Makefile.am` declares
`SUBDIRS = osal ccec`, so `check` never descends into `tests/`.  Use
`make -C tests/L1Tests check` from the submodule root when you want automake to drive it.

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
./run_coverage.sh --build --run     # build, run, capture, report, gate at 80% lines
```

Artifacts (`coverage.info`, `filtered_coverage.info`, `coverage/index.html`
and the per-file tables) have **no fixed default path**.  With no
`--output-dir` the script mints a fresh directory per run with
`mktemp -d "${TMPDIR:-/tmp}/hdmicec-l1-coverage.XXXXXXXX"` at mode 0700 and
prints it as the `artifacts:` line at startup — read that line to find the
files.  An unpredictable name cannot be pre-created by another local
account, and it is outside the git tree because `.gitignore` does not cover
these names.  Pass `--output-dir DIR` (or set `COVERAGE_OUTPUT_DIR`) when
you need a stable location; `--output-dir .` reproduces the CI layout
in-tree, at which point they are build artifacts that must not be
committed.  The per-file gate is on by default, so a single file below 80%
fails the run.  See `./run_coverage.sh --help` for the full option and
environment reference.

## Test Executable

- **Name**: `run_L1Tests`
- **Location**: `tests/L1Tests/`
- **Type**: Google Test executable
- **Sources**: 15 test translation units + test_main.cpp (all listed in
  `run_L1Tests_SOURCES`, which is the only gate on what gets compiled)
- **Total Tests**: 483 individual test cases in 18 fixtures — all passing,
  none disabled — measured with `./run_L1Tests --gtest_list_tests`.  There are
  more fixtures than translation units because two files declare two fixtures
  each: `ccec/test_LibCCEC.cpp` (`LibCCECTest` and `LibCCECUninitializedTest`)
  and `ccec/test_MessageDecoder.cpp` (`MessageDecoderTest` and
  `MessageDecoderTrackingTest`)

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

## Next Steps

1. Provide GoogleTest so that `pkg-config` can find a `gtest.pc` — either
   `sudo apt-get install libgtest-dev libgmock-dev` where the distribution's copy still compiles
   as C++14, or a source build into a private prefix exported as `GTEST_PREFIX` (see
   [Quick Start](#quick-start))
2. Generate the stubs and the HAL-mock symlink, then `autoreconf -if` and
   `./configure --enable-l1tests` with `CPPFLAGS`/`PKG_CONFIG_PATH` set
3. Build: `make all` **and then** `make -C tests/L1Tests all`
4. Test: `./run_L1Tests` from `tests/L1Tests`, or `make -C tests/L1Tests check`

Or let `./run_coverage.sh --build --run` do all four.

See **UNIT_TEST_SETUP.md** for complete documentation.
