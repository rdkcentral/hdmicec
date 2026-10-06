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


/**
* @defgroup hdmicec
* @{
* @defgroup ccec
* @{
**/


#include "ccec/Driver.hpp"
#include "ccec/Util.hpp"
#include "DriverImpl.hpp"
#include "DriverAidlImpl.hpp"

CCEC_BEGIN_NAMESPACE

namespace {

/**
 * @brief Format of the single line that names the selected HDMI CEC HAL back-end
 *
 * The functional suites, the coverage runner and device-level validation match this line to
 * learn which back-end resolved; the Driver interface has no introspection API.
 *
 * @warning Only the substituted back-end name may vary; rewording the format breaks those consumers.
 * @see SELECTED_BACK_END_AIDL, SELECTED_BACK_END_LEGACY
 */
const char *const SELECTED_BACK_END_LOG_FORMAT =
	"Driver::getInstance : HDMI CEC HAL back-end selected : %s\r\n";

/** @brief Name substituted into SELECTED_BACK_END_LOG_FORMAT for the AIDL back-end. */
const char *const SELECTED_BACK_END_AIDL   = "AIDL";

/** @brief Name substituted into SELECTED_BACK_END_LOG_FORMAT for the legacy back-end. */
const char *const SELECTED_BACK_END_LEGACY = "legacy";

/**
 * @brief Resolves, exactly once, which of the two HDMI CEC HAL back-ends this process uses
 *
 * Both back-ends are constructed first; only then is the AIDL one asked whether a compatible
 * service is available, and otherwise the legacy one is selected and the recorded reason logged.
 *
 * @return Driver& - The selected back-end, valid for the lifetime of the process.
 *
 * @pre None. Safe on a platform with no binder driver, no service manager and no AIDL HAL.
 * @post Exactly one selected-path line is logged and the choice is fixed for the process lifetime.
 * @warning Neither back-end's constructor nor its query may re-enter Driver::getInstance().
 * @see DriverAidlImpl::isServiceAvailable(), DriverAidlImpl::unavailabilityReason()
 */
Driver &resolveBackEnd(void)
{
	// Construct-then-query: both back-ends always exist, and the legacy fallback is declared
	// first so that it is destroyed last.
	static DriverImpl     legacyBackEnd;
	static DriverAidlImpl aidlBackEnd;

	if (aidlBackEnd.isServiceAvailable()) {
		CCEC_LOG(LOG_INFO, SELECTED_BACK_END_LOG_FORMAT, SELECTED_BACK_END_AIDL);
		return aidlBackEnd;
	}

	// The reason is read back from the query that decided it, never re-derived by a second
	// preflight; a missing reason is still logged, as a warning.
	const char *unavailability = aidlBackEnd.unavailabilityReason();

	if (unavailability != NULL) {
		CCEC_LOG(LOG_INFO, "Driver::getInstance : AIDL HDMI CEC service is not usable : %s\r\n", unavailability);
	}
	else {
		CCEC_LOG(LOG_WARN, "Driver::getInstance : AIDL HDMI CEC service is not usable : the availability query declined without recording a reason\r\n");
	}

	CCEC_LOG(LOG_INFO, SELECTED_BACK_END_LOG_FORMAT, SELECTED_BACK_END_LEGACY);
	return legacyBackEnd;
}

} // anonymous namespace

Driver &Driver::getInstance()
{
	static Driver &instance = resolveBackEnd();
	return instance;
}

CCEC_END_NAMESPACE


/** @} */
/** @} */
