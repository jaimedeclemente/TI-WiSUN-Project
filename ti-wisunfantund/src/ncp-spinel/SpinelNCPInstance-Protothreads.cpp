/*
 *
 * Copyright (c) 2016 Nest Labs, Inc.
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Modified by Texas Instruments - 2021
 * 
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include "SpinelNCPInstance.h"
#include "time-utils.h"
#include "assert-macros.h"
#include <syslog.h>
#include <errno.h>
#include "socket-utils.h"
#include <stdexcept>
#include <sys/file.h>
#include "SuperSocket.h"
#include "SpinelNCPTask.h"
#include "spinel-extra.h"

#include "SpinelNCPTaskSendCommand.h"
#include "SpinelNCPTaskScan.h"
#include "SpinelNCPTaskForm.h"
#include "SpinelNCPTaskLeave.h"
#include "SpinelNCPTaskJoin.h"
#include "SpinelNCPTaskWake.h"
#include "SpinelNCPTaskDeepSleep.h"

#include "wisun_config.h"

using namespace nl;
using namespace wpantund;

int
SpinelNCPInstance::vprocess_disabled(int event, va_list args)
{
	EH_BEGIN_SUB(&mSubPT);

	while(!mEnabled) {
		// If the association state is uninitialized, fail early.
		if (get_ncp_state() == UNINITIALIZED) {
			syslog(LOG_NOTICE, "Cannot attempt to sleep until NCP is initialized.");
			EH_EXIT();
		}

		// Wait for any tasks or commands to complete.
		EH_WAIT_UNTIL_WITH_TIMEOUT(
			NCP_DEFAULT_COMMAND_RESPONSE_TIMEOUT,
			mEnabled || !is_busy()
		);

		if (mEnabled) {
			break;
		}

		require(!is_initializing_ncp(), timeout_error);

		reset_tasks(kWPANTUNDStatus_Canceled);

		if ((get_ncp_state() != DEEP_SLEEP) && (get_ncp_state() != FAULT)) {
			start_new_task(boost::shared_ptr<SpinelNCPTask>(new SpinelNCPTaskDeepSleep(this, NilReturn())));

			EH_WAIT_UNTIL_WITH_TIMEOUT(
				NCP_DEFAULT_COMMAND_RESPONSE_TIMEOUT,
				(get_ncp_state() == DEEP_SLEEP) || mTaskQueue.empty()
			);
		}

		// If we didn't enter deep sleep then we need to bail early.
		if ((get_ncp_state() != DEEP_SLEEP) && (get_ncp_state() != FAULT)) {
			if (!ncp_state_is_initializing(get_ncp_state())) {
				get_control_interface().reset();
			}
			EH_EXIT();
		}

		EH_WAIT_UNTIL(!IS_EVENT_FROM_NCP(event));

		EH_REQUIRE_WITHIN(
			NCP_DEEP_SLEEP_TICKLE_TIMEOUT,
			(get_ncp_state() != DEEP_SLEEP)
			|| mEnabled
			|| IS_EVENT_FROM_NCP(event),
			do_deep_sleep_tickle
		);

		continue;

		// Exceptions

do_deep_sleep_tickle:
		// ...Go ahead and reset the NCP and state machine.
		// This will make sure the NCP is in a known state
		// and allow it to recover if the NCP is
		// unresponsive.
		syslog(LOG_WARNING, "DEEP-SLEEP-TICKLE: Resetting NCP . . .");

		// Send a RESET.
		CONTROL_REQUIRE_PREP_TO_SEND_COMMAND_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, timeout_error);
		mOutboundBufferLen = spinel_datatype_pack(GetInstance(this)->mOutboundBuffer, sizeof(GetInstance(this)->mOutboundBuffer), "Ci", 0, SPINEL_CMD_RESET);
		CONTROL_REQUIRE_OUTBOUND_BUFFER_FLUSHED_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, timeout_error);

		mResetIsExpected = true;

		// Wait for the response.
		EH_REQUIRE_WITHIN(
			NCP_DEFAULT_COMMAND_RESPONSE_TIMEOUT,
			event == EVENT_NCP_RESET,
			timeout_error
		);

		continue;

timeout_error:
		EH_EXIT();
	}

	set_ncp_power(true);

	if (ncp_state_is_sleeping(get_ncp_state())) {
		start_new_task(boost::shared_ptr<SpinelNCPTask>(
			new SpinelNCPTaskWake(
				this,
				NilReturn()
			)
		));
	}

	EH_END();
}

int
SpinelNCPInstance::vprocess_resume(int event, va_list args)
{
	Data command;
	bool is_commissioned = false;
	int ret;

	EH_BEGIN_SUB(&mSubPT);

	if (!mIsCommissioned) {
		syslog(LOG_NOTICE, "NCP is NOT commissioned. Cannot resume.");
		EH_EXIT();
	}

	syslog(LOG_NOTICE, "NCP is commissioned. Resuming...");

	// Resume by setting `NET_IF_UP` and `NET_STACK_UP` to `true`

	CONTROL_REQUIRE_PREP_TO_SEND_COMMAND_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);

	command = SpinelPackData(
		SPINEL_FRAME_PACK_CMD_PROP_VALUE_SET(SPINEL_DATATYPE_BOOL_S),
		SPINEL_PROP_NET_IF_UP,
		true
	);

	require(command.size() < sizeof(mOutboundBuffer), on_error);
	memcpy(mOutboundBuffer, command.data(), command.size());
	mOutboundBufferLen = static_cast<spinel_ssize_t>(command.size());
	CONTROL_REQUIRE_OUTBOUND_BUFFER_FLUSHED_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);
	CONTROL_REQUIRE_COMMAND_RESPONSE_WITHIN(NCP_DEFAULT_COMMAND_RESPONSE_TIMEOUT, on_error);
	ret = peek_ncp_callback_status(event, args);
	require_noerr(ret, on_error);

	CONTROL_REQUIRE_PREP_TO_SEND_COMMAND_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);
	command = SpinelPackData(
		SPINEL_FRAME_PACK_CMD_PROP_VALUE_SET(SPINEL_DATATYPE_BOOL_S),
		SPINEL_PROP_NET_STACK_UP,
		true
	);
	require(command.size() < sizeof(mOutboundBuffer), on_error);
	memcpy(mOutboundBuffer, command.data(), command.size());
	mOutboundBufferLen = static_cast<spinel_ssize_t>(command.size());
	CONTROL_REQUIRE_OUTBOUND_BUFFER_FLUSHED_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);
	CONTROL_REQUIRE_COMMAND_RESPONSE_WITHIN(NCP_DEFAULT_COMMAND_RESPONSE_TIMEOUT, on_error);
	ret = peek_ncp_callback_status(event, args);
	require_noerr(ret, on_error);

	EH_EXIT();

on_error:

	syslog(LOG_ERR, "NCP is misbehaving or unresponsive");
	reinitialize_ncp();

	EH_END();
}

int
SpinelNCPInstance::vprocess_associated(int event, va_list args)
{
	// Conditions under which this protothread should be exited gracefully.
	const bool should_exit = !mEnabled
		|| !ncp_state_is_joining_or_joined(get_ncp_state());

	EH_BEGIN_SUB(&mSubPT);

	EH_WAIT_UNTIL_WITH_TIMEOUT(
		NCP_TICKLE_TIMEOUT,
		should_exit || !(IS_EVENT_FROM_NCP(event))
	);

	// This is not a typo. Despite the above "WAIT_UNTIL" looking
	// very similar, it is not the same! DO NOT REMOVE!
	EH_WAIT_UNTIL_WITH_TIMEOUT(
		NCP_TICKLE_TIMEOUT,
		should_exit
	);

	if (eh_did_timeout) {
		syslog(LOG_INFO, "Tickle...");
		CONTROL_REQUIRE_PREP_TO_SEND_COMMAND_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);
		mOutboundBufferLen = spinel_datatype_pack(GetInstance(this)->mOutboundBuffer, sizeof(GetInstance(this)->mOutboundBuffer), "Ci", 0, SPINEL_CMD_NOOP);
		CONTROL_REQUIRE_OUTBOUND_BUFFER_FLUSHED_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);

		CONTROL_REQUIRE_COMMAND_RESPONSE_WITHIN(NCP_DEFAULT_COMMAND_RESPONSE_TIMEOUT, on_error);
		mFailureCount = 0;
	}

	EH_EXIT();

on_error:

	syslog(LOG_ERR, "NCP is misbehaving or unresponsive");
	reinitialize_ncp();

	EH_END();
}

int
SpinelNCPInstance::vprocess_offline(int event, va_list args)
{
	// Conditions under which this protothread should be exited gracefully.
	const bool should_exit = ncp_state_is_interface_up(get_ncp_state())
		|| !mEnabled
		|| (mOutboundBufferLen>0);

	float sleep_timeout = mAutoDeepSleepTimeout;

	if (!mNetworkKey.empty() || (mNetworkKeyIndex != 0)) {
		sleep_timeout += 60;
	}

	EH_BEGIN_SUB(&mSubPT);

	// Wait for auto deep sleep to be turned on, or if there is an exit condition.
	EH_WAIT_UNTIL(should_exit || mAutoDeepSleep);

	// Wait for auto deep sleep to be turned off or for us to be not asleep, or if there is an exit condition
	EH_WAIT_UNTIL(
	   should_exit
	   || !mAutoDeepSleep
	   || !ncp_state_is_sleeping(get_ncp_state())
	);

	// Wait within a timeout for us to enter sleep state, or for auto deep sleep to be turned off, or
	// if we have a command to send to NCP or receive a callback/event from NCP, or if there is an exit condition
	EH_WAIT_UNTIL_WITH_TIMEOUT(
		sleep_timeout,
		should_exit
		|| !mAutoDeepSleep
		|| !mTaskQueue.empty()
		|| (IS_EVENT_FROM_NCP(event))
		|| ncp_state_is_sleeping(get_ncp_state())
	);

	if (eh_did_timeout) {
		start_new_task(boost::shared_ptr<SpinelNCPTask>(new SpinelNCPTaskDeepSleep(this, NilReturn())));
	}

	EH_END();
}

int
SpinelNCPInstance::vprocess_init(int event, va_list args)
{
	int status = 0;

	if (event == EVENT_NCP_RESET) {
		if (mDriverState == INITIALIZING) {
			syslog(LOG_ERR, "Unexpected reset during NCP initialization.");
			mFailureCount++;
			PT_INIT(&mSubPT);
		} else if (mDriverState == INITIALIZING_WAITING_FOR_RESET) {
			mDriverState = INITIALIZING;
		}
	}

	EH_BEGIN_SUB(&mSubPT);

	if (get_ncp_state() == UPGRADING) {
		EH_WAIT_UNTIL(get_upgrade_status() != EINPROGRESS);

		status = get_upgrade_status();

		if (status == 0) {
			syslog(LOG_INFO, "Firmware Update Complete.");
		} else {
			syslog(LOG_ERR, "Firmware Update Failed with Error %d", status);
			mFailureCount++;

			if (mFailureCount > mFailureThreshold) {
				change_ncp_state(FAULT);
			}
		}
	}

	if (get_ncp_state() == FAULT) {
		EH_EXIT();
	}

	syslog(LOG_INFO, "Initializing NCP");

	set_initializing_ncp(true);

	change_ncp_state(UNINITIALIZED);

	set_ncp_power(true);

	remove_ncp_originated_address_prefix_route_entries();

	mNCPRegion = region;
	mNCPModeID = 0;
	mNCPProtocolVersionMajor = 0;
	mNCPProtocolVersionMinor = 0;
	mNCPVersionString = "";
	mNCPInterfaceTypeInt = 0;
	mNCPCCAThresholdInt = ccathreshold;
	mNCPTXPowerInt = txpower;
	mNCPFrequencyDouble = 0;
	mStackUp = false;
	mIfUp = false;
	mConnectedDevices = 0;
	mNumConnectedDevices = 0;
	mMacFilterMode = 0;
	mCh0mhz = 0;
	mCh0khz = 0;
	mChSpacing = 0;
	mBCInterval = bcinterval;
	mUCChFunction = ucchfunction;
	mBCChFunction = bcchfunction;
	mUCDwellInterval = ucdwellinterval;
	mBCDwellInterval = bcdwellinterval;
	mUnicastChList = "";
	mBroadcastChList = "";
	mAsyncChList = "";
	mDodagRouteDest = "";

	mDriverState = INITIALIZING_WAITING_FOR_RESET;

	if (mResetIsExpected) {
		EH_WAIT_UNTIL_WITH_TIMEOUT(NCP_DEFAULT_COMMAND_RESPONSE_TIMEOUT, mResetIsExpected == false);

		if (eh_did_timeout) {
			// mResetIsExpected has been set for too long and we
			// haven't gotten a reset yet. This can prevent us from sleeping.
			// by incrementing the failure count here we will cause
			// another reset to occur in the code below.
			mFailureCount++;
			mResetIsExpected = false;
			syslog(LOG_ERR, "Was waiting for a reset, but we never got one.");
		}
	} else {
		// Backoff delay. Normaly zero. May increase if we are in a reset loop.
		EH_SLEEP_FOR(mRunawayResetBackoffManager.delay_for_unexpected_reset());
	}

	do {
		EH_SLEEP_FOR(0.1);

#ifndef TI_WISUN_FAN
		if (mFailureCount > mFailureThreshold) {
			syslog(LOG_ALERT, "The NCP is misbehaving: Repeatedly unable to initialize NCP. Entering fault state.");
			change_ncp_state(FAULT);
			EH_EXIT();
		}

		if ( mAutoUpdateFirmware
		  && (mFailureCount > (mFailureThreshold - 1))
		  && can_upgrade_firmware()
		) {
			syslog(LOG_ALERT, "The NCP is misbehaving: Attempting a firmware update");
			upgrade_firmware();
			EH_RESTART();
		}

		if ((event != EVENT_NCP_RESET) && (mFailureCount > 0)) {
			syslog(LOG_ERR, "Resetting and trying again... (retry %d)", mFailureCount);

			change_ncp_state(UNINITIALIZED);

			mNetworkKey = Data();
			mNetworkKeyIndex = 0;

			reset_tasks(kWPANTUNDStatus_Canceled);

			// Do a hard reset only on even attempts.
			if ((mFailureCount & 1) == 0) {
				hard_reset_ncp();
			} else {
				CONTROL_REQUIRE_PREP_TO_SEND_COMMAND_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);
				mOutboundBufferLen = spinel_datatype_pack(GetInstance(this)->mOutboundBuffer, sizeof(GetInstance(this)->mOutboundBuffer), "Ci", 0, SPINEL_CMD_RESET);
				CONTROL_REQUIRE_OUTBOUND_BUFFER_FLUSHED_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);
			}

			mDriverState = INITIALIZING_WAITING_FOR_RESET;

			EH_REQUIRE_WITHIN(
				NCP_DEFAULT_RESET_RESPONSE_TIMEOUT,
				event == EVENT_NCP_RESET,
				on_error
			);
		}
#endif

		// This next line causes any resets received after this
		// point to cause the control protothread to be restarted.
		mDriverState = INITIALIZING;

		// Get the protocol version
		CONTROL_REQUIRE_PREP_TO_SEND_COMMAND_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);
		GetInstance(this)->mOutboundBufferLen = spinel_cmd_prop_value_get(GetInstance(this)->mOutboundBuffer, sizeof(GetInstance(this)->mOutboundBuffer), SPINEL_PROP_PROTOCOL_VERSION);
		CONTROL_REQUIRE_OUTBOUND_BUFFER_FLUSHED_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);
#ifndef TI_WISUN_FAN
		/* Known Issue: Junk Char Observed at start of wfantund */		
		CONTROL_REQUIRE_COMMAND_RESPONSE_WITHIN(NCP_DEFAULT_COMMAND_RESPONSE_TIMEOUT, on_error);
#endif

		status = peek_ncp_callback_status(event, args);
		require_noerr(status, on_error);

		if (get_ncp_state() == UNINITIALIZED) {
			// Get the thread state
			CONTROL_REQUIRE_PREP_TO_SEND_COMMAND_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);
			GetInstance(this)->mOutboundBufferLen = spinel_cmd_prop_value_get(GetInstance(this)->mOutboundBuffer, sizeof(GetInstance(this)->mOutboundBuffer), SPINEL_PROP_NET_STACK_UP);
			CONTROL_REQUIRE_OUTBOUND_BUFFER_FLUSHED_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);
			//
			CONTROL_REQUIRE_COMMAND_RESPONSE_WITHIN(NCP_DEFAULT_COMMAND_RESPONSE_TIMEOUT, on_error);
			require(get_ncp_state() != UNINITIALIZED, on_error);
		}

		// If we are "joining" at this point, then we must start over.
		// This will cause a reset to occur.
		require(!ncp_state_is_joining(get_ncp_state()), on_error);

		if (mIsPcapInProgress) {
			CONTROL_REQUIRE_PREP_TO_SEND_COMMAND_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);
			GetInstance(this)->mOutboundBufferLen = spinel_cmd_prop_value_set_uint(GetInstance(this)->mOutboundBuffer, sizeof(GetInstance(this)->mOutboundBuffer), SPINEL_PROP_MAC_RAW_STREAM_ENABLED, 1);
			CONTROL_REQUIRE_OUTBOUND_BUFFER_FLUSHED_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);

			CONTROL_REQUIRE_COMMAND_RESPONSE_WITHIN(NCP_DEFAULT_COMMAND_RESPONSE_TIMEOUT, on_error);
		}

		if (mEnabled) {
			// Refresh our internal copies of the following radio parameters:
			static const struct {
				spinel_prop_key_t property;
				unsigned int capability;     // Use 0 if not tied to any capability.
			} props_to_fetch[] = {
				{ SPINEL_PROP_PROTOCOL_VERSION, 0 },
				{ SPINEL_PROP_NCP_VERSION, 0 },
				{ SPINEL_PROP_INTERFACE_TYPE, 0 },
				{ SPINEL_PROP_HWADDR, 0 },
				{ SPINEL_PROP_PHY_CCA_THRESHOLD, 0 },
				{ SPINEL_PROP_PHY_TX_POWER, 0 },
				{ SPINEL_PROP_PHY_REGION, 0 },
				{ SPINEL_PROP_PHY_MODE_ID, 0 },
				{ SPINEL_PROP_PHY_UNICAST_CHANNEL_LIST, 0 },
				{ SPINEL_PROP_PHY_BROADCAST_CHANNEL_LIST, 0 },
				{ SPINEL_PROP_PHY_ASYNC_CHANNEL_LIST, 0 },
				{ SPINEL_PROP_PHY_CH_SPACING, 0 },
				{ SPINEL_PROP_PHY_CHO_CENTER_FREQ, 0 },
				{ SPINEL_PROP_MAC_15_4_PANID, 0 },
				{ SPINEL_PROP_MAC_UC_DWELL_INTERVAL, 0 },
				{ SPINEL_PROP_MAC_BC_DWELL_INTERVAL, 0 },
				{ SPINEL_PROP_MAC_BC_INTERVAL, 0 },
				{ SPINEL_PROP_MAC_UC_CHANNEL_FUNCTION, 0 },
				{ SPINEL_PROP_MAC_BC_CHANNEL_FUNCTION, 0 },
				{ SPINEL_PROP_MAC_MAC_FILTER_LIST, 0 },
				{ SPINEL_PROP_NET_IF_UP, 0 },
				{ SPINEL_PROP_NET_STACK_UP, 0 },
				{ SPINEL_PROP_NET_ROLE, 0 },
				{ SPINEL_PROP_NET_NETWORK_NAME, 0 },
			};

			for (mSubPTIndex = 0; mSubPTIndex < sizeof(props_to_fetch) / sizeof(props_to_fetch[0]); mSubPTIndex++) {
				if ((props_to_fetch[mSubPTIndex].capability != 0)
					&& !mCapabilities.count(props_to_fetch[mSubPTIndex].capability)
				) {
					continue;
				}

				CONTROL_REQUIRE_PREP_TO_SEND_COMMAND_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);
				GetInstance(this)->mOutboundBufferLen = spinel_cmd_prop_value_get(
					GetInstance(this)->mOutboundBuffer, sizeof(GetInstance(this)->mOutboundBuffer),
					props_to_fetch[mSubPTIndex].property);
				CONTROL_REQUIRE_OUTBOUND_BUFFER_FLUSHED_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);
#ifndef TI_WISUN_FAN
				/* Known Issue: Junk Char Observed at start of wfantund */
				CONTROL_REQUIRE_COMMAND_RESPONSE_WITHIN(NCP_DEFAULT_COMMAND_RESPONSE_TIMEOUT, on_error);
#endif

				status = peek_ncp_callback_status(event, args);

				if (status != 0) {
					syslog(LOG_WARNING, "Unsuccessful fetching property %s from NCP: \"%s\" (%d)",
						spinel_prop_key_to_cstr(props_to_fetch[mSubPTIndex].property),
						spinel_status_to_cstr(static_cast<spinel_status_t>(status)), status);
				}
			}

			// Restore all the saved settings
			for (mSettingsIter = mSettings.begin(); mSettingsIter != mSettings.end(); mSettingsIter++) {

				syslog(LOG_NOTICE, "Restoring property \"%s\" on NCP", mSettingsIter->first.c_str());

				// Skip the settings if capability is not present.
				if ((mSettingsIter->second.mCapability != 0) &&!mCapabilities.count(mSettingsIter->second.mCapability)) {
					continue;
				}

				CONTROL_REQUIRE_PREP_TO_SEND_COMMAND_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);

				if (mSettingsIter->second.mSpinelCommand.size() > sizeof(GetInstance(this)->mOutboundBuffer))
				{
					syslog(LOG_WARNING,
						"Spinel command for restoring property \"%s\" does not fit in outbound buffer (require %d bytes but only %u bytes available)",
						mSettingsIter->first.c_str(),
						(int)mSettingsIter->second.mSpinelCommand.size(),
						(unsigned int)sizeof(GetInstance(this)->mOutboundBuffer)
					);

					continue;
				}

				GetInstance(this)->mOutboundBufferLen = (spinel_ssize_t)mSettingsIter->second.mSpinelCommand.size();
				memcpy(GetInstance(this)->mOutboundBuffer, mSettingsIter->second.mSpinelCommand.data(), mSettingsIter->second.mSpinelCommand.size());

				CONTROL_REQUIRE_OUTBOUND_BUFFER_FLUSHED_WITHIN(NCP_DEFAULT_COMMAND_SEND_TIMEOUT, on_error);
#ifndef TI_WISUN_FAN
				/* Known Issue: Junk Char Observed at start of wfantund */
				CONTROL_REQUIRE_COMMAND_RESPONSE_WITHIN(NCP_DEFAULT_COMMAND_RESPONSE_TIMEOUT, on_error);
#endif

				status = peek_ncp_callback_status(event, args);

				if (status != 0) {
					syslog(LOG_WARNING, "Unsuccessful in restoring property \"%s\" on NCP: \"%s\" (%d)", mSettingsIter->first.c_str(), spinel_status_to_cstr(static_cast<spinel_status_t>(status)), status);
				}
			}
		}

		break;

on_error:
		if (status) {
			syslog(LOG_ERR, "Initialization error: %d", status);
		}
		EH_SLEEP_FOR(0.5);
		mFailureCount++;
	} while (true);

	mIsPcapInProgress = false;
	mFailureCount = 0;
	mResetIsExpected = false;
	mXPANIDWasExplicitlySet = false;
	set_initializing_ncp(false);
	mDriverState = NORMAL_OPERATION;

	syslog(LOG_NOTICE, "Finished initializing NCP");

	EH_END();
}

int
SpinelNCPInstance::vprocess_event(int event, va_list args)
{
	if (get_ncp_state() == FAULT) {
		// We perform no processing in the fault state.
		PT_INIT(&mControlPT);
		return 0;
	}

	while (!mTaskQueue.empty()) {
		va_list tmp;
		boost::shared_ptr<SpinelNCPTask> current_task(mTaskQueue.front());
		va_copy(tmp, args);
		char ret = current_task->vprocess_event(event, tmp);
		va_end(tmp);

		if (ret == PT_ENDED || ret == PT_EXITED) {
			mTaskQueue.pop_front();
			continue;
		}

		break;
	}

	EH_BEGIN();

	EH_SPAWN(&mSubPT, vprocess_init(event, args));

	if (get_ncp_state() == FAULT) {
		EH_EXIT();
	}

	EH_WAIT_UNTIL(mTaskQueue.empty());

	// If we are commissioned and autoResume is enabled
	if (mAutoResume && mEnabled && mIsCommissioned
	    && !ncp_state_is_joining_or_joined(get_ncp_state()) && !ncp_state_is_initializing(get_ncp_state())
	) {
		#if AUTO_RESUME == 1
			syslog(LOG_NOTICE, "AutoResume is enabled. Trying to resume.");
			EH_SPAWN(&mSubPT, vprocess_resume(event, args));
		#endif
	}

	while (1) {
		// Yield for one loop cycle only. This prevents
		// us from entering any endless loops.
		EH_SLEEP_FOR(0);

		if (ncp_state_is_initializing(get_ncp_state())) {
			EH_RESTART();

		} else if (!mEnabled) {
			syslog(LOG_NOTICE, "Driver disabled.");
			EH_SPAWN(&mSubPT, vprocess_disabled(event, args));
			if (mEnabled) {
				syslog(LOG_NOTICE, "Driver enabled - reinitializing NCP.");
				EH_RESTART();
			}

		} else if (ncp_state_is_joining_or_joined(get_ncp_state())) {
			EH_SPAWN(&mSubPT, vprocess_associated(event, args));

		} else if (!ncp_state_is_interface_up(get_ncp_state())) {
			EH_SPAWN(&mSubPT, vprocess_offline(event, args));

		} else {
			syslog(
				LOG_WARNING,
				"Unexpected NCP state %d (%s)",
				get_ncp_state(),
				ncp_state_to_string(get_ncp_state()).c_str()
			);

			// Yield for one main loop cycle without
			// specifying a timeout to avoid high CPU usage
			// cases like this.
			EH_YIELD();
		}

	} // while(1)

	EH_END();
}