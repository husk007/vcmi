/*
 * LanRelaunchSupport.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

/// Defined on platforms where the client can restart itself to load a temporary LAN mod preset.
/// iOS has no such mechanism, so a changed mod set must be prepared manually there.
#if !defined(VCMI_MOBILE) || defined(VCMI_ANDROID)
#define VCMI_LAN_RELAUNCH_SUPPORTED 1
#endif
