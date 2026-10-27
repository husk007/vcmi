/*
 * LanRelaunchProcess.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#if !defined(VCMI_MOBILE)
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <unistd.h>
#endif
#endif

/// Starts a desktop VCMI client with a previously persisted one-shot session marker.
namespace LanRelaunchProcess
{
inline bool isValidToken(const std::string & token)
{
	return token.size() == 64 && std::all_of(token.begin(), token.end(), [](unsigned char value)
	{
		return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
	});
}

#if !defined(VCMI_MOBILE)
/// A successful return means the replacement was started, not that it finished initialization.
inline std::uint64_t spawn(const std::filesystem::path & executable, const std::string & token)
{
	if(executable.empty())
		throw std::invalid_argument("LAN relaunch executable path is empty");
	if(!isValidToken(token))
		throw std::invalid_argument("LAN relaunch token must contain 64 lowercase hexadecimal characters");

	const auto absoluteExecutable = std::filesystem::absolute(executable);
#ifdef _WIN32
	const std::wstring binary = absoluteExecutable.native();
	// Windows executable paths cannot contain double quotes. The token contains only hex digits.
	std::wstring command = L"\"" + binary + L"\" --lan-relaunch-token ";
	command.append(token.begin(), token.end());
	std::vector<wchar_t> writable(command.begin(), command.end());
	writable.push_back(L'\0');

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	if(!CreateProcessW(binary.c_str(), writable.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP,
		nullptr, nullptr, &startup, &process))
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Unable to spawn LAN relaunch client");

	const std::uint64_t pid = process.dwProcessId;
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return pid;
#else
	std::string binary = absoluteExecutable.native();
	std::string option = "--lan-relaunch-token";
	std::string marker = token;
	char * arguments[] = {binary.data(), option.data(), marker.data(), nullptr};
	pid_t pid{};
	const int result = ::posix_spawn(&pid, binary.c_str(), nullptr, nullptr, arguments, environ);
	if(result != 0)
		throw std::system_error(result, std::generic_category(), "Unable to spawn LAN relaunch client");
	return static_cast<std::uint64_t>(pid);
#endif
}

/// Persist the one-shot marker before spawning; restore the player's preset if spawn fails.
/// The caller must release gameplay resources before using this handoff.
template<typename PersistMarker, typename RestorePreset>
inline std::uint64_t spawnWithRollback(const std::filesystem::path & executable,
	PersistMarker && persistMarker, RestorePreset && restorePreset)
{
	const std::string token = persistMarker();
	try
	{
		return spawn(executable, token);
	}
	catch(const std::exception & launchError)
	{
		try
		{
			restorePreset();
		}
		catch(const std::exception & restoreError)
		{
			throw std::runtime_error(std::string("LAN client relaunch failed: ") + launchError.what()
				+ "; original preset restoration also failed: " + restoreError.what());
		}
		throw;
	}
}
#endif
}
