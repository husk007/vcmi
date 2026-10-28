/*
 * LanModRecoveryJournal.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include "AtomicModSettings.h"

#include <filesystem>
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
	const auto backup = pathForSettings(settings);
	// Keep an unresolved recovery snapshot, including a dangling symlink, intact.
	if(std::filesystem::symlink_status(backup).type() != std::filesystem::file_type::not_found)
		throw std::filesystem::filesystem_error("Unresolved LAN recovery backup already exists", backup,
			std::make_error_code(std::errc::file_exists));
	return AtomicModSettings::replace(backup, original);
}

/// Remove the backup only after the normal settings have been restored and synced.
inline bool discard(const std::filesystem::path & settings)
{
	std::error_code error;
	std::filesystem::remove(pathForSettings(settings), error);
	return !error;
}
}
