/*
 * LanModSyncTest.cpp, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */

#include "StdInc.h"

#include <boost/dll/runtime_symbol_info.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

#if !defined(VCMI_MOBILE) && !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "../../lib/network/NetworkDiscoveryProtocol.h"
#include "../../lib/modding/AtomicModSettings.h"
#include "../../lib/modding/LanModRecoveryJournal.h"
#include "../../lib/modding/LanRelaunchToken.h"
#include "../../lib/modding/LanRelaunchTokenFormat.h"
#include "../../clientapp/LanRelaunchProcess.h"
#include "../../lib/modding/ModArchiveDigest.h"
#include "../../lib/modding/LanModArchiveCache.h"

#include "../../lib/serializer/CMemorySerializer.h"
#include "../../lib/networkPacks/PacksForLobby.h"
#include "../../lib/json/JsonNode.h"
#include "../../lib/modding/ModManager.h"
#include "../../lib/modding/ModVerificationInfo.h"
#include "../../lib/modding/CModHandler.h"
#include "../../lib/modding/ModDescription.h"
#include "../../lib/modding/ModScope.h"
#include "../../lib/GameLibrary.h"

TEST(LanModSyncTest, LanTcpOptInPreviewRoundTripsHostModListWithoutAlteringLegacyPackets)
{
	LobbyQueryState legacyQuery;
	LobbyQueryModSync optInQuery;
	const auto legacyQueryCopy = CMemorySerializer::deepCopy<CPack>(legacyQuery);
	const auto optInQueryCopy = CMemorySerializer::deepCopy<CPack>(optInQuery);
	ASSERT_NE(legacyQueryCopy, nullptr);
	ASSERT_NE(optInQueryCopy, nullptr);
	EXPECT_EQ(dynamic_cast<LobbyQueryModSync *>(legacyQueryCopy.get()), nullptr);
	EXPECT_NE(dynamic_cast<LobbyQueryModSync *>(optInQueryCopy.get()), nullptr);

	ModVerificationInfo mod;
	mod.name = "Host Mod";
	mod.version = CModVersion(1, 2, 3);

	LobbyModsCheck legacyResponse;
	legacyResponse.vcmiVersion = "test-version";
	legacyResponse.hostAccountDisplayName = "Host";
	legacyResponse.participantNames = {"Host", "Guest"};
	legacyResponse.mods.emplace("hostmod", mod);

	LobbyModSyncPreview optInResponse;
	static_cast<LobbyModsCheck &>(optInResponse) = legacyResponse;

	const auto legacyResponseCopy = CMemorySerializer::deepCopy<CPack>(legacyResponse);
	const auto optInResponseCopy = CMemorySerializer::deepCopy<CPack>(optInResponse);
	ASSERT_NE(legacyResponseCopy, nullptr);
	ASSERT_NE(optInResponseCopy, nullptr);
	EXPECT_EQ(dynamic_cast<LobbyModSyncPreview *>(legacyResponseCopy.get()), nullptr);
	const auto * decoded = dynamic_cast<LobbyModSyncPreview *>(optInResponseCopy.get());
	ASSERT_NE(decoded, nullptr);
	EXPECT_EQ(decoded->vcmiVersion, legacyResponse.vcmiVersion);
	EXPECT_EQ(decoded->hostAccountDisplayName, "Host");
	EXPECT_EQ(decoded->participantNames, legacyResponse.participantNames);
	ASSERT_EQ(decoded->mods.size(), 1);
	EXPECT_EQ(decoded->mods.at("hostmod").name, mod.name);
	EXPECT_EQ(decoded->mods.at("hostmod").version, mod.version);
}

TEST(LanModSyncTest, InstalledOnlyHostPlanChangesTemporaryPresetAndRestoresAllPersonalSettings)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["mods"].Vector().emplace_back("personalmod");
	config["presets"]["personal"]["settings"]["personalmod"]["variants"].Bool() = true;
	config["presets"]["personal"]["settings"]["hostmod"]["units"].Bool() = false;
	config["presets"]["personal"]["settings"]["hostmod"]["units.elite"].Bool() = false;
	config["presets"]["other"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = config.toCompactString();

	const ModListVerificationStatus statuses = {
		{"hostmod.units.elite", ModVerificationStatus::DISABLED},
		{"personalmod", ModVerificationStatus::EXCESSIVE},
		{"personalmod.variants", ModVerificationStatus::EXCESSIVE}
	};
	const auto plan = ModVerificationInfo::buildInstalledOnlyPlan(statuses);
	ASSERT_TRUE(plan.has_value());
	LanModSessionState::begin(config);
	LanModSessionState::applyInstalledOnlyPlan(config, *plan);

	const JsonNode & staged = config;
	const auto & temporary = staged["presets"][LanModSessionState::temporaryPresetName()];
	const auto & roots = temporary["mods"].Vector();
	EXPECT_NE(std::find(roots.begin(), roots.end(), JsonNode("hostmod")), roots.end());
	EXPECT_EQ(std::find(roots.begin(), roots.end(), JsonNode("personalmod")), roots.end());
	EXPECT_TRUE(temporary["settings"]["hostmod"]["units"].Bool());
	EXPECT_TRUE(temporary["settings"]["hostmod"]["units.elite"].Bool());
	EXPECT_FALSE(temporary["settings"]["personalmod"]["variants"].Bool());
	EXPECT_EQ(staged["presets"]["personal"]["settings"]["personalmod"]["variants"].Bool(), true);
	EXPECT_EQ(staged["presets"]["other"]["mods"].Vector().size(), 1);

	EXPECT_TRUE(LanModSessionState::restore(config));
	EXPECT_EQ(config.toCompactString(), original);
}

TEST(LanModSyncTest, LanRejoinRestoresSelectedEndpointAndPreservesPersonalPreset)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["settings"]["personalmod"]["submod"].Bool() = true;
	const auto original = config.toCompactString();

	LanModSessionState::begin(config);
	const LanRejoinTarget selected{"192.168.1.42", 3030, true};
	LanModSessionState::setRejoinTarget(config, selected);
	const std::string token(64, 'a');
	LanModSessionState::prepareControlledRelaunch(config, token);

	EXPECT_EQ(LanModSessionState::recoverOnStartup(config, token), LanSessionStartupAction::RESUMED);
	const auto resumed = LanModSessionState::getRejoinTarget(config);
	ASSERT_TRUE(resumed.has_value());
	EXPECT_EQ(resumed->hostname, selected.hostname);
	EXPECT_EQ(resumed->port, selected.port);
	EXPECT_TRUE(resumed->previewCapability);
	EXPECT_TRUE(LanModSessionState::shouldRestoreOnShutdown(config));

	EXPECT_TRUE(LanModSessionState::restore(config));
	EXPECT_FALSE(LanModSessionState::getRejoinTarget(config).has_value());
	EXPECT_EQ(config.toCompactString(), original);
}

TEST(LanModSyncTest, LanRejoinRejectsInvalidHostOrPortWithoutModifyingSession)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	LanModSessionState::begin(config);
	const auto snapshot = config.toCompactString();

	EXPECT_THROW(LanModSessionState::setRejoinTarget(config, {"host\nother", 3030, true}), std::invalid_argument);
	EXPECT_THROW(LanModSessionState::setRejoinTarget(config, {"192.168.1.42", 0, true}), std::invalid_argument);
	EXPECT_EQ(config.toCompactString(), snapshot);
	EXPECT_FALSE(LanModSessionState::getRejoinTarget(config).has_value());
}

TEST(LanModSyncTest, InstalledOnlyPlanRefusesArchiveWorkWithoutModifyingSession)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	LanModSessionState::begin(config);
	const std::string snapshot = config.toCompactString();

	ModSyncPlan plan;
	plan.downloadOrUpdate.insert("newmod");
	EXPECT_THROW(LanModSessionState::applyInstalledOnlyPlan(config, plan), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), snapshot);
}

TEST(LanModSyncTest, LanPreviewPlansInstalledDisabledModsWithoutArchiveTransfer)
{
	const ModListVerificationStatus statuses = {
		{"hostmod", ModVerificationStatus::DISABLED},
		{"personalmod", ModVerificationStatus::EXCESSIVE},
		{"matchingmod", ModVerificationStatus::FULL_MATCH}
	};
	const auto plan = ModVerificationInfo::buildInstalledOnlyPlan(statuses);
	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->activate.size(), 1);
	EXPECT_TRUE(plan->activate.contains("hostmod"));
	EXPECT_EQ(plan->deactivate.size(), 1);
	EXPECT_TRUE(plan->deactivate.contains("personalmod"));
	EXPECT_TRUE(plan->downloadOrUpdate.empty());
	EXPECT_TRUE(plan->reuseCachedArchive.empty());
	EXPECT_TRUE(plan->requiresChanges());
}

TEST(LanModSyncTest, LanPreviewDoesNotOfferLocalOnlyPlanForMissingOrMismatchedMods)
{
	EXPECT_FALSE(ModVerificationInfo::buildInstalledOnlyPlan({
		{"missingmod", ModVerificationStatus::NOT_INSTALLED},
		{"alreadyhere", ModVerificationStatus::DISABLED}
	}).has_value());
	EXPECT_FALSE(ModVerificationInfo::buildInstalledOnlyPlan({
		{"differentversion", ModVerificationStatus::VERSION_MISMATCH}
	}).has_value());
	EXPECT_FALSE(ModVerificationInfo::buildInstalledOnlyPlan({
		{"../unsafe", ModVerificationStatus::DISABLED}
	}).has_value());
}

TEST(LanModSyncTest, LanDiscoveryNegotiatesOptionalPreviewWithoutChangingLegacyPackets)
{
	EXPECT_EQ(LanModDiscovery::replyForProbe(LanModDiscovery::legacyProbe, 3030), "VCMI_SERVER:3030");
	EXPECT_EQ(LanModDiscovery::replyForProbe(LanModDiscovery::previewProbe, 3030),
		"VCMI_SERVER:3030;lanModSync=preview");
	EXPECT_TRUE(LanModDiscovery::replyForProbe("UNKNOWN_PROBE", 3030).empty());

	const auto oldHost = LanModDiscovery::parseReply("VCMI_SERVER:3030");
	ASSERT_TRUE(oldHost.has_value());
	EXPECT_EQ(oldHost->port, 3030);
	EXPECT_FALSE(oldHost->supportsModPreview);

	const auto newHost = LanModDiscovery::parseReply("VCMI_SERVER:3030;lanModSync=preview");
	ASSERT_TRUE(newHost.has_value());
	EXPECT_EQ(newHost->port, 3030);
	EXPECT_TRUE(newHost->supportsModPreview);

	const auto futureHost = LanModDiscovery::parseReply("VCMI_SERVER:3030;otherFeature=v2");
	ASSERT_TRUE(futureHost.has_value());
	EXPECT_FALSE(futureHost->supportsModPreview);
}

TEST(LanModSyncTest, LanDiscoveryKeepsUpgradedPreviewCapabilityAcrossLegacyReplies)
{
	const auto legacy = LanModDiscovery::parseReply("VCMI_SERVER:3030");
	const auto upgraded = LanModDiscovery::parseReply("VCMI_SERVER:3030;lanModSync=preview");
	ASSERT_TRUE(legacy.has_value());
	ASSERT_TRUE(upgraded.has_value());

	bool selectedPreview = legacy->supportsModPreview;
	EXPECT_FALSE(selectedPreview);
	selectedPreview = LanModDiscovery::retainPreviewCapability(selectedPreview, upgraded->supportsModPreview);
	EXPECT_TRUE(selectedPreview);
	selectedPreview = LanModDiscovery::retainPreviewCapability(selectedPreview, legacy->supportsModPreview);
	EXPECT_TRUE(selectedPreview);
}

TEST(LanModSyncTest, LanDiscoveryRejectsMalformedPortWithoutDroppingLegacyCapability)
{
	for(const auto & invalid : {"VCMI_SERVER:", "VCMI_SERVER:0", "VCMI_SERVER:65536",
		"VCMI_SERVER:-1", "VCMI_SERVER:3030oops", "NOT_VCMI_SERVER:3030"})
		EXPECT_FALSE(LanModDiscovery::parseReply(invalid).has_value()) << invalid;
	EXPECT_TRUE(LanModDiscovery::replyForProbe(LanModDiscovery::previewProbe, 0).empty());
}

TEST(LanModSyncTest, InactiveInstalledModWithDifferentVersionRequiresUpdate)
{
	const auto & active = LIBRARY->modh->getActiveMods();
	for(const auto & modId : LIBRARY->modh->getAllMods())
	{
		if(modId == ModScope::scopeBuiltin() || std::find(active.begin(), active.end(), modId) != active.end())
			continue;

		const CModVersion localVersion = LIBRARY->modh->getModInfo(modId).getVersion();
		ModVerificationInfo hostMod;
		hostMod.impactsGameplay = true;
		hostMod.version = CModVersion(localVersion.major == 0 ? 1 : 0, localVersion.minor, localVersion.patch);

		const auto mismatch = ModVerificationInfo::verifyListAgainstLocalMods({{modId, hostMod}});
		ASSERT_TRUE(mismatch.contains(modId));
		EXPECT_EQ(mismatch.at(modId), ModVerificationStatus::VERSION_MISMATCH);

		hostMod.version = localVersion;
		const auto matching = ModVerificationInfo::verifyListAgainstLocalMods({{modId, hostMod}});
		ASSERT_TRUE(matching.contains(modId));
		EXPECT_EQ(matching.at(modId), ModVerificationStatus::DISABLED);
		return;
	}

	GTEST_SKIP() << "Test environment has no installed inactive mod";
}

TEST(LanModSyncTest, PlansMissingDisabledAndExcessModsWithoutUninstall)
{
	const ModListVerificationStatus states = {
		{"hota", ModVerificationStatus::NOT_INSTALLED},
		{"courtyard", ModVerificationStatus::DISABLED},
		{"tides-of-war", ModVerificationStatus::EXCESSIVE},
		{"core", ModVerificationStatus::EXCESSIVE}
	};

	const auto plan = ModVerificationInfo::buildSyncPlan(states, {{"hota", std::string(64, 'a')}}, {});

	EXPECT_TRUE(plan.requiresChanges());
	EXPECT_EQ(plan.downloadOrUpdate, std::set<TModID>({"hota"}));
	EXPECT_EQ(plan.activate, (std::set<TModID>{"hota", "courtyard"}));
	EXPECT_EQ(plan.deactivate, std::set<TModID>({"tides-of-war"}));
	EXPECT_TRUE(plan.reuseCachedArchive.empty());
}

TEST(LanModSyncTest, ReusesPreviouslyDownloadedMatchingArchive)
{
	const std::string hash(64, 'a');
	const ModListVerificationStatus states = {
		{"hota", ModVerificationStatus::NOT_INSTALLED},
		{"hota.units", ModVerificationStatus::VERSION_MISMATCH}
	};

	const auto plan = ModVerificationInfo::buildSyncPlan(states, {{"hota", hash}}, {{"hota", hash}});

	EXPECT_TRUE(plan.downloadOrUpdate.empty());
	EXPECT_EQ(plan.reuseCachedArchive, std::set<TModID>({"hota"}));
	EXPECT_EQ(plan.activate, (std::set<TModID>{"hota", "hota.units"}));
}

TEST(LanModSyncTest, RequiredSubmodKeepsParentActiveWhenStatusListIsIncomplete)
{
	const std::string hash(64, 'b');
	const ModListVerificationStatus states = {
		{"hota", ModVerificationStatus::EXCESSIVE},
		{"hota.units", ModVerificationStatus::VERSION_MISMATCH},
		{"hota.sounds", ModVerificationStatus::EXCESSIVE},
		{"extra", ModVerificationStatus::EXCESSIVE}
	};

	const auto plan = ModVerificationInfo::buildSyncPlan(states, {{"hota", hash}}, {{"hota", hash}});

	EXPECT_TRUE(plan.downloadOrUpdate.empty());
	EXPECT_EQ(plan.reuseCachedArchive, std::set<TModID>({"hota"}));
	EXPECT_EQ(plan.activate, (std::set<TModID>{"hota", "hota.units"}));
	EXPECT_EQ(plan.deactivate, (std::set<TModID>{"extra", "hota.sounds"}));
}

TEST(LanModSyncTest, NestedSubmodsKeepAllAncestorsAvailable)
{
	const std::string hash(64, 'b');
	const ModListVerificationStatus states = {
		{"hota", ModVerificationStatus::EXCESSIVE},
		{"hota.units", ModVerificationStatus::EXCESSIVE},
		{"hota.units.elite", ModVerificationStatus::VERSION_MISMATCH},
		{"hota.music", ModVerificationStatus::EXCESSIVE},
		{"hota.music.tracks", ModVerificationStatus::FULL_MATCH},
		{"extra", ModVerificationStatus::EXCESSIVE}
	};

	const auto plan = ModVerificationInfo::buildSyncPlan(states, {{"hota", hash}}, {{"hota", hash}});

	EXPECT_TRUE(plan.downloadOrUpdate.empty());
	EXPECT_EQ(plan.reuseCachedArchive, std::set<TModID>({"hota"}));
	EXPECT_EQ(plan.activate, (std::set<TModID>{"hota", "hota.units", "hota.units.elite"}));
	EXPECT_EQ(plan.deactivate, std::set<TModID>({"extra"}));
}

TEST(LanModSyncTest, DownloadsChangedArchiveInsteadOfReusingStaleCache)
{
	const ModListVerificationStatus states = {{"hota", ModVerificationStatus::VERSION_MISMATCH}};
	const auto plan = ModVerificationInfo::buildSyncPlan(
		states, {{"hota", std::string(64, 'a')}}, {{"hota", std::string(64, 'b')}});

	EXPECT_EQ(plan.downloadOrUpdate, std::set<TModID>({"hota"}));
	EXPECT_TRUE(plan.reuseCachedArchive.empty());
}

TEST(LanModSyncTest, RejectsMalformedMatchingCachedArchiveDigests)
{
	const ModListVerificationStatus states = {{"hota", ModVerificationStatus::NOT_INSTALLED}};
	const std::vector<std::string> invalidDigests = {
		"identical-version-label",
		std::string(63, 'a'),
		std::string(64, 'g'),
		"sha256:" + std::string(64, 'a')
	};

	for(const auto & digest : invalidDigests)
	{
		EXPECT_FALSE(ModArchiveDigest::sameSha256(digest, digest));
		const ModArchiveHashes invalidManifest = {{"hota", digest}};
		EXPECT_THROW(ModVerificationInfo::buildSyncPlan(states, invalidManifest, invalidManifest), std::runtime_error);
	}
}

TEST(LanModSyncTest, RejectsMissingHostManifestDigestForRequiredArchive)
{
	const ModListVerificationStatus states = {{"hota.units", ModVerificationStatus::NOT_INSTALLED}};
	const ModArchiveHashes unrelatedManifest = {{"other", std::string(64, 'a')}};

	EXPECT_THROW(ModVerificationInfo::buildSyncPlan(states, {}, {}), std::runtime_error);
	EXPECT_THROW(ModVerificationInfo::buildSyncPlan(states, unrelatedManifest, {}), std::runtime_error);
}

TEST(LanModSyncTest, RejectsUnsafeModIdsBeforePlanningTransfersOrPresetChanges)
{
	const std::vector<TModID> unsafeIds = {
		"", ".hota", "hota.", "hota..units", "../hota",
		"hota/units", "hota\\units", "C:payload",
		"hota\nunits", "hota?units", "hota|units",
		std::string("hota\0units", 10),
		"CON", "prn", "Aux", "NUL", "com1", "COM9", "lpt1", "LPT9",
		"conin$", "CONOUT$", "clock$",
		"hota.CON.units", "hota.units.PrN", "hota.com4.elite",
		"hota ", "hota.units ", "hota .units",
		"HotA", "hota.Units", "hota.ABC"
	};
	const std::vector<ModVerificationStatus> states = {
		ModVerificationStatus::NOT_INSTALLED,
		ModVerificationStatus::VERSION_MISMATCH,
		ModVerificationStatus::DISABLED,
		ModVerificationStatus::EXCESSIVE,
		ModVerificationStatus::FULL_MATCH
	};

	for(const auto & modId : unsafeIds)
	{
		for(const auto status : states)
		{
			SCOPED_TRACE(modId);
			const ModListVerificationStatus list = {{modId, status}};
			const ModArchiveHashes manifest = {{modId, std::string(64, 'a')}};
			try
			{
				ModVerificationInfo::buildSyncPlan(list, manifest, {});
				FAIL() << "Unsafe LAN mod ID was accepted";
			}
			catch(const std::runtime_error & e)
			{
				EXPECT_STREQ(e.what(), "LAN mod compatibility list contains unsafe mod ID");
			}
		}
	}
}

TEST(LanModSyncTest, RejectsUnknownCompatibilityStatusInsteadOfSilentlyTreatingItAsCompatible)
{
	const ModListVerificationStatus statuses = {{"hota", static_cast<ModVerificationStatus>(999)}};
	try
	{
		ModVerificationInfo::buildSyncPlan(statuses, {}, {});
		FAIL() << "An unknown LAN compatibility status must fail closed";
	}
	catch(const std::runtime_error & e)
	{
		EXPECT_STREQ(e.what(), "LAN mod compatibility list contains unknown mod status");
	}
}

TEST(LanModSyncTest, KeepsPortableNamesThatOnlyResembleWindowsDevices)
{
	const ModListVerificationStatus states = {
		{"conquest", ModVerificationStatus::EXCESSIVE},
		{"com10", ModVerificationStatus::EXCESSIVE},
		{"hota.company", ModVerificationStatus::EXCESSIVE},
		{"hota.lpt10", ModVerificationStatus::EXCESSIVE}
	};
	const auto plan = ModVerificationInfo::buildSyncPlan(states, {}, {});

	EXPECT_EQ(plan.deactivate, (std::set<TModID>{"conquest", "com10", "hota.company", "hota.lpt10"}));
	EXPECT_TRUE(plan.downloadOrUpdate.empty());
}

TEST(LanModSyncTest, CorruptCachedDigestRequiresVerifiedTransfer)
{
	const ModListVerificationStatus states = {{"hota", ModVerificationStatus::VERSION_MISMATCH}};
	const auto plan = ModVerificationInfo::buildSyncPlan(
		states, {{"hota", std::string(64, 'a')}}, {{"hota", std::string(64, 'g')}});

	EXPECT_EQ(plan.downloadOrUpdate, std::set<TModID>({"hota"}));
	EXPECT_TRUE(plan.reuseCachedArchive.empty());
}

TEST(LanModSyncTest, ReusesValidDigestRegardlessOfHexLetterCase)
{
	const std::string expected = std::string(32, 'a') + std::string(32, 'F');
	const std::string cached = std::string(32, 'A') + std::string(32, 'f');
	const ModListVerificationStatus states = {{"hota", ModVerificationStatus::VERSION_MISMATCH}};

	EXPECT_TRUE(ModArchiveDigest::sameSha256(expected, cached));
	const auto plan = ModVerificationInfo::buildSyncPlan(
		states, {{"hota", expected}}, {{"hota", cached}});
	EXPECT_TRUE(plan.downloadOrUpdate.empty());
	EXPECT_EQ(plan.reuseCachedArchive, std::set<TModID>({"hota"}));
	EXPECT_EQ(plan.activate, std::set<TModID>({"hota"}));
}

TEST(LanModSyncTest, ActivatesAlreadyInstalledModWithoutDownloading)
{
	const ModListVerificationStatus states = {{"hota", ModVerificationStatus::DISABLED}};
	const auto plan = ModVerificationInfo::buildSyncPlan(states, {}, {});

	EXPECT_TRUE(plan.downloadOrUpdate.empty());
	EXPECT_TRUE(plan.reuseCachedArchive.empty());
	EXPECT_EQ(plan.activate, std::set<TModID>({"hota"}));
}

TEST(LanModSyncTest, MatchingModsRequireNoChanges)
{
	const ModListVerificationStatus states = {{"hota", ModVerificationStatus::FULL_MATCH}};
	const auto plan = ModVerificationInfo::buildSyncPlan(states, {}, {});
	EXPECT_FALSE(plan.requiresChanges());
}

TEST(LanModSyncTest, RestoresOriginalPresetAndSubmodSelections)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["mods"].Vector().emplace_back("hota");
	config["presets"]["personal"]["settings"]["hota"]["portraits"].Bool() = false;
	config["presets"]["personal"]["settings"]["hota"]["units"].Bool() = true;
	config["presets"]["other"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	EXPECT_EQ(config["activePreset"].String(), LanModSessionState::temporaryPresetName());
	EXPECT_EQ(config["presets"]["personal"]["settings"]["hota"]["units"].Bool(), true);

	config["presets"][LanModSessionState::temporaryPresetName()]["mods"].Vector().emplace_back("courtyard");
	config["presets"][LanModSessionState::temporaryPresetName()]["settings"]["hota"]["units"].Bool() = false;

	EXPECT_TRUE(LanModSessionState::restore(config));
	EXPECT_EQ(config.toCompactString(), original);
	EXPECT_FALSE(LanModSessionState::restore(config));
}

TEST(LanModSyncTest, PersonalPresetsRemainLockedUntilLanSessionRestoration)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
	const std::string original = config.toCompactString();

	EXPECT_NO_THROW(LanModSessionState::requirePersonalPresetUnlocked(config));
	LanModSessionState::begin(config);
	const std::string pending = config.toCompactString();
	EXPECT_THROW(LanModSessionState::requirePersonalPresetUnlocked(config), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), pending);

	EXPECT_TRUE(LanModSessionState::restore(config));
	EXPECT_EQ(config.toCompactString(), original);
	EXPECT_NO_THROW(LanModSessionState::requirePersonalPresetUnlocked(config));
}

TEST(LanModSyncTest, DoesNotOverwriteSnapshotForNestedSession)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	LanModSessionState::begin(config);
	const std::string snapshot = config["lanSession"].toCompactString();

	EXPECT_THROW(LanModSessionState::begin(config), std::runtime_error);
	EXPECT_EQ(config["lanSession"].toCompactString(), snapshot);
	EXPECT_TRUE(LanModSessionState::restore(config));
}

TEST(LanModSyncTest, RestoresSnapshotAfterSerializedSessionReload)
{
	JsonNode config;
	config["activePreset"].String() = "my_mods";
	config["presets"]["my_mods"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["my_mods"]["settings"]["hota"]["units"].Bool() = false;
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	config["presets"][LanModSessionState::temporaryPresetName()]["settings"]["hota"]["units"].Bool() = true;
	const std::string onDisk = config.toCompactString();

	JsonNode recovered(onDisk.data(), onDisk.size(), "LAN session recovery test");
	EXPECT_TRUE(LanModSessionState::restore(recovered));
	EXPECT_EQ(recovered.toCompactString(), original);
}

TEST(LanModSyncTest, RefusesToRestoreIntoUnrelatedActivePreset)
{
	JsonNode config;
	config["activePreset"].String() = "original";
	config["presets"]["original"]["mods"].Vector().emplace_back("vcmi");
	LanModSessionState::begin(config);
	config["activePreset"].String() = "different";
	const std::string before = config.toCompactString();

	EXPECT_THROW(LanModSessionState::restore(config), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), before);
}

#ifndef _WIN32
TEST(LanModSyncTest, InterruptedPosixSyncRetriesWithoutIgnoringPersistentErrors)
{
	int attempts = 0;
	const bool recovered = AtomicModSettings::syncDescriptorWithRetry(17, [&](int descriptor)
	{
		EXPECT_EQ(descriptor, 17);
		++attempts;
		if(attempts <= 2)
		{
			errno = EINTR;
			return -1;
		}
		return 0;
	});
	EXPECT_TRUE(recovered);
	EXPECT_EQ(attempts, 3);

	int failures = 0;
	const bool synced = AtomicModSettings::syncDescriptorWithRetry(19, [&](int)
	{
		++failures;
		errno = EIO;
		return -1;
	});
	EXPECT_FALSE(synced);
	EXPECT_EQ(failures, 1);
}
#endif

TEST(LanModSyncTest, AtomicWritePreservesSettingsWhenReplacementFails)
{
	const auto testDirectory = std::filesystem::temp_directory_path() /
		("vcmi-lan-atomic-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directory(testDirectory);
	const auto settings = testDirectory / "modSettings.json";

	ASSERT_TRUE(AtomicModSettings::replace(settings, "{\"old\":true}"));
	ASSERT_TRUE(AtomicModSettings::replace(settings, "{\"new\":true}"));
	{
		std::ifstream file(settings, std::ios::binary);
		const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		EXPECT_EQ(content, "{\"new\":true}");
	}
	EXPECT_FALSE(std::filesystem::exists(testDirectory / "modSettings.json.pending"));

	const auto directoryTarget = testDirectory / "do-not-replace";
	std::filesystem::create_directory(directoryTarget);
	EXPECT_THROW(AtomicModSettings::replace(directoryTarget, "unexpected"), std::system_error);
	EXPECT_TRUE(std::filesystem::is_directory(directoryTarget));
	EXPECT_FALSE(std::filesystem::exists(testDirectory / "do-not-replace.pending"));

	// Legacy .pending staging data may belong to an interrupted or parallel writer.
	const auto legacyStage = testDirectory / "modSettings.json.pending";
	std::filesystem::create_directory(legacyStage);
	const auto marker = legacyStage / "keep.txt";
	{
		std::ofstream foreign(marker);
		foreign << "other writer";
	}
	ASSERT_TRUE(AtomicModSettings::replace(settings, "{\"newer\":true}"));
	EXPECT_TRUE(std::filesystem::exists(marker));

	{
		std::ifstream recovered(settings, std::ios::binary);
		const std::string preserved((std::istreambuf_iterator<char>(recovered)), std::istreambuf_iterator<char>());
		EXPECT_EQ(preserved, "{\"newer\":true}");
	}
	for(const auto & entry : std::filesystem::directory_iterator(testDirectory))
	{
		const std::string filename = entry.path().filename().string();
		EXPECT_FALSE(filename.starts_with("modSettings.json.pending.")) << entry.path();
		EXPECT_FALSE(filename.starts_with("do-not-replace.pending.")) << entry.path();
	}

	std::filesystem::remove_all(testDirectory);
}

TEST(LanModSyncTest, IndependentLanRecoveryBackupSurvivesTemporaryPresetChanges)
{
	namespace fs = std::filesystem;
	const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-backup-" + LanRelaunchToken::generate());
	ASSERT_TRUE(fs::create_directories(directory));
	const fs::path settings = directory / "modSettings.json";
	const fs::path backup = LanModRecoveryJournal::pathForSettings(settings);

	JsonNode personal;
	personal["activePreset"].String() = "personal";
	personal["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	personal["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
	personal["presets"]["personal"]["settings"]["hota"]["music"].Bool() = true;
	personal["presets"]["another"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = personal.toCompactString();
	ASSERT_TRUE(AtomicModSettings::replace(settings, original));

	// A journal write failure must leave the original settings intact.
	ASSERT_TRUE(fs::create_directory(backup));
	EXPECT_THROW(LanModRecoveryJournal::persist(settings, original), std::system_error);
	{
		std::ifstream input(settings, std::ios::binary);
		EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), original);
	}
	ASSERT_TRUE(fs::remove(backup));

	ASSERT_TRUE(LanModRecoveryJournal::persist(settings, original));
	LanModSessionState::begin(personal);
	ASSERT_TRUE(AtomicModSettings::replace(settings, personal.toCompactString()));
	personal["presets"][LanModSessionState::temporaryPresetName()]["settings"]["hota"]["units"].Bool() = true;
	ASSERT_TRUE(AtomicModSettings::replace(settings, personal.toCompactString()));
	{
		std::ifstream input(backup, std::ios::binary);
		EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), original);
	}

	ASSERT_TRUE(LanModSessionState::restore(personal));
	ASSERT_TRUE(AtomicModSettings::replace(settings, personal.toCompactString()));
	EXPECT_TRUE(LanModRecoveryJournal::discard(settings));
	EXPECT_FALSE(fs::exists(backup));
	EXPECT_EQ(personal.toCompactString(), original);

	std::error_code cleanupError;
	fs::remove_all(directory, cleanupError);
}

TEST(LanModSyncTest, ExistingIndependentLanRecoveryBackupCannotBeClobbered)
{
	namespace fs = std::filesystem;
	const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-backup-retain-" + LanRelaunchToken::generate());
	ASSERT_TRUE(fs::create_directories(directory));
	const fs::path settings = directory / "modSettings.json";
	const fs::path backup = LanModRecoveryJournal::pathForSettings(settings);

	JsonNode personal;
	personal["activePreset"].String() = "personal";
	personal["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	personal["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
	const std::string original = personal.toCompactString();

	ASSERT_TRUE(LanModRecoveryJournal::persist(settings, original));
	personal["presets"]["personal"]["settings"]["hota"]["units"].Bool() = true;
	EXPECT_THROW(LanModRecoveryJournal::persist(settings, personal.toCompactString()), fs::filesystem_error);
	{
		std::ifstream input(backup, std::ios::binary);
		EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), original);
	}
	ASSERT_TRUE(LanModRecoveryJournal::discard(settings));
	EXPECT_TRUE(LanModRecoveryJournal::persist(settings, personal.toCompactString()));
	ASSERT_TRUE(LanModRecoveryJournal::discard(settings));

#ifndef _WIN32
	fs::create_symlink("missing-target", backup);
	EXPECT_THROW(LanModRecoveryJournal::persist(settings, original), fs::filesystem_error);
	EXPECT_TRUE(fs::is_symlink(fs::symlink_status(backup)));
	EXPECT_FALSE(fs::exists(directory / "missing-target"));
#endif

	std::error_code cleanupError;
	fs::remove_all(directory, cleanupError);
}

TEST(LanModSyncTest, UnrelatedPresetChangesCannotEraseLanRecoveryBackup)
{
	namespace fs = std::filesystem;
	const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-backup-other-preset-" + LanRelaunchToken::generate());
	ASSERT_TRUE(fs::create_directories(directory));
	const fs::path settings = directory / "modSettings.json";
	const fs::path backup = LanModRecoveryJournal::pathForSettings(settings);

	JsonNode personal;
	personal["activePreset"].String() = "personal";
	personal["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	personal["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
	personal["presets"]["other"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = personal.toCompactString();
	ASSERT_TRUE(AtomicModSettings::replace(settings, original));
	ASSERT_TRUE(LanModRecoveryJournal::persist(settings, original));

	LanModSessionState::begin(personal);
	personal["presets"]["other"]["settings"]["hota"]["music"].Bool() = true;
	ASSERT_TRUE(LanModSessionState::restore(personal));
	ASSERT_TRUE(AtomicModSettings::replace(settings, personal.toCompactString()));
	EXPECT_NE(personal.toCompactString(), original);
	EXPECT_FALSE(LanModRecoveryJournal::discardIfMatches(settings, personal.toCompactString()));
	EXPECT_EQ(LanModRecoveryJournal::load(settings), original);
	EXPECT_TRUE(fs::exists(backup));

	ASSERT_TRUE(AtomicModSettings::replace(settings, original));
	EXPECT_TRUE(LanModRecoveryJournal::discardIfMatches(settings, original));
	EXPECT_FALSE(fs::exists(backup));
	std::error_code cleanupError;
	fs::remove_all(directory, cleanupError);
}

TEST(LanModSyncTest, CompleteLanRollbackRestoresUnrelatedPresetAndSubmodFlags)
{
	namespace fs = std::filesystem;
	const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-full-restore-" + LanRelaunchToken::generate());
	ASSERT_TRUE(fs::create_directories(directory));
	const fs::path settings = directory / "modSettings.json";

	JsonNode personal;
	personal["activePreset"].String() = "personal";
	personal["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	personal["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
	personal["presets"]["other"]["mods"].Vector().emplace_back("vcmi");
	personal["presets"]["other"]["settings"]["hota"]["music"].Bool() = true;
	const std::string original = personal.toCompactString();
	ASSERT_TRUE(LanModRecoveryJournal::persist(settings, original));

	LanModSessionState::begin(personal);
	personal["presets"]["other"]["settings"]["hota"]["music"].Bool() = false;
	personal["presets"][LanModSessionState::temporaryPresetName()]["settings"]["hota"]["units"].Bool() = true;
	JsonNode restored = personal;
	ASSERT_TRUE(LanModSessionState::restore(restored));
	ASSERT_NE(restored.toCompactString(), original);
	ASSERT_TRUE(LanModSessionState::restoreCompleteBackupIfPresent(restored, personal, settings));
	EXPECT_EQ(restored.toCompactString(), original);
	ASSERT_TRUE(AtomicModSettings::replace(settings, restored.toCompactString()));
	EXPECT_TRUE(LanModRecoveryJournal::discardIfMatches(settings, restored.toCompactString()));
	EXPECT_FALSE(fs::exists(LanModRecoveryJournal::pathForSettings(settings)));

	std::error_code cleanupError;
	fs::remove_all(directory, cleanupError);
}

TEST(LanModSyncTest, ConflictingLanRecoveryBackupCannotReplaceOriginalSession)
{
	namespace fs = std::filesystem;
	const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-foreign-backup-" + LanRelaunchToken::generate());
	ASSERT_TRUE(fs::create_directories(directory));
	const fs::path settings = directory / "modSettings.json";

	JsonNode personal;
	personal["activePreset"].String() = "personal";
	personal["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	personal["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
	ASSERT_TRUE(LanModRecoveryJournal::persist(settings, personal.toCompactString()));

	personal["presets"]["personal"]["settings"]["hota"]["units"].Bool() = true;
	LanModSessionState::begin(personal);
	JsonNode restored = personal;
	ASSERT_TRUE(LanModSessionState::restore(restored));
	const std::string embedded = restored.toCompactString();
	EXPECT_THROW(LanModSessionState::restoreCompleteBackupIfPresent(restored, personal, settings), std::runtime_error);
	EXPECT_EQ(restored.toCompactString(), embedded);
	EXPECT_TRUE(fs::exists(LanModRecoveryJournal::pathForSettings(settings)));

	ASSERT_TRUE(LanModRecoveryJournal::discard(settings));
	EXPECT_FALSE(LanModSessionState::restoreCompleteBackupIfPresent(restored, personal, settings));
	EXPECT_EQ(restored.toCompactString(), embedded);

	std::error_code cleanupError;
	fs::remove_all(directory, cleanupError);
}

TEST(LanModSyncTest, StaleLanRecoveryBackupClearsOnlyAfterExactRestoration)
{
	namespace fs = std::filesystem;
	const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-backup-stale-" + LanRelaunchToken::generate());
	ASSERT_TRUE(fs::create_directories(directory));
	const fs::path settings = directory / "modSettings.json";
	const fs::path backup = LanModRecoveryJournal::pathForSettings(settings);

	JsonNode personal;
	personal["activePreset"].String() = "personal";
	personal["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	personal["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
	personal["presets"]["other"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = personal.toCompactString();

	ASSERT_TRUE(AtomicModSettings::replace(settings, original));
	ASSERT_TRUE(LanModRecoveryJournal::persist(settings, original));

	personal["presets"]["personal"]["settings"]["hota"]["units"].Bool() = true;
	ASSERT_TRUE(AtomicModSettings::replace(settings, personal.toCompactString()));
	EXPECT_FALSE(LanModRecoveryJournal::discardIfMatches(settings, personal.toCompactString()));
	EXPECT_EQ(LanModRecoveryJournal::load(settings), original);

	ASSERT_TRUE(AtomicModSettings::replace(settings, original));
	EXPECT_TRUE(LanModRecoveryJournal::discardIfMatches(settings, original));
	EXPECT_FALSE(fs::exists(backup));

	std::error_code cleanupError;
	fs::remove_all(directory, cleanupError);
}

TEST(LanModSyncTest, RejectsUnreadableLanRecoveryBackupSizeBeforeWriting)
{
	namespace fs = std::filesystem;
	const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-backup-size-" + LanRelaunchToken::generate());
	ASSERT_TRUE(fs::create_directories(directory));
	const fs::path settings = directory / "modSettings.json";
	const fs::path backup = LanModRecoveryJournal::pathForSettings(settings);
	const std::string oversized(static_cast<std::size_t>(LanModRecoveryJournal::maxBackupBytes) + 1, 'x');

	EXPECT_THROW(LanModRecoveryJournal::persist(settings, ""), std::runtime_error);
	EXPECT_THROW(LanModRecoveryJournal::persist(settings, oversized), std::runtime_error);
	EXPECT_FALSE(fs::exists(backup));

	const std::string original = R"({"activePreset":"personal","presets":{"personal":{"mods":["vcmi"]}}})";
	ASSERT_TRUE(LanModRecoveryJournal::persist(settings, original));
	EXPECT_EQ(LanModRecoveryJournal::load(settings), original);
	EXPECT_THROW(LanModRecoveryJournal::persist(settings, oversized), std::runtime_error);
	EXPECT_EQ(LanModRecoveryJournal::load(settings), original);

	std::error_code cleanupError;
	fs::remove_all(directory, cleanupError);
}

TEST(LanModSyncTest, RecoveryBackupReaderRejectsNonRegularFilesAndOversize)
{
	namespace fs = std::filesystem;
	const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-backup-read-" + LanRelaunchToken::generate());
	ASSERT_TRUE(fs::create_directories(directory));
	const fs::path settings = directory / "modSettings.json";
	const fs::path backup = LanModRecoveryJournal::pathForSettings(settings);
	const std::string original = R"({"activePreset":"personal","presets":{"personal":{"mods":["vcmi"]}}})";

	ASSERT_TRUE(LanModRecoveryJournal::persist(settings, original));
	EXPECT_EQ(LanModRecoveryJournal::load(settings), original);
	ASSERT_TRUE(fs::remove(backup));
	{
		std::ofstream empty(backup, std::ios::binary);
		ASSERT_TRUE(empty.good());
	}
	EXPECT_THROW(LanModRecoveryJournal::load(settings), std::exception);
	ASSERT_TRUE(fs::remove(backup));
	{
		std::ofstream oversized(backup, std::ios::binary);
		ASSERT_TRUE(oversized.good());
		oversized.seekp(16 * 1024 * 1024);
		oversized.put('x');
		ASSERT_TRUE(oversized.good());
	}
	EXPECT_THROW(LanModRecoveryJournal::load(settings), std::exception);
	ASSERT_TRUE(fs::remove(backup));

#ifndef _WIN32
	fs::create_symlink("missing-target", backup);
	EXPECT_THROW(LanModRecoveryJournal::load(settings), std::exception);
	ASSERT_TRUE(fs::remove(backup));
	fs::create_directory(backup);
	EXPECT_THROW(LanModRecoveryJournal::load(settings), std::exception);
#endif
	std::error_code cleanupError;
	fs::remove_all(directory, cleanupError);
}

TEST(LanModSyncTest, DamagedMainSettingsRestoreFullIndependentPresetAndSubmods)
{
	namespace fs = std::filesystem;
	const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-backup-recover-" + LanRelaunchToken::generate());
	ASSERT_TRUE(fs::create_directories(directory));
	const fs::path settings = directory / "modSettings.json";
	const fs::path backup = LanModRecoveryJournal::pathForSettings(settings);

	JsonNode personal;
	personal["activePreset"].String() = "personal";
	personal["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	personal["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
	personal["presets"]["personal"]["settings"]["hota"]["music"].Bool() = true;
	personal["presets"]["another"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = personal.toCompactString();

	ASSERT_TRUE(LanModRecoveryJournal::persist(settings, original));
	ASSERT_TRUE(AtomicModSettings::replace(settings, "{\"activePreset\":"));
	const JsonNode restored = LanModSessionState::restoreDamagedSettings(settings);
	EXPECT_EQ(restored.toCompactString(), original);
	std::ifstream input(settings, std::ios::binary);
	EXPECT_EQ(std::string((std::istreambuf_iterator<char>(input)), {}), original);
	EXPECT_FALSE(fs::exists(backup));

	std::error_code cleanupError;
	fs::remove_all(directory, cleanupError);
}

TEST(LanModSyncTest, InvalidIndependentBackupCannotReplaceDamagedMainSettings)
{
	namespace fs = std::filesystem;
	const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-backup-invalid-" + LanRelaunchToken::generate());
	ASSERT_TRUE(fs::create_directories(directory));
	const fs::path settings = directory / "modSettings.json";
	const fs::path backup = LanModRecoveryJournal::pathForSettings(settings);
	const std::string damaged = "{\"activePreset\":";
	ASSERT_TRUE(AtomicModSettings::replace(settings, damaged));
	ASSERT_TRUE(LanModRecoveryJournal::persist(settings, "{\"activePreset\":\"missing\",\"presets\":{}}"));

	EXPECT_THROW(LanModSessionState::restoreDamagedSettings(settings), std::exception);
	std::ifstream input(settings, std::ios::binary);
	EXPECT_EQ(std::string((std::istreambuf_iterator<char>(input)), {}), damaged);
	EXPECT_TRUE(fs::exists(backup));

	std::error_code cleanupError;
	fs::remove_all(directory, cleanupError);
}

TEST(LanModSyncTest, ConcurrentIndependentLanBackupPublicationIsExclusive)
{
	namespace fs = std::filesystem;
	const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-backup-race-" + LanRelaunchToken::generate());
	ASSERT_TRUE(fs::create_directories(directory));
	const fs::path settings = directory / "modSettings.json";
	const fs::path backup = LanModRecoveryJournal::pathForSettings(settings);

	std::atomic<int> published{0};
	std::atomic<int> conflicts{0};
	std::atomic<int> unexpected{0};
	std::vector<std::string> snapshots;
	for(int i = 0; i < 12; ++i)
		snapshots.emplace_back(4096, static_cast<char>('A' + i));

	std::vector<std::thread> writers;
	for(int i = 0; i < 12; ++i)
	{
		writers.emplace_back([&, i]()
		{
			try
			{
				if(LanModRecoveryJournal::persist(settings, snapshots[i]))
					++published;
				else
					++unexpected;
			}
			catch(const fs::filesystem_error & error)
			{
				if(error.code() == std::make_error_code(std::errc::file_exists))
					++conflicts;
				else
					++unexpected;
			}
			catch(const std::exception &)
			{
				++unexpected;
			}
		});
	}
	for(auto & writer : writers)
		writer.join();

	EXPECT_EQ(published.load(), 1);
	EXPECT_EQ(conflicts.load(), 11);
	EXPECT_EQ(unexpected.load(), 0);
	std::ifstream input(backup, std::ios::binary);
	const std::string saved((std::istreambuf_iterator<char>(input)), {});
	EXPECT_TRUE(std::find(snapshots.begin(), snapshots.end(), saved) != snapshots.end());
	for(const auto & entry : fs::directory_iterator(directory))
		EXPECT_EQ(entry.path().filename().string().find(".pending."), std::string::npos);

	std::error_code cleanupError;
	fs::remove_all(directory, cleanupError);
}

#ifndef _WIN32
TEST(LanModSyncTest, ConcurrentAtomicWritesDoNotMixOrLeaveStagingFiles)
{
	const auto testDirectory = std::filesystem::temp_directory_path() /
		("vcmi-lan-parallel-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directory(testDirectory);
	const auto settings = testDirectory / "modSettings.json";

	std::atomic<int> failures{0};
	std::vector<std::thread> writers;
	for(int i = 0; i < 12; ++i)
	{
		writers.emplace_back([&, i]()
		{
			try
			{
				if(!AtomicModSettings::replace(settings, std::string(4096, static_cast<char>('A' + i))))
					++failures;
			}
			catch(const std::exception &)
			{
				++failures;
			}
		});
	}
	for(auto & writer : writers)
		writer.join();

	EXPECT_EQ(failures.load(), 0);
	std::ifstream file(settings, std::ios::binary);
	const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	EXPECT_EQ(content.size(), 4096);
	if(!content.empty())
		EXPECT_EQ(content, std::string(content.size(), content[0]));

	for(const auto & entry : std::filesystem::directory_iterator(testDirectory))
		EXPECT_FALSE(entry.path().filename().string().starts_with("modSettings.json.pending."))
			<< entry.path();

	std::filesystem::remove_all(testDirectory);
}
#endif

TEST(LanModSyncTest, ControlledRelaunchResumesOnlyOnce)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	config["presets"][LanModSessionState::temporaryPresetName()]["mods"].Vector().emplace_back("hota");
	LanModSessionState::prepareControlledRelaunch(config, std::string(64, '1'));

	const std::string serialized = config.toCompactString();
	JsonNode nextProcess(serialized.data(), serialized.size(), "Controlled LAN relaunch test");

	EXPECT_EQ(LanModSessionState::recoverOnStartup(nextProcess, std::string(64, '1')), LanSessionStartupAction::RESUMED);
	EXPECT_EQ(nextProcess["activePreset"].String(), LanModSessionState::temporaryPresetName());
	EXPECT_TRUE(nextProcess["lanSession"]["relaunchToken"].isNull());
	EXPECT_FALSE(nextProcess["lanSession"]["originalPreset"].isNull());

	// If the resumed client crashes, the now-consumed token cannot bypass recovery again.
	EXPECT_EQ(LanModSessionState::recoverOnStartup(nextProcess, std::string(64, '1')), LanSessionStartupAction::RESTORED);
	EXPECT_EQ(nextProcess.toCompactString(), original);
}

TEST(LanModSyncTest, NormalDisconnectAfterControlledRelaunchRestoresExactPreset)
{
	JsonNode config;
	config["activePreset"].String() = "player";
	config["presets"]["player"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["player"]["mods"].Vector().emplace_back("maps");
	config["presets"]["player"]["settings"]["maps"]["large"].Bool() = true;
	config["presets"]["player"]["settings"]["maps"]["small"].Bool() = false;
	config["presets"]["other"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	const std::string & temporary = LanModSessionState::temporaryPresetName();
	config["presets"][temporary]["mods"].Vector().emplace_back("hota");
	config["presets"][temporary]["settings"]["maps"]["large"].Bool() = false;
	config["presets"][temporary]["settings"]["maps"]["small"].Bool() = true;
	LanModSessionState::prepareControlledRelaunch(config, std::string(64, '2'));

	const std::string onDisk = config.toCompactString();
	JsonNode restarted(onDisk.data(), onDisk.size(), "Restarted LAN preset test");
	EXPECT_EQ(LanModSessionState::recoverOnStartup(restarted, std::string(64, '2')), LanSessionStartupAction::RESUMED);
	EXPECT_EQ(restarted["activePreset"].String(), temporary);
	EXPECT_TRUE(restarted["presets"][temporary]["settings"]["maps"]["small"].Bool());

	// Leaving the multiplayer room restores personal and unrelated presets exactly.
	EXPECT_TRUE(LanModSessionState::restore(restarted));
	EXPECT_EQ(restarted.toCompactString(), original);
}

TEST(LanModSyncTest, CancellingLobbyJoinBeforeRelaunchRestoresOriginalSubmods)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["mods"].Vector().emplace_back("maps");
	config["presets"]["personal"]["settings"]["maps"]["large"].Bool() = true;
	config["presets"]["personal"]["settings"]["maps"]["small"].Bool() = false;
	config["presets"]["other"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	const auto & temporary = LanModSessionState::temporaryPresetName();
	config["presets"][temporary]["mods"].Vector().emplace_back("host_mod");
	config["presets"][temporary]["settings"]["maps"]["large"].Bool() = false;
	config["presets"][temporary]["settings"]["maps"]["small"].Bool() = true;

	const std::string token(64, 'd');
	LanModSessionState::prepareControlledRelaunch(config, token);

	EXPECT_TRUE(LanModSessionState::restore(config));
	EXPECT_EQ(config.toCompactString(), original);
	EXPECT_EQ(LanModSessionState::recoverOnStartup(config, token), LanSessionStartupAction::NONE);
	EXPECT_FALSE(LanModSessionState::restore(config));
}

TEST(LanModSyncTest, RepeatedFailedLanJoinsRestoreOriginalPreset)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["mods"].Vector().emplace_back("hota");
	config["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
	config["presets"]["other"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = config.toCompactString();

	for(int attempt = 0; attempt < 2; ++attempt)
	{
		LanModSessionState::begin(config);
		const auto & temporary = LanModSessionState::temporaryPresetName();
		config["presets"][temporary]["mods"].Vector().emplace_back("host_mod");
		config["presets"][temporary]["settings"]["hota"]["units"].Bool() = true;

		EXPECT_TRUE(LanModSessionState::restore(config));
		EXPECT_EQ(config.toCompactString(), original);
	}
	EXPECT_FALSE(LanModSessionState::restore(config));
}

TEST(LanModSyncTest, WrongRelaunchTokenRestoresOriginalPreset)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["settings"]["hota"]["units"].Bool() = true;
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	LanModSessionState::prepareControlledRelaunch(config, std::string(64, '3'));
	EXPECT_EQ(LanModSessionState::recoverOnStartup(config, std::string(64, '4')), LanSessionStartupAction::RESTORED);
	EXPECT_EQ(config.toCompactString(), original);
}

TEST(LanModSyncTest, MissingRelaunchTokenRestoresInterruptedSession)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	LanModSessionState::prepareControlledRelaunch(config, std::string(64, '5'));
	EXPECT_EQ(LanModSessionState::recoverOnStartup(config, ""), LanSessionStartupAction::RESTORED);
	EXPECT_EQ(config.toCompactString(), original);
	EXPECT_EQ(LanModSessionState::recoverOnStartup(config, std::string(64, '5')), LanSessionStartupAction::NONE);
}

TEST(LanModSyncTest, RejectsInvalidOrDuplicateRelaunchMarker)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");

	EXPECT_THROW(LanModSessionState::prepareControlledRelaunch(config, std::string(64, '8')), std::runtime_error);
	LanModSessionState::begin(config);
	EXPECT_THROW(LanModSessionState::prepareControlledRelaunch(config, ""), std::invalid_argument);
	EXPECT_THROW(LanModSessionState::prepareControlledRelaunch(config, std::string(129, 'x')), std::invalid_argument);
	EXPECT_THROW(LanModSessionState::prepareControlledRelaunch(config, std::string(63, 'a')), std::invalid_argument);
	EXPECT_THROW(LanModSessionState::prepareControlledRelaunch(config, std::string(64, 'A')), std::invalid_argument);
	EXPECT_THROW(LanModSessionState::prepareControlledRelaunch(config, std::string(63, 'a') + "\n"), std::invalid_argument);

	LanModSessionState::prepareControlledRelaunch(config, std::string(64, '6'));
	const std::string pending = config.toCompactString();
	EXPECT_THROW(LanModSessionState::prepareControlledRelaunch(config, std::string(64, '7')), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), pending);
}

TEST(LanModSyncTest, CorruptedRelaunchSnapshotIsNotDiscarded)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	LanModSessionState::begin(config);
	LanModSessionState::prepareControlledRelaunch(config, std::string(64, '8'));
	config["lanSession"]["originalPresetName"].String() = LanModSessionState::temporaryPresetName();
	const std::string corrupted = config.toCompactString();

	EXPECT_THROW(LanModSessionState::recoverOnStartup(config, std::string(64, '8')), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), corrupted);
	EXPECT_THROW(LanModSessionState::recoverOnStartup(config, std::string(64, '9')), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), corrupted);
}


TEST(LanModSyncTest, NumericRelaunchMarkerRestoresPresetWithoutCrashing)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["settings"]["maps"]["small"].Bool() = false;
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	config["presets"][LanModSessionState::temporaryPresetName()]["settings"]["maps"]["small"].Bool() = true;
	config["lanSession"]["relaunchToken"].Integer() = 42;

	const std::string persisted = config.toCompactString();
	JsonNode restarted(persisted.data(), persisted.size(), "Non-string LAN relaunch marker");
	EXPECT_TRUE(LanModSessionState::shouldRestoreOnShutdown(restarted));
	EXPECT_EQ(LanModSessionState::recoverOnStartup(restarted, std::string(64, 'a')),
		LanSessionStartupAction::RESTORED);
	EXPECT_EQ(restarted.toCompactString(), original);
}

TEST(LanModSyncTest, MalformedStoredSnapshotPreservesRecoveryData)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	LanModSessionState::begin(config);

	config["lanSession"]["originalPresetName"].Integer() = 17;
	const std::string badName = config.toCompactString();
	EXPECT_THROW(LanModSessionState::restore(config), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), badName);

	config["lanSession"]["originalPresetName"].String() = "personal";
	config["lanSession"]["originalPreset"].String() = "not-a-preset";
	const std::string badPreset = config.toCompactString();
	EXPECT_THROW(LanModSessionState::recoverOnStartup(config, ""), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), badPreset);
}

TEST(LanModSyncTest, InvalidPersonalPresetCannotStartOrEraseRecoveryJournal)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["settings"]["hota"]["units"].Integer() = 1;
	const std::string invalidOriginal = config.toCompactString();

	EXPECT_THROW(LanModSessionState::begin(config), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), invalidOriginal);

	config["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
	LanModSessionState::begin(config);
	config["lanSession"]["originalPreset"]["mods"].Vector().front().Integer() = 42;
	const std::string invalidSnapshot = config.toCompactString();

	EXPECT_THROW(LanModSessionState::restore(config), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), invalidSnapshot);
	EXPECT_THROW(LanModSessionState::recoverOnStartup(config, ""), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), invalidSnapshot);
}

TEST(LanModSyncTest, CorruptedTemporaryPresetCannotResumeControlledRelaunch)
{
	for(int damage = 0; damage < 2; ++damage)
	{
		JsonNode config;
		config["activePreset"].String() = "personal";
		config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
		config["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
		const std::string original = config.toCompactString();

		LanModSessionState::begin(config);
		const std::string token(64, 'd');
		LanModSessionState::prepareControlledRelaunch(config, token);

		const auto & sessionName = LanModSessionState::temporaryPresetName();
		if(damage == 0)
			config["presets"][sessionName]["mods"].Vector().front().Integer() = 7;
		else
			config["presets"][sessionName]["settings"]["hota"]["units"].Integer() = 7;

		EXPECT_TRUE(LanModSessionState::shouldRestoreOnShutdown(config));
		EXPECT_EQ(LanModSessionState::recoverOnStartup(config, token), LanSessionStartupAction::RESTORED);
		EXPECT_EQ(config.toCompactString(), original);
	}
}

TEST(LanModSyncTest, MissingTemporaryPresetStillRestoresPersonalSubmods)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	const std::string token(64, 'e');
	LanModSessionState::prepareControlledRelaunch(config, token);
	config["presets"].Struct().erase(LanModSessionState::temporaryPresetName());

	EXPECT_TRUE(LanModSessionState::shouldRestoreOnShutdown(config));
	EXPECT_EQ(LanModSessionState::recoverOnStartup(config, token), LanSessionStartupAction::RESTORED);
	EXPECT_EQ(config.toCompactString(), original);
}

TEST(LanModSyncTest, InvalidActivePresetTypeCannotStartLanSession)
{
	JsonNode config;
	config["activePreset"].Integer() = 1;
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	const std::string previous = config.toCompactString();
	EXPECT_THROW(LanModSessionState::begin(config), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), previous);
}

TEST(LanModSyncTest, InvalidPersistedRelaunchMarkerCannotBypassRecovery)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	config["presets"][LanModSessionState::temporaryPresetName()]["settings"]["hota"]["units"].Bool() = true;
	// An older or tampered journal must not keep the temporary preset active.
	config["lanSession"]["relaunchToken"].String() = "legacy-unusable-marker";

	EXPECT_TRUE(LanModSessionState::shouldRestoreOnShutdown(config));
	EXPECT_EQ(LanModSessionState::recoverOnStartup(config, "legacy-unusable-marker"), LanSessionStartupAction::RESTORED);
	EXPECT_EQ(config.toCompactString(), original);
}

TEST(LanModSyncTest, NoSessionRecoveryIsReadOnly)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = config.toCompactString();

	EXPECT_EQ(LanModSessionState::recoverOnStartup(config, std::string(64, 'a')), LanSessionStartupAction::NONE);
	EXPECT_FALSE(LanModSessionState::restore(config));
	EXPECT_THROW(LanModSessionState::prepareControlledRelaunch(config, std::string(64, 'a')), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), original);
}

TEST(LanModSyncTest, InvalidSessionStartIsReadOnly)
{
	JsonNode config;
	config["activePreset"].String() = "missing";
	const std::string original = config.toCompactString();

	EXPECT_THROW(LanModSessionState::begin(config), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), original);
}

TEST(LanModSyncTest, GeneratedRelaunchTokensAreUniqueAndCLICompatible)
{
	std::set<std::string> seen;

	for(int i = 0; i < 64; ++i)
	{
		const std::string token = LanRelaunchToken::generate();
		EXPECT_TRUE(LanRelaunchTokenFormat::isValid(token));
		EXPECT_TRUE(seen.insert(token).second);
	}

	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = config.toCompactString();
	LanModSessionState::begin(config);
	const std::string token = LanRelaunchToken::generate();
	LanModSessionState::prepareControlledRelaunch(config, token);

	const std::string onDisk = config.toCompactString();
	JsonNode relaunched(onDisk.data(), onDisk.size(), "Generated LAN token test");
	EXPECT_EQ(LanModSessionState::recoverOnStartup(relaunched, token), LanSessionStartupAction::RESUMED);
	EXPECT_TRUE(LanModSessionState::restore(relaunched));
	EXPECT_EQ(relaunched.toCompactString(), original);
}

TEST(LanModSyncTest, CleanShutdownRestoresExactPreset)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["mods"].Vector().emplace_back("maps");
	config["presets"]["personal"]["settings"]["maps"]["large"].Bool() = false;
	config["presets"]["other"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = config.toCompactString();

	EXPECT_FALSE(LanModSessionState::shouldRestoreOnShutdown(config));
	LanModSessionState::begin(config);
	config["presets"][LanModSessionState::temporaryPresetName()]["mods"].Vector().emplace_back("hota");
	config["presets"][LanModSessionState::temporaryPresetName()]["settings"]["maps"]["large"].Bool() = true;

	EXPECT_TRUE(LanModSessionState::shouldRestoreOnShutdown(config));
	EXPECT_TRUE(LanModSessionState::restore(config));
	EXPECT_EQ(config.toCompactString(), original);
	EXPECT_FALSE(LanModSessionState::shouldRestoreOnShutdown(config));
}

TEST(LanModSyncTest, PendingRelaunchSkipsShutdownRollbackButConsumedMarkerAllowsIt)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	config["presets"][LanModSessionState::temporaryPresetName()]["settings"]["hota"]["units"].Bool() = true;
	LanModSessionState::prepareControlledRelaunch(config, std::string(64, 'b'));
	const std::string persisted = config.toCompactString();

	EXPECT_FALSE(LanModSessionState::shouldRestoreOnShutdown(config));
	EXPECT_EQ(config.toCompactString(), persisted);
	EXPECT_EQ(LanModSessionState::recoverOnStartup(config, std::string(64, 'b')), LanSessionStartupAction::RESUMED);
	EXPECT_TRUE(LanModSessionState::shouldRestoreOnShutdown(config));
	EXPECT_TRUE(LanModSessionState::restore(config));
	EXPECT_EQ(config.toCompactString(), original);
}

#ifndef VCMI_MOBILE
TEST(LanModSyncTest, DesktopRelaunchResolvesActualExecutablePath)
{
	const std::filesystem::path executable(boost::dll::program_location().native());
	ASSERT_TRUE(executable.is_absolute());
	ASSERT_TRUE(std::filesystem::is_regular_file(executable));
#ifdef __linux__
	EXPECT_TRUE(std::filesystem::equivalent(executable, std::filesystem::path("/proc/self/exe")));
#endif
}
#endif

TEST(LanModSyncTest, DesktopRelaunchRejectsInvalidTokensBeforeCreatingProcesses)
{
	const std::string validToken(64, 'a');
	EXPECT_TRUE(LanRelaunchProcess::isValidToken(validToken));
	EXPECT_TRUE(LanRelaunchProcess::isValidToken(std::string(64, '0')));
	EXPECT_FALSE(LanRelaunchProcess::isValidToken(std::string(63, 'a')));
	EXPECT_FALSE(LanRelaunchProcess::isValidToken(std::string(65, 'a')));
	EXPECT_FALSE(LanRelaunchProcess::isValidToken(std::string(64, 'A')));
	EXPECT_FALSE(LanRelaunchProcess::isValidToken(std::string(63, 'a') + "z"));
	EXPECT_FALSE(LanRelaunchProcess::isValidToken(std::string(63, 'a') + "\n"));
	EXPECT_FALSE(LanRelaunchProcess::isValidToken(std::string(63, 'a') + std::string(1, '\0')));

#if !defined(VCMI_MOBILE)
	EXPECT_THROW(LanRelaunchProcess::spawn({}, validToken), std::invalid_argument);
	EXPECT_THROW(LanRelaunchProcess::spawn("vcmi-client", std::string(64, 'g')), std::invalid_argument);
#ifndef _WIN32
	EXPECT_THROW(LanRelaunchProcess::spawn("/__vcmi_lan_relaunch_missing__", validToken), std::system_error);
#endif
#endif
}

#ifndef VCMI_MOBILE
TEST(LanModSyncTest, FailedDesktopRelaunchRestoresSavedPersonalPreset)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["settings"]["maps"]["large"].Bool() = false;
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	config["presets"][LanModSessionState::temporaryPresetName()]["settings"]["maps"]["large"].Bool() = true;

	int prepared = 0;
	int restored = 0;
	EXPECT_THROW(LanRelaunchProcess::spawnWithRollback("/__vcmi_lan_relaunch_missing__",
		[&]()
		{
			++prepared;
			const std::string token(64, 'a');
			LanModSessionState::prepareControlledRelaunch(config, token);
			return token;
		},
		[&]()
		{
			++restored;
			EXPECT_TRUE(LanModSessionState::restore(config));
		}), std::system_error);

	EXPECT_EQ(prepared, 1);
	EXPECT_EQ(restored, 1);
	EXPECT_EQ(config.toCompactString(), original);
}

TEST(LanModSyncTest, FailedMarkerPersistenceRestoresPersonalPreset)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["settings"]["maps"]["large"].Bool() = false;
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	config["presets"][LanModSessionState::temporaryPresetName()]["settings"]["maps"]["large"].Bool() = true;
	int restored = 0;

	// A failed persistence acknowledgement can occur after the marker has been written.
	EXPECT_THROW(LanRelaunchProcess::spawnWithRollback("/__vcmi_lan_relaunch_missing__",
		[&]() -> std::string
		{
			LanModSessionState::prepareControlledRelaunch(config, std::string(64, 'c'));
			throw std::runtime_error("marker persistence failed");
		},
		[&]()
		{
			++restored;
			EXPECT_TRUE(LanModSessionState::restore(config));
		}), std::runtime_error);

	EXPECT_EQ(restored, 1);
	EXPECT_EQ(config.toCompactString(), original);
}

TEST(LanModSyncTest, FailedRelaunchReportsPresetRestoreFailureWithoutDiscardingJournal)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	LanModSessionState::begin(config);
	const std::string token(64, 'b');

	std::string error;
	try
	{
		LanRelaunchProcess::spawnWithRollback("/__vcmi_lan_relaunch_missing__",
			[&]()
			{
				LanModSessionState::prepareControlledRelaunch(config, token);
				return token;
			},
			[]()
			{
				throw std::runtime_error("recovery storage unavailable");
			});
		FAIL() << "Missing executable must cause a relaunch error";
	}
	catch(const std::runtime_error & e)
	{
		error = e.what();
	}

	EXPECT_NE(error.find("LAN client relaunch failed"), std::string::npos);
	EXPECT_NE(error.find("recovery storage unavailable"), std::string::npos);
	EXPECT_EQ(config["lanSession"]["relaunchToken"].String(), token);
	EXPECT_FALSE(config["lanSession"]["originalPreset"].isNull());
}
#endif

#if !defined(VCMI_MOBILE) && !defined(_WIN32)
TEST(LanModSyncTest, SuccessfulDesktopRelaunchPreservesMarkerForReplacement)
{
	const std::filesystem::path executable("/bin/true");
	if(!std::filesystem::exists(executable))
		GTEST_SKIP() << "The POSIX smoke-test executable is not installed";

	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	LanModSessionState::begin(config);

	int prepared = 0;
	int restored = 0;
	const std::string token(64, 'a');
	const auto pid = LanRelaunchProcess::spawnWithRollback(executable,
		[&]()
		{
			++prepared;
			LanModSessionState::prepareControlledRelaunch(config, token);
			return token;
		},
		[&]()
		{
			++restored;
			LanModSessionState::restore(config);
		});

	int status = 0;
	ASSERT_EQ(::waitpid(static_cast<pid_t>(pid), &status, 0), static_cast<pid_t>(pid));
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
	EXPECT_EQ(prepared, 1);
	EXPECT_EQ(restored, 0);
	EXPECT_EQ(config["lanSession"]["relaunchToken"].String(), token);
	EXPECT_EQ(config["activePreset"].String(), LanModSessionState::temporaryPresetName());
}
#endif


TEST(LanModSyncTest, ArchiveDigestRejectsBytesBeyondDeclaredLimit)
{
	const auto archive = std::filesystem::temp_directory_path() /
		("vcmi-lan-hash-bound-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".zip");
	const std::string payload(64 * 1024 + 1, 'x');
	{
		std::ofstream output(archive, std::ios::binary);
		output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
	}

	const auto expected = ModArchiveDigest::sha256File(archive);
	ASSERT_TRUE(expected.has_value());
	EXPECT_EQ(ModArchiveDigest::sha256File(archive, payload.size()), expected);
	EXPECT_FALSE(ModArchiveDigest::sha256File(archive, payload.size() - 1));
	EXPECT_FALSE(ModArchiveDigest::sha256File(archive, 0));
	EXPECT_TRUE(ModArchiveDigest::matchesFileSha256(archive, *expected, payload.size()));
	EXPECT_FALSE(ModArchiveDigest::matchesFileSha256(archive, *expected, payload.size() - 1));

	std::filesystem::remove(archive);
}

TEST(LanModSyncTest, ComputesActualArchiveSHA256AcrossBlockBoundaries)
{
    EXPECT_EQ(ModArchiveDigest::Sha256{}.finish(),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    ModArchiveDigest::Sha256 small;
    const std::string abc = "abc";
    small.update(abc.data(), abc.size());
    EXPECT_EQ(small.finish(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    ModArchiveDigest::Sha256 many;
    const std::string data(1000000, 'a');
    for(std::size_t pos = 0; pos < data.size(); pos += 17)
        many.update(data.data() + pos, std::min<std::size_t>(17, data.size() - pos));
    EXPECT_EQ(many.finish(), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(LanModSyncTest, VerifiesCachedArchiveFileBeforeReusingAndDetectsTampering)
{
    namespace fs = std::filesystem;
    const fs::path cacheFolder = fs::temp_directory_path() / ("vcmi-lan-hash-" + LanRelaunchToken::generate());
    fs::create_directories(cacheFolder);
    const fs::path archive = cacheFolder / "hota.zip";
    const std::string expected = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    {
        std::ofstream output(archive, std::ios::binary);
        output << "abc";
    }
    const auto digest = ModArchiveDigest::sha256File(archive);
    EXPECT_TRUE(digest.has_value());
    EXPECT_TRUE(ModArchiveDigest::matchesFileSha256(archive, expected));
    if(digest)
    {
        const auto reusable = ModVerificationInfo::buildSyncPlan(
            {{"hota", ModVerificationStatus::NOT_INSTALLED}}, {{"hota", expected}}, {{"hota", *digest}});
        EXPECT_EQ(reusable.reuseCachedArchive, std::set<TModID>({"hota"}));
        EXPECT_TRUE(reusable.downloadOrUpdate.empty());
    }

    {
        std::ofstream output(archive, std::ios::binary | std::ios::trunc);
        output << "abd";
    }
    const auto changed = ModArchiveDigest::sha256File(archive);
    EXPECT_TRUE(changed.has_value());
    EXPECT_FALSE(ModArchiveDigest::matchesFileSha256(archive, expected));
    if(changed)
    {
        const auto transfer = ModVerificationInfo::buildSyncPlan(
            {{"hota", ModVerificationStatus::NOT_INSTALLED}}, {{"hota", expected}}, {{"hota", *changed}});
        EXPECT_EQ(transfer.downloadOrUpdate, std::set<TModID>({"hota"}));
        EXPECT_TRUE(transfer.reuseCachedArchive.empty());
    }

    EXPECT_FALSE(ModArchiveDigest::matchesFileSha256(archive, "not-a-sha256"));
    EXPECT_FALSE(ModArchiveDigest::sha256File(cacheFolder));
    EXPECT_FALSE(ModArchiveDigest::sha256File(cacheFolder / "missing.zip"));
    std::error_code error;
    fs::create_symlink(archive, cacheFolder / "linked.zip", error);
    if(!error)
        EXPECT_FALSE(ModArchiveDigest::sha256File(cacheFolder / "linked.zip"));
    fs::remove_all(cacheFolder, error);
}

#ifndef _WIN32
TEST(LanModSyncTest, PosixArchiveDigestPinsFileBeforePathReplacement)
{
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-descriptor-" + LanRelaunchToken::generate());
    fs::create_directories(directory);
    const fs::path original = directory / "archive.zip";
    const fs::path moved = directory / "moved.zip";
    {
        std::ofstream output(original, std::ios::binary);
        output << "abc";
    }

    const int descriptor = ModArchiveDigest::detail::openRegularArchiveNoFollow(original);
    ASSERT_GE(descriptor, 0);
    fs::rename(original, moved);
    fs::create_symlink(moved, original);

    EXPECT_LT(ModArchiveDigest::detail::openRegularArchiveNoFollow(original), 0);
    EXPECT_FALSE(ModArchiveDigest::sha256File(original));
    char payload[3]{};
    EXPECT_EQ(::read(descriptor, payload, sizeof(payload)), static_cast<ssize_t>(sizeof(payload)));
    EXPECT_EQ(std::string(payload, sizeof(payload)), "abc");
    EXPECT_EQ(::close(descriptor), 0);
    EXPECT_EQ(ModArchiveDigest::sha256File(moved),
        std::optional<std::string>("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    const fs::path fifo = directory / "pipe.zip";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    EXPECT_LT(ModArchiveDigest::detail::openRegularArchiveNoFollow(fifo), 0);
    std::error_code error;
    fs::remove_all(directory, error);
}
#endif

#ifdef _WIN32
TEST(LanModSyncTest, WindowsArchiveDigestPinsHandleAcrossPathReplacement)
{
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-win-digest-" + LanRelaunchToken::generate());
    fs::create_directories(directory);
    const fs::path original = directory / "archive.zip";
    const fs::path moved = directory / "moved.zip";
    {
        std::ofstream output(original, std::ios::binary);
        output << "abc";
    }

    const HANDLE handle = ModArchiveDigest::detail::openRegularArchiveNoFollow(original);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    std::error_code error;
    fs::rename(original, moved, error);
    if(error)
    {
        CloseHandle(handle);
        fs::remove_all(directory);
        FAIL() << "Cannot rename archive with pinned Windows read handle: " << error.message();
        return;
    }
    {
        std::ofstream output(original, std::ios::binary);
        output << "abd";
    }

    char payload[3]{};
    DWORD read = 0;
    EXPECT_TRUE(ReadFile(handle, payload, static_cast<DWORD>(sizeof(payload)), &read, nullptr));
    EXPECT_EQ(read, static_cast<DWORD>(sizeof(payload)));
    EXPECT_EQ(std::string(payload, sizeof(payload)), "abc");
    EXPECT_TRUE(CloseHandle(handle));
    EXPECT_EQ(ModArchiveDigest::sha256File(moved),
        std::optional<std::string>("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    EXPECT_FALSE(ModArchiveDigest::matchesFileSha256(original,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    fs::remove_all(directory, error);
}
#endif

TEST(LanModSyncTest, PersistentGuestPreflightReusesOnlyRehashedArchiveBytes)
{
	namespace fs = std::filesystem;
	const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-preflight-" + LanRelaunchToken::generate());
	const fs::path userData = directory / "user-data";
	const fs::path staged = directory / "staged.zip";
	const std::string digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
	fs::create_directories(directory);
	{
		std::ofstream file(staged, std::ios::binary);
		file << "abc";
	}

	const ModListVerificationStatus statuses = {{"hota", ModVerificationStatus::NOT_INSTALLED}};
	const ModArchiveHashes manifest = {{"hota", digest}};
	const auto missing = ModVerificationInfo::buildSyncPlanFromPersistentCache(statuses, manifest, userData);
	EXPECT_EQ(missing.downloadOrUpdate, std::set<TModID>({"hota"}));
	EXPECT_TRUE(missing.reuseCachedArchive.empty());

	const auto published = LanModArchiveCache::publishVerifiedArchive(userData, "hota", digest, staged);
	ASSERT_TRUE(published);
	const auto repeated = ModVerificationInfo::buildSyncPlanFromPersistentCache(statuses, manifest, userData);
	EXPECT_TRUE(repeated.downloadOrUpdate.empty());
	EXPECT_EQ(repeated.reuseCachedArchive, std::set<TModID>({"hota"}));

	// A matching installed but disabled mod needs activation only, not archive I/O.
	const auto installed = ModVerificationInfo::buildSyncPlanFromPersistentCache(
		{{"hota", ModVerificationStatus::DISABLED}}, {}, userData);
	EXPECT_TRUE(installed.downloadOrUpdate.empty());
	EXPECT_TRUE(installed.reuseCachedArchive.empty());
	EXPECT_EQ(installed.activate, std::set<TModID>({"hota"}));

	{
		std::ofstream file(*published, std::ios::binary | std::ios::trunc);
		file << "abd";
	}
	const auto tampered = ModVerificationInfo::buildSyncPlanFromPersistentCache(statuses, manifest, userData);
	EXPECT_EQ(tampered.downloadOrUpdate, std::set<TModID>({"hota"}));
	EXPECT_TRUE(tampered.reuseCachedArchive.empty());

	std::error_code cleanupError;
	fs::remove_all(directory, cleanupError);
}

TEST(LanModSyncTest, PersistentLanArchiveCacheReusesOnlyVerifiedContent)
{
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-cache-" + LanRelaunchToken::generate());
    const fs::path userData = directory / "user-data";
    const fs::path staged = directory / "staged.zip";
    const std::string digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    fs::create_directories(directory);
    {
        std::ofstream file(staged, std::ios::binary);
        file << "abc";
    }

    EXPECT_FALSE(LanModArchiveCache::findVerifiedArchive(userData, "hota", digest));
    const auto published = LanModArchiveCache::publishVerifiedArchive(userData, "hota", digest, staged);
    ASSERT_TRUE(published);
    EXPECT_EQ(LanModArchiveCache::findVerifiedArchive(userData, "hota", digest), published);
    const auto verified = LanModArchiveCache::verifiedArchiveDigests(userData, {{"hota", digest}});
    const auto plan = ModVerificationInfo::buildSyncPlan(
        {{"hota", ModVerificationStatus::NOT_INSTALLED}}, {{"hota", digest}}, verified);
    EXPECT_TRUE(plan.downloadOrUpdate.empty());
    EXPECT_EQ(plan.reuseCachedArchive, std::set<TModID>({"hota"}));
    EXPECT_EQ(LanModArchiveCache::publishVerifiedArchive(userData, "hota", digest, staged), published);
    EXPECT_NE(LanModArchiveCache::archivePath(userData, "anothermod", digest), published);
    EXPECT_EQ(LanModArchiveCache::archivePath(userData, "hota", "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"), published);
    EXPECT_FALSE(LanModArchiveCache::archivePath(userData, "hota.maps", digest));
    EXPECT_FALSE(LanModArchiveCache::archivePath(userData, "../unsafe", "invalid-digest"));

    // A changed staged file cannot replace the matching cached archive.
    {
        std::ofstream file(staged, std::ios::binary | std::ios::trunc);
        file << "abd";
    }
    EXPECT_FALSE(LanModArchiveCache::publishVerifiedArchive(userData, "hota", digest, staged));
    EXPECT_EQ(LanModArchiveCache::findVerifiedArchive(userData, "hota", digest), published);

    // Tampering with cached bytes invalidates the hit even when the name is unchanged.
    {
        std::ofstream file(*published, std::ios::binary | std::ios::trunc);
        file << "abd";
    }
    EXPECT_FALSE(LanModArchiveCache::findVerifiedArchive(userData, "hota", digest));
    {
        std::ofstream file(staged, std::ios::binary | std::ios::trunc);
        file << "abc";
    }
    EXPECT_FALSE(LanModArchiveCache::publishVerifiedArchive(userData, "hota", digest, staged));
    EXPECT_TRUE(LanModArchiveCache::verifiedArchiveDigests(userData, {{"hota", digest}}).empty());

    std::error_code cleanupError;
    fs::remove_all(directory, cleanupError);
}

TEST(LanModSyncTest, PersistentLanArchiveCacheRejectsOversizedAndNonRegularPackages)
{
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-size-" + LanRelaunchToken::generate());
    fs::create_directories(directory);
    const fs::path small = directory / "small.zip";
    {
        std::ofstream file(small, std::ios::binary);
        file << "abc";
    }
    EXPECT_TRUE(LanModArchiveCache::isAllowedArchiveSize(small));
    EXPECT_FALSE(LanModArchiveCache::isAllowedArchiveSize(directory));
    EXPECT_FALSE(LanModArchiveCache::isAllowedArchiveSize(directory / "missing.zip"));

    // An empty file has a valid SHA-256 but is not a usable mod archive.
    const fs::path empty = directory / "empty.zip";
    {
        std::ofstream file(empty, std::ios::binary);
        ASSERT_TRUE(file.good());
    }
    EXPECT_EQ(fs::file_size(empty), 0U);
    EXPECT_FALSE(LanModArchiveCache::isAllowedArchiveSize(empty));
    const std::string emptyDigest = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    EXPECT_FALSE(LanModArchiveCache::publishVerifiedArchive(directory / "user-data", "hota", emptyDigest, empty));
    EXPECT_FALSE(LanModArchiveCache::findVerifiedArchive(directory / "user-data", "hota", emptyDigest));

#ifndef _WIN32
    // A sparse file tests the limit without writing gigabytes of content.
    const fs::path oversized = directory / "oversized.zip";
    {
        std::ofstream file(oversized, std::ios::binary);
        file.seekp(static_cast<std::streamoff>(LanModArchiveCache::maxArchiveBytes));
        file.put('\0');
        ASSERT_TRUE(file.good());
    }
    EXPECT_EQ(fs::file_size(oversized), LanModArchiveCache::maxArchiveBytes + 1);
    EXPECT_FALSE(LanModArchiveCache::isAllowedArchiveSize(oversized));
    EXPECT_FALSE(LanModArchiveCache::publishVerifiedArchive(directory / "user-data", "hota",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", oversized));
#endif
    std::error_code cleanupError;
    fs::remove_all(directory, cleanupError);
}

TEST(LanModSyncTest, PersistentLanArchiveCacheRejectsLinkedCacheDirectory)
{
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-link-" + LanRelaunchToken::generate());
    const fs::path userData = directory / "user-data";
    const fs::path staged = directory / "staged.zip";
    const fs::path redirected = directory / "outside";
    const std::string digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const auto expectedArchive = LanModArchiveCache::archivePath(userData, "hota", digest);
    ASSERT_TRUE(expectedArchive);
    const auto modDirectoryName = expectedArchive->parent_path().filename();
    const auto redirectedArchive = redirected / modDirectoryName / expectedArchive->filename();
    fs::create_directories(userData);
    fs::create_directories(redirectedArchive.parent_path());
    {
        std::ofstream file(staged, std::ios::binary);
        file << "abc";
    }
    {
        std::ofstream file(redirectedArchive, std::ios::binary);
        file << "abc";
    }

    // A matching archive outside userData must never count as a cache hit.
    std::error_code error;
    const fs::path cacheRoot = userData / "lan-mod-archives";
    fs::create_directory_symlink(redirected, cacheRoot, error);
    if(error)
    {
        fs::remove_all(directory, error);
        GTEST_SKIP() << "Directory symlinks unavailable on this filesystem";
    }
    EXPECT_FALSE(LanModArchiveCache::findVerifiedArchive(userData, "hota", digest));
    EXPECT_TRUE(LanModArchiveCache::verifiedArchiveDigests(userData, {{"hota", digest}}).empty());
    EXPECT_FALSE(LanModArchiveCache::publishVerifiedArchive(userData, "hota", digest, staged));
    EXPECT_EQ(fs::file_size(redirectedArchive), 3U);

    fs::remove(cacheRoot, error);
    ASSERT_FALSE(error);
    fs::create_directories(cacheRoot);
    fs::create_directory_symlink(redirectedArchive.parent_path(), cacheRoot / modDirectoryName, error);
    ASSERT_FALSE(error);
    EXPECT_FALSE(LanModArchiveCache::findVerifiedArchive(userData, "hota", digest));
    EXPECT_TRUE(LanModArchiveCache::verifiedArchiveDigests(userData, {{"hota", digest}}).empty());
    EXPECT_FALSE(LanModArchiveCache::publishVerifiedArchive(userData, "hota", digest, staged));

    std::error_code cleanupError;
    fs::remove_all(directory, cleanupError);
}


#ifdef _WIN32
TEST(LanModSyncTest, WindowsArchiveStagingCopyRejectsGrowthAndReparsePoints)
{
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-windows-copy-" + LanRelaunchToken::generate());
    ASSERT_TRUE(fs::create_directories(directory));
    const fs::path staged = directory / "stage.zip";
    const fs::path accepted = directory / "accepted.zip";
    const fs::path tooSmall = directory / "too-small.zip";
    const fs::path existing = directory / "existing.zip";
    const std::string payload(64 * 1024 + 1, 'a');
    {
        std::ofstream output(staged, std::ios::binary);
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    EXPECT_TRUE(LanModArchiveCache::detail::copyStagedArchiveBounded(staged, accepted, payload.size()));
    EXPECT_EQ(ModArchiveDigest::sha256File(accepted), ModArchiveDigest::sha256File(staged));
    EXPECT_FALSE(LanModArchiveCache::detail::copyStagedArchiveBounded(staged, tooSmall, payload.size() - 1));
    EXPECT_LE(fs::file_size(tooSmall), payload.size() - 1);

    {
        std::ofstream output(existing, std::ios::binary);
        output << "private";
    }
    EXPECT_FALSE(LanModArchiveCache::detail::copyStagedArchiveBounded(staged, existing, payload.size()));
    EXPECT_EQ(fs::file_size(existing), 7U);
    EXPECT_FALSE(LanModArchiveCache::detail::copyStagedArchiveBounded(
        directory, directory / "directory-copy.zip", payload.size()));

    std::error_code error;
    const fs::path linkedSource = directory / "linked-stage.zip";
    fs::create_symlink(staged, linkedSource, error);
    if(!error)
    {
        EXPECT_FALSE(LanModArchiveCache::detail::copyStagedArchiveBounded(
            linkedSource, directory / "linked-copy.zip", payload.size()));
        EXPECT_FALSE(fs::exists(directory / "linked-copy.zip"));
    }

    fs::remove_all(directory, error);
}
#endif

#ifndef _WIN32
TEST(LanModSyncTest, PersistentLanArchiveStagingCopyRejectsGrowthAndSymlinkRaces)
{
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-bounded-copy-" + LanRelaunchToken::generate());
    ASSERT_TRUE(fs::create_directories(directory));
    const fs::path staged = directory / "stage.zip";
    const fs::path accepted = directory / "accepted.zip";
    const fs::path rejected = directory / "rejected.zip";
    const fs::path existing = directory / "existing.zip";
    const std::string payload(64 * 1024 + 1, 'a');
    {
        std::ofstream out(staged, std::ios::binary);
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    EXPECT_TRUE(LanModArchiveCache::detail::copyStagedArchiveBounded(staged, accepted, payload.size()));
    EXPECT_EQ(ModArchiveDigest::sha256File(accepted), ModArchiveDigest::sha256File(staged));
    EXPECT_FALSE(LanModArchiveCache::detail::copyStagedArchiveBounded(staged, rejected, payload.size() - 1));
    EXPECT_LE(fs::file_size(rejected), payload.size() - 1);

    {
        std::ofstream out(existing, std::ios::binary);
        out << "private";
    }
    EXPECT_FALSE(LanModArchiveCache::detail::copyStagedArchiveBounded(staged, existing, payload.size()));
    EXPECT_EQ(fs::file_size(existing), 7U);

    const fs::path linkedSource = directory / "linked-source.zip";
    fs::create_symlink(staged, linkedSource);
    EXPECT_FALSE(LanModArchiveCache::detail::copyStagedArchiveBounded(
        linkedSource, directory / "linked-copy.zip", payload.size()));
    EXPECT_FALSE(fs::exists(directory / "linked-copy.zip"));

    const fs::path linkedDestination = directory / "linked-destination.zip";
    const fs::path outside = directory / "outside.zip";
    fs::create_symlink(outside, linkedDestination);
    EXPECT_FALSE(LanModArchiveCache::detail::copyStagedArchiveBounded(staged, linkedDestination, payload.size()));
    EXPECT_FALSE(fs::exists(outside));

    const fs::path fifo = directory / "pipe.zip";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    EXPECT_FALSE(LanModArchiveCache::detail::copyStagedArchiveBounded(
        fifo, directory / "pipe-copy.zip", payload.size()));
    EXPECT_FALSE(fs::exists(directory / "pipe-copy.zip"));

    std::error_code error;
    fs::remove_all(directory, error);
}
#endif

TEST(LanModSyncTest, PersistentLanArchivePublishNeverReplacesRacingWriter)
{
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-publish-" + LanRelaunchToken::generate());
    fs::create_directories(directory);
    const fs::path first = directory / "first.part";
    const fs::path second = directory / "second.part";
    const fs::path destination = directory / "archive.zip";
    {
        std::ofstream out(first, std::ios::binary);
        out << "original";
    }
    {
        std::ofstream out(second, std::ios::binary);
        out << "replacement";
    }

    // Emulate another writer publishing after the initial destination-exists check.
    ASSERT_TRUE(LanModArchiveCache::detail::publishWithoutReplacing(first, destination));
    EXPECT_FALSE(LanModArchiveCache::detail::publishWithoutReplacing(second, destination));
    {
        std::ifstream in(destination, std::ios::binary);
        std::string saved;
        std::getline(in, saved);
        EXPECT_EQ(saved, "original");
    }
    EXPECT_TRUE(fs::exists(second));

    // When multiple verified writers finish together, precisely one archive wins.
    constexpr int writers = 8;
    std::atomic<int> published{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    const fs::path sharedDestination = directory / "raced.zip";
    for(int index = 0; index < writers; ++index)
    {
        const fs::path part = directory / ("racer-" + std::to_string(index) + ".part");
        {
            std::ofstream out(part, std::ios::binary);
            out << index;
        }
        workers.emplace_back([&, part]
        {
            while(!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            if(LanModArchiveCache::detail::publishWithoutReplacing(part, sharedDestination))
                published.fetch_add(1, std::memory_order_relaxed);
        });
    }
    start.store(true, std::memory_order_release);
    for(auto & worker : workers)
        worker.join();
    EXPECT_EQ(published.load(), 1);
    EXPECT_TRUE(fs::is_regular_file(sharedDestination));

    std::error_code cleanupError;
    fs::remove_all(directory, cleanupError);
}

#ifndef _WIN32
TEST(LanModSyncTest, PersistentLanArchiveSyncRejectsInvalidEntries)
{
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-sync-" + LanRelaunchToken::generate());
    fs::create_directories(directory);
    const fs::path archive = directory / "staged.zip";
    {
        std::ofstream out(archive, std::ios::binary);
        out << "abc";
    }

    EXPECT_TRUE(LanModArchiveCache::detail::syncCacheEntry(archive, false));
    EXPECT_TRUE(LanModArchiveCache::detail::syncCacheEntry(directory, true));
    EXPECT_FALSE(LanModArchiveCache::detail::syncCacheEntry(archive, true));
    EXPECT_FALSE(LanModArchiveCache::detail::syncCacheEntry(directory, false));
    EXPECT_FALSE(LanModArchiveCache::detail::syncCacheEntry(directory / "missing.zip", false));

    std::error_code error;
    fs::create_symlink(archive, directory / "linked.zip", error);
    if(!error)
        EXPECT_FALSE(LanModArchiveCache::detail::syncCacheEntry(directory / "linked.zip", false));

    fs::remove_all(directory, error);
}
#endif
