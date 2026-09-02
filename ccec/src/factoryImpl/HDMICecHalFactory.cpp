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

namespace {
    class HalFactoryUtility {
    public:
        enum class BackendType {
            UNKNOWN,
            LEGACY,
            AIDL
        };

        static BackendType mBackendType;

        static bool isAidlServiceAvailable(const android::String16 &expectedServiceName)
        {
            CCEC_LOG(LOG_INFO, "isAidlServiceAvailable invoked\r\n");

            if (mBackendType == BackendType::AIDL) {
                return true;
            } else if (mBackendType == BackendType::LEGACY) {
                return false;
            }

            if (!isServiceManagerAvailable()) {
                CCEC_LOG(LOG_INFO, "Binder driver not available; falling back to legacy HAL\r\n");
                mBackendType = BackendType::LEGACY;
                return false;
            }

            android::sp<android::IServiceManager> serviceManager = android::defaultServiceManager();
            if (serviceManager == nullptr) {
                CCEC_LOG(LOG_ERROR, "isAidlServiceAvailable failed: IServiceManager unavailable\r\n");
                mBackendType = BackendType::LEGACY;
                return false;
            }

            CCEC_LOG(LOG_INFO, "Successfully obtained IServiceManager\r\n");

            android::Vector<android::String16> services = serviceManager->listServices();
            size_t discoveredServiceCount = 0;
            bool matched = false;

            for (size_t index = 0; index < services.size(); ++index) {
                if (services[index] != mServiceManagerName) {
                    ++discoveredServiceCount;
                }
            }

            CCEC_LOG(LOG_INFO, "isAidlServiceAvailable discovered %zu binder services\r\n", discoveredServiceCount);
            if (discoveredServiceCount == 0) {
                CCEC_LOG(LOG_INFO,
                    "isAidlServiceAvailable found no binder services beyond the ServiceManager entry while searching for '%s'\r\n",
                    android::String8(expectedServiceName).string());
                mBackendType = BackendType::LEGACY;
                return false;
            }

            CCEC_LOG(LOG_INFO,
                "isAidlServiceAvailable inspecting %zu registered binder services for '%s'\r\n",
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
                    "isAidlServiceAvailable discovered binder service[%zu]='%s'\r\n",
                    index,
                    discoveredServiceName.string());
            }

            if (matched) {
                CCEC_LOG(LOG_INFO,
                    "isAidlServiceAvailable found AIDL service '%s'\r\n",
                    android::String8(expectedServiceName).string());
                mBackendType = BackendType::AIDL;
                return true;
            }

            CCEC_LOG(LOG_INFO,
                "isAidlServiceAvailable did not find AIDL service '%s'\r\n",
                android::String8(expectedServiceName).string());
            mBackendType = BackendType::LEGACY;
            return false;
        }
    };
    HalFactoryUtility::BackendType HalFactoryUtility::mBackendType
                                    = HalFactoryUtility::BackendType::UNKNOWN;
}

std::unique_ptr<IHDMICecHal> HDMICecHalFactory::Create()
{
    CCEC_LOG(LOG_INFO, "HDMICecHalFactory::Create invoked\r\n");

    try {
        if (HalFactoryUtility::isAidlServiceAvailable(android::String16(IHdmiCec::serviceName().c_str()))) {
            CCEC_LOG(LOG_INFO, "HDMICecHalFactory: Aidl Service is available — using HDMICecAidlHAL\r\n");
            return std::make_unique<HDMICecAidlHAL>();
        }
    } catch (...) {
        CCEC_LOG(LOG_ERROR, "HDMICecHalFactory: Exception thrown while creating AIDL HAL,\r\n");
    }

    CCEC_LOG(LOG_INFO, "HDMICecHalFactory: Aidl Service is not available — using legacy HDMICecRdkVHAL\r\n");
    return std::make_unique<HDMICecRdkVHAL>();
}

