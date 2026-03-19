# HDMI-CEC L1 Tests

This directory contains the L1 unit tests for the hdmicec library using Google Test (gtest/gmock).

## Framework

- **Test Framework**: Google Test (gtest) v1.10.0+
- **Mocking Framework**: Google Mock (gmock)
- **Language**: C++11
- **Build System**: Autotools

## Prerequisites

Install Google Test development package:

```bash
# Ubuntu/Debian
sudo apt-get install libgtest-dev libgmock-dev

# Build from source if needed
cd /usr/src/gtest
sudo cmake .
sudo make
sudo cp lib/*.a /usr/lib
```

## Building Tests

```bash
# Configure with L1 tests enabled
./configure --enable-l1tests

# Build and run tests
make check

# Or build and run explicitly
cd tests/L1Tests
make
./run_L1Tests
```

## Test Structure

```
tests/L1Tests/
├── Makefile.am           # Autotools build configuration
├── test_main.cpp         # Test runner entry point
├── ccec/                 # CCEC library tests
│   ├── test_CECFrame.cpp
│   ├── test_Connection.cpp
│   ├── test_LibCCEC.cpp
│   ├── test_MessageEncoder.cpp
│   ├── test_MessageDecoder.cpp
│   ├── test_OpCode.cpp
│   └── test_Operands.cpp
└── osal/                 # OSAL library tests
    ├── test_Mutex.cpp
    ├── test_Thread.cpp
    └── test_ConditionVariable.cpp
```

## Running Tests

```bash
# Run all tests
make check

# Run specific test with filter
./run_L1Tests --gtest_filter="CECFrameTest.*"

# Run with verbose output
./run_L1Tests --gtest_verbose

# Generate XML report
./run_L1Tests --gtest_output=xml:test_results.xml

# List all tests
./run_L1Tests --gtest_list_tests
```

## Writing New Tests

1. Create test file in appropriate directory (ccec/ or osal/)
2. Add to `run_L1Tests_SOURCES` in Makefile.am
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

## Test Categories

- **Unit Tests**: Test individual classes/functions in isolation
- **DISABLED_** prefix: Tests requiring hardware/driver mocking (currently disabled)

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

## Notes

- Some tests are disabled (DISABLED_ prefix) because they require hardware access
- Hardware-dependent tests need driver mocking implementation
- Thread-related tests may need timing adjustments on slow systems
