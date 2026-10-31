/*
 * LanModPackageStage.h, part of VCMI engine
 *
 * Stages verified, user-approved host ZIPs by digest without exposing them to
 * the active mod filesystem. Loading/activating staged mods is a separate step.
 * License: GNU General Public License v2.0 or later.
 */
#pragma once

#include "LanModArchiveCache.h"
#include "../filesystem/CZipLoader.h"
#include "../filesystem/CFilesystemLoader.h"
#include "../filesystem/AdapterLoaders.h"
#include "ModVerificationInfo.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace LanModPackageStage
{
constexpr std::uint64_t maxUnpackedBytes = 512ULL * 1024 * 1024;
constexpr std::uint64_t maxEntryBytes = 128ULL * 1024 * 1024;
constexpr std::size_t maxEntries = 8192;

inline bool isSafeRelativePath(std::string_view value)
{
	if(value.empty() || value.front() == '/' || value.find('\\') != value.npos
		|| value.find(':') != value.npos)
		return false;
	std::size_t start = 0;
	while(start < value.size())
	{
		const auto separator = value.find('/', start);
		const auto part = value.substr(start, separator == value.npos ? value.npos : separator - start);
		if(part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
			return false;
		for(unsigned char ch : part)
			if(ch < 0x20 || ch == 0x7f || ch == '<' || ch == '>' || ch == '|' || ch == '*' || ch == '?')
				return false;
		if(separator == value.npos)
			break;
		start = separator + 1;
	}
	return true;
}

/// Extract into a persistent, versioned LAN-only tree. Never touch userData/Mods
/// (including privately installed mod versions) and never mount content here.
inline std::optional<std::filesystem::path> stageVerifiedArchive(
	const std::filesystem::path & userData, std::string_view rootId, std::string_view digest)
{
	namespace fs = std::filesystem;
	if(rootId.empty() || rootId.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != rootId.npos)
		return std::nullopt;
	const auto cached = LanModArchiveCache::findVerifiedArchive(userData, rootId, digest);
	const auto identity = LanModArchiveCache::archivePath(userData, rootId, digest);
	if(!cached || !identity)
		return std::nullopt;

	const fs::path parent = userData / "lan-mod-staged" / identity->parent_path().filename();
	const fs::path finalRoot = parent / std::string(digest);
	const fs::path modFolder = finalRoot / "Mods" / std::string(rootId);
	std::error_code error;
	if(fs::exists(finalRoot, error))
	{
		// Existing staging may be used for inspection, not auto-activation.
		return !error && fs::is_regular_file(fs::symlink_status(modFolder / "mod.json", error))
			&& !error ? std::optional<fs::path>(modFolder) : std::nullopt;
	}
	if(error)
		return std::nullopt;

	try
	{
		ZipArchive archive{boost::filesystem::path(cached->native())};
		const auto entries = archive.listEntries();
		if(entries.empty() || entries.size() > maxEntries)
			return std::nullopt;

		bool rootDefinition = false;
		std::string packageFolder;
		for(const auto & entry : entries)
		{
			if(entry.name == "mod.json")
				rootDefinition = true;
			const auto slash = entry.name.find('/');
			if(slash != std::string::npos && slash == entry.name.rfind('/')
				&& entry.name.substr(slash + 1) == "mod.json")
			{
				if(!packageFolder.empty() && packageFolder != entry.name.substr(0, slash))
					return std::nullopt;
				packageFolder = entry.name.substr(0, slash);
			}
		}
		if(rootDefinition == !packageFolder.empty())
			return std::nullopt;

		std::uint64_t totalBytes = 0;
		std::set<std::string> names;
		for(const auto & entry : entries)
		{
			const bool directory = !entry.name.empty() && entry.name.back() == '/';
			const std::string_view clean(entry.name.data(), entry.name.size() - (directory ? 1 : 0));
			if(!isSafeRelativePath(clean))
				return std::nullopt;
			if(!packageFolder.empty() && clean != packageFolder
				&& !clean.starts_with(packageFolder + "/"))
				return std::nullopt;
			std::string folded(clean);
			std::transform(folded.begin(), folded.end(), folded.begin(),
				[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
			if(!names.insert(folded).second)
				return std::nullopt;
			const auto unixType = (entry.externalAttributes >> 16) & 0170000U;
			if((entry.madeBy >> 8) == 3 && unixType != 0
				&& unixType != (directory ? 0040000U : 0100000U))
				return std::nullopt; // reject symlinks and special files
			if((directory && entry.uncompressedSize != 0)
				|| entry.uncompressedSize > maxEntryBytes
				|| entry.uncompressedSize > maxUnpackedBytes - totalBytes)
				return std::nullopt;
			totalBytes += entry.uncompressedSize;
		}

		fs::create_directories(parent);
		const fs::path temporary = parent / (".partial-" + LanRelaunchToken::generate());
		if(!fs::create_directory(temporary))
			return std::nullopt;
		try
		{
			const fs::path extractRoot = temporary / "extract";
			fs::create_directory(extractRoot);
			const boost::filesystem::path boostExtractRoot(extractRoot.native());
			for(const auto & entry : entries)
				if(!archive.extractLimited(boostExtractRoot, entry.name, entry.uncompressedSize))
					throw std::runtime_error("Invalid or oversized ZIP entry");
			const fs::path unpacked = packageFolder.empty() ? extractRoot : extractRoot / packageFolder;
			if(!fs::is_regular_file(fs::symlink_status(unpacked / "mod.json")))
				throw std::runtime_error("Missing root mod definition");
			const fs::path destination = temporary / "Mods" / std::string(rootId);
			fs::create_directories(destination.parent_path());
			fs::rename(unpacked, destination);
			if(fs::exists(extractRoot))
				fs::remove_all(extractRoot);
			if(fs::exists(finalRoot))
				throw std::runtime_error("LAN version staging already exists");
			fs::rename(temporary, finalRoot);
			return modFolder;
		}
		catch(...)
		{
			std::error_code cleanup;
			fs::remove_all(temporary, cleanup);
			return std::nullopt;
		}
	}
	catch(const std::exception &)
	{
		return std::nullopt;
	}
}

/**
 * Make approved, digest-selected host mods visible to the normal VCMI mod
 * discovery on a controlled relaunch. Only a root that is completely absent
 * locally can be mounted: overlaying a private version would accidentally
 * mix files missing from the guest archive with the player's own mod.
 *
 * Call after createInitial(), but before ModManager scans mod definitions.
 * No file in the player's Mods directory is changed.
 */
inline bool mountControlledStagedMods(CFilesystemList & initial,
	const std::filesystem::path & userData, const ModArchiveHashes & selected)
{
	std::vector<std::unique_ptr<CFilesystemLoader>> toMount;
	for(const auto & [rootId, digest] : selected)
	{
		if(rootId.empty() || rootId.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != rootId.npos
			|| !ModArchiveDigest::isSha256(digest))
			return false;

		std::string upperId = rootId;
		std::transform(upperId.begin(), upperId.end(), upperId.begin(),
			[](unsigned char c){ return static_cast<char>(std::toupper(c)); });
		const std::string prefix = "MODS/" + upperId + "/";
		const ResourcePath definition(prefix + "MOD", EResType::JSON);
		if(initial.existsResource(definition))
			return false; // Never shadow an installed private root mod.

		const auto staged = stageVerifiedArchive(userData, rootId, digest);
		if(!staged)
			return false;
		auto loader = std::make_unique<CFilesystemLoader>(prefix,
			boost::filesystem::path(staged->native()), 64, true);
		if(!loader->existsResource(definition))
			return false;
		toMount.emplace_back(std::move(loader));
	}
	for(auto & loader : toMount)
		initial.addLoader(std::move(loader), false);
	return true;
}

} // namespace LanModPackageStage
