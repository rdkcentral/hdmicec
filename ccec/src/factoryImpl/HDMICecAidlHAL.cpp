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

#include "HDMICecAidlHAL.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ccec/Util.hpp"
#include "ccec/Exception.hpp"
#include "ccec/drivers/hdmi_cec_driver.h"

using CCEC_OSAL::AutoLock;
using android::sp;
using android::String16;
using android::defaultServiceManager;
using android::interface_cast;
using namespace com::rdk::hal::hdmicec;

/**
 * @brief HDMI CEC AIDL Event Listener — bridges binder callbacks to the
 *        C-style callback function pointers stored in HDMICecAidlHAL.
 */
class HDMICecAidlHALEventListener : public BnHdmiCecEventListener {
public:
    HDMICecAidlHALEventListener(HDMICecAidlHAL *AidlHal)
        : mAidlHal(AidlHal) {}

    android::binder::Status onMessageReceived(const std::vector<uint8_t>& message) override {
        if (mAidlHal && !message.empty()) {
            std::vector<uint8_t> mutableMessage(message);
            mAidlHal->dispatchRx(reinterpret_cast<unsigned char*>(mutableMessage.data()),
                                  static_cast<int>(mutableMessage.size()));
        }
        return android::binder::Status::ok();
    }

    android::binder::Status onStateChanged(State oldState, State newState) override {
        // Handle state changes if needed
        return android::binder::Status::ok();
    }

    android::binder::Status onMessageSent(const std::vector<uint8_t>& message, SendMessageStatus status) override {
        if (mAidlHal) {
             int result = HDMI_CEC_IO_SENT_FAILED;
             if (status == SendMessageStatus::ACK_STATE_0) {
                 result = HDMI_CEC_IO_SENT_AND_ACKD;
             } else if (status == SendMessageStatus::ACK_STATE_1) {
                 result = HDMI_CEC_IO_SENT_BUT_NOT_ACKD;
             }
            mAidlHal->dispatchTx(result);
        }
        return android::binder::Status::ok();
    }

private:
    HDMICecAidlHAL *mAidlHal;
};


// Return the standard logical-address candidates for a given device type.
// Used only as a fallback when AIDL reports no allocated addresses yet.
std::vector<int32_t> preferredLogicalAddressesForDeviceType(int devType)
{
    // Common CEC type ids used in middleware:
    // 0: TV, 1: RecordingDevice, 3: Tuner, 4: PlaybackDevice, 5: AudioSystem.
    switch (devType) {
        case 0: return std::vector<int32_t>{0}; // TV
        case 1: return std::vector<int32_t>{1, 2, 9}; // Recorder
        case 3: return std::vector<int32_t>{3, 6, 7, 10}; // Tuner
        case 5: return std::vector<int32_t>{5}; // AudioSystem
        case 4:
        default:
            return std::vector<int32_t>{4, 8, 11}; // PlaybackDevice fallback
    }
}

HDMICecAidlHAL::HDMICecAidlHAL()
    : mAidlService(nullptr),
      mAidlController(nullptr),
    mEventListener(nullptr),
    mRxCb(nullptr),
    mTxCb(nullptr),
    mRxCbData(nullptr),
    mTxCbData(nullptr)
{
}

HDMICecAidlHAL::~HDMICecAidlHAL()
{
    AutoLock lock_(mAidlMutex);
    mAidlController = nullptr;
    mAidlService = nullptr;
    mEventListener = nullptr;
}

android::sp<IHdmiCec> HDMICecAidlHAL::getAidlService()
{
    AutoLock lock_(mAidlMutex);
    if (mAidlService == nullptr) {
        initAidlService();
    }
    return mAidlService;
}

void HDMICecAidlHAL::initAidlService()
{
    android::ProcessState::self()->startThreadPool();

    sp<android::IServiceManager> sm = defaultServiceManager();
    if (sm != nullptr) {
        mAidlService = interface_cast<IHdmiCec>(
            sm->getService(String16(IHdmiCec::serviceName().c_str())));
        if (mAidlService == nullptr) {
            CCEC_LOG(LOG_EXP, "Failed to get AIDL HdmiCec service\r\n");
            throw IOException();
        } 
        CCEC_LOG(LOG_DEBUG, "Successfully obtained AIDL HdmiCec service\r\n");
    } else {
        CCEC_LOG(LOG_EXP, "Failed to get service manager\n");
        throw IOException();
    }
}

int HDMICecAidlHAL::open(int *handle)
{
    CCEC_LOG(LOG_INFO, "HDMICecAidlHAL::open invoked\n");

    if (handle == nullptr) {
        CCEC_LOG(LOG_ERROR, "HDMICecAidlHAL::open failed: invalid handle pointer\r\n");
        throw IOException();
    }

    android::sp<IHdmiCec> service = getAidlService();
    if (service == nullptr) {
        CCEC_LOG(LOG_ERROR, "HDMICecAidlHAL::open failed: IHdmiCec service unavailable\r\n");
        throw IOException();
    }
    // Create event listener
    mEventListener = new HDMICecAidlHALEventListener(this);

    // Open AIDL interface
    android::sp<IHdmiCecController> controller;
    android::binder::Status status = service->open(mEventListener, &controller);
    if (!status.isOk() || controller == nullptr) {
        CCEC_LOG(LOG_ERROR, "HDMICecAidlHAL::open failed: service->open status not OK or controller is null\r\n");
        throw IOException();
    }

    mAidlController = controller;

    *handle = 1;  /* Dummy handle — AIDL uses controller object */

    return 0;
}

int HDMICecAidlHAL::close(int handle)
{
    CCEC_LOG(LOG_INFO, "HDMICecAidlHAL::close invoked\n");
    (void)handle;

    if (mAidlController != nullptr) {
        android::sp<IHdmiCec> service = getAidlService();
        if (service != nullptr) {
            bool result = false;
            android::binder::Status status = service->close(mAidlController, &result);
            if (!status.isOk()) {
                CCEC_LOG(LOG_EXP, "Failed to close AIDL HdmiCec interface: %s\r\n", status.toString8().c_str());
                CCEC_LOG(LOG_ERROR, "HDMICecAidlHAL::close failed: service->close status not OK\n");
                throw IOException();
            }
            if (!result) {
                CCEC_LOG(LOG_ERROR, "HDMICecAidlHAL::close failed: service->close returned false\n");
                throw IOException();
            }
        }
        mAidlController = nullptr;
    }

    mEventListener = nullptr;

    CCEC_LOG(LOG_DEBUG, "Successfully closed AIDL HdmiCec interface\r\n");
    return 0;
}

int HDMICecAidlHAL::addLogicalAddress(int handle, int logicalAddresses)
{
    (void)handle;
    if (mAidlController == nullptr) {
        throw IOException();
    }
    std::vector<int32_t> addresses;
    addresses.push_back(logicalAddresses);
    bool result = false;
    android::binder::Status status = mAidlController->addLogicalAddresses(addresses, &result);

    if (!status.isOk()) {
        CCEC_LOG(LOG_EXP, "Failed to add logical address via AIDL: %s\r\n", status.toString8().c_str());
        throw IOException();
    }

    if (!result) {
        throw AddressNotAvailableException();
    }

    CCEC_LOG(LOG_DEBUG, "Successfully added logical address addr=%d via AIDL\n", logicalAddresses);
    return 0;
}

int HDMICecAidlHAL::removeLogicalAddress(int handle, int logicalAddresses)
{
    (void)handle;
    if (mAidlController == nullptr) {
        throw IOException();
    }

    std::vector<int32_t> addresses;
    addresses.push_back(logicalAddresses);
    bool result = false;
    android::binder::Status status = mAidlController->removeLogicalAddresses(addresses, &result);
    if (!status.isOk() || !result) {
        CCEC_LOG(LOG_EXP, "Failed to remove logical address via AIDL: %s\n", status.toString8().c_str());
        throw IOException();
    }

    CCEC_LOG(LOG_DEBUG, "Successfully removed logical address addr=%d via AIDL\n", logicalAddresses);
    return 0;
}

int HDMICecAidlHAL::getLogicalAddress(int handle, int devType, int *logicalAddress)
{
    (void)handle;
    if (logicalAddress == nullptr) {
        CCEC_LOG(LOG_ERROR, "HDMICecAidlHAL::getLogicalAddress invalid output pointer\r\n");
        throw IOException();
    }

    *logicalAddress = 0;
    if (mAidlService == nullptr) {
        android::sp<IHdmiCec> service = getAidlService();
        if (service == nullptr) {
            throw IOException();
        }
        mAidlService = service;
    }

    std::vector<int32_t> addresses;
    android::binder::Status status = mAidlService->getLogicalAddresses(&addresses);
    if (status.isOk() && addresses.size() > 0) {
        *logicalAddress = addresses[0];
    }else {
        CCEC_LOG(LOG_WARN,
            "HDMICecAidlHAL::getLogicalAddress no allocated LA from AIDL (statusOk=%d, count=%zu devType=%d). Trying fallback allocation.\r\n",
            status.isOk() ? 1 : 0,
            addresses.size(),
            devType);
        if (mAidlController != nullptr) {
            const std::vector<int32_t> preferred = preferredLogicalAddressesForDeviceType(devType);
            for (std::vector<int32_t>::const_iterator it = preferred.begin(); it != preferred.end(); ++it) {
                std::vector<int32_t> candidate;
                candidate.push_back(*it);
                bool addResult = false;
                android::binder::Status addStatus = mAidlController->addLogicalAddresses(candidate, &addResult);
                CCEC_LOG(LOG_DEBUG,
                    "HDMICecAidlHAL::getLogicalAddress fallback addLogicalAddresses candidate=%d addOk=%d addResult=%d\r\n",
                    candidate[0],
                    addStatus.isOk() ? 1 : 0,
                    addResult ? 1 : 0);
                addresses.clear();
                android::binder::Status retryStatus = mAidlService->getLogicalAddresses(&addresses);
                if (retryStatus.isOk() && addresses.size() > 0) {
                    *logicalAddress = addresses[0];
                    break;
                }
            }
        }
    }

    
    CCEC_LOG( LOG_DEBUG, "HDMICecAidlHAL::getLogicalAddress completed\r\n");

    return 0;
}

int HDMICecAidlHAL::getPhysicalAddress(int handle, unsigned int *physicalAddress)
{
    if (physicalAddress != nullptr) {
        *physicalAddress = 0;
    }

    CCEC_LOG( LOG_DEBUG, "HDMICecAidlHAL::getPhysicalAddress completed\r\n");

    return 0;
}

void HDMICecAidlHAL::dispatchRx(unsigned char *buf, int len)
{
    if (mRxCb == nullptr) {
        CCEC_LOG(LOG_DEBUG, "HDMICecAidlHAL::dispatchRx callback not registered\r\n");
        return;
    }

    // Track initiator LA from inbound frames so 1-byte poll can be emulated
    // locally on AIDL backends that reject 1-byte sendMessage payloads.
    if (buf != nullptr && len >= 1) {
        const uint8_t srcLA = static_cast<uint8_t>((buf[0] >> 4) & 0x0F);
        if (srcLA <= 0x0E) {
            AutoLock lock_(mAidlMutex);
            mSeenLogicalAddresses.insert(srcLA);
        }
    }
    mRxCb(0, mRxCbData, buf, len);
}

void HDMICecAidlHAL::dispatchTx(int result)
{
    if (mTxCb == nullptr) {
        CCEC_LOG(LOG_DEBUG, "HDMICecAidlHAL::dispatchTx callback not registered\r\n");
        return;
    }

    mTxCb(0, mTxCbData, result);
}

int HDMICecAidlHAL::setRxCallback(int handle, HdmiCecRxCallback_t cbfunc, void *data)
{
    (void)handle;

    mRxCb = cbfunc;
    mRxCbData = data;

    CCEC_LOG(LOG_DEBUG, "HDMICecAidlHAL::setRxCallback invoked\r\n");
    return 0;
}

int HDMICecAidlHAL::setTxCallback(int handle, HdmiCecTxCallback_t cbfunc, void *data)
{
    (void)handle;

    mTxCb = cbfunc;
    mTxCbData = data;

    CCEC_LOG(LOG_DEBUG, "HDMICecAidlHAL::setTxCallback invoked\r\n");
    return 0;
}

int HDMICecAidlHAL::tx(int handle, const unsigned char *buf, int len, int *result)
{
    (void)handle;
    if (mAidlController == nullptr) {
        throw IOException();
    }

    if (result == nullptr) {
        CCEC_LOG(LOG_ERROR, "HDMICecAidlHAL::tx invalid result pointer\n");
        throw IOException();
    }

    if (buf == nullptr || len <= 0) {
        CCEC_LOG(LOG_ERROR, "HDMICecAidlHAL::tx invalid buffer or length\n");
        throw IOException();
    }

    if(emulateAckForPollFrames(buf, len)) {
        *result = HDMI_CEC_IO_SUCCESS;
        return 0;
    }

    std::vector<uint8_t> message(buf, buf + len);
    SendMessageStatus sendStatus;
    android::binder::Status status = mAidlController->sendMessage(message, &sendStatus);
    if (!status.isOk()) {
        CCEC_LOG(LOG_ERROR, "AIDL sendMessage failed: %s\r\n", status.toString8().c_str());
        throw IOException();
    }

    // Map AIDL SendMessageStatus to HAL error codes
    *result = HDMI_CEC_IO_SUCCESS;
    if (sendStatus == SendMessageStatus::ACK_STATE_0) {
        *result = HDMI_CEC_IO_SENT_AND_ACKD;
    } else if (sendStatus == SendMessageStatus::ACK_STATE_1) {
        *result = HDMI_CEC_IO_SENT_BUT_NOT_ACKD;
    } else if (sendStatus == SendMessageStatus::BUSY){
        *result = HDMI_CEC_IO_SENT_FAILED;
        throw IOException();
    }

    CCEC_LOG( LOG_DEBUG, "AIDL sendMessage DONE, result %x\r\n", *result);

    return 0;
}

int HDMICecAidlHAL::txAsync(int handle, const unsigned char *buf, int len)
{
    (void)handle;
    if (mAidlController == nullptr) {
        throw IOException();
    }
    if (buf == nullptr || len <= 0) {
        CCEC_LOG(LOG_ERROR, "HDMICecAidlHAL::txAsync invalid buffer or length\n");
        throw IOException();
    }

    std::vector<uint8_t> message(buf, buf + len);
    SendMessageStatus sendStatus;
    android::binder::Status status = mAidlController->sendMessage(message, &sendStatus);
    if (!status.isOk()) {
        CCEC_LOG(LOG_ERROR, "AIDL sendMessage failed: %s\r\n", status.toString8().c_str());
        throw IOException();
    }

    if (sendStatus == SendMessageStatus::BUSY) {
        CCEC_LOG(LOG_ERROR, "AIDL sendMessage busy in txAsync\r\n");
        throw IOException();
    }

    CCEC_LOG( LOG_DEBUG, "AIDL txAsync completed, status: %d\r\n", static_cast<int>(sendStatus));

    return 0;
}

bool HDMICecAidlHAL::emulateAckForPollFrames(const unsigned char *buf, int len)
{
    if (len <= 1) {
        /*
         * Poll frame (header only): emulate ACK based on seen-LA cache
         * and 2-byte probe. This keeps HdmiCecSource ping-based discovery
         * working on AIDL backend.
         */
        const uint8_t destination = (buf != NULL) ? (buf[0] & 0x0F) : 0xFF;

        if (destination <= 0x0E) {
            /* Check seen-LA cache first */
            {
                AutoLock lock_(mAidlMutex);
                if (mSeenLogicalAddresses.count(destination) > 0) {
                    CCEC_LOG(LOG_DEBUG,
                        "HDMICecAidlHAL::emulateAckForPollFrames destination=0x%X present in seen-LA set. Emulating ack.\r\n",
                        destination);
                    return true;
                }
            }

            /* Probe with a 2-byte directed frame (GiveDevicePowerStatus) */
            {
                AutoLock lock_(mAidlMutex);
                std::vector<uint8_t> probe;
                probe.reserve(2);
                probe.push_back(buf ? buf[0] : 0);
                probe.push_back(0x8F); // GiveDevicePowerStatus

                SendMessageStatus probeStatus = SendMessageStatus::BUSY;
                android::binder::Status aidlStatus = mAidlController->sendMessage(probe, &probeStatus);
                if (aidlStatus.isOk() && probeStatus == SendMessageStatus::ACK_STATE_0) {
                    mSeenLogicalAddresses.insert(destination);
                    CCEC_LOG(LOG_DEBUG,
                        "HDMICecAidlHAL::emulateAckForPollFrames destination=0x%X ACKed by 2-byte probe. Emulating ack.\r\n",
                        destination);
                    return true;
                }

                CCEC_LOG(LOG_DEBUG,
                    "HDMICecAidlHAL::emulateAckForPollFrames destination=0x%X probe NACK/failed (aidlOk=%d status=%d).\r\n",
                    destination,
                    aidlStatus.isOk() ? 1 : 0,
                    static_cast<int>(probeStatus));
            }
        }

        CCEC_LOG(LOG_DEBUG,
            "HDMICecAidlHAL::emulateAckForPollFrames destination=0x%X not present. Returning no-ack.\r\n",
            (buf != NULL) ? (buf[0] & 0x0F) : 0xFF);
        throw CECNoAckException();
    }

    if (static_cast<size_t>(len) > kAidlMaxCecFrameSize) {
        CCEC_LOG(LOG_EXP,
            "HDMICecAidlHAL::emulateAckForPollFrames blocking unsupported CEC frame length=%zu on AIDL backend (valid range: 2..16).\r\n",
            static_cast<size_t>(len));
        throw IOException();
    }

    return false;
}
