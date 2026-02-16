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

#pragma once

#include <gmock/gmock.h>
#include "hdmi_cec_driver.h"

/**
 * @brief Interface class for HDMI CEC driver
 * 
 * This interface defines all the HDMI CEC driver functions that can be mocked.
 */
class HdmiCecDriverInterface {
public:
    virtual ~HdmiCecDriverInterface() = default;
    
    virtual int HdmiCecOpen(int *handle) = 0;
    virtual int HdmiCecClose(int handle) = 0;
    virtual int HdmiCecSetRxCallback(int handle, HdmiCecRxCallback_t cbfunc, void *data) = 0;
    virtual int HdmiCecSetTxCallback(int handle, HdmiCecTxCallback_t cbfunc, void *data) = 0;
    virtual int HdmiCecTx(int handle, const unsigned char *buf, int len, int *result) = 0;
    virtual int HdmiCecTxAsync(int handle, const unsigned char *buf, int len) = 0;
    virtual int HdmiCecAddLogicalAddress(int handle, int logicalAddress) = 0;
    virtual int HdmiCecRemoveLogicalAddress(int handle, int logicalAddress) = 0;
    virtual int HdmiCecGetPhysicalAddress(int handle, unsigned int *physicalAddress) = 0;
    virtual int HdmiCecGetLogicalAddress(int handle, int devType, int *logicalAddress) = 0;
};

/**
 * @brief Mock class for HDMI CEC driver
 * 
 * This class provides a Google Mock implementation of the HDMI CEC driver.
 * Tests can use ON_CALL and EXPECT_CALL to control behavior.
 */
class HdmiCecDriverMock : public HdmiCecDriverInterface {
public:
    HdmiCecDriverMock();
    virtual ~HdmiCecDriverMock();
    
    // Google Mock methods
    MOCK_METHOD(int, HdmiCecOpen, (int *handle), (override));
    MOCK_METHOD(int, HdmiCecClose, (int handle), (override));
    MOCK_METHOD(int, HdmiCecSetRxCallback, (int handle, HdmiCecRxCallback_t cbfunc, void *data), (override));
    MOCK_METHOD(int, HdmiCecSetTxCallback, (int handle, HdmiCecTxCallback_t cbfunc, void *data), (override));
    MOCK_METHOD(int, HdmiCecTx, (int handle, const unsigned char *buf, int len, int *result), (override));
    MOCK_METHOD(int, HdmiCecTxAsync, (int handle, const unsigned char *buf, int len), (override));
    MOCK_METHOD(int, HdmiCecAddLogicalAddress, (int handle, int logicalAddress), (override));
    MOCK_METHOD(int, HdmiCecRemoveLogicalAddress, (int handle, int logicalAddress), (override));
    MOCK_METHOD(int, HdmiCecGetPhysicalAddress, (int handle, unsigned int *physicalAddress), (override));
    MOCK_METHOD(int, HdmiCecGetLogicalAddress, (int handle, int devType, int *logicalAddress), (override));
    
    /**
     * @brief Get the singleton instance
     * @return Pointer to the mock instance
     */
    static HdmiCecDriverMock* getInstance();
    
    /**
     * @brief Set the mock instance
     * @param newMock Pointer to the new mock instance
     */
    static void setInstance(HdmiCecDriverMock* newMock);
    
    /**
     * @brief Inject a received CEC message (helper for tests)
     * @param buf Buffer containing the CEC message
     * @param len Length of the message
     */
    void injectReceivedMessage(const unsigned char* buf, int len);
    
    /**
     * @brief Simulate a transmission result callback (helper for tests)
     * @param result Result code to send to TX callback
     */
    void simulateTxResult(int result);
    
    // Public members for test access
    int currentHandle;
    HdmiCecRxCallback_t rxCallback;
    HdmiCecTxCallback_t txCallback;
    void* rxCallbackData;
    void* txCallbackData;
    
private:
    static HdmiCecDriverMock* instance;
};
