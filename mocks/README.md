# HDMI CEC Mocks

This directory contains mock implementations of external dependencies for HDMI CEC L1 testing.

## Structure

```
mocks/
├── hdmicec/                    # HDMI CEC specific mocks
│   ├── hdmi_cec_driver.h       # HAL driver interface
│   ├── hdmi_cec_driver_mock.h  # Mock class header
│   └── hdmi_cec_driver_mock.cpp # Mock implementation
├── telemetry_busmessage_sender.h # Telemetry stub
└── README.md
```

## Files

### hdmicec/hdmi_cec_driver.h
Header file defining the HDMI CEC HAL driver interface. This matches the interface expected by the CEC implementation.

### hdmicec/hdmi_cec_driver_mock.h / hdmi_cec_driver_mock.cpp
Mock implementation of the HDMI CEC driver. This provides:
- Controllable driver behavior for testing
- Methods to inject received CEC messages
- Methods to simulate transmission results
- Verification of driver state and logical addresses

### telemetry_busmessage_sender.h
Stub header for RDK telemetry system. Provides no-op macros for telemetry calls used in the CEC code.

## Usage in Tests

```cpp
#include "hdmi_cec_driver_mock.h"

TEST_F(YourTestFixture, TestSomething) {
    // Create mock instance
    HdmiCecDriverMock mock;

    // Configure mock behavior using Google Mock, for example:
    // ON_CALL(mock, open(::testing::_)).WillByDefault(::testing::Return(true));
    // EXPECT_CALL(mock, close()).Times(1);

    // Your test code that uses the CEC driver
    // ...

    // Inject a received message into the mock
    unsigned char msg[] = {0x40, 0x04}; // Example CEC message
    mock.injectReceivedMessage(msg, sizeof(msg));

    // Optionally, simulate a transmit result being reported by the driver
    mock.simulateTxResult(/* txId */ 1, /* success */ true);

    // Verify that the mock's callbacks/handles have been set up as expected
    EXPECT_NE(mock.currentHandle, nullptr);
    EXPECT_TRUE(mock.rxCallback);

    // Additional EXPECT_CALL/ASSERT_* on your system-under-test can go here.
}
```

## Building

These mocks are built as part of the L1 test build process. The Makefile.am in tests/L1Tests includes these source files.
