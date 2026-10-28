/*
 * LanModRecoveryJournal.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include "AtomicModSettings.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

/// Read a bounded recovery backup through a pinned regular-file handle.
inline std::string load(const std::filesystem::path & settings)
{
	constexpr std::uintmax_t maxBackupSize = 16 * 1024 * 1024;
	const auto backup = pathForSettings(settings);
	std::uintmax_t size = 0;
	int descriptor = -1;
#ifdef _WIN32
	const HANDLE handle = CreateFileW(backup.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if(handle == INVALID_HANDLE_VALUE)
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot open LAN recovery backup");

	BY_HANDLE_FILE_INFORMATION metadata{};
	if(GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &metadata)
		|| (metadata.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
	{
		CloseHandle(handle);
		throw std::runtime_error("LAN recovery backup is not a regular file");
	}
	size = (static_cast<std::uintmax_t>(metadata.nFileSizeHigh) << 32) | metadata.nFileSizeLow;
	if(size == 0 || size > maxBackupSize)
	{
		CloseHandle(handle);
		throw std::runtime_error("LAN recovery backup has an invalid size");
	}
	descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
	if(descriptor < 0)
	{
		CloseHandle(handle);
		throw std::runtime_error("Cannot attach LAN recovery backup stream");
	}
#else
#ifndef O_NOFOLLOW
	throw std::runtime_error("Safe LAN recovery backup reading requires O_NOFOLLOW");
#else
	int flags = O_RDONLY | O_NONBLOCK | O_NOFOLLOW;
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
	descriptor = ::open(backup.c_str(), flags);
	if(descriptor < 0)
		throw std::system_error(errno, std::generic_category(), "Cannot open LAN recovery backup");

	struct stat metadata{};
	if(::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode)
		|| metadata.st_size <= 0 || static_cast<std::uintmax_t>(metadata.st_size) > maxBackupSize)
	{
		::close(descriptor);
		throw std::runtime_error("LAN recovery backup has an invalid type or size");
	}
	size = static_cast<std::uintmax_t>(metadata.st_size);
#endif
#endif
#ifdef _WIN32
	std::FILE * raw = _fdopen(descriptor, "rb");
#else
	std::FILE * raw = ::fdopen(descriptor, "rb");
#endif
	if(!raw)
	{
#ifdef _WIN32
		_close(descriptor);
#else
		::close(descriptor);
#endif
		throw std::runtime_error("Cannot attach LAN recovery backup stream");
	}
	const auto closeFile = [](std::FILE * file) { std::fclose(file); };
	std::unique_ptr<std::FILE, decltype(closeFile)> stream(raw, closeFile);
	std::string saved(static_cast<std::size_t>(size), '\0');
	if(std::fread(saved.data(), 1, saved.size(), stream.get()) != saved.size())
		throw std::runtime_error("Cannot read the complete LAN recovery backup");
	if(std::fgetc(stream.get()) != EOF || std::ferror(stream.get()))
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
