/*
 * AtomicModSettings.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace AtomicModSettings
{

/// Replaces an existing settings file after flushing the new contents to storage.
/// Returns false if the replacement succeeded but syncing its parent directory failed.
inline bool replace(const std::filesystem::path & destination, const std::string & serializedConfig)
{
	std::filesystem::path temporary = destination;
	temporary += ".pending";
	std::FILE * file = nullptr;
	bool ownsTemporary = false;

	try
	{
#ifdef _WIN32
		const errno_t openError = _wfopen_s(&file, temporary.c_str(), L"wb");
		if(openError != 0)
			throw std::system_error(openError, std::generic_category(), "Unable to open temporary mod settings file");
#else
		file = std::fopen(temporary.c_str(), "wb");
		if(!file)
			throw std::system_error(errno, std::generic_category(), "Unable to open temporary mod settings file");
#endif

		ownsTemporary = true;
		if(!serializedConfig.empty() && std::fwrite(serializedConfig.data(), 1, serializedConfig.size(), file) != serializedConfig.size())
			throw std::system_error(errno ? errno : EIO, std::generic_category(), "Unable to write temporary mod settings file");

		if(std::fflush(file) != 0)
			throw std::system_error(errno ? errno : EIO, std::generic_category(), "Unable to flush temporary mod settings file");

#ifdef _WIN32
		if(_commit(_fileno(file)) != 0)
#else
		if(::fsync(::fileno(file)) != 0)
#endif
			throw std::system_error(errno ? errno : EIO, std::generic_category(), "Unable to sync temporary mod settings file");

		const int closeError = std::fclose(file);
		file = nullptr;
		if(closeError != 0)
			throw std::system_error(errno ? errno : EIO, std::generic_category(), "Unable to close temporary mod settings file");

#ifdef _WIN32
		if(!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Unable to replace mod settings file");
#else
		std::filesystem::rename(temporary, destination);
#endif
	}
	catch(const std::exception &)
	{
		if(file)
			std::fclose(file);
		if(ownsTemporary)
		{
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
		}
		throw;
	}

#ifndef _WIN32
	const std::filesystem::path directory = destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
	int flags = O_RDONLY;
#ifdef O_DIRECTORY
	flags |= O_DIRECTORY;
#endif
	const int directoryFd = ::open(directory.c_str(), flags);
	if(directoryFd < 0)
		return false;

	const bool synced = ::fsync(directoryFd) == 0;
	const bool closed = ::close(directoryFd) == 0;
	return synced && closed;
#else
	return true;
#endif
}

}
