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
├── Makefile.am                   # Build configuration
├── test_main.cpp                 # Test runner entry point
├── ccec/                         # CCEC library tests
│   ├── test_CECFrame.cpp        # CECFrame class tests
│   ├── test_Connection.cpp      # Connection class tests
│   ├── test_LibCCEC.cpp         # LibCCEC singleton tests
│   ├── test_MessageEncoder.cpp  # Message encoding tests
│   ├── test_MessageDecoder.cpp  # Message decoding tests
│   ├── test_OpCode.cpp          # OpCode enum/class tests
│   └── test_Operands.cpp        # PhysicalAddress/LogicalAddress tests
└── osal/                         # OSAL library tests
    ├── test_Mutex.cpp           # Mutex locking tests
    ├── test_Thread.cpp          # Thread execution tests
    └── test_ConditionVariable.cpp # Condition variable tests
```

## Test Coverage

### CCEC Library Tests (7 test files)
- **CECFrame**: Constructor, copy, hex dump functionality
- **Connection**: Object creation, open/close (hardware-dependent disabled)
- **LibCCEC**: Singleton pattern, initialization (hardware-dependent disabled)
- **MessageEncoder**: Encoding CEC messages (ImageViewOn, TextViewOn, ActiveSource)
- **MessageDecoder**: Decoding CEC frames
- **OpCode**: Constants validation, string conversion
- **Operands**: PhysicalAddress and LogicalAddress creation

### OSAL Library Tests (3 test files)
- **Mutex**: Lock/unlock, concurrency protection
- **Thread**: Creation, execution with Runnable
- **ConditionVariable**: Notify/wait patterns

## Installation Steps

### 1. Install Google Test

#### Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install libgtest-dev libgmock-dev cmake
```

#### Build from source (if packages don't include libraries):
```bash
cd /usr/src/gtest
sudo cmake .
sudo make
sudo cp lib/*.a /usr/lib

cd /usr/src/gmock
sudo cmake .
sudo make
sudo cp lib/*.a /usr/lib
```

#### RHEL/CentOS:
```bash
sudo yum install gtest-devel gmock-devel
```

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

```bash
# Generate build scripts
autoreconf -fi

# Configure with L1 tests enabled
./configure --enable-l1tests

# Build the library and tests
make

# Run all tests
make check
```

## Running Tests

### Basic Usage

```bash
# Run all tests
make check

# Run tests directly
./tests/L1Tests/run_L1Tests

# Run with verbose output
./tests/L1Tests/run_L1Tests --gtest_verbose
```

### Advanced Usage

```bash
# Navigate to test directory
cd tests/L1Tests

# Run specific test suite
./run_L1Tests --gtest_filter="CECFrameTest.*"

# Run multiple test patterns
./run_L1Tests --gtest_filter="*Mutex*:*Thread*"

# List all available tests
./run_L1Tests --gtest_list_tests

# Generate XML report (for CI/CD)
./run_L1Tests --gtest_output=xml:test_results.xml

# Repeat tests for flakiness detection
./run_L1Tests --gtest_repeat=100

# Shuffle test execution order
./run_L1Tests --gtest_shuffle
```

## Writing New Tests

### Example Test File

Create `tests/L1Tests/ccec/test_NewClass.cpp`:

```cpp
#include <gtest/gtest.h>
#include "ccec/NewClass.hpp"

using namespace CCEC;

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

### GitHub Actions Example

```yaml
name: L1 Unit Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y libgtest-dev libgmock-dev libglib2.0-dev
      - name: Build and test
        run: |
          autoreconf -fi
          ./configure --enable-l1tests
          make check
      - name: Upload test results
        if: always()
        uses: actions/upload-artifact@v2
        with:
          name: test-results
          path: tests/L1Tests/*.xml
```

## Next Steps

1. **Enable hardware mocking**: Implement mock drivers for hardware-dependent tests (marked with `DISABLED_`)
2. **Increase coverage**: Add more test cases for edge cases and error paths
3. **Integration tests**: Consider adding integration tests in a separate directory
4. **Code coverage**: Integrate gcov/lcov for coverage reporting
5. **Continuous testing**: Set up CI/CD pipeline with automated test execution

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
# Run with more verbosity
./run_L1Tests --gtest_verbose --gtest_print_time

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
- ✅ 10 test suites with 20+ individual tests
- ✅ Coverage for both CCEC and OSAL libraries
- ✅ Easy to extend and maintain
- ✅ Integrated with build system via `--enable-l1tests`
- ✅ CI/CD ready
- ✅ Production-grade testing infrastructure

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
unit_tests/
├── README.md                     # Documentation
├── Makefile.am                   # Build configuration
├── test_main.cpp                 # Test runner entry point
├── ccec/                         # CCEC library tests
│   ├── test_CECFrame.cpp        # CECFrame class tests
│   ├── test_Connection.cpp      # Connection class tests
│   ├── test_LibCCEC.cpp         # LibCCEC singleton tests
│   ├── test_MessageEncoder.cpp  # Message encoding tests
│   ├── test_MessageDecoder.cpp  # Message decoding tests
│   ├── test_OpCode.cpp          # OpCode enum/class tests
│   └── test_Operands.cpp        # PhysicalAddress/LogicalAddress tests
└── osal/                         # OSAL library tests
    ├── test_Mutex.cpp           # Mutex locking tests
    ├── test_Thread.cpp          # Thread execution tests
    └── test_ConditionVariable.cpp # Condition variable tests
```

## Test Coverage

### CCEC Library Tests (7 test files)
- **CECFrame**: Constructor, copy, hex dump functionality
- **Connection**: Object creation, open/close (hardware-dependent disabled)
- **LibCCEC**: Singleton pattern, initialization (hardware-dependent disabled)
- **MessageEncoder**: Encoding CEC messages (ImageViewOn, TextViewOn, ActiveSource)
- **MessageDecoder**: Decoding CEC frames
- **OpCode**: Constants validation, string conversion
- **Operands**: PhysicalAddress and LogicalAddress creation

### OSAL Library Tests (3 test files)
- **Mutex**: Lock/unlock, concurrency protection
- **Thread**: Creation, execution with Runnable
- **ConditionVariable**: Notify/wait patterns

## Installation Steps

### 1. Install Google Test

#### Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install libgtest-dev libgmock-dev cmake
```

#### Build from source (if packages don't include libraries):
```bash
cd /usr/src/gtest
sudo cmake .
sudo make
sudo cp lib/*.a /usr/lib

cd /usr/src/gmock
sudo cmake .
sudo make
sudo cp lib/*.a /usr/lib
```

#### RHEL/CentOS:
```bash
sudo yum install gtest-devel gmock-devel
```

### 2. Update configure.ac

Add the following to `configure.ac` before `AC_CONFIG_FILES`:

```autoconf
# Optional unit tests support
AC_ARG_ENABLE([tests],
    AS_HELP_STRING([--enable-tests], [Enable unit tests (requires gtest)]),
    [enable_tests=$enableval],
    [enable_tests=no])

AM_CONDITIONAL([ENABLE_TESTS], [test "x$enable_tests" = "xyes"])

if test "x$enable_tests" = "xyes"; then
    PKG_CHECK_MODULES([GTEST], [gtest >= 1.10.0], [],
        [AC_MSG_ERROR([Google Test not found. Install libgtest-dev or use --disable-tests])])
    AC_SUBST([GTEST_CFLAGS])
    AC_SUBST([GTEST_LIBS])
fi
```

And add `unit_tests/Makefile` to `AC_CONFIG_FILES`:

```autoconf
AC_CONFIG_FILES([Makefile
                 cfg/Makefile
                 osal/Makefile
                 osal/src/Makefile
                 ccec/Makefile
                 ccec/src/Makefile
                 tests/Makefile
                 unit_tests/Makefile])
```

### 3. Update Root Makefile.am

Modify the root `Makefile.am` to include unit_tests conditionally:

```makefile
if ENABLE_TESTS
SUBDIRS = osal ccec unit_tests
else
SUBDIRS = osal ccec
endif
DIST_SUBDIRS = cfg osal ccec unit_tests
```

### 4. Configure and Build

```bash
# Generate build scripts
autoreconf -fi

# Configure with tests enabled
./configure --enable-tests

# Build the library and tests
make

# Run all tests
make check
```

## Running Tests

### Basic Usage

```bash
# Run all tests
make check

# Run tests directly
./unit_tests/run_unit_tests

# Run with verbose output
./unit_tests/run_unit_tests --gtest_verbose
```

### Advanced Usage

```bash
# Run specific test suite
./unit_tests/run_unit_tests --gtest_filter="CECFrameTest.*"

# Run multiple test patterns
./unit_tests/run_unit_tests --gtest_filter="*Mutex*:*Thread*"

# List all available tests
./unit_tests/run_unit_tests --gtest_list_tests

# Generate XML report (for CI/CD)
./unit_tests/run_unit_tests --gtest_output=xml:test_results.xml

# Repeat tests for flakiness detection
./unit_tests/run_unit_tests --gtest_repeat=100

# Shuffle test execution order
./unit_tests/run_unit_tests --gtest_shuffle
```

## Writing New Tests

### Example Test File

Create `unit_tests/ccec/test_NewClass.cpp`:

```cpp
#include <gtest/gtest.h>
#include "ccec/NewClass.hpp"

using namespace CCEC;

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

Add to `Makefile.am`:
```makefile
run_unit_tests_SOURCES = \
    ... \
    ccec/test_NewClass.cpp
```

## CI/CD Integration

### GitHub Actions Example

```yaml
name: Unit Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y libgtest-dev libgmock-dev libglib2.0-dev
      - name: Build and test
        run: |
          autoreconf -fi
          ./configure --enable-tests
          make check
      - name: Upload test results
        if: always()
        uses: actions/upload-artifact@v2
        with:
          name: test-results
          path: unit_tests/*.xml
```

## Next Steps

1. **Enable hardware mocking**: Implement mock drivers for hardware-dependent tests (marked with `DISABLED_`)
2. **Increase coverage**: Add more test cases for edge cases and error paths
3. **Integration tests**: Consider adding integration tests in a separate directory
4. **Code coverage**: Integrate gcov/lcov for coverage reporting
5. **Continuous testing**: Set up CI/CD pipeline with automated test execution

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
./configure --enable-tests LDFLAGS="-L/usr/local/lib"
```

### Test failures
```bash
# Run with more verbosity
./run_unit_tests --gtest_verbose --gtest_print_time

# Debug specific test
gdb --args ./run_unit_tests --gtest_filter="FailingTest.*"
```

## Resources

- [Google Test Documentation](https://google.github.io/googletest/)
- [Google Mock Documentation](https://google.github.io/googletest/gmock_for_dummies.html)
- [Google Test Primer](https://google.github.io/googletest/primer.html)
- [Advanced Testing Topics](https://google.github.io/googletest/advanced.html)

## Summary

The unit test framework provides:
- ✅ 10 test suites with 20+ individual tests
- ✅ Coverage for both CCEC and OSAL libraries
- ✅ Easy to extend and maintain
- ✅ Integrated with build system
- ✅ CI/CD ready
- ✅ Production-grade testing infrastructure
