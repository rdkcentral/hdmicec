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
#include "ccec/OpCode.hpp"

using namespace CCEC;

class OpCodeTest : public ::testing::Test {};

TEST_F(OpCodeTest, OpCodeConstants) {
    EXPECT_EQ(OpCode::IMAGE_VIEW_ON, 0x04);
    EXPECT_EQ(OpCode::TEXT_VIEW_ON, 0x0D);
    EXPECT_EQ(OpCode::STANDBY, 0x36);
    EXPECT_EQ(OpCode::ACTIVE_SOURCE, 0x82);
    EXPECT_EQ(OpCode::INACTIVE_SOURCE, 0x9D);
}

TEST_F(OpCodeTest, OpCodeToString) {
    OpCode opcode(OpCode::IMAGE_VIEW_ON);
    EXPECT_NO_THROW({
        std::string name = opcode.toString();
        EXPECT_FALSE(name.empty());
    });
}
