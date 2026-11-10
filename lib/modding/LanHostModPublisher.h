/*
 * LanHostModPublisher.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include "LanModArchiveCache.h"
#include "LanModPackageStage.h"
#include "../filesystem/CZipSaver.h"
#include "../filesystem/COutputStream.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

/// Packs installed root mods of a LAN host into userData/lan-mod-shares, where the host offers them to guests.
/// An archive is rebuilt only when the file list, sizes or modification times of its mod changed.
namespace LanHostModPublisher
{
struct Source
{
	std::string rootId;
	std::filesystem::path directory;
};

/// Original Heroes III archives and installers are never shared.
inline bool isForbiddenFile(const std::filesystem::path & path)
{
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
	static const std::array<std::string, 6> forbidden = {".lod", ".snd", ".vid", ".exe", ".iso", ".cab"};
	return std::find(forbidden.begin(), forbidden.end(), extension) != forbidden.end();
}

struct PackageFile
{
	std::filesystem::path source;
	std::string archiveName;
};

/// Lists the files of a mod package and the fingerprint of their state; logs and returns false if the mod cannot be shared.
inline bool collectFiles(const Source & mod, std::vector<PackageFile> & files, std::string & fingerprint)
{
	namespace fs = std::filesystem;
	std::error_code error;
	if(!fs::is_directory(fs::symlink_status(mod.directory, error)) || error
		|| !fs::is_regular_file(fs::symlink_status(mod.directory / "mod.json", error)))
	{
		logGlobal->warn("LAN mod sharing: mod '%s' has no installed directory with mod.json", mod.rootId);
		return false;
	}

	std::uint64_t totalBytes = 0;
	std::set<std::string> foldedNames;
	std::ostringstream state;
	for(fs::recursive_directory_iterator it(mod.directory, error), end; !error && it != end; it.increment(error))
	{
		const auto status = it->symlink_status(error);
		if(error)
			break;
		const auto relative = it->path().lexically_relative(mod.directory).generic_string();
		if(fs::is_directory(status))
			continue;
		if(!fs::is_regular_file(status))
		{
			logGlobal->warn("LAN mod sharing: mod '%s' contains a link or special file '%s'", mod.rootId, relative);
			return false;
		}
		if(isForbiddenFile(it->path()))
		{
			logGlobal->warn("LAN mod sharing: mod '%s' contains original game data '%s'", mod.rootId, relative);
			return false;
		}
		const auto size = fs::file_size(it->path(), error);
		const auto modified = fs::last_write_time(it->path(), error);
		if(error)
			break;
		std::string folded = relative;
		std::transform(folded.begin(), folded.end(), folded.begin(),
			[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
		const std::string archiveName = mod.rootId + "/" + relative;
		if(!LanModPackageStage::isSafeRelativePath(archiveName) || !foldedNames.insert(folded).second
			|| size > LanModPackageStage::maxEntryBytes || size > LanModPackageStage::maxUnpackedBytes - totalBytes
			|| files.size() >= LanModPackageStage::maxEntries)
		{
			logGlobal->warn("LAN mod sharing: mod '%s' exceeds the LAN package limits at '%s'", mod.rootId, relative);
			return false;
		}
		totalBytes += size;
		files.push_back({it->path(), archiveName});
		// libc++ counts file times in __int128, which streams do not accept.
		const auto modifiedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(modified.time_since_epoch()).count();
		state << relative << '\t' << size << '\t' << static_cast<long long>(modifiedNs) << '\n';
	}
	if(error)
	{
		logGlobal->warn("LAN mod sharing: cannot read mod '%s': %s", mod.rootId, error.message());
		return false;
	}
	// Entry order must not depend on the directory iteration order of the platform.
	std::sort(files.begin(), files.end(), [](const auto & a, const auto & b){ return a.archiveName < b.archiveName; });
	fingerprint = state.str();
	return true;
}

inline std::string readText(const std::filesystem::path & path)
{
	std::ifstream input(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

/// Builds or reuses lan-mod-shares/<root>.zip; returns false if the mod is not shared.
inline bool publish(const Source & mod, const std::filesystem::path & userDataPath, const std::atomic<bool> & cancel)
{
	namespace fs = std::filesystem;
	std::error_code error;
	const fs::path shares = userDataPath / "lan-mod-shares";
	const fs::path archive = shares / (mod.rootId + ".zip");
	const fs::path sourceState = shares / (mod.rootId + ".source");
	std::vector<PackageFile> files;
	std::string fingerprint;
	if(!collectFiles(mod, files, fingerprint))
	{
		// Withdraw an earlier automatic archive; an archive published by hand has no source state.
		if(fs::is_regular_file(fs::symlink_status(sourceState, error)))
		{
			fs::remove(archive, error);
			fs::remove(sourceState, error);
		}
		return false;
	}

	fs::create_directories(shares, error);
	if(error || !fs::is_directory(fs::symlink_status(shares, error)))
	{
		logGlobal->error("LAN mod sharing: cannot create share directory %s", shares.string());
		return false;
	}
	if(fs::is_regular_file(fs::symlink_status(archive, error)) && !fs::is_regular_file(fs::symlink_status(sourceState, error)))
		return true; // published by hand, for example with utils/lan_mod_share.py
	if(fs::is_regular_file(fs::symlink_status(archive, error)) && fs::is_regular_file(fs::symlink_status(sourceState, error))
		&& readText(sourceState) == fingerprint)
		return true;

	const auto started = std::chrono::steady_clock::now();
	const fs::path partial = shares / ("." + mod.rootId + ".part");
	fs::remove(partial, error);
	try
	{
		{
			CZipSaver saver(std::make_shared<CDefaultIOApi>(), boost::filesystem::path(partial.native()));
			std::vector<char> buffer(1024 * 1024);
			for(const auto & file : files)
			{
				if(cancel)
					throw std::runtime_error("cancelled");
				std::ifstream input(file.source, std::ios::binary);
				if(!input)
					throw std::runtime_error("cannot read " + file.source.string());
				auto output = saver.addFile(file.archiveName);
				while(input)
				{
					input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
					if(input.gcount() > 0)
						output->write(reinterpret_cast<const ui8 *>(buffer.data()), input.gcount());
				}
			}
		}
		const auto size = fs::file_size(partial);
		if(size == 0 || size > LanModArchiveCache::maxArchiveBytes)
			throw std::runtime_error("archive exceeds the LAN transfer size limit");
		fs::rename(partial, archive);

		const fs::path partialState = shares / ("." + mod.rootId + ".source.part");
		{
			std::ofstream output(partialState, std::ios::binary | std::ios::trunc);
			output << fingerprint;
			if(!output.flush())
				throw std::runtime_error("cannot write source state");
		}
		fs::rename(partialState, sourceState);
	}
	catch(const std::exception & e)
	{
		fs::remove(partial, error);
		if(!cancel)
			logGlobal->error("LAN mod sharing: cannot pack mod '%s': %s", mod.rootId, e.what());
		return false;
	}
	logGlobal->info("LAN mod sharing: packed mod '%s' (%d files) in %d ms", mod.rootId, files.size(),
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
	return true;
}
}
