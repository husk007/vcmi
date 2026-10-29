/*
 * LanRejoinTarget.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include <cstdint>
#include <string>

/// User-selected LAN host preserved in the recovery journal for a controlled client relaunch.
struct LanRejoinTarget
{
	std::string hostname;
	std::uint16_t port = 0;
	bool previewCapability = false;
};
