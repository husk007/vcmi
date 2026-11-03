/*
 * ModManager.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "ModManager.h"
#include "AtomicModSettings.h"
#include "LanModRecoveryJournal.h"
#include "ModArchiveDigest.h"
#include "LanRelaunchToken.h"
#include "LanRelaunchTokenFormat.h"
#include "ModVerificationInfo.h"

#include "ModDescription.h"
#include "ModScope.h"

#include "../constants/StringConstants.h"
#include "../filesystem/Filesystem.h"
#include "../json/JsonNode.h"
#include "../texts/CGeneralTextHandler.h"

static std::string getModDirectory(const TModID & modName)
{
	std::string result = modName;
	boost::to_upper(result);
	boost::algorithm::replace_all(result, ".", "/MODS/");
	return "MODS/" + result;
}

/// Converts path of a mod definition file, e.g. 'MODS/HOTA/MODS/MAPS/MOD', into mod ID, e.g. 'hota.maps'
/// Returns nullopt if the path is not a valid mod location, e.g. a 'mod.json' shipped as mod content
static std::optional<TModID> getModIDFromDefinitionFile(const std::string & path)
{
	std::vector<std::string> segments;
	boost::split(segments, path, boost::is_any_of("/"));

	// expected form: MODS / <mod> [ / MODS / <submod> ]... / MOD
	if(segments.size() < 3 || segments.size() % 2 == 0 || segments.back() != "MOD")
		return std::nullopt;

	TModID result;
	for(size_t i = 0; i < segments.size() - 1; i += 2)
	{
		if(segments[i] != "MODS")
			return std::nullopt;

		std::string name = boost::to_lower_copy(segments[i + 1]);

		// '.' separates mod from its submods, so it can not be part of a mod name
		if(name.empty() || name.find('.') != std::string::npos || ModScope::isScopeReserved(name))
			return std::nullopt;

		if(!result.empty())
			result += '.';
		result += name;
	}
	return result;
}

/// Returns true if every mod that the specified submod is nested in is a mod on its own
static bool hasValidParentMods(const TModID & modID, const std::set<TModID> & candidates)
{
	for(size_t separator = modID.find('.'); separator != std::string::npos; separator = modID.find('.', separator + 1))
		if(!candidates.count(modID.substr(0, separator)))
			return false;

	return true;
}

static JsonPath getModDefinitionFile(const TModID & modName)
{
	return JsonPath::builtin(getModDirectory(modName) + "/mod");
}

static TextPath getModDescriptionFile(const TModID & modName)
{
	return TextPath::builtin(getModDirectory(modName) + "/description");
}

ModsState::ModsState()
{
	modList.push_back(ModScope::scopeBuiltin());

	// every mod is identified by its definition file, so a single scan of game data locates all of them
	auto definitionFiles = CResourceHandler::get("initial")->getFilteredFiles([](const ResourcePath & id)
	{
		return id.getType() == EResType::JSON && boost::algorithm::ends_with(id.getName(), "/MOD");
	});

	// sorted, so that mod order does not depend on iteration order of an unordered container
	std::set<TModID> candidates;
	for(const auto & definitionFile : definitionFiles)
	{
		auto modID = getModIDFromDefinitionFile(definitionFile.getName());
		if(modID)
			candidates.insert(*modID);
	}

	for(const auto & modID : candidates)
		if(hasValidParentMods(modID, candidates))
			modList.push_back(modID);
}

TModList ModsState::getInstalledMods() const
{
	return modList;
}

double ModsState::getInstalledModSizeMegabytes(const TModID & modName) const
{
	ResourcePath resDir(getModDirectory(modName), EResType::DIRECTORY);
	// iterate over the path itself - a detour through std::string would convert it using the
	// Windows ANSI codepage, replacing any character the codepage lacks with '?'. The directory
	// would then not exist and recursive_directory_iterator would throw
	const boost::filesystem::path path = *CResourceHandler::get()->getResourceName(resDir);

	size_t sizeBytes = 0;
	for(boost::filesystem::recursive_directory_iterator it(path); it != boost::filesystem::recursive_directory_iterator(); ++it)
	{
		if(!boost::filesystem::is_directory(*it))
			sizeBytes += boost::filesystem::file_size(*it);
	}

	double sizeMegabytes = sizeBytes / static_cast<double>(1024*1024);
	return sizeMegabytes;
}

///////////////////////////////////////////////////////////////////////////////

const std::string & LanModSessionState::temporaryPresetName()
{
	static const std::string name = "__vcmi_lan_session";
	return name;
}

static bool isValidLanPreset(const JsonNode & preset)
{
	if(!preset.isStruct() || !preset["mods"].isVector())
		return false;

	for(const auto & mod : preset["mods"].Vector())
		if(!mod.isString())
			return false;

	const JsonNode & settings = preset["settings"];
	if(settings.isNull())
		return true;
	if(!settings.isStruct())
		return false;

	for(const auto & [modId, submods] : settings.Struct())
	{
		if(!submods.isStruct())
			return false;
		for(const auto & [submodId, enabled] : submods.Struct())
			if(!enabled.isBool())
				return false;
	}
	return true;
}

static void validateLanSessionSnapshot(const JsonNode & config)
{
	if(!config["activePreset"].isString()
		|| config["activePreset"].String() != LanModSessionState::temporaryPresetName())
		throw std::runtime_error("LAN mod recovery requires the temporary preset to be active");

	const JsonNode & session = config["lanSession"];
	if(!session.isStruct() || !session["originalPresetName"].isString()
		|| !isValidLanPreset(session["originalPreset"]) || !config["presets"].isStruct())
		throw std::runtime_error("Cannot recover an invalid LAN mod session snapshot");

	const std::string & originalName = session["originalPresetName"].String();
	if(originalName.empty() || originalName == LanModSessionState::temporaryPresetName())
		throw std::runtime_error("Cannot recover an invalid LAN mod session snapshot");
}

void LanModSessionState::requirePersonalPresetUnlocked(const JsonNode & config)
{
	if(!config["lanSession"].isNull())
		throw std::runtime_error("Cannot change personal mod presets during a LAN session");
}

void LanModSessionState::begin(JsonNode & config)
{
	const JsonNode & current = config;
	if(!current["lanSession"].isNull())
		throw std::runtime_error("A LAN mod session is already active");

	const JsonNode & activePreset = current["activePreset"];
	const std::string previous = activePreset.isString() ? activePreset.String() : std::string{};
	const auto & sessionName = temporaryPresetName();
	if(previous.empty() || previous == sessionName || !current["presets"].isStruct()
		|| !isValidLanPreset(current["presets"][previous]))
		throw std::runtime_error("Cannot snapshot an invalid active mod preset");
	if(!current["presets"][sessionName].isNull())
		throw std::runtime_error("Reserved LAN mod preset name is already in use");

	const JsonNode original = current["presets"][previous];
	config["lanSession"]["originalPresetName"].String() = previous;
	config["lanSession"]["originalPreset"] = original;
	config["presets"][sessionName] = original;
	config["activePreset"].String() = sessionName;
}

void LanModSessionState::applyInstalledOnlyPlan(JsonNode & config, const ModSyncPlan & plan)
{
	validateLanSessionSnapshot(config);
	if(!plan.downloadOrUpdate.empty() || !plan.reuseCachedArchive.empty())
		throw std::runtime_error("LAN installed-only preparation cannot require mod archives");

	std::set<TModID> toActivate = plan.activate;
	for(const auto & modId : plan.activate)
	{
		for(size_t dot = modId.find('.'); dot != std::string::npos; dot = modId.find('.', dot + 1))
			toActivate.insert(modId.substr(0, dot));
	}

	for(const auto & modId : toActivate)
		if(plan.deactivate.count(modId))
			throw std::runtime_error("LAN mod preparation contains conflicting activation states");

	JsonNode & temporary = config["presets"][temporaryPresetName()];
	for(const auto & modId : plan.deactivate)
	{
		const size_t dot = modId.find('.');
		if(dot == std::string::npos)
			vstd::erase(temporary["mods"].Vector(), JsonNode(modId));
		else
			temporary["settings"][modId.substr(0, dot)][modId.substr(dot + 1)].Bool() = false;
	}

	for(const auto & modId : toActivate)
	{
		const size_t dot = modId.find('.');
		if(dot == std::string::npos)
		{
			if(!vstd::contains(temporary["mods"].Vector(), JsonNode(modId)))
				temporary["mods"].Vector().emplace_back(modId);
		}
		else
			temporary["settings"][modId.substr(0, dot)][modId.substr(dot + 1)].Bool() = true;
	}
}

static bool validLanRejoinTarget(const LanRejoinTarget & target)
{
	return !target.hostname.empty() && target.hostname.size() <= 255 && target.port != 0
		&& std::none_of(target.hostname.begin(), target.hostname.end(), [](unsigned char ch)
		{
			return ch <= 32 || ch == 127;
		});
}

void LanModSessionState::setRejoinTarget(JsonNode & config, const LanRejoinTarget & target)
{
	validateLanSessionSnapshot(config);
	if(!validLanRejoinTarget(target))
		throw std::invalid_argument("LAN rejoin requires a valid host and nonzero port");
	config["lanSession"]["rejoin"]["hostname"].String() = target.hostname;
	config["lanSession"]["rejoin"]["port"].Integer() = target.port;
	config["lanSession"]["rejoin"]["previewCapability"].Bool() = target.previewCapability;
	config["lanSession"]["rejoin"]["loadGame"].Bool() = target.loadGame;
	if(!target.approvedHostModsSha256.empty())
		config["lanSession"]["rejoin"]["approvedHostModsSha256"].String() = target.approvedHostModsSha256;
	auto & playerNames = config["lanSession"]["rejoin"]["playerNames"].Vector();
	playerNames.clear();
	for(const auto & name : target.playerNames)
		playerNames.emplace_back(name);
}


void LanModSessionState::setStagedMods(JsonNode & config, const ModArchiveHashes & staged)
{
	validateLanSessionSnapshot(config);
	if(!config["lanSession"]["stagedMods"].isNull())
		throw std::runtime_error("LAN staged mod selection was already set");
	for(const auto & [rootId, digest] : staged)
	{
		if(rootId.empty() || rootId.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != rootId.npos
			|| !ModArchiveDigest::isSha256(digest))
			throw std::runtime_error("Invalid staged LAN mod identity or SHA-256");
		config["lanSession"]["stagedMods"][rootId].String() = digest;
	}
}

ModArchiveHashes LanModSessionState::stagedModsForControlledRelaunch(const JsonNode & config, const std::string & token)
{
	const JsonNode & current = config;
	const JsonNode & session = current["lanSession"];
	if(!LanRelaunchTokenFormat::isValid(token) || !session.isStruct()
		|| !session["relaunchToken"].isString() || session["relaunchToken"].String() != token)
		return {};
	validateLanSessionSnapshot(config);
	const JsonNode & selected = session["stagedMods"];
	if(selected.isNull())
		return {};
	if(!selected.isStruct())
		throw std::runtime_error("Invalid staged LAN mod selection");
	ModArchiveHashes result;
	for(const auto & [rootId, digest] : selected.Struct())
	{
		if(rootId.empty() || rootId.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != rootId.npos
			|| !digest.isString() || !ModArchiveDigest::isSha256(digest.String()))
			throw std::runtime_error("Invalid staged LAN mod identity or SHA-256");
		result.emplace(rootId, digest.String());
	}
	return result;
}

std::optional<LanRejoinTarget> LanModSessionState::getRejoinTarget(const JsonNode & config)
{
	if(!config["activePreset"].isString() || config["activePreset"].String() != temporaryPresetName())
		return std::nullopt;
	const JsonNode & rejoin = config["lanSession"]["rejoin"];
	if(!rejoin.isStruct() || !rejoin["hostname"].isString()
		|| rejoin["port"].getType() != JsonNode::JsonType::DATA_INTEGER
		|| !rejoin["previewCapability"].isBool())
		return std::nullopt;
	const si64 port = rejoin["port"].Integer();
	if(port <= 0 || port > 65535)
		return std::nullopt;
	LanRejoinTarget target{rejoin["hostname"].String(), static_cast<std::uint16_t>(port),
		rejoin["previewCapability"].Bool()};
	const JsonNode & playerNames = rejoin["playerNames"];
	if(!playerNames.isNull())
	{
		if(!playerNames.isVector())
			return std::nullopt;
		for(const auto & name : playerNames.Vector())
		{
			if(!name.isString())
				return std::nullopt;
			target.playerNames.push_back(name.String());
		}
	}
	const JsonNode & loadGame = rejoin["loadGame"];
	if(!loadGame.isNull())
	{
		if(!loadGame.isBool())
			return std::nullopt;
		target.loadGame = loadGame.Bool();
	}
	const JsonNode & fingerprint = rejoin["approvedHostModsSha256"];
	if(!fingerprint.isNull())
	{
		if(!fingerprint.isString() || !ModArchiveDigest::isSha256(fingerprint.String()))
			return std::nullopt;
		target.approvedHostModsSha256 = fingerprint.String();
	}
	return validLanRejoinTarget(target) ? std::optional<LanRejoinTarget>{target} : std::nullopt;
}

void LanModSessionState::prepareControlledRelaunch(JsonNode & config, const std::string & relaunchToken)
{
	if(!LanRelaunchTokenFormat::isValid(relaunchToken))
		throw std::invalid_argument("Controlled LAN relaunch requires 64 lowercase hexadecimal characters");
	const JsonNode & current = config;
	if(current["lanSession"].isNull())
		throw std::runtime_error("Cannot relaunch without a LAN mod session");
	validateLanSessionSnapshot(current);
	if(!isValidLanPreset(current["presets"][temporaryPresetName()]))
		throw std::runtime_error("Cannot relaunch with an invalid temporary LAN preset");
	if(!current["lanSession"]["relaunchToken"].isNull())
		throw std::runtime_error("LAN mod session already has a pending controlled relaunch");

	config["lanSession"]["relaunchToken"].String() = relaunchToken;
}

LanSessionStartupAction LanModSessionState::recoverOnStartup(JsonNode & config, const std::string & relaunchToken)
{
	const JsonNode & current = config;
	if(current["lanSession"].isNull())
		return LanSessionStartupAction::NONE;

	const JsonNode & session = current["lanSession"];
	if(!session.isStruct())
		throw std::runtime_error("Cannot recover an invalid LAN mod session snapshot");

	const JsonNode & marker = session["relaunchToken"];
	if(LanRelaunchTokenFormat::isValid(relaunchToken) && marker.isString() && marker.String() == relaunchToken)
	{
		validateLanSessionSnapshot(config);
		if(!isValidLanPreset(current["presets"][temporaryPresetName()]))
		{
			restore(config);
			return LanSessionStartupAction::RESTORED;
		}
		// Consume the marker on the first matching startup. Future crashes restore normally.
		config["lanSession"].Struct().erase("relaunchToken");
		return LanSessionStartupAction::RESUMED;
	}

	restore(config);
	return LanSessionStartupAction::RESTORED;
}

bool LanModSessionState::restore(JsonNode & config)
{
	const JsonNode & current = config;
	const JsonNode session = current["lanSession"];
	if(session.isNull())
		return false;

	validateLanSessionSnapshot(config);

	const std::string previous = session["originalPresetName"].String();
	const JsonNode original = session["originalPreset"];

	config["presets"][previous] = original;
	config["activePreset"].String() = previous;
	config["presets"].Struct().erase(temporaryPresetName());
	config.Struct().erase("lanSession");
	return true;
}

bool LanModSessionState::shouldRestoreOnShutdown(const JsonNode & config)
{
	if(config["lanSession"].isNull())
		return false;

	validateLanSessionSnapshot(config);
	// A relaunch marker is consumed by the replacement client, not by the exiting process.
	const JsonNode & marker = config["lanSession"]["relaunchToken"];
	return !isValidLanPreset(config["presets"][temporaryPresetName()])
		|| !marker.isString() || !LanRelaunchTokenFormat::isValid(marker.String());
}

static JsonNode validatedLanRecoveryBackup(const std::filesystem::path & settings, const std::string & original)
{
	JsonParsingSettings parsing;
	parsing.strict = true;
	parsing.mode = JsonParsingSettings::JsonFormatMode::JSON;
	const auto backupPath = LanModRecoveryJournal::pathForSettings(settings);
	JsonNode recovered(original.data(), original.size(), parsing, backupPath.string());

	JsonNode validation = recovered;
	LanModSessionState::begin(validation);
	return recovered;
}

JsonNode LanModSessionState::restoreDamagedSettings(const std::filesystem::path & settings)
{
	const std::string original = LanModRecoveryJournal::load(settings);
	JsonNode recovered = validatedLanRecoveryBackup(settings, original);
	if(!AtomicModSettings::replace(settings, original))
		throw std::runtime_error("LAN backup restoration could not sync the mod settings directory");
	if(!LanModRecoveryJournal::discard(settings))
		logGlobal->warn("Could not remove independent LAN backup after recovering damaged settings");
	return recovered;
}

bool LanModSessionState::restoreCompleteBackupIfPresent(JsonNode & restored, const JsonNode & sessionConfig,
	const std::filesystem::path & settings)
{
	const auto backupPath = LanModRecoveryJournal::pathForSettings(settings);
	std::error_code lookupError;
	const auto backupStatus = std::filesystem::symlink_status(backupPath, lookupError);
	if(lookupError == std::errc::no_such_file_or_directory
		|| backupStatus.type() == std::filesystem::file_type::not_found)
		return false;
	if(lookupError)
		throw std::filesystem::filesystem_error("Cannot inspect LAN recovery backup", backupPath, lookupError);

	validateLanSessionSnapshot(sessionConfig);
	const std::string original = LanModRecoveryJournal::load(settings);
	JsonNode complete = validatedLanRecoveryBackup(settings, original);
	const std::string & originalPreset = sessionConfig["lanSession"]["originalPresetName"].String();
	const JsonNode & checked = complete;
	if(checked["activePreset"].String() != originalPreset
		|| checked["presets"][originalPreset].toCompactString()
			!= sessionConfig["lanSession"]["originalPreset"].toCompactString())
		throw std::runtime_error("LAN recovery backup does not match the active session snapshot");

	restored = std::move(complete);
	return true;
}

static std::filesystem::path modSettingsStoragePath(const JsonPath & path)
{
	const auto destination = CResourceHandler::get()->getResourceName(ResourcePath(path));
	if(!destination)
		throw std::runtime_error("Cannot locate the mod settings file");
	return std::filesystem::path(destination->native());
}

static void discardLanRecoveryIfMatching(const std::filesystem::path & settings, const JsonNode & restored)
{
	try
	{
		if(!LanModRecoveryJournal::discardIfMatches(settings, restored.toCompactString()))
			logGlobal->warn("Retaining LAN recovery backup: restored settings differ from the original");
	}
	catch(const std::exception & error)
	{
		logGlobal->warn("Cannot verify LAN recovery backup after restoration; retaining it: %s", error.what());
	}
}

ModsPresetState::ModsPresetState(bool useTestPreset, const std::string & relaunchToken)
	: settingsPath(JsonPath::builtin(useTestPreset ? "config/testModSettings.json" : "config/modSettings.json"))
{
	if(CResourceHandler::get("local")->existsResource(ResourcePath(settingsPath)))
	{
		const auto primary = modSettingsStoragePath(settingsPath);
		const auto backup = LanModRecoveryJournal::pathForSettings(primary);
		std::error_code lookupError;
		const auto backupStatus = std::filesystem::symlink_status(backup, lookupError);
		if(lookupError && lookupError != std::errc::no_such_file_or_directory)
			throw std::filesystem::filesystem_error("Cannot inspect LAN mod recovery backup", backup, lookupError);

		if(backupStatus.type() == std::filesystem::file_type::not_found)
			modConfig = JsonNode(settingsPath);
		else
		{
			try
			{
				JsonParsingSettings parsing;
				parsing.strict = true;
				modConfig = JsonNode(settingsPath, parsing);
				const JsonNode & current = modConfig;
				if(!current.isStruct() || !current["activePreset"].isString() || !current["presets"].isStruct())
					throw std::runtime_error("Invalid LAN mod settings structure");
				if(!current["lanSession"].isNull())
				{
					JsonNode checked = current;
					LanModSessionState::recoverOnStartup(checked, relaunchToken);
				}
				else if(!isValidLanPreset(current["presets"][current["activePreset"].String()]))
					throw std::runtime_error("Invalid active mod preset with pending LAN recovery backup");
				else
				{
					try
					{
						if(!LanModRecoveryJournal::discardIfMatches(primary, current.toCompactString()))
							logGlobal->warn("Retaining unresolved LAN recovery backup; another LAN session cannot start");
					}
					catch(const std::exception & error)
					{
						logGlobal->warn("Retaining unreadable LAN recovery backup: %s", error.what());
					}
				}
			}
			catch(const std::exception & error)
			{
				logGlobal->warn("Damaged mod settings with LAN recovery backup: %s", error.what());
				modConfig = LanModSessionState::restoreDamagedSettings(primary);
			}
		}
	}
	else
	{
		// Probably new install. Create initial configuration
		CResourceHandler::get("local")->createResource(settingsPath.getOriginalName() + ".json");
	}

	// Preserve the LAN preset only for a matching one-shot controlled relaunch.
	if(!modConfig["lanSession"].isNull())
	{
		JsonNode recovered = modConfig;
		const auto action = LanModSessionState::recoverOnStartup(recovered, relaunchToken);
		if(action == LanSessionStartupAction::RESTORED)
			LanModSessionState::restoreCompleteBackupIfPresent(recovered, modConfig, modSettingsStoragePath(settingsPath));
		if(!saveConfigurationState(recovered))
			throw std::runtime_error("LAN session startup recovery could not sync the mod settings directory");
		modConfig = std::move(recovered);
		if(action == LanSessionStartupAction::RESTORED)
			discardLanRecoveryIfMatching(modSettingsStoragePath(settingsPath), modConfig);
		if(action == LanSessionStartupAction::RESUMED)
		{
			pendingLanRejoinTarget = LanModSessionState::getRejoinTarget(modConfig);
			logGlobal->info("Resumed a controlled LAN relaunch; recovery snapshot remains available");
		}
		else
			logGlobal->warn("Recovered the original mod preset after an interrupted LAN session");
	}

	if(modConfig["presets"].isNull() || modConfig["presets"].Struct().empty())
	{
		modConfig["activePreset"] = JsonNode("default");
		if(modConfig["activeMods"].isNull())
			createInitialPreset(); // new install
		else
			importInitialPreset(); // 1.5 format import
	}

	auto allPresets = getAllPresets();
	if (!vstd::contains(allPresets, modConfig["activePreset"].String()))
		modConfig["activePreset"] = JsonNode(allPresets.front());

	logGlobal->debug("Loading following mod settings: %s", modConfig.toCompactString());
}

void ModsPresetState::beginLanSession()
{
	JsonNode pending = modConfig;
	LanModSessionState::begin(pending);
	// Persist independent recovery bytes before switching the active preset on disk.
	if(!LanModRecoveryJournal::persist(modSettingsStoragePath(settingsPath), modConfig.toCompactString()))
		throw std::runtime_error("LAN recovery backup could not sync the mod settings directory");
	if(!saveConfigurationState(pending))
		throw std::runtime_error("LAN session snapshot could not sync the mod settings directory");
	modConfig = std::move(pending);
}

void ModsPresetState::prepareInstalledOnlyLanSession(const ModSyncPlan & plan, const LanRejoinTarget & target)
{
	prepareLanSessionWithStagedMods(plan, target, {});
}

void ModsPresetState::prepareLanSessionWithStagedMods(const ModSyncPlan & plan,
	const LanRejoinTarget & target, const ModArchiveHashes & staged)
{
	beginLanSession();
	try
	{
		JsonNode pending = modConfig;
		LanModSessionState::applyInstalledOnlyPlan(pending, plan);
		LanModSessionState::setRejoinTarget(pending, target);
		if(!staged.empty())
			LanModSessionState::setStagedMods(pending, staged);
		if(!saveConfigurationState(pending))
			throw std::runtime_error("LAN temporary preset could not be synced to disk");
		modConfig = std::move(pending);
	}
	catch(...)
	{
		restoreLanSession();
		throw;
	}
}

std::optional<LanRejoinTarget> ModsPresetState::consumeLanRejoinTarget()
{
	auto target = pendingLanRejoinTarget;
	pendingLanRejoinTarget.reset();
	return target;
}

void ModsPresetState::prepareControlledRelaunch(const std::string & relaunchToken)
{
	JsonNode pending = modConfig;
	LanModSessionState::prepareControlledRelaunch(pending, relaunchToken);
	if(!saveConfigurationState(pending))
		throw std::runtime_error("LAN relaunch marker could not sync the mod settings directory");
	modConfig = std::move(pending);
}

void ModsPresetState::restoreLanSession()
{
	pendingLanRejoinTarget.reset();
	JsonNode restored = modConfig;
	if(LanModSessionState::restore(restored))
	{
		LanModSessionState::restoreCompleteBackupIfPresent(restored, modConfig, modSettingsStoragePath(settingsPath));
		if(!saveConfigurationState(restored))
			throw std::runtime_error("LAN session restoration could not sync the mod settings directory");
		modConfig = std::move(restored);
		discardLanRecoveryIfMatching(modSettingsStoragePath(settingsPath), modConfig);
	}
}

void ModsPresetState::restoreLanSessionOnShutdown()
{
	const JsonNode & current = modConfig;
	if(!LanModSessionState::shouldRestoreOnShutdown(current))
		return;

	// Do not discard the in-memory recovery journal until the rollback is safely on disk.
	restoreLanSession();
}

void ModsPresetState::createInitialPreset()
{
	// TODO: scan mods directory for all its content? Probably unnecessary since this looks like new install, but who knows?
	modConfig["presets"]["default"]["mods"].Vector().emplace_back("vcmi");
}

void ModsPresetState::importInitialPreset()
{
	JsonNode preset;

	for(const auto & mod : modConfig["activeMods"].Struct())
	{
		if(mod.second["active"].Bool())
			preset["mods"].Vector().emplace_back(mod.first);

		for(const auto & submod : mod.second["mods"].Struct())
			preset["settings"][mod.first][submod.first] = submod.second["active"];
	}
	modConfig["presets"]["default"] = preset;
}

const JsonNode & ModsPresetState::getActivePresetConfig() const
{
	const std::string & currentPresetName = modConfig["activePreset"].String();
	const JsonNode & currentPreset = modConfig["presets"][currentPresetName];
	return currentPreset;
}

TModList ModsPresetState::getActiveRootMods() const
{
	return getRootMods(getActivePreset());
}

TModList ModsPresetState::getRootMods(const std::string & presetName) const
{
	const JsonNode & modsToActivateJson = modConfig["presets"][presetName]["mods"];
	auto modsToActivate = modsToActivateJson.convertTo<std::vector<TModID>>();
	if (!vstd::contains(modsToActivate, ModScope::scopeBuiltin()))
		modsToActivate.push_back(ModScope::scopeBuiltin());
	return modsToActivate;
}

std::map<TModID, bool> ModsPresetState::getModSettings(const TModID & modID) const
{
	const JsonNode & modSettingsJson = getActivePresetConfig()["settings"][modID];
	auto modSettings = modSettingsJson.convertTo<std::map<TModID, bool>>();
	return modSettings;
}

std::optional<uint32_t> ModsPresetState::getValidatedChecksum(const TModID & modName) const
{
	const JsonNode & node = modConfig["validatedMods"][modName];
	if (node.isNull())
		return std::nullopt;
	else
		return node.Integer();
}

void ModsPresetState::setModActive(const TModID & modID, bool isActive)
{
	size_t dotPos = modID.find('.');

	if(dotPos != std::string::npos)
	{
		std::string rootMod = modID.substr(0, dotPos);
		std::string settingID = modID.substr(dotPos + 1);
		setSettingActive(rootMod, settingID, isActive);
	}
	else
	{
		if (isActive)
			addRootMod(modID);
		else
			eraseRootMod(modID);
	}
}

void ModsPresetState::addRootMod(const TModID & modName)
{
	const std::string & currentPresetName = modConfig["activePreset"].String();
	JsonNode & currentPreset = modConfig["presets"][currentPresetName];

	if (!vstd::contains(currentPreset["mods"].Vector(), JsonNode(modName)))
		currentPreset["mods"].Vector().emplace_back(modName);
}

void ModsPresetState::setSettingActive(const TModID & modName, const TModID & settingName, bool isActive)
{
	const std::string & currentPresetName = modConfig["activePreset"].String();
	JsonNode & currentPreset = modConfig["presets"][currentPresetName];

	currentPreset["settings"][modName][settingName].Bool() = isActive;
}

void ModsPresetState::removeOldMods(const TModList & modsToKeep)
{
	const std::string & currentPresetName = modConfig["activePreset"].String();
	JsonNode & currentPreset = modConfig["presets"][currentPresetName];

	vstd::erase_if(currentPreset["mods"].Vector(), [&](const JsonNode & entry){
		return !vstd::contains(modsToKeep, entry.String());
	});

	vstd::erase_if(currentPreset["settings"].Struct(), [&](const auto & entry){
		return !vstd::contains(modsToKeep, entry.first);
	});

	for (auto & modSettings : currentPreset["settings"].Struct())
	{
		vstd::erase_if(modSettings.second.Struct(), [&](const auto & entry){
			return !vstd::contains(modsToKeep, modSettings.first + "." + entry.first);
		});
	}
}

void ModsPresetState::eraseRootMod(const TModID & modName)
{
	const std::string & currentPresetName = modConfig["activePreset"].String();
	JsonNode & currentPreset = modConfig["presets"][currentPresetName];
	vstd::erase(currentPreset["mods"].Vector(), JsonNode(modName));
}

void ModsPresetState::eraseModSetting(const TModID & modName, const TModID & settingName)
{
	const std::string & currentPresetName = modConfig["activePreset"].String();
	JsonNode & currentPreset = modConfig["presets"][currentPresetName];
	currentPreset["settings"][modName].Struct().erase(settingName);
}

std::vector<TModID> ModsPresetState::getActiveMods() const
{
	TModList activeRootMods = getActiveRootMods();
	TModList allActiveMods;

	for(const auto & activeMod : activeRootMods)
	{
		assert(!vstd::contains(allActiveMods, activeMod));
		allActiveMods.push_back(activeMod);

		for(const auto & submod : getModSettings(activeMod))
		{
			if(submod.second)
			{
				assert(!vstd::contains(allActiveMods, activeMod + '.' + submod.first));
				allActiveMods.push_back(activeMod + '.' + submod.first);
			}
		}
	}
	return allActiveMods;
}

void ModsPresetState::setValidatedChecksum(const TModID & modName, std::optional<uint32_t> value)
{
	if (value.has_value())
		modConfig["validatedMods"][modName].Integer() = *value;
	else
		modConfig["validatedMods"].Struct().erase(modName);
}

bool ModsPresetState::saveConfigurationState(const JsonNode & state) const
{
	const bool directorySynced = AtomicModSettings::replace(modSettingsStoragePath(settingsPath), state.toCompactString());
	if(!directorySynced)
		logGlobal->warn("Mod settings replaced but parent directory sync failed; power-loss durability is not guaranteed");
	return directorySynced;
}

void ModsPresetState::saveConfigurationState() const
{
	saveConfigurationState(modConfig);
}

void ModsPresetState::createNewPreset(const std::string & presetName)
{
	LanModSessionState::requirePersonalPresetUnlocked(modConfig);
	if (modConfig["presets"][presetName].isNull())
		modConfig["presets"][presetName]["mods"].Vector().emplace_back("vcmi");
}

void ModsPresetState::deletePreset(const std::string & presetName)
{
	LanModSessionState::requirePersonalPresetUnlocked(modConfig);
	if (modConfig["presets"].Struct().size() < 2)
		throw std::runtime_error("Unable to delete last preset!");

	modConfig["presets"].Struct().erase(presetName);
}

void ModsPresetState::activatePreset(const std::string & presetName)
{
	if(presetName != LanModSessionState::temporaryPresetName())
		LanModSessionState::requirePersonalPresetUnlocked(modConfig);
	if (modConfig["presets"].Struct().count(presetName) == 0)
		throw std::runtime_error("Unable to activate non-exinsting preset!");

	modConfig["activePreset"].String() = presetName;
}

void ModsPresetState::renamePreset(const std::string & oldPresetName, const std::string & newPresetName)
{
	LanModSessionState::requirePersonalPresetUnlocked(modConfig);
	if (oldPresetName == newPresetName)
		throw std::runtime_error("Unable to rename preset to the same name!");

	if (modConfig["presets"].Struct().count(oldPresetName) == 0)
		throw std::runtime_error("Unable to rename non-existing last preset!");

	if (modConfig["presets"].Struct().count(newPresetName) != 0)
		throw std::runtime_error("Unable to rename preset - preset with such name already exists!");

	modConfig["presets"][newPresetName] = modConfig["presets"][oldPresetName];
	modConfig["presets"].Struct().erase(oldPresetName);

	if (modConfig["activePreset"].String() == oldPresetName)
		modConfig["activePreset"].String() = newPresetName;
}

std::vector<std::string> ModsPresetState::getAllPresets() const
{
	std::vector<std::string> presets;

	for (const auto & preset : modConfig["presets"].Struct())
		presets.push_back(preset.first);

	return presets;
}

std::string ModsPresetState::getActivePreset() const
{
	return modConfig["activePreset"].String();
}

JsonNode ModsPresetState::exportCurrentPreset() const
{
	JsonNode data = getActivePresetConfig();
	std::string presetName = getActivePreset();

	data["name"] = JsonNode(presetName);

	vstd::erase_if(data["settings"].Struct(), [&](const auto & pair){
		return !vstd::contains(data["mods"].Vector(), JsonNode(pair.first));
	});

	return data;
}

std::string ModsPresetState::importPreset(const JsonNode & newConfig)
{
	LanModSessionState::requirePersonalPresetUnlocked(modConfig);
	std::string importedPresetName = newConfig["name"].String();

	if (importedPresetName.empty())
		throw std::runtime_error("Attempt to import invalid preset");

	modConfig["presets"][importedPresetName] = newConfig;
	modConfig["presets"][importedPresetName].Struct().erase("name");

	return importedPresetName;
}

ModsStorage::ModsStorage(const std::vector<TModID> & modsToLoad, const JsonNode & repositoryList)
{
	JsonNode coreModConfig(JsonPath::builtin("config/gameConfig.json"));
	coreModConfig.setModScope(ModScope::scopeBuiltin());
	mods.try_emplace(ModScope::scopeBuiltin(), ModScope::scopeBuiltin(), coreModConfig, JsonNode());

	// MODS COMPATIBILITY: in 1.6, repository list contains mod list directly, in 1.7 it is located in 'availableMods' node
	const auto & availableRepositoryMods = repositoryList["availableMods"].isNull() ? repositoryList : repositoryList["availableMods"];

	for(auto modID : modsToLoad)
	{
		if(ModScope::isScopeReserved(modID))
			continue;

		JsonNode modConfig(getModDefinitionFile(modID));
		modConfig.setModScope(modID);

		if(modConfig["modType"].isNull())
		{
			logMod->error("Can not load mod %s - invalid mod config file!", modID);
			continue;
		}

		// existsResource may report a stale entry (e.g. mod updated to a version without description);
		// tolerate a failing load since description is optional and index/disk can diverge until reload
		if (CResourceHandler::get()->existsResource(getModDescriptionFile(modID)))
		{
			try
			{
				auto data = CResourceHandler::get()->load(getModDescriptionFile(modID))->readAll();
				std::string modDescriptions(reinterpret_cast<const char *>(data.first.get()), data.second);
				ModDescription::mergeModDescriptions(modConfig, modDescriptions);
			}
			catch (const std::exception & e)
			{
				logMod->warn("Failed to load description for mod %s: %s", modID, e.what());
			}
		}

		mods.try_emplace(modID, modID, modConfig, availableRepositoryMods[modID]);
	}

	for(const auto & mod : availableRepositoryMods.Struct())
	{
		if (vstd::contains(modsToLoad, mod.first))
			continue;

		if (mod.second["modType"].isNull() || mod.second["name"].isNull())
			continue;

		mods.try_emplace(mod.first, mod.first, JsonNode(), mod.second);
	}
}

const ModDescription & ModsStorage::getMod(const TModID & fullID) const
{
	try {
		return mods.at(fullID);
	}
	catch (const std::out_of_range & )
	{
		// rethrow with better error message
		throw std::out_of_range("Failed to find mod " + fullID);
	}
}

TModList ModsStorage::getAllMods() const
{
	TModList result;
	for (const auto & mod : mods)
		result.push_back(mod.first);

	return result;
}

ModManager::ModManager()
	:ModManager(JsonNode())
{
}

ModManager::ModManager(const JsonNode & repositoryList, bool useTestPreset, const TModList & newMods, const std::string & relaunchToken)
	: modsState(std::make_unique<ModsState>())
	, modsPreset(std::make_unique<ModsPresetState>(useTestPreset, relaunchToken))
{
	modsStorage = std::make_unique<ModsStorage>(modsState->getInstalledMods(), repositoryList);

	eraseMissingModsFromPreset();
	addNewModsToPreset();

	// Sync roe-demo enabled state with demo data presence
	syncDemoModState();

	// resolved together with active mods, so that mods that depend on new mods are not disabled as broken
	for(const auto & modID : newMods)
		if(vstd::contains(getInstalledValidMods(), modID))
			modsPreset->setModActive(modID, true);

	std::vector<TModID> desiredModList = modsPreset->getActiveMods();
	// Force-activate the test fixtures mod regardless of any persisted preset or its
	// keepDisabled flag; the preset file lives in the user config dir and survives runs.
	if(useTestPreset && !vstd::contains(desiredModList, "vcmi-test"))
		desiredModList.push_back("vcmi-test");
	ModDependenciesResolver newResolver(desiredModList, *modsStorage);
	updatePreset(newResolver);
}

ModManager::~ModManager() = default;

const ModDescription & ModManager::getModDescription(const TModID & modID) const
{
	assert(boost::to_lower_copy(modID) == modID);
	return modsStorage->getMod(modID);
}

bool ModManager::isModSettingActive(const TModID & rootModID, const TModID & modSettingID) const
{
	return modsPreset->getModSettings(rootModID).at(modSettingID);
}

std::map<TModID, bool> ModManager::getModSettings(const TModID & rootModID) const
{
	return modsPreset->getModSettings(rootModID);
}

bool ModManager::isModActive(const TModID & modID) const
{
	return vstd::contains(getActiveMods(), modID);
}

const TModList & ModManager::getActiveMods() const
{
	return depedencyResolver->getActiveMods();
}

std::optional<uint32_t> ModManager::getValidatedChecksum(const TModID & modName) const
{
	return modsPreset->getValidatedChecksum(modName);
}

void ModManager::setValidatedChecksum(const TModID & modName, std::optional<uint32_t> value)
{
	modsPreset->setValidatedChecksum(modName, value);
}

void ModManager::saveConfigurationState() const
{
	modsPreset->saveConfigurationState();
}

TModList ModManager::getAllMods() const
{
	return modsStorage->getAllMods();
}

double ModManager::getInstalledModSizeMegabytes(const TModID & modName) const
{
	return modsState->getInstalledModSizeMegabytes(modName);
}

void ModManager::eraseMissingModsFromPreset()
{
	const TModList & installedMods = getInstalledValidMods();
	const TModList & rootMods = modsPreset->getActiveRootMods();

	modsPreset->removeOldMods(installedMods);

	for(const auto & rootMod : rootMods)
	{
		const auto & modSettings = modsPreset->getModSettings(rootMod);

		for(const auto & modSetting : modSettings)
		{
			TModID fullModID = rootMod + '.' + modSetting.first;
			if(!vstd::contains(installedMods, fullModID))
			{
				modsPreset->eraseModSetting(rootMod, modSetting.first);
				continue;
			}
		}
	}
}

void ModManager::addNewModsToPreset()
{
	const TModList & installedMods = getInstalledValidMods();

	for(const auto & modID : installedMods)
	{
		size_t dotPos = modID.find('.');

		if(dotPos == std::string::npos)
			continue; // only look up submods aka mod settings

		std::string rootMod = modID.substr(0, dotPos);
		std::string settingID = modID.substr(dotPos + 1);

		const auto & modSettings = modsPreset->getModSettings(rootMod);

		if (!modSettings.count(settingID))
			modsPreset->setSettingActive(rootMod, settingID, !modsStorage->getMod(modID).keepDisabled());
	}
}

void ModManager::syncDemoModState()
{
	static const TModID demoModID = "roe-demo";

	if (!vstd::contains(modsStorage->getAllMods(), demoModID))
		return;

	if (!modsStorage->getMod(demoModID).isInstalled())
		return;

	bool hasFullData = CResourceHandler::get()->existsResource(ResourcePath("DATA/TENTCOLR.TXT"));
	bool hasDemoData = !hasFullData && CResourceHandler::get()->existsResource(ResourcePath("MAPS/H3DEMO.H3M"));

	bool isActive = vstd::contains(modsPreset->getActiveRootMods(), demoModID);

	if (hasDemoData && !isActive)
		modsPreset->setModActive(demoModID, true);
	else if (!hasDemoData && isActive)
		modsPreset->setModActive(demoModID, false);
}

TModList ModManager::getInstalledValidMods() const
{
	TModList installedMods = modsState->getInstalledMods();
	TModList validMods = modsStorage->getAllMods();

	TModList result;
	for (const auto & modID : installedMods)
		if (vstd::contains(validMods, modID))
			result.push_back(modID);

	return result;
}

TModList ModManager::collectDependenciesRecursive(const TModID & modID) const
{
	TModList result;
	TModList toTest;

	toTest.push_back(modID);
	while (!toTest.empty())
	{
		TModID currentModID = toTest.back();
		const auto & currentMod = getModDescription(currentModID);
		toTest.pop_back();
		result.push_back(currentModID);

		if (!currentMod.isInstalled())
			throw std::runtime_error("Unable to enable mod " + modID + "! Dependency " + currentModID + " is not installed!");

		for (const auto & dependency : currentMod.getDependencies())
		{
			if (!vstd::contains(result, dependency))
				toTest.push_back(dependency);
		}
	}

	return result;
}

void ModManager::tryEnableMods(const TModList & modList)
{
	TModList requiredActiveMods;
	TModList additionalActiveMods = getActiveMods();

	for (const auto & modName : modList)
	{
		for (const auto & dependency : collectDependenciesRecursive(modName))
		{
			if (!vstd::contains(requiredActiveMods, dependency))
			{
				requiredActiveMods.push_back(dependency);
				vstd::erase(additionalActiveMods, dependency);
			}
		}

		assert(!vstd::contains(additionalActiveMods, modName));
		assert(vstd::contains(requiredActiveMods, modName));// FIXME: fails on attempt to enable broken mod / translation to other language
	}

	ModDependenciesResolver testResolver(requiredActiveMods, *modsStorage);

	testResolver.tryAddMods(additionalActiveMods, *modsStorage);

	TModList additionalActiveSubmods;
	for (const auto & modName : modList)
	{
		if (modName.find('.') != std::string::npos)
			continue;

		auto modSettings = modsPreset->getModSettings(modName);
		for (const auto & entry : modSettings)
		{
			TModID fullModID = modName + '.' + entry.first;
			if (entry.second && !vstd::contains(requiredActiveMods, fullModID))
				additionalActiveSubmods.push_back(fullModID);
		}
	}

	testResolver.tryAddMods(additionalActiveSubmods, *modsStorage);

	for (const auto & modName : modList)
		if (!vstd::contains(testResolver.getActiveMods(), modName))
			logGlobal->error("Failed to enable mod '%s'! This may be caused by a recursive dependency!", modName);

	updatePreset(testResolver);
}

void ModManager::tryDisableMod(const TModID & modName)
{
	auto desiredActiveMods = getActiveMods();
	assert(vstd::contains(desiredActiveMods, modName));

	vstd::erase(desiredActiveMods, modName);

	ModDependenciesResolver testResolver(desiredActiveMods, *modsStorage);

	if (vstd::contains(testResolver.getActiveMods(), modName))
		throw std::runtime_error("Failed to disable mod! Mod " + modName + " remains enabled!");

	modsPreset->setModActive(modName, false);
	updatePreset(testResolver);
}

void ModManager::updatePreset(const ModDependenciesResolver & testResolver)
{
	const auto & newActiveMods = testResolver.getActiveMods();
	const auto & newBrokenMods = testResolver.getBrokenMods();

	for (const auto & modID : newActiveMods)
	{
		assert(vstd::contains(getInstalledValidMods(), modID));
		modsPreset->setModActive(modID, true);
	}

	for (const auto & modID : newBrokenMods)
	{
		const auto & mod = getModDescription(modID);
		if (mod.getTopParentID().empty() || vstd::contains(newActiveMods, mod.getTopParentID()))
			modsPreset->setModActive(modID, false);
	}

	std::vector<TModID> desiredModList = modsPreset->getActiveMods();

	// Try to enable all existing compatibility patches. Ignore on failure
	for (const auto & rootMod : modsPreset->getActiveRootMods())
	{
		for (const auto & modSetting : modsPreset->getModSettings(rootMod))
		{
			if (modSetting.second)
				continue;

			TModID fullModID = rootMod + '.' + modSetting.first;
			const auto & modDescription = modsStorage->getMod(fullModID);

			if (modDescription.isCompatibility())
				desiredModList.push_back(fullModID);
		}
	}

	depedencyResolver = std::make_unique<ModDependenciesResolver>(desiredModList, *modsStorage);
	modsPreset->saveConfigurationState();
}

ModDependenciesResolver::ModDependenciesResolver(const TModList & modsToResolve, const ModsStorage & storage)
{
	tryAddMods(modsToResolve, storage);
}

const TModList & ModDependenciesResolver::getActiveMods() const
{
	return activeMods;
}

const TModList & ModDependenciesResolver::getBrokenMods() const
{
	return brokenMods;
}

void ModDependenciesResolver::tryAddMods(TModList modsToResolve, const ModsStorage & storage)
{
	// Topological sort algorithm.
	std::ranges::sort(modsToResolve); // Sort mods per name
	std::vector<TModID> sortedValidMods(activeMods.begin(), activeMods.end()); // Vector keeps order of elements (LIFO)
	std::set<TModID> resolvedModIDs(activeMods.begin(), activeMods.end()); // Use a set for validation for performance reason, but set does not keep order of elements
	std::set<TModID> notResolvedModIDs(modsToResolve.begin(), modsToResolve.end()); // Use a set for validation for performance reason

	enum class ModResolveStatus {
		RESOLVED, // ok - mod can be added to load order
		WAITING, // maybe - wait for more iterations before deciding
		BROKEN // fail - this mod definitely can't be loaded
	};

	// Mod is resolved if it has no dependencies or all its dependencies are already resolved
	auto isResolved = [&](const ModDescription & mod) -> ModResolveStatus
	{
		if (mod.isTranslation() && CGeneralTextHandler::getPreferredLanguage() != mod.getBaseLanguage())
			return ModResolveStatus::BROKEN;

		if(!mod.isCompatible())
			return ModResolveStatus::BROKEN;

		for(const TModID & dependency : mod.getDependencies())
		{
			if (vstd::contains(sortedValidMods, dependency))
				continue;

			if (vstd::contains(notResolvedModIDs, dependency))
				return ModResolveStatus::WAITING;

			// either not in load list, or dependency is also broken
			return ModResolveStatus::BROKEN;
		}

		for(const TModID & softDependency : mod.getSoftDependencies())
			if(vstd::contains(notResolvedModIDs, softDependency))
				return ModResolveStatus::WAITING;

		for(const TModID & conflict : mod.getConflicts())
			if(vstd::contains(resolvedModIDs, conflict))
				return ModResolveStatus::BROKEN;

		for(const TModID & reverseConflict : resolvedModIDs)
			if(vstd::contains(storage.getMod(reverseConflict).getConflicts(), mod.getID()))
				return ModResolveStatus::BROKEN;

		return ModResolveStatus::RESOLVED;
	};

	while(true)
	{
		std::set<TModID> resolvedOnCurrentTreeLevel;
		for(auto it = modsToResolve.begin(); it != modsToResolve.end();) // One iteration - one level of mods tree
		{
			ModResolveStatus status = isResolved(storage.getMod(*it));

			if (status == ModResolveStatus::RESOLVED)
			{
				resolvedOnCurrentTreeLevel.insert(*it); // Not to the resolvedModIDs, so current node children will be resolved on the next iteration
				assert(!vstd::contains(sortedValidMods, *it));
				sortedValidMods.push_back(*it);
				it = modsToResolve.erase(it);
				continue;
			}
			if (status == ModResolveStatus::BROKEN)
			{
				resolvedOnCurrentTreeLevel.insert(*it);
				brokenMods.push_back(*it);
				it = modsToResolve.erase(it);
				continue;
			}

			it++;
		}
		if(!resolvedOnCurrentTreeLevel.empty())
		{
			resolvedModIDs.insert(resolvedOnCurrentTreeLevel.begin(), resolvedOnCurrentTreeLevel.end());
			for(const auto & it : resolvedOnCurrentTreeLevel)
				notResolvedModIDs.erase(it);
			continue;
		}
		// If there are no valid mods on the current mods tree level, no more mod can be resolved, should be ended.
		break;
	}

	assert(!sortedValidMods.empty());
	activeMods = sortedValidMods;
	brokenMods.insert(brokenMods.end(), modsToResolve.begin(), modsToResolve.end());
}

void ModManager::createNewPreset(const std::string & presetName)
{
	modsPreset->createNewPreset(presetName);
	modsPreset->saveConfigurationState();
}

void ModManager::deletePreset(const std::string & presetName)
{
	modsPreset->deletePreset(presetName);
	modsPreset->saveConfigurationState();
}

void ModManager::activatePreset(const std::string & presetName)
{
	modsPreset->activatePreset(presetName);
	modsPreset->saveConfigurationState();
}

void ModManager::beginLanSession()
{
	modsPreset->beginLanSession();
}

void ModManager::prepareInstalledOnlyLanSession(const ModSyncPlan & plan, const LanRejoinTarget & target, const ModCompatibilityInfo & hostMods)
{
	modsPreset->prepareInstalledOnlyLanSession(plan, target);
	try
	{
		// The startup resolver can drop mods with missing dependencies or conflicts.
		// Check the staged preset before requesting a restart, while rollback is possible.
		ModDependenciesResolver resolved(modsPreset->getActiveMods(), *modsStorage);
		if(!plan.matchesResolvedHostMods(hostMods, resolved.getActiveMods()))
			throw std::runtime_error("LAN temporary preset cannot resolve the host's gameplay mods");
	}
	catch(...)
	{
		modsPreset->restoreLanSession();
		throw;
	}
}


void ModManager::prepareLanSessionWithStagedMods(const ModSyncPlan & plan,
	const LanRejoinTarget & target, const ModArchiveHashes & staged)
{
	if(staged.empty())
		throw std::runtime_error("LAN staged preparation requires a verified host archive");
	// Unknown host mods are deliberately not added to the current resolver: they
	// become discoverable only in the next process after the controlled mount.
	// Its fresh lobby preview verifies every version and the host fingerprint.
	modsPreset->prepareLanSessionWithStagedMods(plan, target, staged);
}

std::optional<LanRejoinTarget> ModManager::consumeLanRejoinTarget()
{
	return modsPreset->consumeLanRejoinTarget();
}

std::string ModManager::prepareControlledRelaunch()
{
	const std::string token = LanRelaunchToken::generate();
	modsPreset->prepareControlledRelaunch(token);
	return token;
}

void ModManager::prepareControlledRelaunch(const std::string & relaunchToken)
{
	modsPreset->prepareControlledRelaunch(relaunchToken);
}

void ModManager::restoreLanSession()
{
	modsPreset->restoreLanSession();
}

void ModManager::restoreLanSessionOnShutdown()
{
	modsPreset->restoreLanSessionOnShutdown();
}

void ModManager::renamePreset(const std::string & oldPresetName, const std::string & newPresetName)
{
	modsPreset->renamePreset(oldPresetName, newPresetName);
	modsPreset->saveConfigurationState();
}

std::vector<std::string> ModManager::getAllPresets() const
{
	return modsPreset->getAllPresets();
}

std::string ModManager::getActivePreset() const
{
	return modsPreset->getActivePreset();
}

JsonNode ModManager::exportCurrentPreset() const
{
	return modsPreset->exportCurrentPreset();
}

std::tuple<std::string, TModList> ModManager::importPreset(const JsonNode & data)
{
	std::string presetName = modsPreset->importPreset(data);

	TModList requiredMods = modsPreset->getRootMods(presetName);
	TModList installedMods = getInstalledValidMods();

	TModList missingMods;
	for (const auto & modID : requiredMods)
	{
		if (!vstd::contains(installedMods, modID))
			missingMods.push_back(modID);
	}

	modsPreset->saveConfigurationState();

	return {presetName, missingMods};
}
