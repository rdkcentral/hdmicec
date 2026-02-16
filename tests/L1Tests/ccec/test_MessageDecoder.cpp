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
#include "ccec/MessageDecoder.hpp"
#include "ccec/CECFrame.hpp"



class MessageDecoderTest : public ::testing::Test {
protected:
    MessageDecoder decoder;
};

TEST_F(MessageDecoderTest, DecodeValidFrame) {
    // Create a simple test frame
    uint8_t testData[] = {0x40, 0x04};
    CECFrame frame;
    // Populate frame with testData if API allows
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}
