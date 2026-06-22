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

#ifndef HDMI_CEC_RDK_V_HAL_H
#define HDMI_CEC_RDK_V_HAL_H

#include "IHDMICecHal.h"

/**
 * @brief Legacy (C HAL) implementation of IHDMICecHal.
 *
 * Each override delegates directly to the corresponding HdmiCec* C function
 * from hdmi_cec_driver.h.
 */
class HDMICecRdkVHAL : public IHDMICecHal {
public:
    /**
     * @brief Open the HDMI CEC HAL driver.
     * Calls HdmiCecOpen().
     */
    int open(int *handle) override;

    /**
     * @brief Close the HDMI CEC HAL driver.
     * Calls HdmiCecClose().
     */
    int close(int handle) override;

    /**
     * @brief Register the receive callback.
     * Calls HdmiCecSetRxCallback().
     */
    int setRxCallback(int handle, HdmiCecRxCallback_t cbfunc, void *data) override;

    /**
     * @brief Register the transmit-status callback.
     * Calls HdmiCecSetTxCallback().
     */
    int setTxCallback(int handle, HdmiCecTxCallback_t cbfunc, void *data) override;

    /**
     * @brief Synchronous CEC transmit.
     * Calls HdmiCecTx(). The send result is written to *result.
     */
    int tx(int handle, const unsigned char *buf, int len, int *result) override;

    /**
     * @brief Asynchronous CEC transmit.
     * Calls HdmiCecTxAsync(). Result is delivered via the TxCallback.
     */
    int txAsync(int handle, const unsigned char *buf, int len) override;

    /**
     * @brief Add a logical address.
     * Calls HdmiCecAddLogicalAddress().
     */
    int addLogicalAddress(int handle, int logicalAddress) override;

    /**
     * @brief Remove a logical address.
     * Calls HdmiCecRemoveLogicalAddress().
     */
    int removeLogicalAddress(int handle, int logicalAddress) override;

    /**
     * @brief Get the device logical address.
     * Calls HdmiCecGetLogicalAddress(). devType is unused on legacy backend.
     */
    int getLogicalAddress(int handle, int devType, int *logicalAddress) override;

    /**
     * @brief Get the device physical address.
     * Calls HdmiCecGetPhysicalAddress().
     */
    int getPhysicalAddress(int handle, unsigned int *physicalAddress) override;
};

#endif // HDMI_CEC_RDK_V_HAL_H


