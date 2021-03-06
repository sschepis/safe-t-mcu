/*
 * This file is part of the TREZOR project, https://trezor.io/
 *
 * Copyright (C) 2014 Pavol Rusnak <stick@satoshilabs.com>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "protect.h"
#include "storage.h"
#include "memory.h"
#include "messages.h"
#include "usb.h"
#include "oled.h"
#include "buttons.h"
#include "pinmatrix.h"
#include "fsm.h"
#include "layout2.h"
#include "util.h"
#include "debug.h"
#include "gettext.h"
#include "memzero.h"

#define MAX_WRONG_PINS 15

bool protectAbortedByInitialize = false;

bool protectButton(ButtonRequestType type, bool confirm_only)
{
	ButtonRequest resp;
	bool result = false;
	bool acked = false;
#if DEBUG_LINK
	bool debug_decided = false;
#endif

	memset(&resp, 0, sizeof(ButtonRequest));
	resp.has_code = true;
	resp.code = type;
	usbTiny(1);
	buttonUpdate(); // Clear button state
	msg_write(MessageType_MessageType_ButtonRequest, &resp);

	for (;;) {
		usbPoll();

		// check for ButtonAck
		if (msg_tiny_id == MessageType_MessageType_ButtonAck) {
			msg_tiny_id = 0xFFFF;
			acked = true;
		}

		// button acked - check buttons
		if (acked) {
			usbSleep(5);
			buttonUpdate();
			if (button.YesUp) {
				result = true;
				break;
			}
			if (!confirm_only && button.NoUp) {
				result = false;
				break;
			}
		}

		// check for Cancel / Initialize
		if (msg_tiny_id == MessageType_MessageType_Cancel || msg_tiny_id == MessageType_MessageType_Initialize) {
			if (msg_tiny_id == MessageType_MessageType_Initialize) {
				protectAbortedByInitialize = true;
			}
			msg_tiny_id = 0xFFFF;
			result = false;
			break;
		}

#if DEBUG_LINK
		// check DebugLink
		if (msg_tiny_id == MessageType_MessageType_DebugLinkDecision) {
			msg_tiny_id = 0xFFFF;
			DebugLinkDecision *dld = (DebugLinkDecision *)msg_tiny;
			result = dld->yes_no;
			debug_decided = true;
		}

		if (acked && debug_decided) {
			break;
		}

		if (msg_tiny_id == MessageType_MessageType_DebugLinkGetState) {
			msg_tiny_id = 0xFFFF;
			fsm_msgDebugLinkGetState((DebugLinkGetState *)msg_tiny);
		}
#endif
	}

	usbTiny(0);

	return result;
}

const char *requestPin(PinMatrixRequestType type, const char *text)
{
	PinMatrixRequest resp;
	memset(&resp, 0, sizeof(PinMatrixRequest));
	resp.has_type = true;
	resp.type = type;
	usbTiny(1);
	msg_write(MessageType_MessageType_PinMatrixRequest, &resp);
	pinmatrix_start(text);
	for (;;) {
		usbPoll();
		if (msg_tiny_id == MessageType_MessageType_PinMatrixAck) {
			msg_tiny_id = 0xFFFF;
			PinMatrixAck *pma = (PinMatrixAck *)msg_tiny;
			pinmatrix_done(pma->pin); // convert via pinmatrix
			usbTiny(0);
			return pma->pin;
		}
		if (msg_tiny_id == MessageType_MessageType_Cancel || msg_tiny_id == MessageType_MessageType_Initialize) {
			pinmatrix_done(0);
			if (msg_tiny_id == MessageType_MessageType_Initialize) {
				protectAbortedByInitialize = true;
			}
			msg_tiny_id = 0xFFFF;
			usbTiny(0);
			return 0;
		}
#if DEBUG_LINK
		if (msg_tiny_id == MessageType_MessageType_DebugLinkGetState) {
			msg_tiny_id = 0xFFFF;
			fsm_msgDebugLinkGetState((DebugLinkGetState *)msg_tiny);
		}
#endif
	}
}

#if CRYPTOMEM
static void protectCheckMaxTry(uint32_t attempts) {
	if (attempts > 0)
		return;
#else
static void protectCheckMaxTry(uint32_t wait) {
	if (wait < (1 << MAX_WRONG_PINS))
		return;
#endif
	storage_wipe();
#if CRYPTOMEM
	int remaining_zones = storage_remaining_zones();
	char remaining_zones_str[20];
	if (remaining_zones > 1) {
		// Display : 1 line
		strlcpy(remaining_zones_str, _("x crypto zones left"), sizeof(remaining_zones_str));
		remaining_zones_str[0] = remaining_zones + '0';
	} else if (remaining_zones == 1)
		// Display : 1 line
		strlcpy(remaining_zones_str, _("1 crypto zone left"), sizeof(remaining_zones_str));
	else
		// Display : 1 line
		strlcpy(remaining_zones_str, _("no crypto zone left"), sizeof(remaining_zones_str));
	// DISPLAY : 6 lines
	layoutDialogSplitFormat(&bmp_icon_error, NULL, NULL, NULL,_("Too many wrong PIN attempts. Storage has been wiped.\n%s\nPlease unplug the device."), remaining_zones_str);
#else
	// DISPLAY : 6 lines
	layoutDialogSplit(&bmp_icon_error, NULL, NULL, NULL,_("Too many wrong PIN attempts. Storage has been wiped.\n\nPlease unplug the device."));
#endif
	for (;;) {} // loop forever
}

bool protectPin(bool use_cached)
{
	if (!storage_hasPin() || (use_cached && session_isPinCached())) {
#if CRYPTOMEM
		if (!storage_hasPin()) {
			if (storage_containsPin("")) // send empty PW = default PW
				session_cachePin();
		}
#endif
		return true;
	}
	uint32_t fails = storage_getPinFailsOffset();
#if CRYPTOMEM
	uint32_t attempts = storage_getPinRemainingAttempts();

	protectCheckMaxTry(attempts);
	usbTiny(1);
	if (attempts < PIN_MAX_ATTEMPTS) {
		char attemptstrbuf[20];
		if (attempts == 1)
			// Display : 1 line
			strlcpy(attemptstrbuf, _("only 1 attempt left"), sizeof(attemptstrbuf));
		else {
			// Display : 1 line
			strlcpy(attemptstrbuf, _("   0 attempts left"), sizeof(attemptstrbuf));
			attemptstrbuf[3] = attempts + '0';
		}
		// DISPLAY : 6 lines
		layoutDialogSplitFormat(&bmp_icon_info, NULL, NULL, NULL,_("Wrong PIN entered\n\nPlease wait to continue ...\n\n%s"), attemptstrbuf);
		// wait 5 seconds
		usbSleep(5000);
		if (msg_tiny_id == MessageType_MessageType_Initialize) {
			protectAbortedByInitialize = true;
			msg_tiny_id = 0xFFFF;
			usbTiny(0);
			fsm_sendFailure(FailureType_Failure_PinCancelled, NULL);
			return false;
		}
	}
#else
	uint32_t wait = storage_getPinWait(fails);
	protectCheckMaxTry(wait);
	usbTiny(1);
	while (wait > 0) {
		// convert wait to secstr string
		char secstrbuf[20];
		strlcpy(secstrbuf, _("________0 seconds"), sizeof(secstrbuf));
		char *secstr = secstrbuf + 9;
		uint32_t secs = wait;
		while (secs > 0 && secstr >= secstrbuf) {
			secstr--;
			*secstr = (secs % 10) + '0';
			secs /= 10;
		}
		if (wait == 1) {
			secstrbuf[16] = 0;
		}
		// DISPLAY: 6 lines
		layoutDialogSplitFormat(&bmp_icon_info, NULL, NULL, NULL, _("Wrong PIN entered\n\nPlease wait %s to continue ..."), secstr);
		// wait one second
		usbSleep(1000);
		if (msg_tiny_id == MessageType_MessageType_Initialize) {
			protectAbortedByInitialize = true;
			msg_tiny_id = 0xFFFF;
			usbTiny(0);
			fsm_sendFailure(FailureType_Failure_PinCancelled, NULL);
			return false;
		}
		wait--;
	}
#endif
	usbTiny(0);
	const char *pin;
	// DISPLAY: 1 line
	pin = requestPin(PinMatrixRequestType_PinMatrixRequestType_Current, _("Please enter current PIN:"));
	if (!pin) {
		fsm_sendFailure(FailureType_Failure_PinCancelled, NULL);
		return false;
	}
#if !CRYPTOMEM
	if (!storage_increasePinFails(fails)) {
		fsm_sendFailure(FailureType_Failure_PinInvalid, NULL);
		return false;
	}
#endif
	if (storage_containsPin(pin)) {
		session_cachePin();
		storage_resetPinFails(fails);
		return true;
	} else {
#if CRYPTOMEM
		protectCheckMaxTry(storage_getPinRemainingAttempts());
#else
		protectCheckMaxTry(storage_getPinWait(fails));
#endif
		fsm_sendFailure(FailureType_Failure_PinInvalid, NULL);
		return false;
	}
}

/*
 * Ask user for pin. If confirmed, either set it directly
 * or if a buffer is provided, store in buffer to allow the
 * caller to handle setting the pin
 */
bool protectChangePin(char *changed_pin, size_t changed_pin_size)
{
	static CONFIDENTIAL char pin_compare[17];

	if (changed_pin && changed_pin_size < sizeof(pin_compare))
		return false;
	// DISPLAY: 1 line
	const char *pin = requestPin(PinMatrixRequestType_PinMatrixRequestType_NewFirst, _("Please enter new PIN:"));

	if (!pin) {
		return false;
	}

	strlcpy(pin_compare, pin, sizeof(pin_compare));
	// DISPLAY: 1 line
	pin = requestPin(PinMatrixRequestType_PinMatrixRequestType_NewSecond, _("Please re-enter new PIN:"));

	const bool result = pin && (strncmp(pin_compare, pin, sizeof(pin_compare)) == 0);

	if (result) {
		if (changed_pin) {
			strlcpy( changed_pin, pin, changed_pin_size);
		} else {
			storage_setPin(pin_compare);
			storage_update();
		}
	}

	memzero(pin_compare, sizeof(pin_compare));

	return result;
}

bool protectPassphrase(void)
{
	if (!storage_hasPassphraseProtection() || session_isPassphraseCached()) {
		return true;
	}

	PassphraseRequest resp;
	memset(&resp, 0, sizeof(PassphraseRequest));
	usbTiny(1);
	msg_write(MessageType_MessageType_PassphraseRequest, &resp);
	// DISPLAY: 6 lines
	layoutDialogSplit(&bmp_icon_info, NULL, NULL, NULL,_("Please enter your passphrase using the computer's keyboard."));

	bool result;
	for (;;) {
		usbPoll();
		// TODO: correctly process PassphraseAck with state field set (mismatch => Failure)
		if (msg_tiny_id == MessageType_MessageType_PassphraseAck) {
			msg_tiny_id = 0xFFFF;
			PassphraseAck *ppa = (PassphraseAck *)msg_tiny;
			session_cachePassphrase(ppa->has_passphrase ? ppa->passphrase : "");
			result = true;
			break;
		}
		if (msg_tiny_id == MessageType_MessageType_Cancel || msg_tiny_id == MessageType_MessageType_Initialize) {
			if (msg_tiny_id == MessageType_MessageType_Initialize) {
				protectAbortedByInitialize = true;
			}
			msg_tiny_id = 0xFFFF;
			result = false;
			break;
		}
	}
	usbTiny(0);
	layoutHome();
	return result;
}
