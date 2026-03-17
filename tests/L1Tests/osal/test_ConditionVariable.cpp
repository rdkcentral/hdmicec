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
    
    // Ensure condition starts in reset state
    condVar.reset();
    
    std::thread waiter([&]() {
        condVar.wait();
        notified = true;
    });
    
    // Give thread time to start waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Signal the waiting thread
    condVar.notify();
    
    // Wait for thread to complete
    waiter.join();
    
    EXPECT_TRUE(notified);
}

TEST_F(ConditionVariableTest, TimedWait) {
    long timeout = 100;
    long result = condVar.wait(timeout);
    // Nothing signaled the condition, so the wait timed out → returns 0.
    EXPECT_EQ(result, 0);
}

// When the condition is signaled BEFORE the timeout expires, wait(timeout)
// must return 1 (non-zero, meaning "condition was set, did not time out").
TEST_F(ConditionVariableTest, SignaledBeforeTimeout) {
    condVar.reset();

    std::atomic<long> result{-1};
    std::thread waiter([&]() {
        result = condVar.wait(2000); // wait up to 2000ms
    });

    // Give the waiter thread time to enter the wait, then signal it.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    condVar.notify();

    waiter.join();
    EXPECT_EQ(result.load(), 1L); // signaled before timeout → returns 1
}
