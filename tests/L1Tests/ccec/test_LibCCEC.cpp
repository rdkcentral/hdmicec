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
#include "ccec/LibCCEC.hpp"



class LibCCECTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup
    }

    void TearDown() override {
        // Cleanup
    }
};

TEST_F(LibCCECTest, GetInstanceReturnsSingleton) {
    LibCCEC& instance1 = LibCCEC::getInstance();
    LibCCEC& instance2 = LibCCEC::getInstance();
    EXPECT_EQ(&instance1, &instance2);
}

TEST_F(LibCCECTest, DISABLED_InitializationTest) {
    // Requires hardware/driver mocking
    // LibCCEC::getInstance().init();
}

TEST_F(LibCCECTest, DISABLED_GetLogicalAddress) {
    // Requires hardware/driver mocking
    // int addr = LibCCEC::getInstance().getLogicalAddress(3);
    // EXPECT_GE(addr, 0);
}
