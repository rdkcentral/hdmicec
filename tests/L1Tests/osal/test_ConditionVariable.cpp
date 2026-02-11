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
#include "osal/ConditionVariable.hpp"
#include "osal/Mutex.hpp"
#include <thread>
#include <chrono>

using namespace CCEC_OSAL;

class ConditionVariableTest : public ::testing::Test {
protected:
    Mutex mutex;
    ConditionVariable condVar;
};

TEST_F(ConditionVariableTest, NotifyOne) {
    bool notified = false;
    
    std::thread waiter([&]() {
        mutex.lock();
        condVar.wait(mutex);
        notified = true;
        mutex.unlock();
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    mutex.lock();
    condVar.notify();
    mutex.unlock();
    
    waiter.join();
    EXPECT_TRUE(notified);
}

TEST_F(ConditionVariableTest, DISABLED_TimedWait) {
    mutex.lock();
    long timeout = 100;
    bool result = condVar.wait(mutex, timeout);
    mutex.unlock();
    EXPECT_FALSE(result);
}
