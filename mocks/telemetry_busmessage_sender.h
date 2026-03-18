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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mock stub for telemetry bus message sender
 * 
 * This is a stub for the RDK telemetry system used by HDMI CEC.
 * For L1 tests, we don't actually send telemetry, so this is a no-op.
 */

// Stub implementation - does nothing
#define t2_init(component)
#define t2_event_s(marker, value)
#define t2_event_d(marker, value)
#define t2_event_f(marker, value)

#ifdef __cplusplus
}
#endif
