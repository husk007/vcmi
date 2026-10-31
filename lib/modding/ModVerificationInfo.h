/*
 * ModVerificationInfo.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "CModVersion.h"

#include <cstdint>
#include <filesystem>
#include <optional>

class JsonNode;
struct ModVerificationInfo;
using ModCompatibilityInfo = std::map<std::string, ModVerificationInfo>;

enum class ModVerificationStatus
{
	NOT_INSTALLED, /// Mod is not installed locally
	DISABLED, /// Mod is installed locally but not enabled
	EXCESSIVE, /// Mod is enabled locally but must be disabled
	VERSION_MISMATCH, /// Mod is present on both sides, but has different version
	FULL_MATCH, /// No issues detected, everything matches
};

using ModListVerificationStatus = std::map<std::string, ModVerificationStatus>;

using ModArchiveHashes = std::map<TModID, std::string>;

/// Metadata for a host archive explicitly staged for LAN sharing; not proof of redistribution rights.
struct DLL_LINKAGE ModArchiveManifestEntry
{
	std::string sha256;
	/// VCMI's binary serializer supports signed int64_t, not unsigned uint64_t.
	std::int64_t bytes = 0;

	template <typename Handler> void serialize(Handler & h)
	{
		h & sha256;
		h & bytes;
	}
};

using ModArchiveManifest = std::map<TModID, ModArchiveManifestEntry>;

/// One bounded read from a host-approved LAN share. This is not an installable mod.
struct DLL_LINKAGE ModArchiveChunk
{
	std::int64_t totalBytes = 0;
	std::string data;
};

/// Read-only package availability for incompatible mods in the existing LAN room preview.
/// A cache hit does not mean the mod is installed or safe to activate.
enum class LanArchivePreviewState
{
	UNAVAILABLE,
	OFFERED_BY_HOST,
	VERIFIED_IN_CACHE
};
using LanArchivePreview = std::map<TModID, LanArchivePreviewState>;

/// Operations required to match the host's mod set without changing the player's normal preset.
struct DLL_LINKAGE ModSyncPlan
{
	std::set<TModID> downloadOrUpdate;
	std::set<TModID> reuseCachedArchive;
	std::set<TModID> activate;
	std::set<TModID> deactivate;

	bool requiresChanges() const;
	/// Confirm that the actual dependency resolver selected every required host mod and disabled extras.
	bool matchesResolvedHostMods(const ModCompatibilityInfo & hostMods, const std::vector<TModID> & resolvedMods) const;
};

struct DLL_LINKAGE ModVerificationInfo
{
	/// human-readable mod name
	std::string name;

	/// version of the mod
	CModVersion version;

	/// CRC-32 checksum of the mod
	ui32 checksum = 0;

	/// parent mod ID, empty if root-level mod
	TModID parent;

	/// for serialization purposes
	bool impactsGameplay = true;

	static JsonNode jsonSerializeList(const ModCompatibilityInfo & input);
	static ModCompatibilityInfo jsonDeserializeList(const JsonNode & input);
	static ModListVerificationStatus verifyListAgainstLocalMods(const ModCompatibilityInfo & input);
	/// Stable fingerprint of the host mod list approved before a LAN relaunch.
	static std::string fingerprintHostMods(const ModCompatibilityInfo & input);
	/// Bind a previously approved automatic LAN rejoin to the host's gameplay mods
	/// AND the exact advertised archive digests/sizes, not just mod versions.
	static std::string fingerprintLanHostOffer(const ModCompatibilityInfo & mods, const ModArchiveManifest & archives);
	/// Advertise only active root mods with archives deliberately placed in userData/lan-mod-shares.
	static ModArchiveManifest buildHostArchiveManifest(const ModCompatibilityInfo & activeMods, const std::filesystem::path & userDataPath);
	/// Read at most 64 KiB from an explicitly staged, hash-matching host share.
	/// The caller must authorize the requesting lobby connection before invoking this.
	static std::optional<ModArchiveChunk> readHostArchiveChunk(const ModCompatibilityInfo & activeMods,
		const std::filesystem::path & userDataPath, const TModID & rootId,
		const std::string & expectedSha256, std::int64_t offset);
	/// Classify each missing/mismatched root once, using the negotiated host manifest
	/// and existing persistent SHA-256 verification. Does not install or trust archives.
	static LanArchivePreview inspectHostArchiveAvailability(const ModListVerificationStatus & statuses,
		const ModArchiveManifest & hostArchives, const std::filesystem::path & userDataPath);
	/// Total bytes of host archives not already verified in the guest cache, counted once per root mod.
	static std::uint64_t estimateUncachedArchiveBytes(const LanArchivePreview & availability, const ModArchiveManifest & hostArchives);
	/// Cache entries must describe archives whose SHA-256 has already been verified.
	/// Missing or malformed host archive digests reject download planning.
	/// Read-only preview of changes possible using locally installed matching-version mods.
	/// Missing or version-mismatched mods cannot be resolved by activation alone.
	/// This is not an archive-integrity or dependency validation.
	static std::optional<ModSyncPlan> buildInstalledOnlyPlan(const ModListVerificationStatus & statuses);
	static ModSyncPlan buildSyncPlan(const ModListVerificationStatus & statuses, const ModArchiveHashes & expectedSha256, const ModArchiveHashes & verifiedCachedSha256);
	/// Rehash only needed persistent archives before deciding whether a download is required.
	static ModSyncPlan buildSyncPlanFromPersistentCache(const ModListVerificationStatus & statuses, const ModArchiveHashes & expectedSha256, const std::filesystem::path & userDataPath);

	template <typename Handler>
	void serialize(Handler & h)
	{
		h & name;
		h & version;
		h & checksum;
		h & parent;
		h & impactsGameplay;
	}
};
