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
#include "HDMICecHalFactory.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/android/binder.h>
#include <binder/IServiceManager.h>
#include <com/rdk/hal/hdmicec/IHdmiCec.h>
#include <utils/String16.h>
#include <utils/String8.h>
#include <utils/Vector.h>

#include "ServiceManagerCheck.h"
#include "HDMICecAidlHAL.h"
#include "HDMICecRdkVHAL.h"
#include "ccec/Util.hpp"

using namespace com::rdk::hal::hdmicec;

static const android::String16 mServiceManagerName("manager");
HDMICecHalFactory::BackendType HDMICecHalFactory::mBackendType = HDMICecHalFactory::BackendType::UNKNOWN;

bool HDMICecHalFactory::isAidlServiceAvailable()
{
    CCEC_LOG(LOG_INFO, "HDMICecHalFactory::isAidlServiceAvailable invoked\r\n");

    if (mBackendType == HDMICecHalFactory::BackendType::AIDL) {
        return true;
    } else if (mBackendType == HDMICecHalFactory::BackendType::LEGACY) {
        return false;
    }

    if (!isServiceManagerAvailable()) {
        CCEC_LOG(LOG_INFO, "Binder driver not available; assuming legacy HDMI CEC HAL\r\n");
        mBackendType = HDMICecHalFactory::BackendType::LEGACY;
        return false;
    }

    android::sp<android::IServiceManager> serviceManager = android::defaultServiceManager();
    if (serviceManager == nullptr) {
        CCEC_LOG(LOG_ERROR, "HDMICecHalFactory::isAidlServiceAvailable failed: IServiceManager unavailable\r\n");
        mBackendType = HDMICecHalFactory::BackendType::LEGACY;
        return false;
    }

    CCEC_LOG(LOG_INFO, "Successfully obtained IServiceManager\r\n");

    const android::String16 expectedServiceName(IHdmiCec::serviceName().c_str());
    android::Vector<android::String16> services = serviceManager->listServices();
    size_t discoveredServiceCount = 0;
    bool matched = false;

    for (size_t index = 0; index < services.size(); ++index) {
        if (services[index] != mServiceManagerName) {
            ++discoveredServiceCount;
        }
    }

    CCEC_LOG(LOG_INFO, "HDMICecHalFactory::isAidlServiceAvailable discovered %zu binder services\r\n", discoveredServiceCount);
    if (discoveredServiceCount == 0) {
        CCEC_LOG(LOG_INFO,
            "HDMICecHalFactory::isAidlServiceAvailable found no binder services beyond the ServiceManager entry while searching for '%s'\r\n",
            android::String8(expectedServiceName).string());
        mBackendType = HDMICecHalFactory::BackendType::LEGACY;
        return false;
    }

    CCEC_LOG(LOG_INFO,
        "HDMICecHalFactory::isAidlServiceAvailable inspecting %zu registered binder services for '%s'\r\n",
        discoveredServiceCount, android::String8(expectedServiceName).string());

    for (size_t index = 0; index < services.size(); ++index) {
        if (services[index] == mServiceManagerName) {
            continue;
        }

        const android::String8 discoveredServiceName(services[index]);
        if (services[index] == expectedServiceName) {
            matched = true;
        }

        CCEC_LOG(LOG_INFO,
            "HDMICecHalFactory::isAidlServiceAvailable discovered binder service[%zu]='%s'\r\n",
            index,
            discoveredServiceName.string());
    }

    if (matched) {
        CCEC_LOG(LOG_INFO,
            "HDMICecHalFactory::isAidlServiceAvailable found HDMI CEC AIDL service '%s'\r\n",
            android::String8(expectedServiceName).string());
        mBackendType = HDMICecHalFactory::BackendType::AIDL;
        return true;
    }

    CCEC_LOG(LOG_INFO,
        "HDMICecHalFactory::isAidlServiceAvailable did not find HDMI CEC AIDL service '%s'\r\n",
        android::String8(expectedServiceName).string());
    mBackendType = HDMICecHalFactory::BackendType::LEGACY;
    return false;
}

std::unique_ptr<IHDMICecHal> HDMICecHalFactory::Create()
{
    CCEC_LOG(LOG_INFO, "HDMICecHalFactory::Create invoked\r\n");

    if (isAidlServiceAvailable()) {
        CCEC_LOG(LOG_INFO, "HDMICecHalFactory: Aidl Service is available — using HDMICecAidlHAL\r\n");
        return std::make_unique<HDMICecAidlHAL>();
    }

    CCEC_LOG(LOG_INFO, "HDMICecHalFactory: Aidl Service is not available — using legacy HDMICecRdkVHAL\r\n");
    return std::make_unique<HDMICecRdkVHAL>();
}

