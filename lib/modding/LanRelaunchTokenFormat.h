/*
 * LanRelaunchTokenFormat.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include <algorithm>
#include <string_view>

/// Restricts the one-shot marker to the format produced by LanRelaunchToken::generate().
namespace LanRelaunchTokenFormat
{
inline bool isValid(std::string_view token)
{
	return token.size() == 64 && std::all_of(token.begin(), token.end(), [](unsigned char value)
	{
		return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
	});
}
}
