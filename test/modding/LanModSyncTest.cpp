/*
 * LanModSyncTest.cpp, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */

#include "StdInc.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "../../lib/modding/AtomicModSettings.h"

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

	const auto plan = ModVerificationInfo::buildSyncPlan(states, {}, {});

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

TEST(LanModSyncTest, DownloadsChangedArchiveInsteadOfReusingStaleCache)
{
	const ModListVerificationStatus states = {{"hota", ModVerificationStatus::VERSION_MISMATCH}};
	const auto plan = ModVerificationInfo::buildSyncPlan(
		states, {{"hota", std::string(64, 'a')}}, {{"hota", std::string(64, 'b')}});

	EXPECT_EQ(plan.downloadOrUpdate, std::set<TModID>({"hota"}));
	EXPECT_TRUE(plan.reuseCachedArchive.empty());
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
	EXPECT_THROW(AtomicModSettings::replace(directoryTarget, "unexpected"), std::filesystem::filesystem_error);
	EXPECT_TRUE(std::filesystem::is_directory(directoryTarget));
	EXPECT_FALSE(std::filesystem::exists(testDirectory / "do-not-replace.pending"));

	// A pre-existing staging directory must never be deleted if opening it fails.
	const auto blockedStage = testDirectory / "modSettings.json.pending";
	std::filesystem::create_directory(blockedStage);
	EXPECT_THROW(AtomicModSettings::replace(settings, "{\"invalid\":true}"), std::system_error);
	EXPECT_TRUE(std::filesystem::is_directory(blockedStage));

	{
		std::ifstream recovered(settings, std::ios::binary);
		const std::string preserved((std::istreambuf_iterator<char>(recovered)), std::istreambuf_iterator<char>());
		EXPECT_EQ(preserved, "{\"new\":true}");
	}

	std::filesystem::remove_all(testDirectory);
}

TEST(LanModSyncTest, ControlledRelaunchResumesOnlyOnce)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["settings"]["hota"]["units"].Bool() = false;
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	config["presets"][LanModSessionState::temporaryPresetName()]["mods"].Vector().emplace_back("hota");
	LanModSessionState::prepareControlledRelaunch(config, "local-restart-token");

	const std::string serialized = config.toCompactString();
	JsonNode nextProcess(serialized.data(), serialized.size(), "Controlled LAN relaunch test");

	EXPECT_EQ(LanModSessionState::recoverOnStartup(nextProcess, "local-restart-token"), LanSessionStartupAction::RESUMED);
	EXPECT_EQ(nextProcess["activePreset"].String(), LanModSessionState::temporaryPresetName());
	EXPECT_TRUE(nextProcess["lanSession"]["relaunchToken"].isNull());
	EXPECT_FALSE(nextProcess["lanSession"]["originalPreset"].isNull());

	// If the resumed client crashes, the now-consumed token cannot bypass recovery again.
	EXPECT_EQ(LanModSessionState::recoverOnStartup(nextProcess, "local-restart-token"), LanSessionStartupAction::RESTORED);
	EXPECT_EQ(nextProcess.toCompactString(), original);
}

TEST(LanModSyncTest, WrongRelaunchTokenRestoresOriginalPreset)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["settings"]["hota"]["units"].Bool() = true;
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	LanModSessionState::prepareControlledRelaunch(config, "correct-token");
	EXPECT_EQ(LanModSessionState::recoverOnStartup(config, "wrong-token"), LanSessionStartupAction::RESTORED);
	EXPECT_EQ(config.toCompactString(), original);
}

TEST(LanModSyncTest, MissingRelaunchTokenRestoresInterruptedSession)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = config.toCompactString();

	LanModSessionState::begin(config);
	LanModSessionState::prepareControlledRelaunch(config, "expected-token");
	EXPECT_EQ(LanModSessionState::recoverOnStartup(config, ""), LanSessionStartupAction::RESTORED);
	EXPECT_EQ(config.toCompactString(), original);
	EXPECT_EQ(LanModSessionState::recoverOnStartup(config, "expected-token"), LanSessionStartupAction::NONE);
}

TEST(LanModSyncTest, RejectsInvalidOrDuplicateRelaunchMarker)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");

	EXPECT_THROW(LanModSessionState::prepareControlledRelaunch(config, "token"), std::runtime_error);
	LanModSessionState::begin(config);
	EXPECT_THROW(LanModSessionState::prepareControlledRelaunch(config, ""), std::invalid_argument);
	EXPECT_THROW(LanModSessionState::prepareControlledRelaunch(config, std::string(129, 'x')), std::invalid_argument);

	LanModSessionState::prepareControlledRelaunch(config, "one-time-token");
	const std::string pending = config.toCompactString();
	EXPECT_THROW(LanModSessionState::prepareControlledRelaunch(config, "another-token"), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), pending);
}

TEST(LanModSyncTest, CorruptedRelaunchSnapshotIsNotDiscarded)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	LanModSessionState::begin(config);
	LanModSessionState::prepareControlledRelaunch(config, "token");
	config["lanSession"]["originalPresetName"].String() = LanModSessionState::temporaryPresetName();
	const std::string corrupted = config.toCompactString();

	EXPECT_THROW(LanModSessionState::recoverOnStartup(config, "token"), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), corrupted);
	EXPECT_THROW(LanModSessionState::recoverOnStartup(config, "other-token"), std::runtime_error);
	EXPECT_EQ(config.toCompactString(), corrupted);
}

TEST(LanModSyncTest, NoSessionRecoveryIsReadOnly)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = config.toCompactString();

	EXPECT_EQ(LanModSessionState::recoverOnStartup(config, "stale-token"), LanSessionStartupAction::NONE);
	EXPECT_FALSE(LanModSessionState::restore(config));
	EXPECT_THROW(LanModSessionState::prepareControlledRelaunch(config, "stale-token"), std::runtime_error);
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
