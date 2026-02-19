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
#include "ccec/LibCCEC.hpp"
#include "ccec/Exception.hpp"
#include "ccec/Operands.hpp"


class LibCCECTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Ensure clean state before each test
    }

    void TearDown() override {
        // Cleanup after each test
        // Note: We can't call term() here as some tests may not have initialized
    }
};

TEST_F(LibCCECTest, GetInstanceReturnsSingleton) {
    LibCCEC& instance1 = LibCCEC::getInstance();
    LibCCEC& instance2 = LibCCEC::getInstance();
    EXPECT_EQ(&instance1, &instance2);
}

TEST_F(LibCCECTest, InitWithValidName) {
    LibCCEC& lib = LibCCEC::getInstance();
    
    // Test initialization with a valid name
    EXPECT_NO_THROW(lib.init("TestCEC"));
    
    // Clean up
    EXPECT_NO_THROW(lib.term());
}

TEST_F(LibCCECTest, InitWithNullName) {
    LibCCEC& lib = LibCCEC::getInstance();
    
    // Test initialization with NULL name (should handle gracefully)
    EXPECT_NO_THROW(lib.init(nullptr));
    
    // Clean up
    EXPECT_NO_THROW(lib.term());
}

TEST_F(LibCCECTest, InitThrowsWhenAlreadyInitialized) {
    LibCCEC& lib = LibCCEC::getInstance();
    
    // Initialize once
    lib.init("TestCEC");
    
    // Try to initialize again - should throw InvalidStateException
    EXPECT_THROW(lib.init("TestCEC2"), InvalidStateException);
    
    // Clean up
    lib.term();
}

TEST_F(LibCCECTest, TermThrowsWhenNotInitialized) {
    LibCCEC& lib = LibCCEC::getInstance();
    
    // Try to terminate without initializing - should throw InvalidStateException
    EXPECT_THROW(lib.term(), InvalidStateException);
}

TEST_F(LibCCECTest, TermSucceedsAfterInit) {
    LibCCEC& lib = LibCCEC::getInstance();
    
    lib.init("TestCEC");
    
    // Termination should succeed
    EXPECT_NO_THROW(lib.term());
}

TEST_F(LibCCECTest, AddLogicalAddressWithoutInit) {
    LibCCEC& lib = LibCCEC::getInstance();
    LogicalAddress addr(LogicalAddress::PLAYBACK_DEVICE_1);
    
    // Should throw InvalidStateException when not initialized
    EXPECT_THROW(lib.addLogicalAddress(addr), InvalidStateException);
}

TEST_F(LibCCECTest, AddLogicalAddressAfterInit) {
    LibCCEC& lib = LibCCEC::getInstance();
    
    lib.init("TestCEC");
    
    LogicalAddress addr(LogicalAddress::PLAYBACK_DEVICE_1);
    
    // Should succeed after initialization
    EXPECT_NO_THROW({
        int result = lib.addLogicalAddress(addr);
        EXPECT_TRUE(result);
    });
    
    lib.term();
}

TEST_F(LibCCECTest, GetLogicalAddressWithoutInit) {
    LibCCEC& lib = LibCCEC::getInstance();
    
    // Should throw InvalidStateException when not initialized
    EXPECT_THROW(lib.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), InvalidStateException);
}

TEST_F(LibCCECTest, GetLogicalAddressAfterInit) {
    LibCCEC& lib = LibCCEC::getInstance();
    
    lib.init("TestCEC");
    
    // Should not throw after initialization (actual value depends on driver mock)
    EXPECT_NO_THROW({
        int logicalAddr = lib.getLogicalAddress(DeviceType::PLAYBACK_DEVICE);
        // Driver mock should return a non-zero value
        EXPECT_NE(logicalAddr, 0);
    });
    
    lib.term();
}

TEST_F(LibCCECTest, GetPhysicalAddressWithoutInit) {
    LibCCEC& lib = LibCCEC::getInstance();
    unsigned int physAddr = 0;
    
    // Should throw InvalidStateException when not initialized
    EXPECT_THROW(lib.getPhysicalAddress(&physAddr), InvalidStateException);
}

TEST_F(LibCCECTest, GetPhysicalAddressAfterInit) {
    LibCCEC& lib = LibCCEC::getInstance();
    
    lib.init("TestCEC");
    
    unsigned int physAddr = 0;
    
    // Should succeed after initialization
    EXPECT_NO_THROW(lib.getPhysicalAddress(&physAddr));
    
    lib.term();
}

TEST_F(LibCCECTest, MultipleInitTermCycles) {
    LibCCEC& lib = LibCCEC::getInstance();
    
    // First cycle
    EXPECT_NO_THROW(lib.init("TestCEC1"));
    EXPECT_NO_THROW(lib.term());
    
    // Second cycle
    EXPECT_NO_THROW(lib.init("TestCEC2"));
    EXPECT_NO_THROW(lib.term());
    
    // Third cycle with NULL name
    EXPECT_NO_THROW(lib.init(nullptr));
    EXPECT_NO_THROW(lib.term());
}

TEST_F(LibCCECTest, AddMultipleLogicalAddresses) {
    LibCCEC& lib = LibCCEC::getInstance();
    
    lib.init("TestCEC");
    
    // Add multiple logical addresses
    LogicalAddress addr1(LogicalAddress::PLAYBACK_DEVICE_1);
    LogicalAddress addr2(LogicalAddress::PLAYBACK_DEVICE_2);
    LogicalAddress addr3(LogicalAddress::AUDIO_SYSTEM);
    
    EXPECT_NO_THROW({
        EXPECT_TRUE(lib.addLogicalAddress(addr1));
        EXPECT_TRUE(lib.addLogicalAddress(addr2));
        EXPECT_TRUE(lib.addLogicalAddress(addr3));
    });
    
    lib.term();
}

TEST_F(LibCCECTest, GetLogicalAddressForDifferentDeviceTypes) {
    LibCCEC& lib = LibCCEC::getInstance();
    
    lib.init("TestCEC");
    
    // Test different device types
    EXPECT_NO_THROW({
        int addr1 = lib.getLogicalAddress(DeviceType::TV);
        EXPECT_NE(addr1, 0);
        
        int addr2 = lib.getLogicalAddress(DeviceType::PLAYBACK_DEVICE);
        EXPECT_NE(addr2, 0);
        
        int addr3 = lib.getLogicalAddress(DeviceType::AUDIO_SYSTEM);
        EXPECT_NE(addr3, 0);
    });
    
    lib.term();
}
