/*
 * CModHandler.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../json/JsonNode.h"
#include "ModVerificationInfo.h"
#include "LanRejoinTarget.h"

#include <atomic>

class CModHandler;
class ModDescription;
class CContentHandler;
class ResourcePath;
class ModManager;
class ISimpleResourceLoader;

using TModID = std::string;

class DLL_LINKAGE CModHandler final : boost::noncopyable
{
	std::unique_ptr<ModManager> modManager;
	std::atomic_bool lanRelaunchRequested{false};
	std::map<std::string, uint32_t> modChecksums;
	std::set<std::string> validationPassed;

	/// Contents of all files that mods list in their mod.json, in the same layout as mod.json itself
	std::map<TModID, JsonNode> modContent;

	/// Mods that list a file that does not exist in their filesystem
	std::set<TModID> modsWithMissingFiles;

	/// Reads files of all active mods and computes their checksums
	void loadModContent();
	JsonNode loadModContent(const TModID & modName, const std::vector<std::string> & contentTypes, const std::string & preferredLanguage, uint32_t & checksum, bool & isValid) const;

	void loadTranslation(const TModID & modName);
	void checkModFilesystemsConflicts(const std::map<TModID, std::unique_ptr<ISimpleResourceLoader>> & modFilesystems);

	bool isModValidationNeeded(const ModDescription & mod) const;

public:
	std::shared_ptr<CContentHandler> content;

	/// receives list of available mods and trying to load mod.json from all of them
	void initializeConfig();
	void loadModFilesystems();

	/// returns ID of mod that provides selected file resource
	TModID findResourceOrigin(const ResourcePath & name) const;

	/// Returns assumed language ID of mod that provides selected file resource
	std::string findResourceLanguage(const ResourcePath & name) const;

	/// Returns assumed encoding of language of mod that provides selected file resource
	std::string findResourceEncoding(const ResourcePath & name) const;

	/// Same as findResourceEncoding, for callers that have already located the providing mod
	std::string getResourceEncoding(const ResourcePath & name, const TModID & modName) const;

	std::string getModLanguage(const TModID & modId) const;

	const std::set<TModID> & getModDependencies(const TModID & modId) const;
	const std::set<TModID> & getModDependencies(const TModID & modId, bool & isModFound) const;
	std::set<TModID> getModSoftDependencies(const TModID & modId) const;
	std::set<TModID> getModEnabledSoftDependencies(const TModID & modId) const;

	/// returns list of all (active) mods
	std::vector<std::string> getAllMods() const;
	const std::vector<std::string> & getActiveMods() const;

	const ModDescription & getModInfo(const TModID & modId) const;

	/// load content from all available mods
	void load();
	void afterLoad();

	/// Read-only guest preflight using verified archives in the persistent user-data directory.
	ModSyncPlan planLanModSync(const ModListVerificationStatus & statuses, const ModArchiveHashes & expectedSha256) const;
	/// Start a recoverable temporary mod preset before applying LAN gameplay changes.
	void beginLanSession();
	/// Persist an installed-only LAN preset and signal a controlled client restart.
	bool prepareInstalledOnlyLanSession(const ModCompatibilityInfo & hostMods, const LanRejoinTarget & target);
	/// Available exactly once after a matching one-shot relaunch token was consumed.
	std::optional<LanRejoinTarget> consumeLanRejoinTarget();
	/// Restore the saved personal preset when a LAN join or session is cancelled.
	/// True only when an active temporary LAN preset was restored; loaded assets need a restart.
	bool restoreLanSession();
	/// Request a one-shot relaunch after gameplay resources are released.
	void requestLanRelaunch();
	bool consumeLanRelaunchRequest();
	std::string prepareControlledLanRelaunch();
	/// Restore the player's original preset on a clean shutdown, unless relaunch is pending.
	void restoreLanSessionOnShutdown();

	explicit CModHandler(bool useTestPreset = false, const std::string & lanRelaunchToken = {});
	~CModHandler();
};
