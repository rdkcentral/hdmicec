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
#include <gmock/gmock.h>
#include "ccec/Connection.hpp"



class ConnectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Mock or stub hardware dependencies as needed
    }

    void TearDown() override {
        // Cleanup
    }
};

TEST_F(ConnectionTest, ConstructorCreatesConnection) {
    EXPECT_NO_THROW({
        Connection conn(LogicalAddress::UNREGISTERED, false);
        conn.close();  // Manually close to test if this prevents segfault
    });
}

// Note: Actual open/close tests would require hardware mocking
TEST_F(ConnectionTest, DISABLED_OpenConnection) {
    Connection conn(LogicalAddress::UNREGISTERED, false);
    // EXPECT_NO_THROW(conn.open());
}

TEST_F(ConnectionTest, DISABLED_CloseConnection) {
    Connection conn(LogicalAddress::UNREGISTERED, false);
    // EXPECT_NO_THROW(conn.close());
}
