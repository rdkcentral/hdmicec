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
#include "ccec/LibCCEC.hpp"
#include "ccec/Exception.hpp"
#include "ccec/Operands.hpp"
#include "hdmi_cec_driver_mock.h"

using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;


class LibCCECTest : public ::testing::Test {
protected:
    void SetUp() override {
        // LibCCEC may already be initialized by other test suites (DriverTest, etc.)
        // Try to initialize, but ignore InvalidStateException if already initialized
        try {
            LibCCEC::getInstance().init("TestCEC");
        } catch (const InvalidStateException&) {
            // Already initialized - this is fine
        }
    }

    void TearDown() override {
        // Don't call term() here to avoid thread cleanup issues
        // LibCCEC will be cleaned up by the global test environment
    }
};

TEST_F(LibCCECTest, GetInstanceReturnsSingleton) {
    LibCCEC& instance1 = LibCCEC::getInstance();
    LibCCEC& instance2 = LibCCEC::getInstance();
    EXPECT_EQ(&instance1, &instance2);
}

TEST_F(LibCCECTest, InitWithValidName) {
    LibCCEC& lib = LibCCEC::getInstance();
    
    // LibCCEC is already initialized in SetUp
    // Verify we can get the instance
    EXPECT_NO_THROW({
        LibCCEC& instance = LibCCEC::getInstance();
        (void)instance;
    });
}

TEST_F(LibCCECTest, InitThrowsWhenAlreadyInitialized) {
    LibCCEC& lib = LibCCEC::getInstance();
    
    // Already initialized in SetUp
    // Try to initialize again - should throw InvalidStateException
    EXPECT_THROW(lib.init("TestCEC2"), InvalidStateException);
}

TEST_F(LibCCECTest, AddLogicalAddressAfterInit) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    LibCCEC& lib = LibCCEC::getInstance();

    // Verify HdmiCecAddLogicalAddress is called with PLAYBACK_DEVICE_1 (4)
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, LogicalAddress::PLAYBACK_DEVICE_1))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SUCCESS));

    LogicalAddress addr(LogicalAddress::PLAYBACK_DEVICE_1);
    int result = 0;
    EXPECT_NO_THROW(result = lib.addLogicalAddress(addr));
    EXPECT_TRUE(result);

    ::testing::Mock::VerifyAndClearExpectations(mock);
}

TEST_F(LibCCECTest, GetLogicalAddressAfterInit) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    LibCCEC& lib = LibCCEC::getInstance();

    // Configure the mock to return a known logical address value (5 = AUDIO_SYSTEM)
    EXPECT_CALL(*mock, HdmiCecGetLogicalAddress(_, _))
        .Times(1)
        .WillOnce(Invoke([](int, int* addr) -> int {
            if (addr) *addr = LogicalAddress::AUDIO_SYSTEM; // 5
            return HDMI_CEC_IO_SUCCESS;
        }));

    int logicalAddr = 0;
    EXPECT_NO_THROW(logicalAddr = lib.getLogicalAddress(DeviceType::PLAYBACK_DEVICE));
    EXPECT_EQ(logicalAddr, LogicalAddress::AUDIO_SYSTEM);

    ::testing::Mock::VerifyAndClearExpectations(mock);
}


TEST_F(LibCCECTest, GetPhysicalAddressAfterInit) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    LibCCEC& lib = LibCCEC::getInstance();

    // Configure the mock to return a specific, known physical address
    const unsigned int expectedPhysAddr = 0x2100;
    EXPECT_CALL(*mock, HdmiCecGetPhysicalAddress(_, _))
        .Times(1)
        .WillOnce(Invoke([expectedPhysAddr](int, unsigned int* addr) -> int {
            if (addr) *addr = expectedPhysAddr;
            return HDMI_CEC_IO_SUCCESS;
        }));

    unsigned int physAddr = 0;
    EXPECT_NO_THROW(lib.getPhysicalAddress(&physAddr));
    EXPECT_EQ(physAddr, expectedPhysAddr);

    ::testing::Mock::VerifyAndClearExpectations(mock);
}


TEST_F(LibCCECTest, AddMultipleLogicalAddresses) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    LibCCEC& lib = LibCCEC::getInstance();

    // Each addLogicalAddress call must reach the driver with the correct address value
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, LogicalAddress::PLAYBACK_DEVICE_1))
        .Times(1).WillOnce(Return(HDMI_CEC_IO_SUCCESS));
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, LogicalAddress::PLAYBACK_DEVICE_2))
        .Times(1).WillOnce(Return(HDMI_CEC_IO_SUCCESS));
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, LogicalAddress::AUDIO_SYSTEM))
        .Times(1).WillOnce(Return(HDMI_CEC_IO_SUCCESS));

    LogicalAddress addr1(LogicalAddress::PLAYBACK_DEVICE_1);
    LogicalAddress addr2(LogicalAddress::PLAYBACK_DEVICE_2);
    LogicalAddress addr3(LogicalAddress::AUDIO_SYSTEM);

    EXPECT_TRUE(lib.addLogicalAddress(addr1));
    EXPECT_TRUE(lib.addLogicalAddress(addr2));
    EXPECT_TRUE(lib.addLogicalAddress(addr3));

    ::testing::Mock::VerifyAndClearExpectations(mock);
}

TEST_F(LibCCECTest, GetLogicalAddressForDifferentDeviceTypes) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    LibCCEC& lib = LibCCEC::getInstance();

    // Note: DriverImpl::getLogicalAddress ignores devType and always calls HdmiCecGetLogicalAddress.
    // The three calls return distinct pre-configured values so we verify the pass-through.
    EXPECT_CALL(*mock, HdmiCecGetLogicalAddress(_, _))
        .Times(3)
        .WillOnce(Invoke([](int, int* a) -> int { if (a) *a = LogicalAddress::TV;             return HDMI_CEC_IO_SUCCESS; }))
        .WillOnce(Invoke([](int, int* a) -> int { if (a) *a = LogicalAddress::PLAYBACK_DEVICE_1; return HDMI_CEC_IO_SUCCESS; }))
        .WillOnce(Invoke([](int, int* a) -> int { if (a) *a = LogicalAddress::AUDIO_SYSTEM;   return HDMI_CEC_IO_SUCCESS; }));

    // getLogicalAddress throws InvalidStateException when the driver returns 0 (TV=0),
    // so we test the two non-zero values and handle TV separately.
    EXPECT_THROW(lib.getLogicalAddress(DeviceType::TV), InvalidStateException); // TV=0 triggers the guard

    int addr2 = 0;
    EXPECT_NO_THROW(addr2 = lib.getLogicalAddress(DeviceType::PLAYBACK_DEVICE));
    EXPECT_EQ(addr2, LogicalAddress::PLAYBACK_DEVICE_1); // 4

    int addr3 = 0;
    EXPECT_NO_THROW(addr3 = lib.getLogicalAddress(DeviceType::AUDIO_SYSTEM));
    EXPECT_EQ(addr3, LogicalAddress::AUDIO_SYSTEM); // 5

    ::testing::Mock::VerifyAndClearExpectations(mock);
}
