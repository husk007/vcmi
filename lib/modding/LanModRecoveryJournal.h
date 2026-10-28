/*
 * LanModRecoveryJournal.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include "AtomicModSettings.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace LanModRecoveryJournal
{
/// Keep an independent copy of the original settings until the restored preset is durable.
inline std::filesystem::path pathForSettings(const std::filesystem::path & settings)
{
	std::filesystem::path journal = settings;
	journal += ".lan-recovery.json";
	return journal;
}

/// A failed durability acknowledgement must prevent starting a temporary LAN preset.
inline bool persist(const std::filesystem::path & settings, const std::string & original)
{
	return AtomicModSettings::createNew(pathForSettings(settings), original);
}

/// Read a bounded regular backup; never follow a recovery path that is a symlink.
inline std::string load(const std::filesystem::path & settings)
{
	constexpr std::uintmax_t maxBackupSize = 16 * 1024 * 1024;
	const auto backup = pathForSettings(settings);
	const auto status = std::filesystem::symlink_status(backup);
	if(!std::filesystem::is_regular_file(status))
		throw std::runtime_error("LAN recovery backup is not a regular file");

	const auto size = std::filesystem::file_size(backup);
	if(size == 0 || size > maxBackupSize)
		throw std::runtime_error("LAN recovery backup has an invalid size");

	std::ifstream stream(backup, std::ios::binary);
	if(!stream)
		throw std::runtime_error("Cannot open LAN recovery backup");

	std::string saved(static_cast<std::size_t>(size), '\0');
	if(!stream.read(saved.data(), static_cast<std::streamsize>(size)))
		throw std::runtime_error("Cannot read the complete LAN recovery backup");
	if(stream.peek() != std::char_traits<char>::eof() || stream.bad())
		throw std::runtime_error("LAN recovery backup changed during reading");
	return saved;
}

/// Remove the backup only after the normal settings have been restored and synced.
inline bool discard(const std::filesystem::path & settings)
{
	std::error_code error;
	std::filesystem::remove(pathForSettings(settings), error);
	return !error;
}
}
