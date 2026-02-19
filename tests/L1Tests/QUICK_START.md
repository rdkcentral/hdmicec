# L1 Unit Test Framework - Quick Reference

## Directory Structure

```
tests/
├── Makefile.am                   # Conditionally includes L1Tests
├── BasicTest.cpp                 # Existing integration tests
├── CECCmd.cpp
├── CECMonitor.cpp
├── CECCmdTest.cpp
└── L1Tests/                      # NEW: L1 Unit Tests
    ├── Makefile.am               # L1 test build configuration
    ├── README.md                 # L1 test documentation
    ├── test_main.cpp             # Test runner entry point
    ├── .gitignore                # Ignore build artifacts
    ├── ccec/                     # CCEC library tests (7 files)
    │   ├── test_CECFrame.cpp
    │   ├── test_Connection.cpp
    │   ├── test_LibCCEC.cpp
    │   ├── test_MessageEncoder.cpp
    │   ├── test_MessageDecoder.cpp
    │   ├── test_OpCode.cpp
    │   └── test_Operands.cpp
    └── osal/                     # OSAL library tests (3 files)
        ├── test_Mutex.cpp
        ├── test_Thread.cpp
        └── test_ConditionVariable.cpp
```

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

### Build with L1 Tests
```bash
autoreconf -fi
./configure --enable-l1tests
make
make check
```

### Build without L1 Tests (default)
```bash
./configure
make
```

### Run Tests Manually
```bash
cd tests/L1Tests
./run_L1Tests
```

### Run Specific Tests
```bash
./run_L1Tests --gtest_filter="CECFrameTest.*"
./run_L1Tests --gtest_filter="*Mutex*"
```

## Test Executable

- **Name**: `run_L1Tests`
- **Location**: `tests/L1Tests/`
- **Type**: Google Test executable
- **Sources**: 10+ test files + test_main.cpp
- **Total Tests**: 200+ individual test cases

## Key Features

✅ **Conditional Build**: Only builds when `--enable-l1tests` is specified  
✅ **Backward Compatible**: Existing tests (BasicTest, CECCmd, etc.) unchanged  
✅ **Clean Separation**: L1 tests in dedicated subdirectory  
✅ **Google Test Framework**: Industry-standard C++ testing  
✅ **Automated Testing**: Integrated with `make check`  

## Configuration Options

| Option | Description | Default |
|--------|-------------|---------|
| `--enable-l1tests` | Enable L1 unit tests | `no` |
| `--disable-l1tests` | Disable L1 unit tests | N/A |

## Next Steps

1. Install Google Test: `sudo apt-get install libgtest-dev libgmock-dev`
2. Configure: `./configure --enable-l1tests`
3. Build: `make`
4. Test: `make check`

See **UNIT_TEST_SETUP.md** for complete documentation.
