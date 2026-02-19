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
├── ccec/                 # CCEC library tests (195+ tests)
│   ├── test_CECFrame.cpp        # 9 tests - frame construction, serialization
│   ├── test_Connection.cpp      # 4 tests - connection management
│   ├── test_LibCCEC.cpp         # 14 tests - singleton, init/term, addresses
│   ├── test_MessageEncoder.cpp  # 10 tests - CEC message encoding
│   ├── test_MessageDecoder.cpp  # 31 tests - all CEC opcodes, edge cases
│   ├── test_OpCode.cpp          # 68 tests - all opcodes, GetOpName coverage
│   ├── test_Operands.cpp        # 69 tests - all operand types, methods
│   ├── test_Driver_Mock.cpp     # Mock driver tests
│   ├── test_Driver.cpp          # Driver implementation tests
│   └── test_Bus.cpp             # Bus communication tests
└── osal/                 # OSAL library tests (10+ tests)
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

## Test Coverage Details

### CCEC Library Tests (195+ tests)

- **test_CECFrame.cpp** (9 tests): Frame construction, copy operations, serialization, hex dump
- **test_Connection.cpp** (4 tests): Connection lifecycle, open/close operations
- **test_LibCCEC.cpp** (14 tests): Singleton pattern, initialization/termination, logical/physical addresses
  - Note: 3 tests DISABLED due to thread safety (multiple init/term cycles cause race conditions in Bus threads)
- **test_MessageEncoder.cpp** (10 tests): Encoding of all major CEC message types
- **test_MessageDecoder.cpp** (31 tests): Decoding all 60+ CEC opcodes, polling messages, edge cases
- **test_OpCode.cpp** (68 tests): Complete GetOpName() coverage for all CEC opcodes, OpCode class methods
- **test_Operands.cpp** (69 tests): All operand classes (PhysicalAddress, LogicalAddress, DeviceType, Version, PowerStatus, AbortReason, OSDString, OSDName, Language, VendorID, UICommand, SystemAudioStatus, AudioStatus, RequestAudioFormat, ShortAudioDescriptor, AllDeviceTypes, RcProfile, DeviceFeatures, LatencyInfo)

### OSAL Library Tests (10+ tests)

- **test_Mutex.cpp**: Lock/unlock, concurrency protection
- **test_Thread.cpp**: Thread creation, execution with Runnable
- **test_ConditionVariable.cpp**: Notify/wait synchronization patterns

## Known Issues and Notes

### Disabled Tests

Some LibCCEC tests are disabled (DISABLED_ prefix) due to thread safety concerns:
- `DISABLED_TermThrowsWhenNotInitialized`
- `DISABLED_TermSucceedsAfterInit`
- `DISABLED_MultipleInitTermCycles`

**Reason**: LibCCEC uses Bus reader/writer threads that experience race conditions when repeatedly started and stopped. The current test approach uses a single initialization in SetUp() to avoid these issues.

### LibCCEC Singleton Behavior

LibCCEC is a singleton shared across all test suites. The test fixture SetUp() catches `InvalidStateException` to handle cases where LibCCEC has already been initialized by other test suites (e.g., DriverTest).

### Thread Timing

- Thread-related tests may need timing adjustments on slow systems
- Bus thread cleanup can take time; avoid rapid init/term cycles

### Hardware Dependencies

- Some Connection tests require actual CEC hardware/driver
- Hardware-dependent tests may need driver mocking implementation for full automation
