/*
 * LanModSyncTest.cpp, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */

#include "StdInc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

#if !defined(VCMI_MOBILE) && !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "../../lib/modding/AtomicModSettings.h"
#include "../../lib/modding/LanRelaunchToken.h"
#include "../../lib/modding/LanRelaunchTokenFormat.h"
#include "../../clientapp/LanRelaunchProcess.h"
#include "../../lib/modding/ModArchiveDigest.h"

#include "../../lib/json/JsonNode.h"
#include "../../lib/modding/ModManager.h"
#include "../../lib/modding/ModVerificationInfo.h"

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

TEST(LanModSyncTest, AtomicWritePreservesSettingsWhenReplacementFails)
{
	const auto testDirectory = std::filesystem::temp_directory_path() /
		("vcmi-lan-atomic-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directory(testDirectory);
	const auto settings = testDirectory / "modSettings.json";

	AtomicModSettings::replace(settings, "{\"old\":true}");
	AtomicModSettings::replace(settings, "{\"new\":true}");
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
	AtomicModSettings::replace(settings, "{\"newer\":true}");
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
				AtomicModSettings::replace(settings, std::string(4096, static_cast<char>('A' + i)));
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
