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
#include "ccec/Operands.hpp"

using namespace CCEC;

class OperandsTest : public ::testing::Test {};

TEST_F(OperandsTest, PhysicalAddressCreation) {
    PhysicalAddress phy(1, 0, 0, 0);
    EXPECT_NO_THROW({
        phy.toString();
    });
}

TEST_F(OperandsTest, PhysicalAddressComponents) {
    PhysicalAddress phy(1, 2, 3, 4);
    EXPECT_NO_THROW({
        std::string str = phy.toString();
        EXPECT_FALSE(str.empty());
    });
}

TEST_F(OperandsTest, LogicalAddressEnum) {
    LogicalAddress tv = LogicalAddress::TV;
    LogicalAddress playback = LogicalAddress::PLAYBACK_DEVICE_1;
    LogicalAddress unreg = LogicalAddress::UNREGISTERED;
    
    EXPECT_NE(tv, playback);
    EXPECT_NE(tv, unreg);
}
