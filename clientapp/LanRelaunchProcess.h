/*
 * LanRelaunchProcess.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "../lib/modding/LanRelaunchTokenFormat.h"

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
	return LanRelaunchTokenFormat::isValid(token);
}

#if !defined(VCMI_MOBILE)
/// When the running binary is inside a temporary AppImage mount, restart the
/// original AppImage. The next process then has its own mount after this one exits.
inline std::filesystem::path executableForRelaunch(
	const std::filesystem::path & executable, const char * appImage, const char * appDir)
{
	const auto absolute = std::filesystem::absolute(executable).lexically_normal();
#if defined(__linux__)
	if(appImage && *appImage && appDir && *appDir)
	{
		const std::filesystem::path image(appImage);
		const std::filesystem::path mount(appDir);
		if(image.is_absolute() && mount.is_absolute())
		{
			const auto relative = absolute.lexically_relative(mount.lexically_normal());
			if(!relative.empty() && relative != "." && *relative.begin() != std::filesystem::path(".."))
				return image.lexically_normal();
		}
	}
#endif
	return absolute;
}

/// A successful return means the replacement was started, not that it finished initialization.
inline std::uint64_t spawnImpl(const std::filesystem::path & executable, const std::string * token)
{
	if(executable.empty())
		throw std::invalid_argument("LAN relaunch executable path is empty");
	if(token && !isValidToken(*token))
		throw std::invalid_argument("LAN relaunch token must contain 64 lowercase hexadecimal characters");

	#if defined(__linux__)
	const auto absoluteExecutable = executableForRelaunch(executable, std::getenv("APPIMAGE"), std::getenv("APPDIR"));
#else
	const auto absoluteExecutable = std::filesystem::absolute(executable);
#endif
#ifdef _WIN32
	const std::wstring binary = absoluteExecutable.native();
	// Windows executable paths cannot contain double quotes. The token contains only hex digits.
	std::wstring command = L"\"" + binary + L"\"";
	if(token)
	{
		command += L" --lan-relaunch-token ";
		command.append(token->begin(), token->end());
	}
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
	std::string marker = token ? *token : std::string{};
	char * withToken[] = {binary.data(), option.data(), marker.data(), nullptr};
	char * noToken[] = {binary.data(), nullptr};
	pid_t pid{};
	const int result = ::posix_spawn(&pid, binary.c_str(), nullptr, nullptr, token ? withToken : noToken, environ);
	if(result != 0)
		throw std::system_error(result, std::generic_category(), "Unable to spawn LAN relaunch client");
	return static_cast<std::uint64_t>(pid);
#endif
}

inline std::uint64_t spawn(const std::filesystem::path & executable, const std::string & token)
{
	return spawnImpl(executable, &token);
}

/// The original mod preset is already restored; pass no one-shot LAN rejoin token.
inline std::uint64_t spawnRestoredPreset(const std::filesystem::path & executable)
{
	return spawnImpl(executable, nullptr);
}

/// Persist the one-shot marker before spawning; restore the player's preset if handoff fails.
/// The caller must release gameplay resources before using this handoff.
template<typename PersistMarker, typename RestorePreset>
inline std::uint64_t spawnWithRollback(const std::filesystem::path & executable,
	PersistMarker && persistMarker, RestorePreset && restorePreset)
{
	try
	{
		const std::string token = persistMarker();
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
