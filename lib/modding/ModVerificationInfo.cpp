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

		if (!vstd::contains(LIBRARY->modh->getActiveMods(), remoteModId))
		{
			result[remoteModId] = ModVerificationStatus::DISABLED;
			continue;
		}

		if(remoteModInfo.version != localVersion)
		{
			result[remoteModId] = ModVerificationStatus::VERSION_MISMATCH;
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

ModSyncPlan ModVerificationInfo::buildSyncPlan(
	const ModListVerificationStatus & statuses,
	const ModArchiveHashes & expectedSha256,
	const ModArchiveHashes & verifiedCachedSha256)
{
	ModSyncPlan plan;

	for(const auto & [modId, status] : statuses)
	{
		if(modId == ModScope::scopeBuiltin())
			continue;

		const TModID rootId = modId.substr(0, modId.find('.'));

		switch(status)
		{
			case ModVerificationStatus::NOT_INSTALLED:
			case ModVerificationStatus::VERSION_MISMATCH:
			{
				const auto expected = expectedSha256.find(rootId);
				const auto cached = verifiedCachedSha256.find(rootId);
				if(expected != expectedSha256.end() && cached != verifiedCachedSha256.end()
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
		}
	}

	for(const auto & rootId : plan.downloadOrUpdate)
		plan.reuseCachedArchive.erase(rootId);

	return plan;
}
