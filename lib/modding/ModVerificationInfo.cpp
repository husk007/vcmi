/*
 * ModVerificationInfo.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "ModVerificationInfo.h"
#include "ModArchiveDigest.h"
#include "LanModArchiveCache.h"

#include "CModHandler.h"
#include "ModDescription.h"
#include "ModIncompatibility.h"
#include "ModScope.h"

#include "../json/JsonNode.h"
#include "../GameLibrary.h"

JsonNode ModVerificationInfo::jsonSerializeList(const ModCompatibilityInfo & input)
{
	JsonNode output;

	for(const auto & mod : input)
	{
		JsonNode modWriter;
		modWriter["modId"].String() = mod.first;
		modWriter["name"].String() = mod.second.name;
		if (!mod.second.parent.empty())
			modWriter["parent"].String() = mod.second.parent;
		modWriter["version"].String() = mod.second.version.toString();
		output.Vector().push_back(modWriter);
	}

	return output;
}

ModCompatibilityInfo ModVerificationInfo::jsonDeserializeList(const JsonNode & input)
{
	ModCompatibilityInfo output;

	for(const auto & mod : input.Vector())
	{
		ModVerificationInfo info;
		info.version = CModVersion::fromString(mod["version"].String());
		info.name = mod["name"].String();
		info.parent = mod["parent"].String();
		info.checksum = 0;
		info.impactsGameplay = true;

		if(!mod["modId"].isNull())
			output[mod["modId"].String()] = info;
		else
			output[mod["name"].String()] = info;
	}

	return output;
}

std::string ModVerificationInfo::fingerprintHostMods(const ModCompatibilityInfo & input)
{
	std::string representation;
	const auto append = [&representation](const std::string & field)
	{
		representation += std::to_string(field.size()) + ":" + field;
	};
	for(const auto & [modId, mod] : input)
	{
		append(modId);
		append(mod.version.toString());
		append(mod.parent);
		append(std::to_string(mod.checksum));
		append(mod.impactsGameplay ? "1" : "0");
	}
	ModArchiveDigest::Sha256 digest;
	digest.update(representation.data(), representation.size());
	return digest.finish();
}

std::string ModVerificationInfo::fingerprintLanHostOffer(const ModCompatibilityInfo & mods, const ModArchiveManifest & archives)
{
	// The host may change a shared archive while the guest restarts. A mod-list
	// fingerprint alone would approve the new package without a fresh consent.
	std::string representation = "lan-mod-sync-host-offer-v1";
	const auto append = [&representation](const std::string & field)
	{
		representation += std::to_string(field.size()) + ":" + field;
	};
	append(fingerprintHostMods(mods));
	for(const auto & [rootId, archive] : archives)
	{
		append(rootId);
		append(archive.sha256);
		append(std::to_string(archive.bytes));
	}
	ModArchiveDigest::Sha256 digest;
	digest.update(representation.data(), representation.size());
	return digest.finish();
}

ModListVerificationStatus ModVerificationInfo::verifyListAgainstLocalMods(const ModCompatibilityInfo & modList)
{
	ModListVerificationStatus result;

	for(const auto & m : LIBRARY->modh->getActiveMods())
	{
		if(modList.count(m))
			continue;

		// FIXME: for some reason parent mod (e.g. hota) is not present in save mod list even if its submods are
		// workaround for this and exclude such mods from comparison list if submods are found
		bool hasSubmod = false;
		for (const auto & mod : modList)
			hasSubmod = hasSubmod || boost::starts_with(mod.first, m + '.');

		if (hasSubmod)
			continue;

		if (m == ModScope::scopeBuiltin())
			continue;

		if(LIBRARY->modh->getModInfo(m).affectsGameplay())
			result[m] = ModVerificationStatus::EXCESSIVE;
	}

	for(const auto & infoPair : modList)
	{
		auto & remoteModId = infoPair.first;
		auto & remoteModInfo = infoPair.second;

		if (remoteModId == ModScope::scopeBuiltin())
			continue;

		bool modAffectsGameplay = remoteModInfo.impactsGameplay;
		//parent mod affects gameplay if child affects too
		for(const auto & subInfoPair : modList)
			modAffectsGameplay |= (subInfoPair.second.impactsGameplay && subInfoPair.second.parent == remoteModId);

		if(!vstd::contains(LIBRARY->modh->getAllMods(), remoteModId))
		{
			result[remoteModId] = ModVerificationStatus::NOT_INSTALLED;
			continue;
		}

		const auto & localVersion = LIBRARY->modh->getModInfo(remoteModId).getVersion();
		modAffectsGameplay |= LIBRARY->modh->getModInfo(remoteModId).affectsGameplay();

		// skip it. Such mods should only be present in old saves or if mod changed and no longer affects gameplay
		if (!modAffectsGameplay)
			continue;

		if(remoteModInfo.version != localVersion)
		{
			result[remoteModId] = ModVerificationStatus::VERSION_MISMATCH;
			continue;
		}

		if (!vstd::contains(LIBRARY->modh->getActiveMods(), remoteModId))
		{
			result[remoteModId] = ModVerificationStatus::DISABLED;
			continue;
		}

		result[remoteModId] = ModVerificationStatus::FULL_MATCH;
	}

	return result;
}

bool ModSyncPlan::requiresChanges() const
{
	return !downloadOrUpdate.empty() || !reuseCachedArchive.empty() || !activate.empty() || !deactivate.empty();
}

bool ModSyncPlan::matchesResolvedHostMods(const ModCompatibilityInfo & hostMods, const std::vector<TModID> & resolvedMods) const
{
	const std::set<TModID> active(resolvedMods.begin(), resolvedMods.end());
	for(const auto & [modId, info] : hostMods)
		if(info.impactsGameplay && !active.contains(modId))
			return false;
	for(const auto & modId : activate)
		if(!active.contains(modId))
			return false;
	for(const auto & modId : deactivate)
		if(active.contains(modId))
			return false;
	return true;
}

static bool isReservedWindowsLanComponent(std::string component)
{
	std::transform(component.begin(), component.end(), component.begin(), [](unsigned char ch){
		return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
	});

	if(component == "con" || component == "prn" || component == "aux" || component == "nul"
		|| component == "conin$" || component == "conout$" || component == "clock$")
		return true;

	return component.size() == 4 && (component.compare(0, 3, "com") == 0 || component.compare(0, 3, "lpt") == 0)
		&& component[3] >= '1' && component[3] <= '9';
}

static bool isValidLanModId(const TModID & modId)
{
	if(modId.empty() || modId.front() == '.' || modId.back() == '.'
		|| modId.find("..") != TModID::npos
		|| modId.find_first_of("<>:\"/\\|?*") != TModID::npos)
		return false;

	if(std::any_of(modId.begin(), modId.end(), [](unsigned char ch){
		return ch < 0x20 || ch == 0x7f || (ch >= 'A' && ch <= 'Z');
	}))
		return false;

	// Every component becomes a directory name when installing nested mods.
	for(size_t begin = 0; begin < modId.size();)
	{
		const size_t end = modId.find('.', begin);
		const std::string component = modId.substr(begin, end - begin);
		if(component.back() == ' ' || isReservedWindowsLanComponent(component))
			return false;
		if(end == TModID::npos)
			break;
		begin = end + 1;
	}
	return true;
}

ModArchiveManifest ModVerificationInfo::buildHostArchiveManifest(
	const ModCompatibilityInfo & activeMods, const std::filesystem::path & userDataPath)
{
	ModArchiveManifest manifest;
	const auto shareDirectory = userDataPath / "lan-mod-shares";
	std::error_code error;
	if(!std::filesystem::is_directory(std::filesystem::symlink_status(shareDirectory, error)) || error)
		return manifest;

	for(const auto & [modId, info] : activeMods)
	{
		if(manifest.size() >= 64)
			break;
		if(modId == ModScope::scopeBuiltin() || !info.parent.empty()
			|| modId.find('.') != TModID::npos || !isValidLanModId(modId))
			continue;

		const auto archive = shareDirectory / (modId + ".zip");
		if(!LanModArchiveCache::isAllowedArchiveSize(archive))
			continue;
		const auto digest = ModArchiveDigest::sha256File(archive, LanModArchiveCache::maxArchiveBytes);
		if(!digest)
			continue;
		const auto size = std::filesystem::file_size(archive, error);
		if(error)
			continue;
		manifest.emplace(modId, ModArchiveManifestEntry{*digest, static_cast<std::uint64_t>(size)});
	}
	return manifest;
}

LanArchivePreview ModVerificationInfo::inspectHostArchiveAvailability(
	const ModListVerificationStatus & statuses, const ModArchiveManifest & hostArchives,
	const std::filesystem::path & userDataPath)
{
	LanArchivePreview availability;
	for(const auto & [modId, status] : statuses)
	{
		if((status != ModVerificationStatus::NOT_INSTALLED && status != ModVerificationStatus::VERSION_MISMATCH)
			|| !isValidLanModId(modId))
			continue;
		const auto rootId = modId.substr(0, modId.find('.'));
		availability.emplace(rootId, LanArchivePreviewState::UNAVAILABLE);
	}

	for(auto & [rootId, state] : availability)
	{
		const auto offered = hostArchives.find(rootId);
		if(offered == hostArchives.end() || !ModArchiveDigest::isSha256(offered->second.sha256)
			|| offered->second.bytes <= 0
			|| static_cast<std::uintmax_t>(offered->second.bytes) > LanModArchiveCache::maxArchiveBytes)
			continue;

		state = LanArchivePreviewState::OFFERED_BY_HOST;
		if(LanModArchiveCache::findVerifiedArchive(userDataPath, rootId, offered->second.sha256))
			state = LanArchivePreviewState::VERIFIED_IN_CACHE;
	}
	return availability;
}

std::uint64_t ModVerificationInfo::estimateUncachedArchiveBytes(
	const LanArchivePreview & availability, const ModArchiveManifest & hostArchives)
{
	std::uint64_t total = 0;
	for(const auto & [rootId, state] : availability)
	{
		if(state != LanArchivePreviewState::OFFERED_BY_HOST)
			continue;
		const auto found = hostArchives.find(rootId);
		if(found != hostArchives.end() && found->second.bytes > 0)
			total += static_cast<std::uint64_t>(found->second.bytes);
	}
	return total;
}

std::optional<ModSyncPlan> ModVerificationInfo::buildInstalledOnlyPlan(const ModListVerificationStatus & statuses)
{
	ModSyncPlan plan;
	for(const auto & [modId, status] : statuses)
	{
		if(!isValidLanModId(modId))
			return std::nullopt;
		if(modId == ModScope::scopeBuiltin())
			continue;

		switch(status)
		{
			case ModVerificationStatus::FULL_MATCH:
				break;
			case ModVerificationStatus::DISABLED:
				plan.activate.insert(modId);
				break;
			case ModVerificationStatus::EXCESSIVE:
				plan.deactivate.insert(modId);
				break;
			case ModVerificationStatus::NOT_INSTALLED:
			case ModVerificationStatus::VERSION_MISMATCH:
				return std::nullopt;
			default:
				return std::nullopt;
		}
	}
	return plan;
}

ModSyncPlan ModVerificationInfo::buildSyncPlan(
	const ModListVerificationStatus & statuses,
	const ModArchiveHashes & expectedSha256,
	const ModArchiveHashes & verifiedCachedSha256)
{
	ModSyncPlan plan;

	for(const auto & [modId, status] : statuses)
	{
		if(!isValidLanModId(modId))
			throw std::runtime_error("LAN mod compatibility list contains unsafe mod ID");
		if(modId == ModScope::scopeBuiltin())
			continue;

		const TModID rootId = modId.substr(0, modId.find('.'));

		switch(status)
		{
			case ModVerificationStatus::NOT_INSTALLED:
			case ModVerificationStatus::VERSION_MISMATCH:
			{
				const auto expected = expectedSha256.find(rootId);
				if(expected == expectedSha256.end() || !ModArchiveDigest::isSha256(expected->second))
					throw std::runtime_error("LAN mod archive is missing a valid SHA-256: " + rootId);
				const auto cached = verifiedCachedSha256.find(rootId);
				if(cached != verifiedCachedSha256.end()
					&& ModArchiveDigest::sameSha256(expected->second, cached->second))
					plan.reuseCachedArchive.insert(rootId);
				else
					plan.downloadOrUpdate.insert(rootId);

				plan.activate.insert(rootId);
				plan.activate.insert(modId);
				break;
			}
			case ModVerificationStatus::DISABLED:
				plan.activate.insert(rootId);
				plan.activate.insert(modId);
				break;
			case ModVerificationStatus::EXCESSIVE:
				plan.deactivate.insert(modId);
				break;
			case ModVerificationStatus::FULL_MATCH:
				break;
			default:
				throw std::runtime_error("LAN mod compatibility list contains unknown mod status");
		}
	}

	for(const auto & rootId : plan.downloadOrUpdate)
		plan.reuseCachedArchive.erase(rootId);

	// Nested submods also require their intermediate parents. A matching submod
	// protects its parents from an inconsistent excessive-mod status.
	for(const auto & [modId, status] : statuses)
	{
		if(modId == ModScope::scopeBuiltin() || status == ModVerificationStatus::EXCESSIVE)
			continue;

		for(size_t separator = modId.find('.'); separator != std::string::npos; separator = modId.find('.', separator + 1))
		{
			const TModID ancestor = modId.substr(0, separator);
			if(status != ModVerificationStatus::FULL_MATCH)
				plan.activate.insert(ancestor);
			plan.deactivate.erase(ancestor);
		}
	}

	// Required mods and submods must not be deactivated by contradictory statuses.
	for(const auto & modId : plan.activate)
		plan.deactivate.erase(modId);

	return plan;
}

ModSyncPlan ModVerificationInfo::buildSyncPlanFromPersistentCache(
	const ModListVerificationStatus & statuses,
	const ModArchiveHashes & expectedSha256,
	const std::filesystem::path & userDataPath)
{
	// Validate the manifest and identify missing packages before accessing disk.
	const auto missing = buildSyncPlan(statuses, expectedSha256, {});
	if(missing.downloadOrUpdate.empty())
		return missing;

	ModArchiveHashes neededDigests;
	for(const auto & rootId : missing.downloadOrUpdate)
		neededDigests.emplace(rootId, expectedSha256.at(rootId));

	return buildSyncPlan(statuses, expectedSha256,
		LanModArchiveCache::verifiedArchiveDigests(userDataPath, neededDigests));
}
