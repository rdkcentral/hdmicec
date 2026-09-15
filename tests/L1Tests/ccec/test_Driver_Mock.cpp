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
#include "hdmi_cec_driver_mock.h"

/**
 * Example test demonstrating HDMI CEC driver mock usage with Google Mock
 * 
 * This shows how to use ON_CALL and EXPECT_CALL to control mock behavior.
 */

class HdmiCecDriverMockTest : public ::testing::Test {
protected:
    HdmiCecDriverMock* mock;

    // The HAL CALLBACK REGISTRATION this fixture found on the process-global mock, captured so
    // TearDown() can put it back.
    //
    // WHY THE REGISTRATION AND NOT JUST THE ON_CALL DEFAULTS.  ReceiveMessageCallback below
    // calls the real HdmiCecSetRxCallback entry point with a callback of its own, and the mock's
    // default action for that entry point STORES what it is handed - `rxCallback = cbfunc;
    // rxCallbackData = data;` in hdmi_cec_driver_mock.cpp - on the mock, which is process-global
    // and outlives every fixture.  The callback it stores is a lambda whose data pointer is a
    // bool on that case's stack frame.  So once that case has run, the mock's inbound route no
    // longer reaches DriverImpl::DriverReceiveCallback: every later
    // HdmiCecDriverMock::injectReceivedMessage in this binary calls a lambda through a dead
    // stack address instead of delivering a frame to the CEC stack.
    //
    // NOTHING ELSE PUTS IT BACK.  DriverImpl::open() registers the driver's callbacks exactly
    // once, because it returns early while the driver is already OPENED (DriverImpl.cpp), and
    // ::testing::Mock::VerifyAndClearExpectations() clears expectations, not data members.  The
    // registration therefore stays hijacked for the rest of the run, and every inbound-frame
    // case that runs after this fixture fails - measured, deterministically, with
    // `--gtest_filter='HdmiCecDriverMockTest.ReceiveMessageCallback:LibCCECUninitializedTest.
    // TermThrowsWhenNotInitialized' --gtest_repeat=2`: iteration 1 passes, iteration 2 fails.
    // Under --gtest_shuffle the same mechanism took out nine cases across three fixtures.
    //
    // Capturing in SetUp() and restoring in TearDown() makes the window this fixture's own.  It
    // is state management on the fixture only: no case in this file is modified, and every one
    // of them still sees exactly the mock it saw before.
    HdmiCecRxCallback_t capturedRxCallback;
    void* capturedRxCallbackData;
    HdmiCecTxCallback_t capturedTxCallback;
    void* capturedTxCallbackData;
    // Captured for the same reason: injectReceivedMessage and simulateTxResult pass
    // currentHandle to the callback, and a case that moves it (OpenDriverWithCustomBehavior's
    // family reads it, ExpectOpenCalled hands back 42) would otherwise leave a handle the
    // driver never opened on a mock the whole binary shares.
    int capturedCurrentHandle;

    void SetUp() override {
        // Use existing global mock if available, otherwise create one
        mock = HdmiCecDriverMock::getInstance();
        if (mock == nullptr) {
            mock = new HdmiCecDriverMock();
            HdmiCecDriverMock::setInstance(mock);
        }

        // Captured BEFORE any case in this fixture runs, so what is put back in TearDown() is
        // what this fixture was handed rather than a value it invented.  On the first case of a
        // full run that is DriverImpl's own registration, installed when the global test
        // environment initialised the library.
        capturedRxCallback = mock->rxCallback;
        capturedRxCallbackData = mock->rxCallbackData;
        capturedTxCallback = mock->txCallback;
        capturedTxCallbackData = mock->txCallbackData;
        capturedCurrentHandle = mock->currentHandle;
    }
    
    void TearDown() override {
        // Clear expectations
        if (mock != nullptr) {
            ::testing::Mock::VerifyAndClearExpectations(mock);
        }

        // Hand the inbound and outbound callback registration back exactly as SetUp() found it,
        // so the next case in this binary - whichever it is - injects into the same CEC stack
        // this fixture inherited.  See the member declarations above for the mechanism and for
        // the measurement that made it necessary.
        if (mock != nullptr) {
            mock->rxCallback = capturedRxCallback;
            mock->rxCallbackData = capturedRxCallbackData;
            mock->txCallback = capturedTxCallback;
            mock->txCallbackData = capturedTxCallbackData;
            mock->currentHandle = capturedCurrentHandle;
        }
        
        // CRITICAL: Restore default ON_CALL behaviors that may have been overridden
        // This is necessary because some tests use ON_CALL to change default behavior
        if (mock != nullptr) {
            // Restore default HdmiCecOpen behavior.
            //
            // THE LAMBDA MUST CAPTURE NOTHING.  The action is stored on the PROCESS-GLOBAL
            // driver mock, which outlives this fixture, so anything it captures from this
            // object is dereferenced by every later HdmiCecOpen call - including calls made
            // long after the fixture has been destroyed.  Capturing `this` and reading
            // `mock->currentHandle` through it is therefore a use-after-free that is latent
            // in the suite's default order, where the released storage still happens to be
            // readable, and a hard crash once shuffling moves a later driver case behind
            // this fixture.  Looking the mock up freshly inside the action gives identical
            // behaviour with no reference to this object's lifetime at all, which is the
            // form to keep.
            ON_CALL(*mock, HdmiCecOpen(::testing::_))
                .WillByDefault(::testing::Invoke(
                    [](int* handle) {
                        HdmiCecDriverMock* current = HdmiCecDriverMock::getInstance();
                        if (handle && current) {
                            *handle = current->currentHandle;
                            return HDMI_CEC_IO_SUCCESS;
                        }
                        return HDMI_CEC_IO_INVALID_ARGUMENT;
                    }));
            
            // Restore default HdmiCecAddLogicalAddress behavior
            ON_CALL(*mock, HdmiCecAddLogicalAddress(::testing::_, ::testing::_))
                .WillByDefault(::testing::Return(HDMI_CEC_IO_SUCCESS));
            
            // Restore default HdmiCecGetPhysicalAddress behavior
            ON_CALL(*mock, HdmiCecGetPhysicalAddress(::testing::_, ::testing::_))
                .WillByDefault(::testing::Invoke(
                    [](int handle, unsigned int* physicalAddress) {
                        if (physicalAddress) {
                            *physicalAddress = 0x1000;
                            return HDMI_CEC_IO_SUCCESS;
                        }
                        return HDMI_CEC_IO_INVALID_ARGUMENT;
                    }));
        }
    }
};

TEST_F(HdmiCecDriverMockTest, OpenDriverSuccess) {
    int handle = 0;
    
    // Default behavior is already set in constructor
    int result = HdmiCecOpen(&handle);
    
    EXPECT_EQ(result, HDMI_CEC_IO_SUCCESS);
    EXPECT_GT(handle, 0);
}

TEST_F(HdmiCecDriverMockTest, OpenDriverWithCustomBehavior) {
    int handle = 0;
    
    // Override default behavior using ON_CALL
    ON_CALL(*mock, HdmiCecOpen(::testing::_))
        .WillByDefault(::testing::Return(HDMI_CEC_IO_GENERAL_ERROR));
    
    int result = HdmiCecOpen(&handle);
    
    EXPECT_EQ(result, HDMI_CEC_IO_GENERAL_ERROR);
}

TEST_F(HdmiCecDriverMockTest, ExpectOpenCalled) {
    int handle = 0;
    
    // Use EXPECT_CALL to verify the function is called
    EXPECT_CALL(*mock, HdmiCecOpen(::testing::_))
        .Times(1)
        .WillOnce(::testing::Invoke([](int* h) {
            if (h) *h = 42;
            return HDMI_CEC_IO_SUCCESS;
        }));
    
    int result = HdmiCecOpen(&handle);
    
    EXPECT_EQ(result, HDMI_CEC_IO_SUCCESS);
    EXPECT_EQ(handle, 42);
}

TEST_F(HdmiCecDriverMockTest, CloseDriver) {
    int handle = 1;
    
    EXPECT_CALL(*mock, HdmiCecClose(handle))
        .Times(1)
        .WillOnce(::testing::Return(HDMI_CEC_IO_SUCCESS));
    
    int result = HdmiCecClose(handle);
    
    EXPECT_EQ(result, HDMI_CEC_IO_SUCCESS);
}

TEST_F(HdmiCecDriverMockTest, AddLogicalAddressWithCustomReturn) {
    int handle = 1;
    
    // Test successful addition
    ON_CALL(*mock, HdmiCecAddLogicalAddress(handle, 4))
        .WillByDefault(::testing::Return(HDMI_CEC_IO_SUCCESS));
    
    int result = HdmiCecAddLogicalAddress(handle, 4);
    EXPECT_EQ(result, HDMI_CEC_IO_SUCCESS);
    
    // Test address unavailable
    ON_CALL(*mock, HdmiCecAddLogicalAddress(handle, 5))
        .WillByDefault(::testing::Return(HDMI_CEC_IO_LOGICALADDRESS_UNAVAILABLE));
    
    result = HdmiCecAddLogicalAddress(handle, 5);
    EXPECT_EQ(result, HDMI_CEC_IO_LOGICALADDRESS_UNAVAILABLE);
}

TEST_F(HdmiCecDriverMockTest, GetPhysicalAddressCustomValue) {
    int handle = 1;
    unsigned int physAddr = 0;
    
    // Set custom physical address using ON_CALL
    ON_CALL(*mock, HdmiCecGetPhysicalAddress(handle, ::testing::_))
        .WillByDefault(::testing::Invoke([](int h, unsigned int* addr) {
            if (addr) {
                *addr = 0x2100;
                return HDMI_CEC_IO_SUCCESS;
            }
            return HDMI_CEC_IO_INVALID_ARGUMENT;
        }));
    
    int result = HdmiCecGetPhysicalAddress(handle, &physAddr);
    
    EXPECT_EQ(result, HDMI_CEC_IO_SUCCESS);
    EXPECT_EQ(physAddr, 0x2100u);
}

TEST_F(HdmiCecDriverMockTest, TransmitMessageExpectSpecificData) {
    int handle = 1;
    unsigned char expectedMsg[] = {0x40, 0x04}; // Image View On
    int txResult = 0;
    
    // Use EXPECT_CALL to verify exact message content
    EXPECT_CALL(*mock, HdmiCecTx(handle, ::testing::_, 2, ::testing::_))
        .Times(1)
        .WillOnce(::testing::Invoke([&expectedMsg](int h, const unsigned char* buf, int len, int* result) {
            // Verify the buffer contents
            EXPECT_EQ(buf[0], expectedMsg[0]);
            EXPECT_EQ(buf[1], expectedMsg[1]);
            if (result) {
                *result = HDMI_CEC_IO_SENT_AND_ACKD;
            }
            return HDMI_CEC_IO_SUCCESS;
        }));
    
    int result = HdmiCecTx(handle, expectedMsg, sizeof(expectedMsg), &txResult);
    
    EXPECT_EQ(result, HDMI_CEC_IO_SUCCESS);
    EXPECT_EQ(txResult, HDMI_CEC_IO_SENT_AND_ACKD);
}

TEST_F(HdmiCecDriverMockTest, TransmitFailure) {
    int handle = 1;
    unsigned char msg[] = {0x40, 0x04};
    int txResult = 0;
    
    // Simulate transmission failure
    ON_CALL(*mock, HdmiCecTx(handle, ::testing::_, ::testing::_, ::testing::_))
        .WillByDefault(::testing::Invoke([](int h, const unsigned char* buf, int len, int* result) {
            if (result) {
                *result = HDMI_CEC_IO_SENT_BUT_NOT_ACKD;
            }
            return HDMI_CEC_IO_SUCCESS;
        }));
    
    int result = HdmiCecTx(handle, msg, sizeof(msg), &txResult);
    
    EXPECT_EQ(result, HDMI_CEC_IO_SUCCESS);
    EXPECT_EQ(txResult, HDMI_CEC_IO_SENT_BUT_NOT_ACKD);
}

TEST_F(HdmiCecDriverMockTest, ReceiveMessageCallback) {
    int handle = 1;
    bool callbackCalled = false;
    
    auto rxCallback = [](int h, void* data, unsigned char* buf, int len) {
        bool* called = static_cast<bool*>(data);
        *called = true;
    };
    
    // Set callback
    HdmiCecSetRxCallback(handle, rxCallback, &callbackCalled);
    
    // Simulate receiving a message
    unsigned char msg[] = {0x04, 0x82, 0x10, 0x00}; // Active Source
    mock->injectReceivedMessage(msg, sizeof(msg));
    
    EXPECT_TRUE(callbackCalled);
}

TEST_F(HdmiCecDriverMockTest, MultipleExpectations) {
    int handle = 1;
    
    // Set up multiple expectations in sequence
    ::testing::InSequence seq;
    
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(handle, 4))
        .WillOnce(::testing::Return(HDMI_CEC_IO_SUCCESS));
    
    EXPECT_CALL(*mock, HdmiCecGetPhysicalAddress(handle, ::testing::_))
        .WillOnce(::testing::Invoke([](int h, unsigned int* addr) {
            *addr = 0x3000;
            return HDMI_CEC_IO_SUCCESS;
        }));
    
    EXPECT_CALL(*mock, HdmiCecRemoveLogicalAddress(handle, 4))
        .WillOnce(::testing::Return(HDMI_CEC_IO_SUCCESS));
    
    // Execute in sequence
    HdmiCecAddLogicalAddress(handle, 4);
    
    unsigned int physAddr = 0;
    HdmiCecGetPhysicalAddress(handle, &physAddr);
    EXPECT_EQ(physAddr, 0x3000u);
    
    HdmiCecRemoveLogicalAddress(handle, 4);
}
