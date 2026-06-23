/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2026 RDK Management
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

/*
 * HDMICecRdkVHAL.cpp — Legacy C HAL implementation of HDMICecHal.
 *
 * Each function wraps the corresponding HdmiCec* C API from hdmi_cec_driver.h.
 */

#include <cstddef>
#include "HDMICecRdkVHAL.h"
#include "ccec/Util.hpp"

/* -----------------------------------------------------------------------
 * open
 * Calls HdmiCecOpen() to initialise the driver and obtain a handle.
 * -------------------------------------------------------------------- */
int HDMICecRdkVHAL::open(int *handle)
{
    int ret = ::HdmiCecOpen(handle);
    CCEC_LOG(LOG_DEBUG, "HDMICecRdkVHAL::open ret=%d handle=%d\r\n", ret, (handle ? *handle : -1));
    return ret;
}

/* -----------------------------------------------------------------------
 * close
 * Calls HdmiCecClose() to release driver resources.
 * -------------------------------------------------------------------- */
int HDMICecRdkVHAL::close(int handle)
{
    int ret = ::HdmiCecClose(handle);
    CCEC_LOG(LOG_DEBUG, "HDMICecRdkVHAL::close handle=%d ret=%d\r\n", handle, ret);
    return ret;
}

/* -----------------------------------------------------------------------
 * setRxCallback
 * Calls HdmiCecSetRxCallback() to register the incoming-message callback.
 * -------------------------------------------------------------------- */
int HDMICecRdkVHAL::setRxCallback(int handle, HdmiCecRxCallback_t cbfunc, void *data)
{
    int ret = ::HdmiCecSetRxCallback(handle, cbfunc, data);
    CCEC_LOG(LOG_DEBUG, "HDMICecRdkVHAL::setRxCallback handle=%d ret=%d\r\n", handle, ret);
    return ret;
}

/* -----------------------------------------------------------------------
 * setTxCallback
 * Calls HdmiCecSetTxCallback() to register the transmit-status callback.
 * -------------------------------------------------------------------- */
int HDMICecRdkVHAL::setTxCallback(int handle, HdmiCecTxCallback_t cbfunc, void *data)
{
    int ret = ::HdmiCecSetTxCallback(handle, cbfunc, data);
    CCEC_LOG(LOG_DEBUG, "HDMICecRdkVHAL::setTxCallback handle=%d ret=%d\r\n", handle, ret);
    return ret;
}

/* -----------------------------------------------------------------------
 * tx
 * Calls HdmiCecTx() for a synchronous transmit; blocks until ACK/NACK.
 * -------------------------------------------------------------------- */
int HDMICecRdkVHAL::tx(int handle, const unsigned char *buf, int len, int *result)
{
    int ret = ::HdmiCecTx(handle, buf, len, result);
    CCEC_LOG(LOG_DEBUG, "HDMICecRdkVHAL::tx handle=%d ret=%d sendResult=%d\r\n", handle, ret, (result ? *result : -1));
    return ret;
}

/* -----------------------------------------------------------------------
 * txAsync
 * Calls HdmiCecTxAsync() for a fire-and-forget transmit.
 * -------------------------------------------------------------------- */
int HDMICecRdkVHAL::txAsync(int handle, const unsigned char *buf, int len)
{
    int ret = ::HdmiCecTxAsync(handle, buf, len);
    CCEC_LOG(LOG_DEBUG, "HDMICecRdkVHAL::txAsync handle=%d ret=%d\r\n", handle, ret);
    return ret;
}

/* -----------------------------------------------------------------------
 * addLogicalAddress
 * Calls HdmiCecAddLogicalAddress() to claim a logical address on the bus.
 * -------------------------------------------------------------------- */
int HDMICecRdkVHAL::addLogicalAddress(int handle, int logicalAddress)
{
    int ret = ::HdmiCecAddLogicalAddress(handle, logicalAddress);
    CCEC_LOG(LOG_DEBUG, "HDMICecRdkVHAL::addLogicalAddress handle=%d addr=%d ret=%d\r\n", handle, logicalAddress, ret);
    return ret;
}

/* -----------------------------------------------------------------------
 * removeLogicalAddress
 * Calls HdmiCecRemoveLogicalAddress() to release a previously claimed
 * logical address.
 * -------------------------------------------------------------------- */
int HDMICecRdkVHAL::removeLogicalAddress(int handle, int logicalAddress)
{
    int ret = ::HdmiCecRemoveLogicalAddress(handle, logicalAddress);
    CCEC_LOG(LOG_DEBUG, "HDMICecRdkVHAL::removeLogicalAddress handle=%d addr=%d ret=%d\r\n", handle, logicalAddress, ret);
    return ret;
}

/* -----------------------------------------------------------------------
 * getLogicalAddress
 * Calls HdmiCecGetLogicalAddress() to retrieve the current logical address.
 * -------------------------------------------------------------------- */
int HDMICecRdkVHAL::getLogicalAddress(int handle, int devType, int *logicalAddress)
{
	(void)devType;
    int ret = ::HdmiCecGetLogicalAddress(handle, logicalAddress);
    CCEC_LOG(LOG_DEBUG, "HDMICecRdkVHAL::getLogicalAddress handle=%d ret=%d addr=%d\r\n",
             handle, ret, (logicalAddress ? *logicalAddress : -1));
    return ret;
}

/* -----------------------------------------------------------------------
 * getPhysicalAddress
 * Calls HdmiCecGetPhysicalAddress() to retrieve the device physical address.
 * -------------------------------------------------------------------- */
int HDMICecRdkVHAL::getPhysicalAddress(int handle, unsigned int *physicalAddress)
{
    int ret = ::HdmiCecGetPhysicalAddress(handle, physicalAddress);
    CCEC_LOG(LOG_DEBUG, "HDMICecRdkVHAL::getPhysicalAddress handle=%d ret=%d addr=0x%x\r\n",
             ret, (physicalAddress ? *physicalAddress : 0));
    return ret;
}

