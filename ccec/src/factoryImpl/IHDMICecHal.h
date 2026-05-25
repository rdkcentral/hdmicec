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

#ifndef I_HDMI_CEC_HAL_H
#define I_HDMI_CEC_HAL_H

#include <cstddef>
#include "ccec/drivers/hdmi_cec_driver.h"

/**
 * @brief Abstract base class for HDMI CEC hardware abstraction.
 *
 * Each virtual function maps 1:1 to a HAL operation. Subclasses provide
 * either a legacy (C HAL) implementation or an AIDL (binder) implementation.
 *
 * - Legacy subclass: delegates directly to HdmiCec* C functions.
 * - AIDL subclass:   communicates with the Android HDMI CEC AIDL service.
 */
class IHDMICecHal {
public:
    virtual ~IHDMICecHal() = default;

    /**
     * @brief Open the HDMI CEC HAL driver.
     *
     * Legacy: calls HdmiCecOpen().
     * AIDL:   connects to the AIDL binder service and obtains a session.
     *
     * @param[out] handle  Receives the driver handle.
     * @return HDMI_CEC_IO_SUCCESS on success, or an error code.
     */
    virtual int open(int *handle) = 0;

    /**
     * @brief Close the HDMI CEC HAL driver.
     *
     * Legacy: calls HdmiCecClose().
     * AIDL:   disconnects from the binder service.
     *
     * @param[in] handle  The driver handle returned by open().
     * @return HDMI_CEC_IO_SUCCESS on success, or an error code.
     */
    virtual int close(int handle) = 0;

    /**
     * @brief Set the receive callback for incoming CEC messages.
     *
     * Legacy: calls HdmiCecSetRxCallback().
     * AIDL:   registers an AIDL callback listener that bridges to cbfunc.
     *
     * @param[in] handle  The driver handle.
     * @param[in] cbfunc  Callback function invoked on message reception.
     * @param[in] data    Opaque user data forwarded to cbfunc.
     * @return HDMI_CEC_IO_SUCCESS on success, or an error code.
     */
    virtual int setRxCallback(int handle, HdmiCecRxCallback_t cbfunc, void *data) = 0;

    /**
     * @brief Set the transmit callback for transmission status notification.
     *
     * Legacy: calls HdmiCecSetTxCallback().
     * AIDL:   registers an AIDL callback listener that bridges to cbfunc.
     *
     * @param[in] handle  The driver handle.
     * @param[in] cbfunc  Callback function invoked with transmit result.
     * @param[in] data    Opaque user data forwarded to cbfunc.
     * @return HDMI_CEC_IO_SUCCESS on success, or an error code.
     */
    virtual int setTxCallback(int handle, HdmiCecTxCallback_t cbfunc, void *data) = 0;

    /**
     * @brief Transmit a CEC message synchronously.
     *
     * Legacy: calls HdmiCecTx().
     * AIDL:   sends message via binder and blocks until result is available.
     *
     * @param[in]  handle  The driver handle.
     * @param[in]  buf     Buffer containing the CEC message.
     * @param[in]  len     Length of the message in bytes.
     * @param[out] result  Receives the transmission result code.
     * @return HDMI_CEC_IO_SUCCESS on success, or an error code.
     */
    virtual int tx(int handle, const unsigned char *buf, int len, int *result) = 0;

    /**
     * @brief Transmit a CEC message asynchronously.
     *
     * Legacy: calls HdmiCecTxAsync().
     * AIDL:   sends message via binder; result delivered through TxCallback.
     *
     * @param[in] handle  The driver handle.
     * @param[in] buf     Buffer containing the CEC message.
     * @param[in] len     Length of the message in bytes.
     * @return HDMI_CEC_IO_SUCCESS on success, or an error code.
     */
    virtual int txAsync(int handle, const unsigned char *buf, int len) = 0;

    /**
     * @brief Add a logical address for receiving CEC messages.
     *
     * Legacy: calls HdmiCecAddLogicalAddress().
     * AIDL:   registers the logical address via binder.
     *
     * @param[in] handle          The driver handle.
     * @param[in] logicalAddress  Logical address to add (0-15).
     * @return HDMI_CEC_IO_SUCCESS on success, or an error code.
     */
    virtual int addLogicalAddress(int handle, int logicalAddress) = 0;

    /**
     * @brief Remove a previously added logical address.
     *
     * Legacy: calls HdmiCecRemoveLogicalAddress().
     * AIDL:   unregisters the logical address via binder.
     *
     * @param[in] handle          The driver handle.
     * @param[in] logicalAddress  Logical address to remove (0-15).
     * @return HDMI_CEC_IO_SUCCESS on success, or an error code.
     */
    virtual int removeLogicalAddress(int handle, int logicalAddress) = 0;

    /**
     * @brief Get the logical address of the device.
     *
     * Legacy: calls HdmiCecGetLogicalAddress().
     * AIDL:   queries the logical address via binder.
     *
     * @param[in]  handle          The driver handle.
     * @param[out] logicalAddress  Pointer to store the logical address.
     * @return HDMI_CEC_IO_SUCCESS on success, or an error code.
     */
    virtual int getLogicalAddress(int handle, int *logicalAddress) = 0;

    /**
     * @brief Get the physical address of the device.
     *
     * Legacy: calls HdmiCecGetPhysicalAddress().
     * AIDL:   queries the physical address via binder.
     *
     * @param[in]  handle           The driver handle.
     * @param[out] physicalAddress  Pointer to store the physical address.
     * @return HDMI_CEC_IO_SUCCESS on success, or an error code.
     */
    virtual int getPhysicalAddress(int handle, unsigned int *physicalAddress) = 0;

    /**
     * @brief Check if a frame of unsupported length should be skipped.
     *
     * Legacy: no-op (returns false).
     * AIDL:   validates frame length against binder service constraints.
     *
     * @param[in] length  Length of the CEC frame.
     * @return true if the frame should be skipped, false otherwise.
     */
    virtual bool skipFrameOfUnsupportedLength(size_t length) = 0;

    /**
     * @brief Emulate acknowledgment for poll frames.
     *
     * Legacy: Ignores this as the legacy HAL does not support emulating ACKs.
     * AIDL:   Check Vdevice topology to determine emulation required or not
     *
     * @param[in] buf  Buffer containing the CEC frame.
     * @param[in] len  Length of the CEC frame.
     * @return true if acknowledgment should be emulated, false otherwise.
     */
    virtual bool emulateAckForPollFrames(const unsigned char *buf, int len) = 0;
};

#endif // I_HDMI_CEC_HAL_H



