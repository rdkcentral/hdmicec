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

#ifndef HDMI_CEC_HAL_FACTORY_H
#define HDMI_CEC_HAL_FACTORY_H

#include "IHDMICecHal.h"
#include <memory>

class HDMICecHalFactory {
public:
    static std::unique_ptr<IHDMICecHal> Create();

private:
    enum class BackendType {
        UNKNOWN,
        LEGACY,
        AIDL
    };

    static BackendType mBackendType;
    static bool isAidlServiceAvailable();
};

#endif // HDMI_CEC_HAL_FACTORY_H
