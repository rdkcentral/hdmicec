/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2016 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "ccec/Driver.hpp"
#include "ccec/Exception.hpp"
#include "ccec/Connection.hpp"
#include "hdmi_cec_driver_mock.h"

using ::testing::_;
using ::testing::Return;
using ::testing::DoAll;
using ::testing::SetArgPointee;

class DriverTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clear any lingering mock expectations
        HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
        ::testing::Mock::VerifyAndClearExpectations(mock);
        
        // Ensure driver is open for each test
        // Try multiple times as it might be in a bad state
        Driver &driver = Driver::getInstance();
        for (int i = 0; i < 2; i++) {
            try {
                driver.open();
                break;
            } catch (...) {
                // Already open or in bad state, try to reset
                try {
                    driver.close();
                } catch (...) {
                    // Ignore close errors
                }
            }
        }
    }

    void TearDown() override {
        // Clear mock expectations after each test
        HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
        ::testing::Mock::VerifyAndClearExpectations(mock);
        
        // Ensure driver is open after each test for other tests
        Driver &driver = Driver::getInstance();
        for (int i = 0; i < 2; i++) {
            try {
                driver.open();
                break;
            } catch (...) {
                // Try to recover
                try {
                    driver.close();
                } catch (...) {
                    // Ignore
                }
            }
        }
    }
};

// Test driver singleton access - runs first alphabetically
TEST_F(DriverTest, AAA_DriverSingletonAccess) {
    EXPECT_NO_THROW({
        Driver &driver = Driver::getInstance();
        (void)driver; // Suppress unused variable warning
    });
}

// Test driver open (already opened by global environment)
TEST_F(DriverTest, DriverAlreadyOpen) {
    Driver &driver = Driver::getInstance();
    
    // Opening again should not throw (handled gracefully)
    EXPECT_NO_THROW({
        driver.open();
        driver.open(); // Second open
    });
}

TEST_F(DriverTest, CloseAndReopen) {
    Driver &driver = Driver::getInstance();
    
    // Ensure we start in a good state
    try {
        driver.open();
    } catch (...) {
        // Already open, that's fine
    }
    
    // Close the driver
    EXPECT_NO_THROW({
        driver.close();
    });
    
    // Reopen it
    EXPECT_NO_THROW({
        driver.open();
    });
    
    // Verify it works by doing a simple operation
    EXPECT_NO_THROW({
        driver.open(); // Should handle gracefully
    });


    // First close
    driver.close();



}

// Test multiple close calls
TEST_F(DriverTest, MultipleClose) {
    Driver &driver = Driver::getInstance();


    // Ensure we start in a good state
    try {
        driver.open();
    } catch (...) {
        // Already open, that's fine
    }
    
    // First close
    driver.close();
    
    // Second close should not throw (handled gracefully)
    EXPECT_NO_THROW({
        driver.close();
    });
    
    // Restore state - reopen the driver
    driver.open();
}

// Test getLogicalAddress
TEST_F(DriverTest, GetLogicalAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Driver &driver = Driver::getInstance();
    
    try {
        driver.open();
    } catch (...) {
        // If this fails, skip the test
        GTEST_SKIP() << "Driver not in valid state for this test";
    }
    
    // Set up mock to return a logical address
    EXPECT_CALL(*mock, HdmiCecGetLogicalAddress(_, _))
        .Times(1)
        .WillOnce(DoAll(
            SetArgPointee<1>(4), // Return logical address 4
            Return(HDMI_CEC_IO_SUCCESS)
        ));
    
    int logicalAddr = -1;
    EXPECT_NO_THROW({
        logicalAddr = driver.getLogicalAddress(0);
    });
    EXPECT_EQ(logicalAddr, 4);
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test getPhysicalAddress
TEST_F(DriverTest, GetPhysicalAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Driver &driver = Driver::getInstance();
    
    // Set up mock to return a physical address
    EXPECT_CALL(*mock, HdmiCecGetPhysicalAddress(_, _))
        .Times(1)
        .WillOnce(DoAll(
            SetArgPointee<1>(0x1000), // Return physical address 1.0.0.0
            Return(HDMI_CEC_IO_SUCCESS)
        ));
    
    unsigned int physicalAddr = 0;
    EXPECT_NO_THROW({
        driver.getPhysicalAddress(&physicalAddr);
    });
    EXPECT_EQ(physicalAddr, 0x1000u);
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test addLogicalAddress success
TEST_F(DriverTest, AddLogicalAddressSuccess) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Driver &driver = Driver::getInstance();
    
    // Set up mock to succeed
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SUCCESS));
    
    LogicalAddress addr(LogicalAddress::PLAYBACK_DEVICE_2);
    bool result = false;
    
    EXPECT_NO_THROW({
        result = driver.addLogicalAddress(addr);
    });
    EXPECT_TRUE(result);
    
    ::testing::Mock::VerifyAndClearExpectations(mock);
    
    // Clean up - remove the address we added
    EXPECT_CALL(*mock, HdmiCecRemoveLogicalAddress(_, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SUCCESS));
    
    driver.removeLogicalAddress(addr);
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test addLogicalAddress - address unavailable
TEST_F(DriverTest, AddLogicalAddressUnavailable) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Driver &driver = Driver::getInstance();
    
    // Set up mock to return unavailable
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_LOGICALADDRESS_UNAVAILABLE));
    
    LogicalAddress addr(LogicalAddress::RECORDING_DEVICE_1);
    
    EXPECT_THROW({
        driver.addLogicalAddress(addr);
    }, AddressNotAvailableException);
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test addLogicalAddress - general error
TEST_F(DriverTest, AddLogicalAddressGeneralError) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Driver &driver = Driver::getInstance();
    
    // Set up mock to return general error
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_GENERAL_ERROR));
    
    LogicalAddress addr(LogicalAddress::TUNER_1);
    
    EXPECT_THROW({
        driver.addLogicalAddress(addr);
    }, IOException);
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test removeLogicalAddress
TEST_F(DriverTest, RemoveLogicalAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Driver &driver = Driver::getInstance();
    
    // First add a logical address
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SUCCESS));
    
    LogicalAddress addr(LogicalAddress::PLAYBACK_DEVICE_3);
    driver.addLogicalAddress(addr);
    
    ::testing::Mock::VerifyAndClearExpectations(mock);
    
    // Now remove it
    EXPECT_CALL(*mock, HdmiCecRemoveLogicalAddress(_, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SUCCESS));
    
    EXPECT_NO_THROW({
        driver.removeLogicalAddress(addr);
    });
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test isValidLogicalAddress - address is valid
TEST_F(DriverTest, IsValidLogicalAddressTrue) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Driver &driver = Driver::getInstance();
    
    // Add a logical address
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SUCCESS));
    
    LogicalAddress addr(LogicalAddress::AUDIO_SYSTEM);
    driver.addLogicalAddress(addr);
    
    ::testing::Mock::VerifyAndClearExpectations(mock);
    
    // Check if it's valid
    EXPECT_TRUE(driver.isValidLogicalAddress(addr));
    
    // Clean up
    EXPECT_CALL(*mock, HdmiCecRemoveLogicalAddress(_, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SUCCESS));
    
    driver.removeLogicalAddress(addr);
    
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test isValidLogicalAddress - address is not valid
TEST_F(DriverTest, IsValidLogicalAddressFalse) {
    Driver &driver = Driver::getInstance();
    
    LogicalAddress addr(LogicalAddress::TUNER_4);
    
    // This address was never added, so it should not be valid
    EXPECT_FALSE(driver.isValidLogicalAddress(addr));
}

// Test write with NACK for non-broadcast
TEST_F(DriverTest, WriteWithNackNonBroadcast) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Driver &driver = Driver::getInstance();
    
    // Set up mock to return NACK
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(DoAll(
            SetArgPointee<3>(HDMI_CEC_IO_SENT_BUT_NOT_ACKD),
            Return(HDMI_CEC_IO_SUCCESS)
        ));
    
    CECFrame frame;
    frame.append(0x40); // Non-broadcast (to = 0, not F)
    frame.append(0x36);
    
    // Should throw NACK exception
    EXPECT_THROW({
        driver.write(frame);
    }, CECNoAckException);
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test write with NACK for broadcast REPORT_PHYSICAL_ADDRESS
TEST_F(DriverTest, WriteWithNackBroadcastReportPhysicalAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Driver &driver = Driver::getInstance();
    
    // Set up mock to return NACK
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(DoAll(
            SetArgPointee<3>(HDMI_CEC_IO_SENT_BUT_NOT_ACKD),
            Return(HDMI_CEC_IO_SUCCESS)
        ));
    
    CECFrame frame;
    frame.append(0x4F); // Broadcast (to = F)
    frame.append(0x84); // REPORT_PHYSICAL_ADDRESS opcode
    frame.append(0x10);
    frame.append(0x00);
    
    // Should throw NACK exception for broadcast report physical address
    EXPECT_THROW({
        driver.write(frame);
    }, CECNoAckException);
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test write with sendResult errors
TEST_F(DriverTest, WriteWithSendResultErrors) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Driver &driver = Driver::getInstance();
    
    int errorCodes[] = {
        HDMI_CEC_IO_INVALID_HANDLE,
        HDMI_CEC_IO_INVALID_ARGUMENT,
        HDMI_CEC_IO_LOGICALADDRESS_UNAVAILABLE,
        HDMI_CEC_IO_SENT_FAILED,
        HDMI_CEC_IO_GENERAL_ERROR
    };
    
    for (int errorCode : errorCodes) {
        EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
            .Times(1)
            .WillOnce(DoAll(
                SetArgPointee<3>(errorCode),
                Return(HDMI_CEC_IO_SUCCESS)
            ));
        
        CECFrame frame;
        frame.append(0x40);
        frame.append(0x36);
        
        // Should throw IOException
        EXPECT_THROW({
            driver.write(frame);
        }, IOException);
        
        ::testing::Mock::VerifyAndClearExpectations(mock);
    }
}

// Test write with HdmiCecTx failure
TEST_F(DriverTest, WriteWithHdmiCecTxFailure) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Driver &driver = Driver::getInstance();
    
    // Set up mock to fail
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_GENERAL_ERROR));
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    
    // Should throw IOException
    EXPECT_THROW({
        driver.write(frame);
    }, IOException);
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test open with HdmiCecOpen failure - runs late to avoid breaking other tests
TEST_F(DriverTest, ZZZ_OpenWithFailure) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Driver &driver = Driver::getInstance();
    
    // Close first
    driver.close();
    
    // Set up mock to fail on open
    EXPECT_CALL(*mock, HdmiCecOpen(_))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_GENERAL_ERROR));
    
    // Should throw IOException
    EXPECT_THROW({
        driver.open();
    }, IOException);
    
    ::testing::Mock::VerifyAndClearExpectations(mock);
    
    // Restore state - successfully reopen the driver
    EXPECT_NO_THROW({
        driver.open();
    });
}

// Test close with HdmiCecClose failure - runs late to avoid breaking other tests
TEST_F(DriverTest, ZZZ_CloseWithFailure) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Driver &driver = Driver::getInstance();
    
    // Set up mock to fail on close
    EXPECT_CALL(*mock, HdmiCecClose(_))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_GENERAL_ERROR));
    
    // Should throw IOException
    EXPECT_THROW({
        driver.close();
    }, IOException);
    
    ::testing::Mock::VerifyAndClearExpectations(mock);
    
    // Restore state - driver is in bad state, force recovery
    // The driver state after failed close is undefined, so try to recover
    for (int i = 0; i < 3; i++) {
        try {
            driver.close();
        } catch (...) {}
        
        try {
            driver.open();
            break; // Successfully recovered
        } catch (...) {
            if (i == 2) {
                // Last attempt failed, this is a problem
                FAIL() << "Could not restore driver to valid state";
            }
        }
    }
}

// Test printFrameDetails with various frames
TEST_F(DriverTest, PrintFrameDetails) {
    Driver &driver = Driver::getInstance();
    
    // Test with normal frame
    CECFrame frame1;
    frame1.append(0x40);
    frame1.append(0x36);
    
    EXPECT_NO_THROW({
        driver.printFrameDetails(frame1);
    });
    
    // Test with longer frame
    CECFrame frame2;
    frame2.append(0x4F);
    frame2.append(0x82);
    frame2.append(0x10);
    frame2.append(0x00);
    frame2.append(0x04);
    
    EXPECT_NO_THROW({
        driver.printFrameDetails(frame2);
    });
    
    // Test with single byte frame (poll)
    CECFrame frame3;
    frame3.append(0x44);
    
    EXPECT_NO_THROW({
        driver.printFrameDetails(frame3);
    });
}

// Test poll through driver
TEST_F(DriverTest, PollAddress) {
    Driver &driver = Driver::getInstance();
    
    LogicalAddress from(LogicalAddress::PLAYBACK_DEVICE_1);
    LogicalAddress to(LogicalAddress::PLAYBACK_DEVICE_1);
    
    EXPECT_NO_THROW({
        driver.poll(from, to);
    });
}

// Test writeAsync
TEST_F(DriverTest, WriteAsync) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Driver &driver = Driver::getInstance();
    
    // Set up mock for async write
    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SUCCESS));
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    
    EXPECT_NO_THROW({
        driver.writeAsync(frame);
    });
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test writeAsync with failure
TEST_F(DriverTest, WriteAsyncWithFailure) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Driver &driver = Driver::getInstance();
    
    // Set up mock to fail
    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_GENERAL_ERROR));
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    
    EXPECT_THROW({
        driver.writeAsync(frame);
    }, IOException);
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}
