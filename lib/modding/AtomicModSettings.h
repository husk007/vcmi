/*
 * AtomicModSettings.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include "LanRelaunchToken.h"

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace AtomicModSettings
{

#ifndef _WIN32
/// Retry interrupted POSIX syncs; other I/O errors must still stop durable LAN journal writes.
template<typename Sync>
inline bool syncDescriptorWithRetry(int descriptor, Sync && sync)
{
	int result;
	do
	{
		result = sync(descriptor);
	}
	while(result < 0 && errno == EINTR);
	return result == 0;
}
#endif

namespace detail
{
/// Write and sync a staged file before publishing it with the selected replacement policy.
inline bool writeAtomic(const std::filesystem::path & destination, const std::string & serializedConfig, bool replaceExisting)
{
	std::filesystem::path temporary = destination;
	temporary += ".pending.";
	temporary += LanRelaunchToken::generate().substr(0, 32);
	std::FILE * file = nullptr;
	bool ownsTemporary = false;

	try
	{
#ifdef _WIN32
		const HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if(handle == INVALID_HANDLE_VALUE)
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Unable to create temporary mod settings file");

		ownsTemporary = true;
		const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_WRONLY | _O_BINARY);
		if(descriptor < 0)
		{
			const int openError = errno ? errno : EIO;
			CloseHandle(handle);
			throw std::system_error(openError, std::generic_category(), "Unable to attach temporary mod settings file");
		}
		file = _fdopen(descriptor, "wb");
		if(!file)
		{
			const int openError = errno ? errno : EIO;
			_close(descriptor);
			throw std::system_error(openError, std::generic_category(), "Unable to open temporary mod settings stream");
		}
#else
		int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
		flags |= O_CLOEXEC;
#endif
		const int descriptor = ::open(temporary.c_str(), flags, 0600);
		if(descriptor < 0)
			throw std::system_error(errno, std::generic_category(), "Unable to create temporary mod settings file");

		ownsTemporary = true;
		file = ::fdopen(descriptor, "wb");
		if(!file)
		{
			const int openError = errno ? errno : EIO;
			::close(descriptor);
			throw std::system_error(openError, std::generic_category(), "Unable to open temporary mod settings stream");
		}
#endif
		if(!serializedConfig.empty() && std::fwrite(serializedConfig.data(), 1, serializedConfig.size(), file) != serializedConfig.size())
			throw std::system_error(errno ? errno : EIO, std::generic_category(), "Unable to write temporary mod settings file");

		if(std::fflush(file) != 0)
			throw std::system_error(errno ? errno : EIO, std::generic_category(), "Unable to flush temporary mod settings file");

#ifdef _WIN32
		if(_commit(_fileno(file)) != 0)
#else
		if(!syncDescriptorWithRetry(::fileno(file), ::fsync))
#endif
			throw std::system_error(errno ? errno : EIO, std::generic_category(), "Unable to sync temporary mod settings file");

		const int closeError = std::fclose(file);
		file = nullptr;
		if(closeError != 0)
			throw std::system_error(errno ? errno : EIO, std::generic_category(), "Unable to close temporary mod settings file");

#ifdef _WIN32
		const DWORD moveFlags = MOVEFILE_WRITE_THROUGH | (replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0);
		if(!MoveFileExW(temporary.c_str(), destination.c_str(), moveFlags))
		{
			const DWORD error = GetLastError();
			if(!replaceExisting)
			{
				std::error_code statusError;
				const auto status = std::filesystem::symlink_status(destination, statusError);
				if(!statusError && status.type() != std::filesystem::file_type::not_found)
					throw std::filesystem::filesystem_error("Existing LAN recovery backup", destination,
						std::make_error_code(std::errc::file_exists));
			}
			throw std::system_error(static_cast<int>(error), std::system_category(), "Unable to publish mod settings file");
		}
#else
		if(replaceExisting)
			std::filesystem::rename(temporary, destination);
		else
		{
			// A hard link cannot replace a concurrent writer's existing journal.
			std::filesystem::create_hard_link(temporary, destination);
			std::filesystem::remove(temporary);
		}
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

	const bool synced = syncDescriptorWithRetry(directoryFd, ::fsync);
	const bool closed = ::close(directoryFd) == 0;
	return synced && closed;
#else
	return true;
#endif
}
}

inline bool replace(const std::filesystem::path & destination, const std::string & serializedConfig)
{
	return detail::writeAtomic(destination, serializedConfig, true);
}

/// Atomically publish a new recovery journal, refusing to overwrite an existing path.
inline bool createNew(const std::filesystem::path & destination, const std::string & serializedConfig)
{
	return detail::writeAtomic(destination, serializedConfig, false);
}

}
