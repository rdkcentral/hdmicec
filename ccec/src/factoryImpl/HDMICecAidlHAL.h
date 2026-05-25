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

#ifndef HDMI_CEC_AIDL_HAL_H
#define HDMI_CEC_AIDL_HAL_H

#include "IHDMICecHal.h"

 #include <cstdint>
 #include <string>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <utils/String16.h>
#include <com/rdk/hal/hdmicec/IHdmiCec.h>
#include <com/rdk/hal/hdmicec/IHdmiCecController.h>
#include <com/rdk/hal/hdmicec/IHdmiCecEventListener.h>
#include <com/rdk/hal/hdmicec/BnHdmiCecEventListener.h>
#include <com/rdk/hal/hdmicec/SendMessageStatus.h>
#include <com/rdk/hal/hdmicec/State.h>

#include "osal/Mutex.hpp"

using CCEC_OSAL::Mutex;

class HDMICecAidlHALEventListener;

class HDMICecAidlHAL : public IHDMICecHal {
public:
    HDMICecAidlHAL();
    ~HDMICecAidlHAL() override;

    int open(int *handle) override;
    int close(int handle) override;
    int addLogicalAddress(int handle, int logicalAddresses) override;
    int removeLogicalAddress(int handle, int logicalAddresses) override;
    int getLogicalAddress(int handle, int *logicalAddress) override;
    int getPhysicalAddress(int handle, unsigned int *physicalAddress) override;
    int setRxCallback(int handle, HdmiCecRxCallback_t cbfunc, void *data) override;
    int setTxCallback(int handle, HdmiCecTxCallback_t cbfunc, void *data) override;
    int tx(int handle, const unsigned char *buf, int len, int *result) override;
    int txAsync(int handle, const unsigned char *buf, int len) override;
    bool skipFrameOfUnsupportedLength(size_t length) override;
    bool emulateAckForPollFrames(const unsigned char *buf, int len) override;

private:
    const size_t kAidlMinCecFrameSize = 2;
    const size_t kAidlMaxCecFrameSize = 16;
    static constexpr const char* kVdeviceTopologyDump = "/tmp/hdmi_cec_device_list_info.txt";

    bool parseLogicalAddressField(const std::string& line, const char* field, int& value);
    bool isPresentInVdeviceTopology(const uint8_t destination);
    android::sp<com::rdk::hal::hdmicec::IHdmiCec> getAidlService();
    void initAidlService();
    void dispatchRx(unsigned char *buf, int len);
    void dispatchTx(int result);

    android::sp<com::rdk::hal::hdmicec::IHdmiCec> mAidlService;
    android::sp<com::rdk::hal::hdmicec::IHdmiCecController> mAidlController;
    android::sp<com::rdk::hal::hdmicec::IHdmiCecEventListener> mEventListener;
    HdmiCecRxCallback_t mRxCb;
    HdmiCecTxCallback_t mTxCb;
    void* mRxCbData;
    void* mTxCbData;
    mutable Mutex mAidlMutex;

    friend class HDMICecAidlHALEventListener;
};

#endif // HDMI_CEC_AIDL_HAL_H

