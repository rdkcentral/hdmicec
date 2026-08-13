# HDMI-CEC L1 Tests

This directory contains the L1 unit tests for the hdmicec library using Google Test (gtest/gmock).

## Framework

- **Test Framework**: Google Test (gtest) v1.10.0+
- **Mocking Framework**: Google Mock (gmock)
- **Language**: C++14 — set by `AM_CXXFLAGS = -Wall -std=c++14 -g` in `Makefile.am`
- **Build System**: Autotools
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
libraries were built there.

## Building Tests

**The short answer:** `./run_coverage.sh --build --run` from this directory does the whole
sequence below — build, run, capture, report and gate — and is the recipe this suite is
maintained against.

The long answer, for building by hand.  Four prerequisites are easy to miss, and every one of
them is fatal rather than degrading:

1. **`stubs/` must exist.**  The middleware includes IARM bus headers it does not ship.
2. **The HAL driver mock must be symlinked over the driver header.**  That symlink is the entire
   mocking seam; without it the build looks for a real HDMI-CEC driver.
3. **`CPPFLAGS` must carry `mocks/` and `stubs/`, and `PKG_CONFIG_PATH` must reach a `gtest.pc`.**
4. **`make` at the top level does not build this suite.**  `tests/L1Tests` needs its own
   `make` pass — see [Running Tests](#running-tests) for why root `make check` does not help
   either.

```bash
# From the hdmicec submodule root
export GTEST_PREFIX="$HOME/.local/gtest-1.15.0"       # or wherever gtest.pc lives

# 1+2. Stub headers, and the HAL driver mock injected over the driver header
mkdir -p stubs/rdk/iarmbus stubs/ccec/drivers/iarmbus
touch stubs/rdk/iarmbus/libIARM.h \
      stubs/rdk/iarmbus/libIBus.h \
      stubs/rdk/iarmbus/libIBusDaemon.h \
      stubs/ccec/drivers/iarmbus/CecIARMBusMgr.h
ln -sf ../../../mocks/hdmicec/hdmi_cec_driver.h stubs/ccec/drivers/hdmi_cec_driver.h

# 3. Generate and configure.  Drop the two coverage flags for a plain build.
autoreconf -if
PKG_CONFIG_PATH="$GTEST_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
CPPFLAGS="-I$PWD/mocks -I$PWD/stubs -I$GTEST_PREFIX/include" \
LDFLAGS="-L$GTEST_PREFIX/lib -fprofile-arcs -ftest-coverage" \
CXXFLAGS="-fprofile-arcs -ftest-coverage" \
  ./configure --enable-l1tests

# 4. Build the libraries, then the suite
make -j"$(nproc)" all
make -C tests/L1Tests all
```

`autoreconf`/`configure` rewrite six git-tracked `Makefile` files in this submodule.  They are
local toolchain output and must never be committed; `./run_coverage.sh --restore` puts them back.

## Test Structure

```
tests/L1Tests/
├── Makefile.am           # Autotools build configuration; run_L1Tests_SOURCES is the ONLY
│                         #   gate on what gets compiled, and its order is significant
├── run_coverage.sh       # gcov/lcov runner, per-file table and >=80% line-coverage gate
├── .lcovrc_l1            # lcov configuration with branch collection enabled
├── test_main.cpp         # Test runner entry point; installs the single global
│                         #   testing::Environment that creates the driver mock
├── ccec/                 # CCEC library tests (456 tests)
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
│   └── test_Util.cpp            # 12 tests - log-level configuration, buffer dump
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

One further translation unit is compiled into `run_L1Tests` from outside this directory:
`../../mocks/hdmicec/hdmi_cec_driver_mock.cpp`, the GoogleMock HDMI-CEC HAL driver mock.  It
defines no test cases of its own, so it contributes none of the 483.

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

## Running Tests

Run from **this directory**, and run the `run_L1Tests` that sits here — it is the libtool wrapper
script, and it sets the library search path the real binary under `.libs/` needs.  Invoking
`.libs/run_L1Tests` directly is the usual cause of a loader error that looks like a build failure.

Root `make check` does **not** run this suite.  The top-level `Makefile.am` declares
`SUBDIRS = osal ccec`, so a `check` at the top level never descends into `tests/`.  The two
invocations that do work are `make -C tests/L1Tests check` from the submodule root (this
directory's `Makefile.am` declares `TESTS = run_L1Tests`) and running the wrapper directly:

```bash
# Run all tests -- from tests/L1Tests
./run_L1Tests

# ...or through automake's check target, from the hdmicec submodule root
make -C tests/L1Tests check

# Run specific test with filter
./run_L1Tests --gtest_filter="CECFrameTest.*"

# Run with per-test output and timings (there is no --gtest_verbose flag; see note below)
./run_L1Tests --gtest_print_time=1 --gtest_brief=0

# Generate XML report
./run_L1Tests --gtest_output=xml:test_results.xml

# List all tests
./run_L1Tests --gtest_list_tests

# Diagnostic only: expose inter-test order dependencies (see Known Issues).
# Seed 2 reproduces the two known legacy failures every run; seed 12345 is green about
# seven runs in ten and crashes in the others, so one run of one seed proves nothing.
./run_L1Tests --gtest_shuffle --gtest_random_seed=2

# Deterministic, seed-free check that a new test does not depend on the mock's
# leftover callback registration (see Test Order Dependence)
./run_L1Tests --gtest_filter='HdmiCecDriverMockTest.*:YourFixture.*' --gtest_repeat=2

# Coverage capture, report and the 80% line gate
./run_coverage.sh --run
```

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
  (`--gtest_filter=YourTest.TestName`) and in the full suite.

## Test Categories

- **Unit Tests**: Test individual classes/functions in isolation
- **Driver-dependent tests**: exercised through the GoogleMock HDMI-CEC HAL driver mock in
  `../../mocks/hdmicec/`, which is registered in `run_L1Tests_SOURCES` and injected by
  symlinking `mocks/hdmicec/hdmi_cec_driver.h` over `stubs/ccec/drivers/hdmi_cec_driver.h`.
  No test requires real CEC hardware, and none is skipped for the want of it.
- **Disabled tests**: there are none.  No test name in `ccec/` or `osal/` carries GoogleTest's
  disable prefix, all 483 cases are enabled and run, and
  `./run_L1Tests --gtest_also_run_disabled_tests --gtest_list_tests` lists the same 483 as a
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

### CCEC Library Tests (456 tests)

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

- **`run_coverage.sh`** — captures gcov data, writes an HTML report, prints a per-file table of
  line, function and branch figures read out of the lcov trace, enumerates every file below the
  bar together with its uncovered line numbers, and **exits non-zero when line coverage misses
  the threshold**.  It reproduces the capture, filter and report recipe of
  `.github/workflows/L1-tests.yml` — including that workflow's exclusion globs verbatim, which is
  what keeps the coverage denominator production-source-only — and adds the two things the
  workflow lacks: branch data and a numeric gate.
- **`.lcovrc_l1`** — the LCOV configuration for this suite, with `lcov_branch_coverage = 1` and
  `lcov_function_coverage = 1`.  Stated plainly: **CI does not read this file.**  The workflow
  passes neither `--config-file` nor `--rc`, so the CI coverage step inherits the system default
  and middleware branch visibility is developer-opt-in.  `run_coverage.sh` is the in-tree consumer;
  it passes this file to `lcov` and `genhtml` and additionally forces `--rc branch_coverage=1`.

```bash
# Build (instrumented), run the suite, capture, report and gate
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

Three files are waived from the per-file **vote** by name, in `COVERAGE_GATE_EXEMPT_FILES` inside
the script, each with the reason it is there and the figure it was measured at:
`ccec/include/ccec/Operand.hpp` at 0.0% (0/6, abstract-interface inline members every concrete
operand overrides), `ccec/include/ccec/Exception.hpp` at 33.3% (4/12, constructor bodies of
exceptions the driver mock's return codes never raise) and `ccec/include/ccec/Messages.hpp` at
76.5% (228/298, inline serialise/deserialise members of message classes the authorised test
inventory does not construct).  A waiver is **not** an exclusion glob: all three stay in the trace,
stay in the aggregate denominator that `lcov --fail-under-lines` judges, keep their real figures in
the per-file table, and have their uncovered line numbers written to `uncovered_lines.txt`.  All
three carry exactly those figures without this project's four new translation units, so they are a
pre-existing property of the suite rather than a regression.

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
  `libglib2.0-dev` is a hard requirement of `configure`.  `autoreconf`/`configure` also rewrite six
  git-tracked `Makefile` files in this submodule; those are local toolchain output and must never
  be committed.  `./run_coverage.sh --restore` restores them.

### Coverage output is a build artifact — never commit it

`run_coverage.sh` writes `coverage.info`, `filtered_coverage.info`, `coverage/`,
`per_file_coverage.tsv`, `uncovered_lines.txt`, `rdkL1TestResults.json` and its per-step logs.
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
artifacts out of a commit is yours to manage.

## Known Issues and Notes

### No Disabled Tests

**This suite has no disabled tests.**  `./run_L1Tests` reports 483 tests run, 483 passed,
0 disabled.  Verify with:

```bash
./run_L1Tests --gtest_list_tests | grep DISABLED_    # expect no output
```

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
[Test Order Dependence](#test-order-dependence-diagnostic): **14 consecutive full-suite runs in the
default order, 14 green (483 / 483), 0 exit-139** — and the crash still fires as soon as the order
changes, at **11 exit-139 in 81 shuffled full-suite runs (13.6%)**, spread across seeds 7, 15 and
12345.

**No seed is a reliable reproducer, and no seed is safe.**  Seed 12345 crashed 3 runs in 10, seed 15
4 in 16 and seed 7 3 in 16, while seed 5 (6 runs), seed 2 (10 runs) and seed 54321 (10 runs) did not
crash once — the remaining seeds in the tables below had one run each, which settles nothing about
their rate either way.  Seed 54321 is the reason this paragraph is worded the way it is: an earlier
revision of this file quoted it as crashing **5 runs out of 5**, and it is now green **10 runs out of
10**.  A per-seed crash figure is a snapshot of a race, not a property of the seed, so re-measure it
rather than quoting one; the per-seed numbers behind these totals are in
[Test Order Dependence](#test-order-dependence-diagnostic).  The default order keeps the window
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

The suite is green in its default order — 483 of 483, exit `0`.  **Under a randomised order it is
not**, and a single green seed proves nothing either way, so run more than one:

```bash
./run_L1Tests --gtest_shuffle --gtest_random_seed=<seed>
```

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

**What remains is the pre-existing fragility, and it is now exactly two cases**:
`ConnectionTest.MatchSourceMismatchedAddress` and `ConnectionTest.SendAsyncMatchSource`.  They pass
in the default order, they are not this project's tests, and they are deliberately left as they are.
Six further legacy cases that used to fail under a shuffle — `BusTest.ListenerFiltering`,
`BusTest.UnregisteredConnectionReceivesAll`, `ConnectionTest.MultipleListenersNotification`,
`ConnectionTest.DefaultFilterUnregistered`, `ConnectionTest.DefaultFilterSpecificAddress` and
`ConnectionTest.DefaultFilterBroadcast` — stopped failing without being edited, because they were
victims of the same hijacked callback registration.

#### Assertion outcome, per seed

Fifteen seeds, measured on this tree after the fix.  Every run that reached the summary reported 483
tests run and zero skipped:

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

The failing set is specific to the seed *and* to the test inventory: re-derive it after adding
tests rather than trusting the tables above, because shuffling reorders the whole program and a
different seed exposes a different subset of the same underlying fragility.  Corroborating evidence
sits in the driver file:
`DriverTest.AAA_DriverSingletonAccess` is commented "runs first alphabetically", and
`DriverTest.ZZZ_OpenWithFailure` and `DriverTest.ZZZ_CloseWithFailure` are commented "runs late
to avoid breaking other tests" — deliberate alphabetical-ordering prefixes, which only make
sense as a workaround for this fragility.  `ZZZ_OpenWithFailure` is also the case that was running
in all 11 crashing runs, so the prefix bounds the assertion problem and not the race.

Order dependence in this suite is therefore **reduced, not resolved**, and **randomised-order runs
remain diagnostic here, not an acceptance gate**.  Three consequences for anyone adding tests:

- Place new translation units in `run_L1Tests_SOURCES` *after* every pre-existing entry of their
  layer, so no established suite is reordered.  GoogleTest registers each `TEST_F` during static
  initialisation, in link order, and runs the suites in registration order, so a file inserted
  ahead of an existing suite reorders that suite.
- A new test must establish its own preconditions and leave shared state untouched, and must be
  verified both in isolation (`--gtest_filter=YourFixture.*`) and in the full suite.
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
default run, and every shuffled run in the tables above that reached its summary, report 483 tests
run and zero skipped — green seeds and failing seeds alike.  They are dead defensive code
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
