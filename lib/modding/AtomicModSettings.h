/*
 * AtomicModSettings.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace AtomicModSettings
{

/// Replaces a settings file without ever truncating the existing file in place.
inline void replace(const std::filesystem::path & destination, const std::string & serializedConfig)
{
	std::filesystem::path temporary = destination;
	temporary += ".pending";

	try
	{
		std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
		if(!file)
			throw std::runtime_error("Unable to open temporary mod settings file");

		file.write(serializedConfig.data(), static_cast<std::streamsize>(serializedConfig.size()));
		file.flush();
		if(!file)
			throw std::runtime_error("Unable to write temporary mod settings file");

		file.close();
		if(!file)
			throw std::runtime_error("Unable to close temporary mod settings file");

		std::filesystem::rename(temporary, destination);
	}
	catch(const std::exception &)
	{
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		throw;
	}
}

}
