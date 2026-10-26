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

inline bool sameSha256(std::string_view expected, std::string_view cached)
{
	// Reject names, versions, truncated checksums, and empty placeholders as cache keys.
	if(expected.size() != 64 || cached.size() != 64)
		return false;

	for(std::size_t index = 0; index < 64; ++index)
	{
		const int left = hexDigit(expected[index]);
		const int right = hexDigit(cached[index]);
		if(left < 0 || right < 0 || left != right)
			return false;
	}

	return true;
}
}
