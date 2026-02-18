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
#include "hdmi_cec_driver_mock.h"
#include "ccec/LibCCEC.hpp"

// Global test environment to set up mocks
class CecTestEnvironment : public ::testing::Environment {
public:
    HdmiCecDriverMock* driverMock;
    
    void SetUp() override {
        std::cout << "[CecTestEnvironment] SetUp START" << std::endl;
        
        // Create and install the driver mock
        std::cout << "[CecTestEnvironment] Creating HdmiCecDriverMock..." << std::endl;
        driverMock = new HdmiCecDriverMock();
        std::cout << "[CecTestEnvironment] Mock created at: " << (void*)driverMock << std::endl;
        
        std::cout << "[CecTestEnvironment] Setting instance..." << std::endl;
        HdmiCecDriverMock::setInstance(driverMock);
        
        std::cout << "[CecTestEnvironment] Verifying getInstance returns mock..." << std::endl;
        HdmiCecDriverMock* verify = HdmiCecDriverMock::getInstance();
        std::cout << "[CecTestEnvironment] getInstance returned: " << (void*)verify << std::endl;
        
        // Initialize the Bus so it's ready for tests
        std::cout << "[CecTestEnvironment] Initializing LibCCEC..." << std::endl;
        try {
            LibCCEC::getInstance().init("CEC_TEST");
            std::cout << "[CecTestEnvironment] LibCCEC initialized successfully" << std::endl;
        } catch (...) {
            std::cout << "[CecTestEnvironment] LibCCEC already initialized (ignored)" << std::endl;
        }
        
        std::cout << "[CecTestEnvironment] SetUp COMPLETE" << std::endl;
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
