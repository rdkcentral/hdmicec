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
#include "osal/Mutex.hpp"
#include <thread>

using namespace CCEC_OSAL;

class MutexTest : public ::testing::Test {
protected:
    Mutex mutex;
};

TEST_F(MutexTest, LockUnlock) {
    EXPECT_NO_THROW({
        mutex.lock();
        mutex.unlock();
    });
}

TEST_F(MutexTest, MultipleLockUnlock) {
    EXPECT_NO_THROW({
        mutex.lock();
        mutex.unlock();
        mutex.lock();
        mutex.unlock();
    });
}

TEST_F(MutexTest, ConcurrentAccess) {
    int sharedCounter = 0;
    const int iterations = 1000;
    
    auto incrementer = [&]() {
        for (int i = 0; i < iterations; ++i) {
            mutex.lock();
            sharedCounter++;
            mutex.unlock();
        }
    };
    
    std::thread t1(incrementer);
    std::thread t2(incrementer);
    
    t1.join();
    t2.join();
    
    EXPECT_EQ(sharedCounter, iterations * 2);
}
