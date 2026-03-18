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

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// HDMI CEC I/O Result Codes
#define HDMI_CEC_IO_SUCCESS                    0
#define HDMI_CEC_IO_SENT_AND_ACKD             0
#define HDMI_CEC_IO_SENT_BUT_NOT_ACKD         1
#define HDMI_CEC_IO_SENT_FAILED               2
#define HDMI_CEC_IO_GENERAL_ERROR             3
#define HDMI_CEC_IO_INVALID_HANDLE            4
#define HDMI_CEC_IO_INVALID_ARGUMENT          5
#define HDMI_CEC_IO_INVALID_STATE             6
#define HDMI_CEC_IO_LOGICALADDRESS_UNAVAILABLE 7
#define HDMI_CEC_IO_NOT_OPENED                8
#define HDMI_CEC_IO_ALREADY_OPEN              9

// Callback function types
typedef void (*HdmiCecRxCallback_t)(int handle, void *callbackData, unsigned char *buf, int len);
typedef void (*HdmiCecTxCallback_t)(int handle, void *callbackData, int result);

/**
 * @brief Open HDMI CEC HAL driver
 *
 * @param[out] handle Pointer to store the driver handle
 * @return HDMI_CEC_IO_SUCCESS on success, error code otherwise
 */
int HdmiCecOpen(int *handle);

/**
 * @brief Close HDMI CEC HAL driver
 *
 * @param[in] handle Driver handle
 * @return HDMI_CEC_IO_SUCCESS on success, error code otherwise
 */
int HdmiCecClose(int handle);

/**
 * @brief Set receive callback for incoming CEC messages
 *
 * @param[in] handle Driver handle
 * @param[in] cbfunc Callback function to handle received messages
 * @param[in] data User data to pass to callback
 * @return HDMI_CEC_IO_SUCCESS on success, error code otherwise
 */
int HdmiCecSetRxCallback(int handle, HdmiCecRxCallback_t cbfunc, void *data);

/**
 * @brief Set transmit callback for transmission status
 *
 * @param[in] handle Driver handle
 * @param[in] cbfunc Callback function to handle transmit status
 * @param[in] data User data to pass to callback
 * @return HDMI_CEC_IO_SUCCESS on success, error code otherwise
 */
int HdmiCecSetTxCallback(int handle, HdmiCecTxCallback_t cbfunc, void *data);

/**
 * @brief Transmit CEC message (synchronous)
 *
 * @param[in] handle Driver handle
 * @param[in] buf Buffer containing CEC message
 * @param[in] len Length of message
 * @param[out] result Pointer to store transmission result
 * @return HDMI_CEC_IO_SUCCESS on success, error code otherwise
 */
int HdmiCecTx(int handle, const unsigned char *buf, int len, int *result);

/**
 * @brief Transmit CEC message (asynchronous)
 *
 * @param[in] handle Driver handle
 * @param[in] buf Buffer containing CEC message
 * @param[in] len Length of message
 * @return HDMI_CEC_IO_SUCCESS on success, error code otherwise
 */
int HdmiCecTxAsync(int handle, const unsigned char *buf, int len);

/**
 * @brief Add logical address for receiving CEC messages
 *
 * @param[in] handle Driver handle
 * @param[in] logicalAddress Logical address to add (0-15)
 * @return HDMI_CEC_IO_SUCCESS on success, error code otherwise
 */
int HdmiCecAddLogicalAddress(int handle, int logicalAddress);

/**
 * @brief Remove logical address
 *
 * @param[in] handle Driver handle
 * @param[in] logicalAddress Logical address to remove (0-15)
 * @return HDMI_CEC_IO_SUCCESS on success, error code otherwise
 */
int HdmiCecRemoveLogicalAddress(int handle, int logicalAddress);

/**
 * @brief Get physical address of the device
 *
 * @param[in] handle Driver handle
 * @param[out] physicalAddress Pointer to store physical address
 * @return HDMI_CEC_IO_SUCCESS on success, error code otherwise
 */
int HdmiCecGetPhysicalAddress(int handle, unsigned int *physicalAddress);

/**
 * @brief Get logical address
 *
 * @param[in] handle Driver handle
 * @param[out] logicalAddress Pointer to store logical address
 * @return HDMI_CEC_IO_SUCCESS on success, error code otherwise
 */
int HdmiCecGetLogicalAddress(int handle, int *logicalAddress);

#ifdef __cplusplus
}
#endif
