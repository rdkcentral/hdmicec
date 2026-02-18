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

#include <gtest/gtest.h>
#include <iostream>
#include "hdmi_cec_driver_mock.h"
#include "ccec/LibCCEC.hpp"

// Global test environment to set up mocks
class CecTestEnvironment : public ::testing::Environment {
public:
    HdmiCecDriverMock* driverMock;
    
    void SetUp() override {
        // Create and install the driver mock
        driverMock = new HdmiCecDriverMock();
        HdmiCecDriverMock::setInstance(driverMock);
        
        // Initialize the Bus so it's ready for tests
        try {
            LibCCEC::getInstance().init("CEC_TEST");
        } catch (...) {
            // Ignore if already initialized
        }
    }
    
    void TearDown() override {
        // Clean up
        try {
            LibCCEC::getInstance().term();
        } catch (...) {
            // Ignore cleanup errors
        }
        
        delete driverMock;
        HdmiCecDriverMock::setInstance(nullptr);
    }
};

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new CecTestEnvironment);
    return RUN_ALL_TESTS();
}
