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
#include "osal/Thread.hpp"
#include "osal/Runnable.hpp"
#include <thread>
#include <chrono>

using namespace CCEC_OSAL;

class TestRunnable : public Runnable {
public:
    bool executed = false;
    
    void run() override {
        executed = true;
    }
};

class ThreadTest : public ::testing::Test {};

TEST_F(ThreadTest, DISABLED_ThreadCreation) {
    TestRunnable runnable;
    EXPECT_NO_THROW({
        Thread thread(runnable);
    });
}

TEST_F(ThreadTest, DISABLED_ThreadExecution) {
    TestRunnable runnable;
    Thread thread(runnable);
    
    thread.start();
    
    // Give the thread time to execute and complete since it's detached
    // Use a longer sleep to ensure thread completes before test ends
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    EXPECT_TRUE(runnable.executed);
}

