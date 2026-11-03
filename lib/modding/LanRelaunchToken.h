/*
 * LanRelaunchToken.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include <boost/uuid/random_generator.hpp>

#include <string>

/// Generates a one-shot local relaunch token using operating-system entropy.
namespace LanRelaunchToken
{
inline std::string generate()
{
	boost::uuids::random_generator_pure random;
	constexpr char hex[] = "0123456789abcdef";
	std::string token;
	token.reserve(64);

	for(int i = 0; i < 2; ++i)
	{
		for(const auto byte : random())
		{
			token.push_back(hex[byte >> 4]);
			token.push_back(hex[byte & 0x0f]);
		}
	}

	return token;
}
}
