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

#include "hdmi_cec_driver_mock.h"
#include <cstring>
#include <iostream>

// Static instance pointer
HdmiCecDriverMock* HdmiCecDriverMock::instance = nullptr;

HdmiCecDriverMock::HdmiCecDriverMock()
    : currentHandle(1)
    , rxCallback(nullptr)
    , txCallback(nullptr)
    , rxCallbackData(nullptr)
    , txCallbackData(nullptr)
{
    // Set up default behaviors
    ON_CALL(*this, HdmiCecOpen(::testing::_))
        .WillByDefault(::testing::Invoke(
            [this](int* handle) {
                if (handle) {
                    *handle = currentHandle;
                    return HDMI_CEC_IO_SUCCESS;
                }
                return HDMI_CEC_IO_INVALID_ARGUMENT;
            }));
    
    ON_CALL(*this, HdmiCecClose(::testing::_))
        .WillByDefault(::testing::Return(HDMI_CEC_IO_SUCCESS));
    
    ON_CALL(*this, HdmiCecSetRxCallback(::testing::_, ::testing::_, ::testing::_))
        .WillByDefault(::testing::Invoke(
            [this](int handle, HdmiCecRxCallback_t cbfunc, void* data) {
                if (handle == currentHandle) {
                    rxCallback = cbfunc;
                    rxCallbackData = data;
                    return HDMI_CEC_IO_SUCCESS;
                }
                return HDMI_CEC_IO_INVALID_HANDLE;
            }));
    
    ON_CALL(*this, HdmiCecSetTxCallback(::testing::_, ::testing::_, ::testing::_))
        .WillByDefault(::testing::Invoke(
            [this](int handle, HdmiCecTxCallback_t cbfunc, void* data) {
                if (handle == currentHandle) {
                    txCallback = cbfunc;
                    txCallbackData = data;
                    return HDMI_CEC_IO_SUCCESS;
                }
                return HDMI_CEC_IO_INVALID_HANDLE;
            }));
    
    ON_CALL(*this, HdmiCecTx(::testing::_, ::testing::_, ::testing::_, ::testing::_))
        .WillByDefault(::testing::Invoke(
            [](int handle, const unsigned char* buf, int len, int* result) {
                if (result) {
                    *result = HDMI_CEC_IO_SENT_AND_ACKD;
                }
                return HDMI_CEC_IO_SUCCESS;
            }));
    
    ON_CALL(*this, HdmiCecTxAsync(::testing::_, ::testing::_, ::testing::_))
        .WillByDefault(::testing::Return(HDMI_CEC_IO_SUCCESS));
    
    ON_CALL(*this, HdmiCecAddLogicalAddress(::testing::_, ::testing::_))
        .WillByDefault(::testing::Return(HDMI_CEC_IO_SUCCESS));
    
    ON_CALL(*this, HdmiCecRemoveLogicalAddress(::testing::_, ::testing::_))
        .WillByDefault(::testing::Return(HDMI_CEC_IO_SUCCESS));
    
    ON_CALL(*this, HdmiCecGetPhysicalAddress(::testing::_, ::testing::_))
        .WillByDefault(::testing::Invoke(
            [](int handle, unsigned int* physicalAddress) {
                if (physicalAddress) {
                    *physicalAddress = 0x1000; // Default physical address
                    return HDMI_CEC_IO_SUCCESS;
                }
                return HDMI_CEC_IO_INVALID_ARGUMENT;
            }));
    
    ON_CALL(*this, HdmiCecGetLogicalAddress(::testing::_, ::testing::_))
        .WillByDefault(::testing::Invoke(
            [](int handle, int* logicalAddress) {
                if (logicalAddress) {
                    *logicalAddress = 4; // Default: Playback device
                    return HDMI_CEC_IO_SUCCESS;
                }
                return HDMI_CEC_IO_INVALID_ARGUMENT;
            }));
}

HdmiCecDriverMock::~HdmiCecDriverMock()
{
    if (instance == this) {
        instance = nullptr;
    }
}

HdmiCecDriverMock* HdmiCecDriverMock::getInstance()
{
    std::cout << "[Mock::getInstance] Returning instance: " << (void*)instance << std::endl;
    return instance;
}

void HdmiCecDriverMock::setInstance(HdmiCecDriverMock* newMock)
{
    std::cout << "[Mock::setInstance] Setting instance from " << (void*)instance << " to " << (void*)newMock << std::endl;
    instance = newMock;
    std::cout << "[Mock::setInstance] Instance is now: " << (void*)instance << std::endl;
}

void HdmiCecDriverMock::injectReceivedMessage(const unsigned char* buf, int len)
{
    if (rxCallback) {
        rxCallback(currentHandle, rxCallbackData, const_cast<unsigned char*>(buf), len);
    }
}

void HdmiCecDriverMock::simulateTxResult(int result)
{
    if (txCallback) {
        txCallback(currentHandle, txCallbackData, result);
    }
}

// C API implementations that delegate to mock instance

extern "C" {

int HdmiCecOpen(int *handle)
{
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    if (!mock) {
        return HDMI_CEC_IO_GENERAL_ERROR;
    }
    return mock->HdmiCecOpen(handle);
}

int HdmiCecClose(int handle)
{
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    if (!mock) {
        return HDMI_CEC_IO_GENERAL_ERROR;
    }
    return mock->HdmiCecClose(handle);
}

int HdmiCecSetRxCallback(int handle, HdmiCecRxCallback_t cbfunc, void *data)
{
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    if (!mock) {
        return HDMI_CEC_IO_GENERAL_ERROR;
    }
    return mock->HdmiCecSetRxCallback(handle, cbfunc, data);
}

int HdmiCecSetTxCallback(int handle, HdmiCecTxCallback_t cbfunc, void *data)
{
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    if (!mock) {
        return HDMI_CEC_IO_GENERAL_ERROR;
    }
    return mock->HdmiCecSetTxCallback(handle, cbfunc, data);
}

int HdmiCecTx(int handle, const unsigned char *buf, int len, int *result)
{
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    if (!mock) {
        return HDMI_CEC_IO_GENERAL_ERROR;
    }
    return mock->HdmiCecTx(handle, buf, len, result);
}

int HdmiCecTxAsync(int handle, const unsigned char *buf, int len)
{
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    if (!mock) {
        return HDMI_CEC_IO_GENERAL_ERROR;
    }
    return mock->HdmiCecTxAsync(handle, buf, len);
}

int HdmiCecAddLogicalAddress(int handle, int logicalAddress)
{
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    if (!mock) {
        return HDMI_CEC_IO_GENERAL_ERROR;
    }
    return mock->HdmiCecAddLogicalAddress(handle, logicalAddress);
}

int HdmiCecRemoveLogicalAddress(int handle, int logicalAddress)
{
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    if (!mock) {
        return HDMI_CEC_IO_GENERAL_ERROR;
    }
    return mock->HdmiCecRemoveLogicalAddress(handle, logicalAddress);
}

int HdmiCecGetPhysicalAddress(int handle, unsigned int *physicalAddress)
{
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    if (!mock) {
        return HDMI_CEC_IO_GENERAL_ERROR;
    }
    return mock->HdmiCecGetPhysicalAddress(handle, physicalAddress);
}

int HdmiCecGetLogicalAddress(int handle, int *logicalAddress)
{
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    if (!mock) {
        return HDMI_CEC_IO_GENERAL_ERROR;
    }
    return mock->HdmiCecGetLogicalAddress(handle, logicalAddress);
}

} // extern "C"
