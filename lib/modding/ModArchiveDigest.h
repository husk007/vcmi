/*
 * ModArchiveDigest.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include <cstddef>
#include <string_view>

/// Comparison of canonical SHA-256 archive digests supplied by the LAN manifest and verified cache.
namespace ModArchiveDigest
{
inline int hexDigit(char digit)
{
	if(digit >= '0' && digit <= '9')
		return digit - '0';
	if(digit >= 'a' && digit <= 'f')
		return digit - 'a' + 10;
	if(digit >= 'A' && digit <= 'F')
		return digit - 'A' + 10;
	return -1;
}

inline bool isSha256(std::string_view digest)
{
	if(digest.size() != 64)
		return false;

	for(char digit : digest)
		if(hexDigit(digit) < 0)
			return false;

	return true;
}

inline bool sameSha256(std::string_view expected, std::string_view cached)
{
	if(!isSha256(expected) || !isSha256(cached))
		return false;

	for(std::size_t index = 0; index < 64; ++index)
		if(hexDigit(expected[index]) != hexDigit(cached[index]))
			return false;

	return true;
}
}
