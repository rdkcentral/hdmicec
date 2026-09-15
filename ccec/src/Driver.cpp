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
 * One format string with one substitution - the back-end name - so that the emitted line
 * is identical across every selection arm apart from that name.@n
 * This is a test and tooling contract rather than a diagnostic convenience, which is why
 * it is defined once, here, and referenced nowhere else. The functional suites match on
 * it to establish which back-end resolved, the coverage runner greps it once per
 * invocation and treats its absence as a failed invocation, and device-level validation
 * relies on it wherever the concrete back-end headers are out of scope. It is also the
 * only way the selection is observable: no introspection API is added to the Driver
 * interface, deliberately, because that would grow the middleware public surface.
 *
 * @warning Rewording this string breaks every consumer named above. The back-end name is
 *          the only part that varies.
 *
 * @see SELECTED_BACK_END_AIDL
 * @see SELECTED_BACK_END_LEGACY
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
 * This is the single selection point of the middleware. The two back-ends implement the
 * same Driver interface over different transports - DriverImpl over the legacy in-process
 * C ABI, DriverAidlImpl over the out-of-process `com.rdk.hal.hdmicec` AIDL HAL - and both
 * are compiled into the library in every build, for every SOC vendor. Which one is used is
 * decided here, at run time, on the AIDL service's availability and its compatibility -
 * both, not availability alone. A service that is present but whose metadata halcompat
 * rejects selects the legacy back-end exactly as an absent one does, which is why the
 * question this function asks is isServiceAvailable() and not a bare presence test.@n
 * The order of the two steps is load bearing and is not a matter of taste. Both back-ends
 * are constructed first, as function-local statics, and only then is the AIDL one asked
 * whether its service came up. Gating construction on availability would invert that
 * order, and it would also be unsafe: the availability query is the first thing in the
 * process that may touch libbinder, and on the pinned binder stack an unguarded lookup on
 * a platform with no binder driver aborts rather than returning an error. DriverAidlImpl's
 * constructor deliberately touches no binder at all, so constructing it is safe on a
 * legacy-only SOC, and its isServiceAvailable() is documented never to abort, never to
 * block indefinitely and never to propagate an exception.
 *
 * Three arms exist, each logged, because "absent" and "present but not usable" are
 * different platform conditions and validation gates them separately:
 * -# the service is present and compatible, so the AIDL back-end is selected;
 * -# the binder transport is reachable but no compatible service resolved - either none is
 *    registered or the one that is cannot be spoken to compatibly - so the legacy back-end
 *    is selected;
 * -# the binder transport itself is unavailable, so the legacy back-end is selected.
 *
 * The distinction between arms 2 and 3 is read back from the query that drew it, through
 * DriverAidlImpl::unavailabilityReason(). Nothing is asked twice: the query runs the
 * bounded preflight as its own first stage and records which stage declined, so this
 * function reports that record rather than re-deriving it. Re-deriving it would be wrong
 * in two independent ways - it would pay the preflight's context-manager timeout a second
 * time, and a servicemanager appearing or dying between the two calls could make the
 * reported reason name a condition that did not cause the fallback.@n
 * Which specific compatibility rule rejected a service - an empty or "-1" interface hash,
 * an unfrozen development server, or an interface version outside the compiled-against
 * era and major - along with the server's own reported version and hash, is logged by
 * DriverAidlImpl::isServiceAvailable() itself and is deliberately not restated here,
 * because this function cannot observe it and must not invent it.
 *
 * @return Driver& - The selected back-end. Both candidates have static storage duration,
 *                   so the returned reference stays valid for the lifetime of the process.
 *
 * @pre None. Safe on a platform with no binder driver, no service manager and no AIDL HAL.
 * @post Exactly one selected-path line has been emitted, and the choice is fixed for the
 *       lifetime of the process.
 * @warning Called only from the one-time initializer of Driver::getInstance(), which the
 *          language serializes, so no lock is taken here and none is needed. In particular
 *          Driver::instanceMutex is not used: it is declared but never defined anywhere in
 *          the tree, and referencing it would leave the library with an undefined symbol.
 * @warning Neither construction nor the query throws, so this function cannot leave the
 *          enclosing static uninitialized and cannot be re-entered on a later call.
 * @warning Neither back-end's constructor nor its availability query may call
 *          Driver::getInstance(). Doing so would re-enter the very initializer that is
 *          still running, which is undefined behaviour rather than a recoverable error.
 *          Both back-ends reach the incoming frame queue through their own accessor
 *          precisely so that this cannot happen.
 *
 * @see Driver::getInstance()
 * @see DriverAidlImpl::isServiceAvailable()
 * @see DriverAidlImpl::unavailabilityReason()
 */
Driver &resolveBackEnd(void)
{
	/*
	 * Construct-then-query. Both back-ends are always built and always constructible;
	 * nothing above gates whether the AIDL one exists. Declaring the legacy back-end
	 * first also makes it the last destroyed, which matches its role as the fallback.
	 */
	static DriverImpl     legacyBackEnd;
	static DriverAidlImpl aidlBackEnd;

	if (aidlBackEnd.isServiceAvailable()) {
		CCEC_LOG(LOG_INFO, SELECTED_BACK_END_LOG_FORMAT, SELECTED_BACK_END_AIDL);
		return aidlBackEnd;
	}

	/*
	 * Why the AIDL back-end was not selected, taken from the query that decided it.
	 *
	 * The reason is read back rather than reconstructed. Calling
	 * DriverAidlImpl::isBinderPreflightOk() here to work out which arm declined would be
	 * wrong in two independent ways, because isServiceAvailable() already runs that same
	 * bounded preflight as its own first stage. It would pay the context-manager timeout
	 * a second time, doubling the worst case this function adds to LibCCEC::init() on
	 * precisely the platform least able to absorb it; and the second answer need not
	 * match the first, because a servicemanager that starts or dies between the two calls
	 * flips the preflight's verdict, so the reported reason could name a condition that
	 * did not cause the fallback at all.
	 *
	 * The phrases themselves live beside their producer in DriverAidlImpl.cpp, where two
	 * of the three are a contract: the coverage runner and the L2 tier both transcribe
	 * the line this emits, so rewording either phrase breaks them. The wording also stays
	 * deliberately unlike the selected-path line above, so grepping for that literal
	 * yields exactly one hit per process.
	 *
	 * NULL is not expected on this path - isServiceAvailable() records a reason on every
	 * one of its false exits - but it is handled rather than assumed, because a silent
	 * fallback with no reason at all would be the one outcome nobody could diagnose.
	 */
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
