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
#include <cstdio>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

#if !defined(VCMI_MOBILE) && !defined(_WIN32)
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#endif

#include "../../lib/network/NetworkDiscoveryProtocol.h"
#include "../../lib/network/NetworkDiscovery.h"
#include "../../lib/modding/AtomicModSettings.h"
#include "../../lib/modding/LanModRecoveryJournal.h"
#include "../../lib/modding/LanRelaunchToken.h"
#include "../../lib/modding/LanRelaunchTokenFormat.h"
#include "../../clientapp/LanRelaunchProcess.h"
#include "../../lib/modding/ModArchiveDigest.h"
#include "../../lib/modding/LanModArchiveCache.h"
#include "../../lib/modding/LanModPackageStage.h"
#include "../../lib/modding/LanHostArchiveTransfer.h"
#include "../../lib/filesystem/MinizipExtensions.h"
#include "../../lib/filesystem/AdapterLoaders.h"
#include "../../lib/filesystem/CFilesystemLoader.h"

#include "../../lib/serializer/CMemorySerializer.h"
#include "../../lib/serializer/GameConnection.h"
#include "../../lib/networkPacks/PacksForLobby.h"
#include "../../lib/json/JsonNode.h"
#include "../../lib/modding/ModManager.h"
#include "../../lib/modding/ModVerificationInfo.h"
#include "../../lib/modding/CModHandler.h"
#include "../../lib/modding/ModDescription.h"
#include "../../lib/modding/ModScope.h"
#include "../../lib/GameLibrary.h"
#include "../../lib/StartInfo.h"
#include "../../lib/mapping/CMapInfo.h"
#include "../../lib/mapping/CMap.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/VCMIDirs.h"
#include "../../lib/ScopeGuard.h"

TEST(LanModSyncTest, RealUdpDiscoveryKeepsLegacyReplyAndNegotiatesLanPreviewOnLoopback)
{
	struct HostAnnouncer final : IServerDiscoveryAnnouncer
	{
		bool inLobby = true;
		bool isInLobby() const override { return inLobby; }
		std::uint16_t getPort() const override { return 3031; }
	} host;

	NetworkContext context;
	using Udp = boost::asio::ip::udp;
	Udp::socket reservation(context, Udp::endpoint(Udp::v4(), 0));
	const auto discoveryPort = reservation.local_endpoint().port();
	reservation.close();
	auto listener = std::make_shared<ServerDiscoveryListener>(context, host, discoveryPort);
	listener->start();

	Udp::socket guest(context, Udp::endpoint(Udp::v4(), 0));
	const auto probe = [&](std::string_view query)
	{
		auto buffer = std::make_shared<std::array<char, 128>>();
		auto sender = std::make_shared<Udp::endpoint>();
		auto response = std::make_shared<std::string>();
		guest.async_receive_from(boost::asio::buffer(*buffer), *sender,
			[buffer, sender, response](const boost::system::error_code & error, std::size_t bytes)
			{
				if(!error)
					response->assign(buffer->data(), bytes);
			});

		const std::string message(query);
		guest.send_to(boost::asio::buffer(message),
			Udp::endpoint(boost::asio::ip::address_v4::loopback(), discoveryPort));
		context.run_for(std::chrono::milliseconds(250));
		if(response->empty())
		{
			guest.cancel();
			context.restart();
			context.poll();
		}
		context.restart();
		return *response;
	};

	const auto legacy = probe(LanModDiscovery::legacyProbe);
	ASSERT_EQ(legacy, "VCMI_SERVER:3031");
	const auto legacyReply = LanModDiscovery::parseReply(legacy);
	ASSERT_TRUE(legacyReply.has_value());
	EXPECT_FALSE(legacyReply->supportsModPreview);

	const auto upgraded = probe(LanModDiscovery::previewProbe);
	ASSERT_EQ(upgraded, "VCMI_SERVER:3031;lanModSync=preview");
	const auto upgradedReply = LanModDiscovery::parseReply(upgraded);
	ASSERT_TRUE(upgradedReply.has_value());
	EXPECT_TRUE(upgradedReply->supportsModPreview);

	host.inLobby = false;
	EXPECT_TRUE(probe(LanModDiscovery::previewProbe).empty());
	listener->stop();
}

TEST(LanModSyncTest, DirectIpJoinNegotiatesOptInPreviewAndFallsBackForLegacyHost)
{
	struct HostAnnouncer final : IServerDiscoveryAnnouncer
	{
		bool isInLobby() const override { return true; }
		std::uint16_t getPort() const override { return 31317; }
	} host;

	NetworkContext hostContext;
	using Udp = boost::asio::ip::udp;
	Udp::socket reservation(hostContext, Udp::endpoint(Udp::v4(), 0));
	const auto discoveryPort = reservation.local_endpoint().port();
	reservation.close();

	auto announcer = std::make_shared<ServerDiscoveryListener>(hostContext, host, discoveryPort);
	announcer->start();

	// The old discovery response does not authorize the new TCP packet.
	Udp::socket legacy(hostContext, Udp::endpoint(Udp::v4(), 0));
	const auto legacyPort = legacy.local_endpoint().port();
	std::array<char, 128> legacyBuffer{};
	Udp::endpoint legacySender;
	legacy.async_receive_from(boost::asio::buffer(legacyBuffer), legacySender,
		[&](const boost::system::error_code & error, std::size_t)
		{
			if(!error)
			{
				const std::string legacyReply = "VCMI_SERVER:31317";
				legacy.send_to(boost::asio::buffer(legacyReply), legacySender);
			}
		});

	std::thread hostThread([&]() { hostContext.run(); });
	const bool upgraded = ServerDiscovery::probeHostPreview("127.0.0.1", 31317, discoveryPort);
	const bool wrongGamePort = ServerDiscovery::probeHostPreview("127.0.0.1", 31318, discoveryPort);
	const bool legacyFallback = ServerDiscovery::probeHostPreview("127.0.0.1", 31317, legacyPort);

	boost::asio::post(hostContext, [announcer]() { announcer->stop(); });
	hostThread.join();

	// The same endpoint can become a legacy host while the guest restarts.
	// Re-probing must downgrade to LobbyQueryState rather than using stale capability.
	NetworkContext replacementContext;
	Udp::socket replacementHost(replacementContext, Udp::endpoint(Udp::v4(), discoveryPort));
	std::array<char, 128> replacementBuffer{};
	Udp::endpoint replacementSender;
	replacementHost.async_receive_from(boost::asio::buffer(replacementBuffer), replacementSender,
		[&](const boost::system::error_code & error, std::size_t)
		{
			if(!error)
			{
				const std::string legacyReply = "VCMI_SERVER:31317";
				replacementHost.send_to(boost::asio::buffer(legacyReply), replacementSender);
			}
		});
	std::thread replacementThread([&]() { replacementContext.run_for(std::chrono::milliseconds(900)); });
	const bool staleCapability = ServerDiscovery::probeHostPreview("127.0.0.1", 31317, discoveryPort);
	replacementThread.join();

	EXPECT_FALSE(staleCapability);
	EXPECT_TRUE(upgraded);
	EXPECT_FALSE(wrongGamePort);
	EXPECT_FALSE(legacyFallback);
}

TEST(LanModSyncTest, DirectIpPreviewIgnoresLegacyReplyBeforeOptInReply)
{
	NetworkContext hostContext;
	using Udp = boost::asio::ip::udp;
	Udp::socket host(hostContext, Udp::endpoint(Udp::v4(), 0));
	const auto discoveryPort = host.local_endpoint().port();
	std::array<char, 128> buffer{};
	Udp::endpoint guest;
	host.async_receive_from(boost::asio::buffer(buffer), guest,
		[&](const boost::system::error_code & error, std::size_t)
		{
			if(error)
				return;
			const auto legacy = LanModDiscovery::replyForProbe(LanModDiscovery::legacyProbe, 31317);
			const auto upgraded = LanModDiscovery::replyForProbe(LanModDiscovery::previewProbe, 31317);
			host.send_to(boost::asio::buffer(legacy), guest);
			host.send_to(boost::asio::buffer(upgraded), guest);
		});
	std::thread hostThread([&]() { hostContext.run(); });
	const bool supportsPreview = ServerDiscovery::probeHostPreview("127.0.0.1", 31317, discoveryPort);
	hostThread.join();
	EXPECT_TRUE(supportsPreview) << "Legacy UDP reply suppressed the later opt-in LAN preview";
}

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
	optInResponse.acceptingPlayers = false;
	optInResponse.archives.emplace("hostmod", ModArchiveManifestEntry{std::string(64, 'a'), 42});
	// The 2 GiB archive limit must also survive VCMI signed-varint serialization.
	optInResponse.archives.emplace("largehostmod", ModArchiveManifestEntry{std::string(64, 'b'), static_cast<std::int64_t>(LanModArchiveCache::maxArchiveBytes)});

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
	EXPECT_FALSE(decoded->acceptingPlayers);
	ASSERT_EQ(decoded->mods.size(), 1);
	EXPECT_EQ(decoded->mods.at("hostmod").name, mod.name);
	EXPECT_EQ(decoded->mods.at("hostmod").version, mod.version);
	ASSERT_EQ(decoded->archives.size(), 2);
	EXPECT_EQ(decoded->archives.at("hostmod").sha256, std::string(64, 'a'));
	EXPECT_EQ(decoded->archives.at("hostmod").bytes, 42);
	EXPECT_EQ(decoded->archives.at("largehostmod").sha256, std::string(64, 'b'));
	EXPECT_EQ(decoded->archives.at("largehostmod").bytes, static_cast<std::int64_t>(LanModArchiveCache::maxArchiveBytes));
}

TEST(LanModSyncTest, RealTcpLobbyPreviewTransportsLegacyAndOptInPacketsOnLoopback)
{
	// Keep the io_context alive until both TCP peers and their sockets are destroyed.
	auto network = INetworkHandler::createHandler();
	struct Host final : INetworkServerListener
	{
		std::map<NetworkConnectionPtr, std::unique_ptr<GameConnection>> connections;
		int legacyQueries = 0;
		int optInQueries = 0;
		bool acceptingPlayers = true;

		void onNewConnection(const NetworkConnectionPtr & connection) override
		{
			connections.emplace(connection, std::make_unique<GameConnection>(connection));
		}

		void onDisconnected(const NetworkConnectionPtr & connection, const std::string &) override
		{
			connections.erase(connection);
		}

		void onPacketReceived(const NetworkConnectionPtr & connection, const std::vector<std::byte> & bytes) override
		{
			auto & peer = *connections.at(connection);
			const auto query = peer.retrievePack(bytes);
			LobbyModsCheck response;
			response.vcmiVersion = VCMI_VERSION_STRING;
			response.hostAccountDisplayName = "LAN host";
			response.participantNames = {"LAN host"};
			ModVerificationInfo mod;
			mod.name = "Host gameplay mod";
			mod.version = CModVersion(1, 2, 3);
			response.mods.emplace("hostmod", mod);

			if(dynamic_cast<LobbyQueryModSync *>(query.get()))
			{
				++optInQueries;
				LobbyModSyncPreview extended;
				static_cast<LobbyModsCheck &>(extended) = response;
				extended.acceptingPlayers = acceptingPlayers;
				extended.archives.emplace("hostmod", ModArchiveManifestEntry{std::string(64, 'a'), 3});
				peer.sendPack(extended);
			}
			else if(dynamic_cast<LobbyQueryState *>(query.get()))
			{
				++legacyQueries;
				peer.sendPack(response);
			}
		}
	} host;

	struct Guest final : INetworkClientListener
	{
		bool optIn = false;
		std::string failure;
		NetworkConnectionPtr connection;
		std::unique_ptr<GameConnection> peer;
		std::unique_ptr<CPack> response;

		explicit Guest(bool upgraded) : optIn(upgraded) {}

		void onConnectionFailed(const std::string & error) override { failure = error; }
		void onConnectionEstablished(const NetworkConnectionPtr & connected) override
		{
			connection = connected;
			peer = std::make_unique<GameConnection>(connection);
			if(optIn)
			{
				LobbyQueryModSync query;
				peer->sendPack(query);
			}
			else
			{
				LobbyQueryState query;
				peer->sendPack(query);
			}
		}
		void onDisconnected(const NetworkConnectionPtr &, const std::string & error) override { failure = error; }
		void onPacketReceived(const NetworkConnectionPtr &, const std::vector<std::byte> & bytes) override
		{
			response = peer->retrievePack(bytes);
		}
	} legacy(false), upgraded(true);

	// Use VCMI's actual framed TCP transport and GameConnection packet serializer.
	auto server = network->createServerTCP(host);
	const auto port = server->start(0);
	ASSERT_NE(port, 0);
	network->connectToRemote(legacy, "127.0.0.1", port);
	network->connectToRemote(upgraded, "127.0.0.1", port);
	network->getContext().run_for(std::chrono::milliseconds(1500));

	EXPECT_TRUE(legacy.failure.empty()) << legacy.failure;
	EXPECT_TRUE(upgraded.failure.empty()) << upgraded.failure;
	EXPECT_EQ(host.legacyQueries, 1);
	EXPECT_EQ(host.optInQueries, 1);
	const auto * legacyPreview = dynamic_cast<LobbyModsCheck *>(legacy.response.get());
	const auto * optInPreview = dynamic_cast<LobbyModSyncPreview *>(upgraded.response.get());
	ASSERT_NE(legacyPreview, nullptr);
	EXPECT_EQ(dynamic_cast<LobbyModSyncPreview *>(legacy.response.get()), nullptr);
	ASSERT_NE(optInPreview, nullptr);
	EXPECT_EQ(legacyPreview->mods.size(), 1);
	EXPECT_EQ(legacyPreview->mods.at("hostmod").version, CModVersion(1, 2, 3));
	EXPECT_EQ(optInPreview->mods.size(), legacyPreview->mods.size());
	EXPECT_EQ(optInPreview->mods.at("hostmod").name, legacyPreview->mods.at("hostmod").name);
	EXPECT_EQ(optInPreview->mods.at("hostmod").version, legacyPreview->mods.at("hostmod").version);
	ASSERT_EQ(optInPreview->archives.size(), 1);
	EXPECT_EQ(optInPreview->archives.at("hostmod").bytes, 3);
	EXPECT_EQ(optInPreview->archives.at("hostmod").sha256, std::string(64, 'a'));
	EXPECT_TRUE(optInPreview->acceptingPlayers);

	// Exercise the guest's recoverable installed-only preset with mods received over actual TCP.
	JsonNode guestPreset;
	guestPreset["activePreset"].String() = "personal";
	guestPreset["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	guestPreset["presets"]["personal"]["mods"].Vector().emplace_back("personalmod");
	guestPreset["presets"]["personal"]["settings"]["personalmod"]["units"].Bool() = true;
	guestPreset["presets"]["personal"]["settings"]["hostmod"]["variants"].Bool() = false;
	const auto originalPreset = guestPreset.toCompactString();

	ModListVerificationStatus guestStatuses;
	for(const auto & hostMod : optInPreview->mods)
		guestStatuses.emplace(hostMod.first, ModVerificationStatus::DISABLED);
	guestStatuses.emplace("personalmod", ModVerificationStatus::EXCESSIVE);
	const auto plan = ModVerificationInfo::buildInstalledOnlyPlan(guestStatuses);
	ASSERT_TRUE(plan.has_value());
	EXPECT_TRUE(plan->downloadOrUpdate.empty());

	LanModSessionState::begin(guestPreset);
	LanModSessionState::applyInstalledOnlyPlan(guestPreset, *plan);
	LanModSessionState::setRejoinTarget(guestPreset, {"127.0.0.1", port, true, {"Guest Alice"}, false});
	const std::string relaunchToken(64, 'c');
	LanModSessionState::prepareControlledRelaunch(guestPreset, relaunchToken);
	ASSERT_EQ(LanModSessionState::recoverOnStartup(guestPreset, relaunchToken), LanSessionStartupAction::RESUMED);

	const auto rejoin = LanModSessionState::getRejoinTarget(guestPreset);
	ASSERT_TRUE(rejoin.has_value());
	EXPECT_EQ(rejoin->hostname, "127.0.0.1");
	EXPECT_EQ(rejoin->port, port);
	EXPECT_TRUE(rejoin->previewCapability);
	EXPECT_EQ(rejoin->playerNames, std::vector<std::string>({"Guest Alice"}));
	EXPECT_FALSE(rejoin->loadGame);
	const auto & staged = guestPreset["presets"][LanModSessionState::temporaryPresetName()]["mods"].Vector();
	EXPECT_NE(std::find(staged.begin(), staged.end(), JsonNode("hostmod")), staged.end());
	EXPECT_EQ(std::find(staged.begin(), staged.end(), JsonNode("personalmod")), staged.end());
	EXPECT_TRUE(plan->matchesResolvedHostMods(optInPreview->mods, {"vcmi", "hostmod"}));

	// The restarted client must establish a second real TCP connection to the saved host.
	// Closing the original query connection also exercises the host's disconnect path.
	upgraded.connection->close();
	// A host that starts gameplay while the guest relaunches must refuse a fresh join.
	host.acceptingPlayers = false;
	Guest resumedGuest(true);
	network->getContext().restart();
	network->connectToRemote(resumedGuest, rejoin->hostname, rejoin->port);
	network->getContext().run_for(std::chrono::milliseconds(1500));

	EXPECT_TRUE(resumedGuest.failure.empty()) << resumedGuest.failure;
	EXPECT_EQ(host.optInQueries, 2);
	const auto * freshPreview = dynamic_cast<LobbyModSyncPreview *>(resumedGuest.response.get());
	ASSERT_NE(freshPreview, nullptr);
	ASSERT_EQ(freshPreview->mods.size(), 1);
	EXPECT_EQ(freshPreview->mods.at("hostmod").version, CModVersion(1, 2, 3));
	ASSERT_EQ(freshPreview->archives.size(), 1);
	EXPECT_EQ(freshPreview->archives.at("hostmod").sha256, optInPreview->archives.at("hostmod").sha256);
	EXPECT_FALSE(freshPreview->acceptingPlayers);
	EXPECT_TRUE(plan->matchesResolvedHostMods(freshPreview->mods, {"vcmi", "hostmod"}));

	EXPECT_TRUE(LanModSessionState::restore(guestPreset));
	EXPECT_EQ(guestPreset.toCompactString(), originalPreset);

	network->stop();
}


TEST(LanModSyncTest, VerifiedCachedZipStagesSideBySideWithoutTouchingPrivateMod)
{
	namespace fs = std::filesystem;
	const fs::path base = fs::temp_directory_path() / ("vcmi-lan-stage-" + LanRelaunchToken::generate());
	ASSERT_TRUE(fs::create_directories(base));
	const auto cleanup = vstd::makeScopeGuard([&](){ std::error_code ec; fs::remove_all(base, ec); });
	const auto privateDefinition = base / "guest" / "Mods" / "hostmod" / "mod.json";
	ASSERT_TRUE(fs::create_directories(privateDefinition.parent_path()));
	{
		std::ofstream original(privateDefinition);
		original << "PRIVATE_VERSION";
	}
	const auto makeZip = [](const fs::path & path, const std::string & definitionPath) -> bool
	{
		zipFile writer = zipOpen(path.string().c_str(), APPEND_STATUS_CREATE);
		if(!writer)
			return false;
		const std::string payload = R"({"name":"LAN test","version":"1.0"})";
		const bool valid = zipOpenNewFileInZip(writer, definitionPath.c_str(), nullptr,
			nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED, Z_DEFAULT_COMPRESSION) == ZIP_OK
			&& zipWriteInFileInZip(writer, payload.data(), static_cast<unsigned int>(payload.size())) == ZIP_OK
			&& zipCloseFileInZip(writer) == ZIP_OK;
		return zipClose(writer, nullptr) == ZIP_OK && valid;
	};
	const auto good = base / "good.zip";
	ASSERT_TRUE(makeZip(good, "hostmod/mod.json"));
	const auto digest = ModArchiveDigest::sha256File(good, LanModArchiveCache::maxArchiveBytes);
	ASSERT_TRUE(digest);
	ASSERT_TRUE(LanModArchiveCache::publishVerifiedArchive(base / "guest", "hostmod", *digest, good));
	const auto staged = LanModPackageStage::stageVerifiedArchive(base / "guest", "hostmod", *digest);
	ASSERT_TRUE(staged);
	EXPECT_TRUE(fs::is_regular_file(*staged / "mod.json"));
	EXPECT_NE(staged->parent_path().parent_path().parent_path(), privateDefinition.parent_path());
	std::ifstream stillPrivate(privateDefinition);
	std::string value;
	std::getline(stillPrivate, value);
	EXPECT_EQ(value, "PRIVATE_VERSION");
	EXPECT_EQ(LanModPackageStage::stageVerifiedArchive(base / "guest", "hostmod", *digest), staged);
	EXPECT_TRUE(LanModArchiveCache::findVerifiedArchive(base / "guest", "hostmod", *digest));

	// A private version must never be shadowed, not even by a fully cached ZIP.
	CFilesystemList privateView;
	CFilesystemList privateContent;
	privateView.addLoader(std::make_unique<CFilesystemLoader>("MODS/HOSTMOD/",
		boost::filesystem::path(privateDefinition.parent_path().native()), 64, true), false);
	EXPECT_FALSE(LanModPackageStage::mountControlledStagedMods(privateView, privateContent,
		base / "guest", {{"hostmod", *digest}}));

	// Without a private version the same staged package participates in actual
	// VCMI mod discovery under its canonical MODS/<ROOT>/MOD resource name.
	CFilesystemList guestView;
	CFilesystemList guestContent;
	ASSERT_TRUE(LanModPackageStage::mountControlledStagedMods(guestView, guestContent,
		base / "guest", {{"hostmod", *digest}}));
	const ResourcePath modDefinition("MODS/HOSTMOD/MOD", EResType::JSON);
	const auto definition = guestView.getResourceName(modDefinition);
	const auto contentDefinition = guestContent.getResourceName(modDefinition);
	ASSERT_TRUE(definition);
	ASSERT_TRUE(contentDefinition);
	EXPECT_EQ(fs::path(definition->native()), *staged / "mod.json");
	EXPECT_EQ(fs::path(contentDefinition->native()), *staged / "mod.json");
	EXPECT_FALSE(guestView.existsResource(ResourcePath("MODS/UNRELATED/MOD", EResType::JSON)));

	const auto malicious = base / "traversal.zip";
	ASSERT_TRUE(makeZip(malicious, "../mod.json"));
	const auto badDigest = ModArchiveDigest::sha256File(malicious, LanModArchiveCache::maxArchiveBytes);
	ASSERT_TRUE(badDigest);
	ASSERT_TRUE(LanModArchiveCache::publishVerifiedArchive(base / "guest", "hostmod", *badDigest, malicious));
	EXPECT_FALSE(LanModPackageStage::stageVerifiedArchive(base / "guest", "hostmod", *badDigest));
	EXPECT_FALSE(fs::exists(base / "guest" / "lan-mod-staged" / "mod.json"));
}


TEST(LanModSyncTest, LanLobbyPreviewShowsOnlyActionsActuallyAvailableToGuest)
{
	using Action = LanModPreviewAction;
	using Status = ModVerificationStatus;
	using Archive = LanArchivePreviewState;
	EXPECT_EQ(ModVerificationInfo::getLanPreviewAction(Status::FULL_MATCH, Archive::UNAVAILABLE, false), Action::NONE);
	EXPECT_EQ(ModVerificationInfo::getLanPreviewAction(Status::DISABLED, Archive::UNAVAILABLE, false), Action::ENABLE);
	EXPECT_EQ(ModVerificationInfo::getLanPreviewAction(Status::EXCESSIVE, Archive::UNAVAILABLE, false), Action::DISABLE);
	EXPECT_EQ(ModVerificationInfo::getLanPreviewAction(Status::NOT_INSTALLED, Archive::VERIFIED_IN_CACHE, true), Action::USE_CACHE);
	EXPECT_EQ(ModVerificationInfo::getLanPreviewAction(Status::NOT_INSTALLED, Archive::OFFERED_BY_HOST, true), Action::DOWNLOAD);
	EXPECT_EQ(ModVerificationInfo::getLanPreviewAction(Status::NOT_INSTALLED, Archive::UNAVAILABLE, true), Action::MANUAL);
	// An incomplete offer cannot be presented as a usable download or cached Join path.
	EXPECT_EQ(ModVerificationInfo::getLanPreviewAction(Status::NOT_INSTALLED, Archive::OFFERED_BY_HOST, false), Action::MANUAL);
	EXPECT_EQ(ModVerificationInfo::getLanPreviewAction(Status::NOT_INSTALLED, Archive::VERIFIED_IN_CACHE, false), Action::MANUAL);
	// Private versions are never silently replaced by host archives.
	EXPECT_EQ(ModVerificationInfo::getLanPreviewAction(Status::VERSION_MISMATCH, Archive::OFFERED_BY_HOST, true), Action::MANUAL);
}

TEST(LanModSyncTest, GuestPreparesOnlyCompleteHostArchiveOffersWithoutPrivateVersionConflicts)
{
	const ModListVerificationStatus required = {
		{"disabledlocal", ModVerificationStatus::DISABLED},
		{"missingone", ModVerificationStatus::NOT_INSTALLED},
		{"missingtwo.submod", ModVerificationStatus::NOT_INSTALLED},
		{"personalonly", ModVerificationStatus::EXCESSIVE}
	};
	const std::vector<TModID> installed = {"disabledlocal", "personalonly"};
	LanArchivePreview available = {
		{"missingone", LanArchivePreviewState::OFFERED_BY_HOST},
		{"missingtwo", LanArchivePreviewState::UNAVAILABLE}
	};
	// A partial host offer must not expose Prepare or start any TCP transfer.
	EXPECT_FALSE(ModVerificationInfo::canPrepareMissingHostArchives(required, available, installed));
	available["missingtwo"] = LanArchivePreviewState::VERIFIED_IN_CACHE;
	// One downloaded root and one previously verified root can be prepared together.
	EXPECT_TRUE(ModVerificationInfo::canPrepareMissingHostArchives(required, available, installed));
	available["missingone"] = LanArchivePreviewState::VERIFIED_IN_CACHE;
	EXPECT_TRUE(ModVerificationInfo::canPrepareMissingHostArchives(required, available, installed));
	// A private root with a missing submod must never be shadowed.
	auto withPrivateRoot = installed;
	withPrivateRoot.push_back("missingtwo");
	EXPECT_FALSE(ModVerificationInfo::canPrepareMissingHostArchives(required, available, withPrivateRoot));
	// Available host packages cannot solve an installed version conflict.
	auto withWrongVersion = required;
	withWrongVersion["missingone"] = ModVerificationStatus::VERSION_MISMATCH;
	EXPECT_FALSE(ModVerificationInfo::canPrepareMissingHostArchives(withWrongVersion, available, installed));
	EXPECT_FALSE(ModVerificationInfo::canPrepareMissingHostArchives(
		{{"disabledlocal", ModVerificationStatus::DISABLED}}, available, installed));
}

TEST(LanModSyncTest, HostArchiveConsentIdentifiesEachRootAndActiveSubmodBeforeTrust)
{
	ModCompatibilityInfo hostMods;
	ModVerificationInfo first;
	first.version = CModVersion::fromString("1.0");
	hostMods["lan-first"] = first;
	ModVerificationInfo child;
	child.version = CModVersion::fromString("2.0");
	hostMods["lan-second.units"] = child;
	const ModArchiveManifest archives = {
		{"lan-first", {std::string(64, 'a'), 4096}},
		{"lan-second", {std::string(64, 'b'), 8192}},
		{"lan-hidden", {std::string(64, 'c'), 16384}}
	};
	const LanArchivePreview availability = {
		{"lan-first", LanArchivePreviewState::OFFERED_BY_HOST},
		{"lan-second", LanArchivePreviewState::VERIFIED_IN_CACHE},
		{"lan-hidden", LanArchivePreviewState::UNAVAILABLE}
	};
	const auto descriptions = ModVerificationInfo::describeLanArchiveConsent(hostMods, availability, archives);
	ASSERT_EQ(descriptions.size(), 2);
	EXPECT_NE(descriptions.at(0).find("lan-first (" + first.version.toString() + ")"), std::string::npos);
	EXPECT_NE(descriptions.at(0).find(std::string(12, 'a')), std::string::npos);
	EXPECT_NE(descriptions.at(1).find("lan-second.units (" + child.version.toString() + ")"), std::string::npos);
	EXPECT_NE(descriptions.at(1).find(std::string(12, 'b')), std::string::npos);
	for(const auto & description : descriptions)
		EXPECT_EQ(description.find("lan-hidden"), std::string::npos);
}

TEST(LanModSyncTest, HostOfferConsentPersistsForIdenticalContentOnly)
{
	namespace fs = std::filesystem;
	const auto userData = fs::temp_directory_path() / ("vcmi-lan-consent-" + LanRelaunchToken::generate());
	const auto offer = LanRelaunchToken::generate();
	const auto changedOffer = LanRelaunchToken::generate();
	ASSERT_NE(offer, changedOffer);
	EXPECT_FALSE(LanModArchiveCache::hasApprovedHostOffer(userData, offer));
	EXPECT_FALSE(LanModArchiveCache::rememberApprovedHostOffer(userData, "invalid-digest"));
	ASSERT_TRUE(LanModArchiveCache::rememberApprovedHostOffer(userData, offer));
	EXPECT_TRUE(LanModArchiveCache::hasApprovedHostOffer(userData, offer));
	EXPECT_TRUE(LanModArchiveCache::rememberApprovedHostOffer(userData, offer));
	EXPECT_FALSE(LanModArchiveCache::hasApprovedHostOffer(userData, changedOffer));
	std::error_code cleanup;
	fs::remove_all(userData, cleanup);
}

TEST(LanModSyncTest, OnlyMatchingControlledRelaunchMountsSelectedLANRoot)
{
	JsonNode settings;
	settings["activePreset"].String() = "personal";
	settings["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	settings["presets"]["other"]["mods"].Vector().emplace_back("vcmi");
	const std::string original = settings.toCompactString();
	LanModSessionState::begin(settings);
	const std::string digest(64, 'a');
	const ModArchiveHashes selected = {{"hostmod", digest}};
	LanModSessionState::setStagedMods(settings, selected);
	const auto token = LanRelaunchToken::generate();
	LanModSessionState::prepareControlledRelaunch(settings, token);
	EXPECT_EQ(LanModSessionState::stagedModsForControlledRelaunch(settings, token), selected);
	EXPECT_TRUE(LanModSessionState::stagedModsForControlledRelaunch(settings, LanRelaunchToken::generate()).empty());
	EXPECT_TRUE(LanModSessionState::stagedModsForControlledRelaunch(settings, "").empty());

	EXPECT_EQ(LanModSessionState::recoverOnStartup(settings, token), LanSessionStartupAction::RESUMED);
	EXPECT_TRUE(LanModSessionState::stagedModsForControlledRelaunch(settings, token).empty());
	EXPECT_TRUE(LanModSessionState::restore(settings));
	EXPECT_EQ(settings.toCompactString(), original);
}

TEST(LanModSyncTest, RegisteredLobbyGuestFetchesAndMountsRealZipOnlyOnceOverTcp)
{
	namespace fs = std::filesystem;
	const auto directory = fs::temp_directory_path() / ("vcmi-lan-chunk-transport-" + LanRelaunchToken::generate());
	const auto shares = directory / "host" / "lan-mod-shares";
	const auto guestData = directory / "guest";
	const auto concurrentGuestData = directory / "guest-concurrent";
	ASSERT_TRUE(fs::create_directories(shares));
	// A real, redistributable ZIP must pass the whole host TCP -> guest cache ->
	// isolated mod discovery path. Store the payload without compression so the
	// production chunk transport necessarily crosses the 64 KiB boundary.
	const auto archivePath = shares / "hostmod.zip";
	{
		zipFile writer = zipOpen(archivePath.string().c_str(), APPEND_STATUS_CREATE);
		ASSERT_NE(writer, nullptr);
		const auto addStoredFile = [writer](const std::string & name, const std::string & data)
		{
			if(zipOpenNewFileInZip(writer, name.c_str(), nullptr,
				nullptr, 0, nullptr, 0, nullptr, 0, 0) != ZIP_OK)
				return false;
			const bool written = zipWriteInFileInZip(writer, data.data(),
				static_cast<unsigned int>(data.size())) == ZIP_OK;
			return zipCloseFileInZip(writer) == ZIP_OK && written;
		};
		ASSERT_TRUE(addStoredFile("hostmod/mod.json",
			R"({"name":"LAN test host mod","version":"1.0","author":"VCMI test","modType":"Mechanics","settings":{}})"));
		ASSERT_TRUE(addStoredFile("hostmod/assets/payload.bin", std::string(64 * 1024 + 37, 'a')));
		ASSERT_EQ(zipClose(writer, nullptr), ZIP_OK);
	}
	const auto archiveBytes = fs::file_size(archivePath);
	ASSERT_GT(archiveBytes, 64U * 1024U);
	ASSERT_LT(archiveBytes, 128U * 1024U);
	ModCompatibilityInfo activeMods;
	ModVerificationInfo root;
	root.name = "LAN test host mod";
	activeMods.emplace("hostmod", root);
	const auto manifest = ModVerificationInfo::buildHostArchiveManifest(activeMods, directory / "host");
	ASSERT_EQ(manifest.size(), 1);
	const auto digest = manifest.at("hostmod").sha256;
	const ModArchiveHashes hashes = {{"hostmod", digest}};
	const ModListVerificationStatus missing = {{"hostmod", ModVerificationStatus::NOT_INSTALLED}};

	// The new packet IDs must travel through VCMI's production serializer,
	// independently of the old lobby preview packet layouts.
	LobbyQueryLanModArchiveChunk sample;
	sample.rootId = "hostmod";
	sample.sha256 = digest;
	auto roundTrip = CMemorySerializer::deepCopy<CPack>(sample);
	ASSERT_NE(dynamic_cast<LobbyQueryLanModArchiveChunk *>(roundTrip.get()), nullptr);
	EXPECT_EQ(dynamic_cast<LobbyQueryLanModArchiveChunk *>(roundTrip.get())->sha256, digest);

	auto network = INetworkHandler::createHandler();
	struct Host final : INetworkServerListener
	{
		std::map<NetworkConnectionPtr, std::unique_ptr<GameConnection>> peers;
		std::map<NetworkConnectionPtr, std::unique_ptr<LanHostArchiveTransfer>> transfers;
		ModCompatibilityInfo mods;
		fs::path userData;
		int requests = 0;
		std::size_t sentBytes = 0;

		void onNewConnection(const NetworkConnectionPtr & connection) override
		{
			auto & peer = peers[connection] = std::make_unique<GameConnection>(connection);
			peer->enterLobbyConnectionMode();
		}
		void onDisconnected(const NetworkConnectionPtr & connection, const std::string &) override
		{
			transfers.erase(connection);
			peers.erase(connection);
		}
		void onPacketReceived(const NetworkConnectionPtr & connection, const std::vector<std::byte> & buffer) override
		{
			auto & peer = *peers.at(connection);
			auto packet = peer.retrievePack(buffer);
			if(auto * join = dynamic_cast<LobbyClientConnected *>(packet.get()))
			{
				peer.connectionID = GameConnectionID::FIRST_CONNECTION;
				LobbyClientConnected accepted = *join;
				accepted.clientId = peer.connectionID;
				accepted.hostClientId = peer.connectionID;
				peer.sendPack(accepted);
				return;
			}
			if(auto * request = dynamic_cast<LobbyQueryLanModArchiveChunk *>(packet.get()))
			{
				if(peer.connectionID == GameConnectionID::INVALID)
					return;
				++requests;
				LobbyLanModArchiveChunk response;
				response.rootId = request->rootId;
				response.sha256 = request->sha256;
				response.offset = request->offset;
				if(request->offset == 0)
				{
					transfers.erase(connection);
					if(auto transfer = LanHostArchiveTransfer::open(mods, userData,
						request->rootId, request->sha256))
						transfers.emplace(connection, std::move(transfer));
				}
				const auto transfer = transfers.find(connection);
				if(transfer != transfers.end())
				{
					const auto chunk = transfer->second->next(request->rootId, request->sha256, request->offset);
					if(chunk)
					{
						response.granted = true;
						response.totalBytes = chunk->totalBytes;
						response.data = chunk->data;
						sentBytes += response.data.size();
					}
					if(!chunk || transfer->second->complete())
						transfers.erase(transfer);
				}
				peer.sendPack(response);
			}
		}
	} host;
	host.mods = activeMods;
	host.userData = directory / "host";

	struct Guest final : INetworkClientListener
	{
		NetworkConnectionPtr connection;
		std::unique_ptr<GameConnection> peer;
		fs::path userData;
		ModArchiveHashes hashes;
		std::string failure;
		std::string uuid = "11111111-1111-4111-8111-111111111111";
		std::size_t receivedBytes = 0;
		std::size_t expectedBytes = 0;
		std::string receivedArchive;
		bool done = false;

		bool canMountVerifiedHostRoot()
		{
			const auto staged = LanModPackageStage::stageVerifiedArchive(
				userData, "hostmod", hashes.at("hostmod"));
			CFilesystemList discovery;
			CFilesystemList content;
			const ResourcePath definition("MODS/HOSTMOD/MOD", EResType::JSON);
			if(!staged || !LanModPackageStage::mountControlledStagedMods(discovery, content, userData, hashes)
				|| !discovery.existsResource(definition) || !content.existsResource(definition))
			{
				failure = "Verified host archive is not visible to LAN mod discovery";
				return false;
			}
			const auto path = discovery.getResourceName(definition);
			const auto contentPath = content.getResourceName(definition);
			if(!path || !contentPath || fs::path(path->native()) != *staged / "mod.json"
				|| fs::path(contentPath->native()) != *staged / "mod.json")
			{
				failure = "Staged host mod resolves to an unexpected path";
				return false;
			}
			return true;
		}

		void onConnectionFailed(const std::string & error) override { failure = error; }
		void onConnectionEstablished(const NetworkConnectionPtr & ready) override
		{
			connection = ready;
			peer = std::make_unique<GameConnection>(ready);
			peer->enterLobbyConnectionMode();
			LobbyClientConnected join;
			join.uuid = uuid;
			join.names = {"LAN guest"};
			join.mode = EStartMode::NEW_GAME;
			peer->sendPack(join);
		}
		void onDisconnected(const NetworkConnectionPtr &, const std::string & error) override
		{
			if(!done)
				failure = error;
		}
		void onPacketReceived(const NetworkConnectionPtr &, const std::vector<std::byte> & buffer) override
		{
			auto packet = peer->retrievePack(buffer);
			if(dynamic_cast<LobbyClientConnected *>(packet.get()))
			{
				const auto plan = ModVerificationInfo::buildSyncPlanFromPersistentCache(
					{{"hostmod", ModVerificationStatus::NOT_INSTALLED}}, hashes, userData);
				if(plan.reuseCachedArchive.contains("hostmod"))
				{
					// The second join must actually mount the cached ZIP, not just
					// skip downloading it based on its digest.
					done = canMountVerifiedHostRoot();
					return;
				}
				LobbyQueryLanModArchiveChunk request;
				request.rootId = "hostmod";
				request.sha256 = hashes.at("hostmod");
				peer->sendPack(request);
			}
			else if(const auto * chunk = dynamic_cast<LobbyLanModArchiveChunk *>(packet.get()))
			{
				if(!chunk->granted || chunk->data.empty() || chunk->data.size() > 64 * 1024
					|| chunk->totalBytes != expectedBytes || chunk->sha256 != hashes.at("hostmod")
					|| chunk->offset != receivedArchive.size())
				{
					failure = "Host returned an invalid archive chunk";
					return;
				}
				receivedArchive += chunk->data;
				receivedBytes += chunk->data.size();
				if(receivedArchive.size() < expectedBytes)
				{
					LobbyQueryLanModArchiveChunk request;
					request.rootId = "hostmod";
					request.sha256 = hashes.at("hostmod");
					request.offset = receivedArchive.size();
					peer->sendPack(request);
					return;
				}
				const auto staged = userData.parent_path() / (userData.filename().string() + "-staged.zip");
				std::ofstream output(staged, std::ios::binary);
				output.write(receivedArchive.data(), static_cast<std::streamsize>(receivedArchive.size()));
				output.close();
				if(!LanModArchiveCache::publishVerifiedArchive(userData, "hostmod", chunk->sha256, staged))
				{
					failure = "Could not verify and publish the received archive";
					return;
				}
				done = canMountVerifiedHostRoot();
			}
		}
	};

	auto server = network->createServerTCP(host);
	const auto port = server->start(0);
	ASSERT_NE(port, 0);
	Guest first;
	first.userData = guestData;
	first.hashes = hashes;
	first.expectedBytes = archiveBytes;
	Guest concurrent;
	concurrent.userData = concurrentGuestData;
	concurrent.hashes = hashes;
	concurrent.expectedBytes = archiveBytes;
	concurrent.uuid = "22222222-2222-4222-8222-222222222222";
	// Both real TCP guests request the same host ZIP at once. Each registered
	// connection must have its own stream offset and verified cache.
	network->connectToRemote(first, "127.0.0.1", port);
	network->connectToRemote(concurrent, "127.0.0.1", port);
	network->getContext().run_for(std::chrono::milliseconds(1500));
	EXPECT_TRUE(first.failure.empty()) << first.failure;
	EXPECT_TRUE(concurrent.failure.empty()) << concurrent.failure;
	ASSERT_TRUE(first.done);
	ASSERT_TRUE(concurrent.done);
	EXPECT_EQ(first.receivedBytes, archiveBytes);
	EXPECT_EQ(concurrent.receivedBytes, archiveBytes);
	EXPECT_EQ(host.requests, 4); // Each guest crosses the 64 KiB boundary independently.
	EXPECT_EQ(host.sentBytes, archiveBytes * 2);
	ASSERT_TRUE(LanModArchiveCache::findVerifiedArchive(guestData, "hostmod", digest));
	ASSERT_TRUE(LanModArchiveCache::findVerifiedArchive(concurrentGuestData, "hostmod", digest));
	first.connection->close();
	concurrent.connection->close();

	Guest second;
	second.userData = guestData;
	second.hashes = hashes;
	second.expectedBytes = archiveBytes;
	network->getContext().restart();
	network->connectToRemote(second, "127.0.0.1", port);
	network->getContext().run_for(std::chrono::milliseconds(1500));
	EXPECT_TRUE(second.failure.empty()) << second.failure;
	EXPECT_TRUE(second.done);
	EXPECT_EQ(second.receivedBytes, 0);
	EXPECT_EQ(host.requests, 4); // Repeated join never requests cached bytes.
	EXPECT_EQ(host.sentBytes, archiveBytes * 2);
	network->stop();
	std::error_code cleanup;
	fs::remove_all(directory, cleanup);
}

TEST(LanModSyncTest, HostStagedArchiveManifestFeedsGuestVerifiedCachePreflight)
{
	namespace fs = std::filesystem;
	const fs::path directory = fs::temp_directory_path() / ("vcmi-lan-host-manifest-" + LanRelaunchToken::generate());
	const fs::path shared = directory / "host" / "lan-mod-shares";
	const fs::path guestData = directory / "guest";
	ASSERT_TRUE(fs::create_directories(shared));
	{
		std::ofstream archive(shared / "hostmod.zip", std::ios::binary);
		archive << "abc";
	}
	{
		std::ofstream archive(shared / "disabled.zip", std::ios::binary);
		archive << "other";
	}

	ModCompatibilityInfo activeMods;
	ModVerificationInfo root;
	root.name = "Host Mod";
	activeMods.emplace("hostmod", root);
	ModVerificationInfo child;
	child.parent = "hostmod";
	activeMods.emplace("hostmod.units", child);

	const auto manifest = ModVerificationInfo::buildHostArchiveManifest(activeMods, directory / "host");
	ASSERT_EQ(manifest.size(), 1);
	const auto & offered = manifest.at("hostmod");
	EXPECT_EQ(offered.sha256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	EXPECT_EQ(offered.bytes, 3);

	// VCMI may announce enabled child mods but omit the parent root. The same
	// explicitly shared root archive must remain visible to the lobby preview
	// and downloadable through the actual host transfer reader.
	const ModCompatibilityInfo submodOnly = {{"hostmod.units", child}};
	const auto submodManifest = ModVerificationInfo::buildHostArchiveManifest(submodOnly, directory / "host");
	ASSERT_EQ(submodManifest.size(), 1);
	EXPECT_EQ(submodManifest.at("hostmod").sha256, offered.sha256);
	const auto transfer = LanHostArchiveTransfer::open(submodOnly, directory / "host", "hostmod", offered.sha256);
	ASSERT_NE(transfer, nullptr);
	const auto chunk = transfer->next("hostmod", offered.sha256, 0);
	ASSERT_TRUE(chunk);
	EXPECT_EQ(chunk->data, "abc");
	EXPECT_EQ(chunk->totalBytes, 3);
	EXPECT_TRUE(transfer->complete());
	// Prefix lookalikes are not active members of the requested root.
	EXPECT_EQ(LanHostArchiveTransfer::open({{"hostmodextra.units", child}},
		directory / "host", "hostmod", offered.sha256), nullptr);

	const ModListVerificationStatus missing = {{"hostmod", ModVerificationStatus::NOT_INSTALLED}};
	const ModListVerificationStatus previewStatuses = {
		{"hostmod", ModVerificationStatus::NOT_INSTALLED},
		{"hostmod.units", ModVerificationStatus::NOT_INSTALLED},
		{"unknown", ModVerificationStatus::NOT_INSTALLED},
		{"disabled", ModVerificationStatus::DISABLED}
	};
	const auto availabilityBefore = ModVerificationInfo::inspectHostArchiveAvailability(previewStatuses, manifest, guestData);
	ASSERT_EQ(availabilityBefore.size(), 2); // root and submod share one package
	EXPECT_EQ(availabilityBefore.at("hostmod"), LanArchivePreviewState::OFFERED_BY_HOST);
	EXPECT_EQ(availabilityBefore.at("unknown"), LanArchivePreviewState::UNAVAILABLE);
	EXPECT_FALSE(availabilityBefore.contains("disabled"));
	EXPECT_EQ(ModVerificationInfo::estimateUncachedArchiveBytes(availabilityBefore, manifest), 3);

	const ModArchiveHashes hashes = {{"hostmod", offered.sha256}};
	const auto before = ModVerificationInfo::buildSyncPlanFromPersistentCache(missing, hashes, guestData);
	EXPECT_TRUE(before.downloadOrUpdate.contains("hostmod"));
	ASSERT_TRUE(LanModArchiveCache::publishVerifiedArchive(guestData, "hostmod", offered.sha256, shared / "hostmod.zip").has_value());
	const auto availabilityAfter = ModVerificationInfo::inspectHostArchiveAvailability(previewStatuses, manifest, guestData);
	EXPECT_EQ(availabilityAfter.at("hostmod"), LanArchivePreviewState::VERIFIED_IN_CACHE);
	EXPECT_EQ(availabilityAfter.at("unknown"), LanArchivePreviewState::UNAVAILABLE);
	EXPECT_EQ(ModVerificationInfo::estimateUncachedArchiveBytes(availabilityAfter, manifest), 0);
	const auto after = ModVerificationInfo::buildSyncPlanFromPersistentCache(missing, hashes, guestData);
	EXPECT_TRUE(after.downloadOrUpdate.empty());
	EXPECT_EQ(after.reuseCachedArchive, std::set<TModID>({"hostmod"}));

	std::error_code error;
	fs::remove_all(directory, error);
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
	const LanRejoinTarget selected{"192.168.1.42", 3030, true, {"Alice", "Bob"}, true, std::string(64, 'b')};
	LanModSessionState::setRejoinTarget(config, selected);
	const std::string token(64, 'a');
	LanModSessionState::prepareControlledRelaunch(config, token);

	EXPECT_EQ(LanModSessionState::recoverOnStartup(config, token), LanSessionStartupAction::RESUMED);
	const auto resumed = LanModSessionState::getRejoinTarget(config);
	ASSERT_TRUE(resumed.has_value());
	EXPECT_EQ(resumed->hostname, selected.hostname);
	EXPECT_EQ(resumed->port, selected.port);
	EXPECT_TRUE(resumed->previewCapability);
	EXPECT_EQ(resumed->playerNames, selected.playerNames);
	EXPECT_TRUE(resumed->loadGame);
	EXPECT_EQ(resumed->approvedHostModsSha256, selected.approvedHostModsSha256);

	// Sessions saved by earlier clients contain only the host endpoint and capability.
	JsonNode legacy = config;
	legacy["lanSession"]["rejoin"].Struct().erase("playerNames");
	legacy["lanSession"]["rejoin"].Struct().erase("loadGame");
	legacy["lanSession"]["rejoin"].Struct().erase("approvedHostModsSha256");
	const auto oldTarget = LanModSessionState::getRejoinTarget(legacy);
	ASSERT_TRUE(oldTarget.has_value());
	EXPECT_TRUE(oldTarget->playerNames.empty());
	EXPECT_FALSE(oldTarget->loadGame);
	EXPECT_TRUE(oldTarget->approvedHostModsSha256.empty());
	EXPECT_TRUE(LanModSessionState::shouldRestoreOnShutdown(config));

	EXPECT_TRUE(LanModSessionState::restore(config));
	EXPECT_FALSE(LanModSessionState::getRejoinTarget(config).has_value());
	EXPECT_EQ(config.toCompactString(), original);
}

TEST(LanModSyncTest, ApprovedHostModListDetectsChangesBeforeAutomaticRejoin)
{
	ModCompatibilityInfo hostMods;
	ModVerificationInfo first;
	first.version = CModVersion::fromString("1.0");
	first.impactsGameplay = true;
	hostMods["lan-ci-fixture"] = first;
	const auto approved = ModVerificationInfo::fingerprintHostMods(hostMods);
	EXPECT_EQ(approved.size(), 64U);
	EXPECT_EQ(approved, ModVerificationInfo::fingerprintHostMods(hostMods));
	ModVerificationInfo second = first;
	second.version = CModVersion::fromString("1.1");
	hostMods["lan-ci-fixture"] = second;
	EXPECT_NE(approved, ModVerificationInfo::fingerprintHostMods(hostMods));
	hostMods["lan-ci-fixture"] = first;
	hostMods["another-gameplay-mod"] = first;
	EXPECT_NE(approved, ModVerificationInfo::fingerprintHostMods(hostMods));
}

TEST(LanModSyncTest, ApprovedLanRejoinRejectsChangedHostArchiveWithoutModVersionChange)
{
	ModCompatibilityInfo hostMods;
	ModVerificationInfo gameplayMod;
	gameplayMod.version = CModVersion::fromString("1.0");
	gameplayMod.impactsGameplay = true;
	hostMods["lan-ci-fixture"] = gameplayMod;
	ModArchiveManifest archives;
	archives.emplace("lan-ci-fixture", ModArchiveManifestEntry{std::string(64, 'a'), 42});
	const auto approved = ModVerificationInfo::fingerprintLanHostOffer(hostMods, archives);
	EXPECT_EQ(approved.size(), 64U);
	EXPECT_EQ(approved, ModVerificationInfo::fingerprintLanHostOffer(hostMods, archives));
	EXPECT_NE(approved, ModVerificationInfo::fingerprintLanHostOffer(hostMods, {}));
	EXPECT_NE(approved, ModVerificationInfo::fingerprintHostMods(hostMods));

	// The advertised gameplay mod is unchanged, but the archive offered by the
	// real host has been replaced while the guest is restarting to activate it.
	archives.at("lan-ci-fixture").sha256 = std::string(64, 'b');
	EXPECT_NE(approved, ModVerificationInfo::fingerprintLanHostOffer(hostMods, archives));
	archives.at("lan-ci-fixture").sha256 = std::string(64, 'a');
	archives.at("lan-ci-fixture").bytes = 43;
	EXPECT_NE(approved, ModVerificationInfo::fingerprintLanHostOffer(hostMods, archives));
	archives.at("lan-ci-fixture").bytes = 42;

	gameplayMod.version = CModVersion::fromString("1.1");
	hostMods["lan-ci-fixture"] = gameplayMod;
	EXPECT_NE(approved, ModVerificationInfo::fingerprintLanHostOffer(hostMods, archives));
}

TEST(LanModSyncTest, AutomaticLanRejoinRequiresTheOriginalOneShotToken)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["settings"]["hostmod"]["units"].Bool() = false;
	const auto original = config.toCompactString();

	LanModSessionState::begin(config);
	LanModSessionState::setRejoinTarget(config, {"192.168.1.42", 3030, true});
	LanModSessionState::prepareControlledRelaunch(config, std::string(64, 'a'));
	EXPECT_EQ(LanModSessionState::recoverOnStartup(config, std::string(64, 'b')), LanSessionStartupAction::RESTORED);
	EXPECT_FALSE(LanModSessionState::getRejoinTarget(config).has_value());
	EXPECT_EQ(config.toCompactString(), original);
}

TEST(LanModSyncTest, CancellingRejoinedLobbyRestoresOriginalModsAndSubmods)
{
	JsonNode config;
	config["activePreset"].String() = "personal";
	config["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	config["presets"]["personal"]["mods"].Vector().emplace_back("personalmod");
	config["presets"]["personal"]["settings"]["personalmod"]["variants"].Bool() = true;
	config["presets"]["personal"]["settings"]["hostmod"]["units"].Bool() = false;
	config["presets"]["other"]["mods"].Vector().emplace_back("vcmi");
	const auto original = config.toCompactString();

	ModSyncPlan plan;
	plan.activate.insert("hostmod.units");
	plan.deactivate.insert("personalmod");
	LanModSessionState::begin(config);
	LanModSessionState::applyInstalledOnlyPlan(config, plan);
	LanModSessionState::setRejoinTarget(config, {"192.168.1.42", 3030, true});
	const std::string token(64, 'a');
	LanModSessionState::prepareControlledRelaunch(config, token);

	ASSERT_EQ(LanModSessionState::recoverOnStartup(config, token), LanSessionStartupAction::RESUMED);
	EXPECT_TRUE(LanModSessionState::getRejoinTarget(config).has_value());
	EXPECT_TRUE(LanModSessionState::restore(config));
	EXPECT_FALSE(LanModSessionState::getRejoinTarget(config).has_value());
	EXPECT_FALSE(LanModSessionState::shouldRestoreOnShutdown(config));
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

TEST(LanModSyncTest, InstalledOnlyLanPlanRequiresResolverToPreserveEveryGameplayMod)
{
	ModCompatibilityInfo hostMods;
	ModVerificationInfo root;
	root.impactsGameplay = true;
	hostMods.emplace("hostmod", root);
	ModVerificationInfo child;
	child.parent = "hostmod";
	child.impactsGameplay = true;
	hostMods.emplace("hostmod.units", child);
	ModVerificationInfo cosmetic;
	cosmetic.impactsGameplay = false;
	hostMods.emplace("cosmetic", cosmetic);

	ModSyncPlan plan;
	plan.activate.insert("hostmod.units");
	plan.deactivate.insert("personalmod");
	EXPECT_TRUE(plan.matchesResolvedHostMods(hostMods, {"vcmi", "hostmod", "hostmod.units"}));
	EXPECT_FALSE(plan.matchesResolvedHostMods(hostMods, {"vcmi", "hostmod"}));
	EXPECT_FALSE(plan.matchesResolvedHostMods(hostMods, {"vcmi", "hostmod.units"}));
	EXPECT_FALSE(plan.matchesResolvedHostMods(hostMods, {"vcmi", "hostmod", "hostmod.units", "personalmod"}));
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

#if defined(__linux__) && !defined(VCMI_MOBILE)
TEST(LanModSyncTest, DesktopRelaunchUsesOriginalAppImageInsteadOfTemporaryMount)
{
	const std::filesystem::path mountedClient("/tmp/.mount_vcmi/usr/bin/vcmiclient");
	const std::filesystem::path originalImage("/home/player/VCMI.AppImage");
	const char * imagePath = "/home/player/VCMI.AppImage";
	const char * mountPath = "/tmp/.mount_vcmi";

	EXPECT_EQ(LanRelaunchProcess::executableForRelaunch(mountedClient, imagePath, mountPath), originalImage);
	// Do not restart an unrelated AppImage for normal installations or for
	// other mounted archives that happen to share the same path prefix.
	EXPECT_EQ(LanRelaunchProcess::executableForRelaunch("/usr/bin/vcmiclient", imagePath, mountPath),
		std::filesystem::path("/usr/bin/vcmiclient"));
	EXPECT_EQ(LanRelaunchProcess::executableForRelaunch("/tmp/.mount_vcmi-other/usr/bin/vcmiclient", imagePath, mountPath),
		std::filesystem::path("/tmp/.mount_vcmi-other/usr/bin/vcmiclient"));
	EXPECT_EQ(LanRelaunchProcess::executableForRelaunch(mountedClient, nullptr, mountPath), mountedClient);
	EXPECT_EQ(LanRelaunchProcess::executableForRelaunch(mountedClient, imagePath, nullptr), mountedClient);
	EXPECT_EQ(LanRelaunchProcess::executableForRelaunch(mountedClient, "VCMI.AppImage", mountPath), mountedClient);
}
#endif

#if !defined(VCMI_MOBILE) && !defined(_WIN32)
TEST(LanModSyncTest, RestoredPersonalPresetRestartDoesNotPassLanRejoinToken)
{
	namespace fs = std::filesystem;
	const auto directory = fs::temp_directory_path() / ("vcmi-lan-personal-restart-" + LanRelaunchToken::generate());
	fs::create_directories(directory);
	const auto cleanup = vstd::makeScopeGuard([&]()
	{
		std::error_code ignored;
		fs::remove_all(directory, ignored);
	});

	const auto executable = directory / "check-no-lan-token.sh";
	{
		std::ofstream script(executable);
		ASSERT_TRUE(script.good());
		script << "#!/bin/sh\n[ \"$#\" -eq 0 ]\n";
	}
	fs::permissions(executable, fs::perms::owner_exec, fs::perm_options::add);
	const auto pid = LanRelaunchProcess::spawnRestoredPreset(executable);
	int status = 0;
	ASSERT_EQ(::waitpid(static_cast<pid_t>(pid), &status, 0), static_cast<pid_t>(pid));
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif

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



#if !defined(VCMI_MOBILE) && !defined(_WIN32)
TEST(LanModSyncTest, RealDesktopClientProcessesResumeInstalledModThenRecoverAfterCrash)
{
	// Exercise the linked desktop executable, not a simulated ModManager.
	// Use only the tiny redistributable test mod created by the CI workflow.
	const char * requiredMod = std::getenv("VCMI_LAN_TEST_REQUIRED_MOD");
	if(!requiredMod || std::string(requiredMod) != "lan-ci-fixture")
		GTEST_SKIP() << "Requires the public installed-only LAN CI fixture";

	namespace fs = std::filesystem;
	const fs::path executable = fs::current_path() / "vcmiclient";
	if(!fs::is_regular_file(executable))
		GTEST_SKIP() << "Requires a linked vcmiclient executable";

	const fs::path sandbox = fs::temp_directory_path() / ("vcmi-lan-process-" + LanRelaunchToken::generate());
	const fs::path configRoot = sandbox / "config";
	const fs::path modSettings = configRoot / "vcmi" / "modSettings.json";
	ASSERT_TRUE(fs::create_directories(modSettings.parent_path()));
	const auto cleanup = vstd::makeScopeGuard([&]()
	{
		std::error_code error;
		fs::remove_all(sandbox, error);
	});

	JsonNode original;
	original["activePreset"].String() = "personal";
	original["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
	// The real client persists the built-in core mod in every normalized preset.
	original["presets"]["personal"]["mods"].Vector().emplace_back("core");
	original["presets"]["personal"]["settings"].Struct();
	// Normal VCMI startup records the installed nested mod as disabled.
	original["presets"]["personal"]["settings"]["lan-ci-extra"]["units"].Bool() = false;
	JsonNode session = original;
	LanModSessionState::begin(session);
	ModSyncPlan installedOnly;
	installedOnly.activate.insert(requiredMod);
	LanModSessionState::applyInstalledOnlyPlan(session, installedOnly);
	LanModSessionState::setRejoinTarget(session, {"127.0.0.1", 3030, true, {"Guest"}, false});
	const std::string token = LanRelaunchToken::generate();
	LanModSessionState::prepareControlledRelaunch(session, token);
	// Match the production sequence: original settings, durable recovery backup,
	// then the temporary LAN preset. A crash must recover the original bytes.
	const std::string originalBytes = original.toCompactString();
	ASSERT_TRUE(AtomicModSettings::replace(modSettings, originalBytes));
	ASSERT_TRUE(LanModRecoveryJournal::persist(modSettings, originalBytes));
	ASSERT_TRUE(AtomicModSettings::replace(modSettings, session.toCompactString()));

	const auto launch = [&](bool controlled) -> pid_t
	{
		const pid_t child = ::fork();
		if(child == 0)
		{
			if(::setenv("XDG_CONFIG_HOME", configRoot.c_str(), 1) != 0)
				::_exit(126);
			::setenv("SDL_VIDEODRIVER", "dummy", 1);
			::setenv("SDL_AUDIODRIVER", "dummy", 1);
			if(controlled)
				::execl(executable.c_str(), executable.c_str(), "--headless", "--nointro",
					"--lan-relaunch-token", token.c_str(), static_cast<char *>(nullptr));
			else
				::execl(executable.c_str(), executable.c_str(), "--headless", "--nointro",
					static_cast<char *>(nullptr));
			::_exit(127);
		}
		return child;
	};

	const auto stop = [](pid_t & pid)
	{
		if(pid <= 0)
			return;
		::kill(pid, SIGKILL); // Intentionally simulate a crashed client.
		int status = 0;
		while(::waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
		pid = -1;
	};

	const auto observe = [&](pid_t & pid, bool resumed)
	{
		for(int attempt = 0; attempt < 120; ++attempt)
		{
			try
			{
				std::ifstream input(modSettings, std::ios::binary);
				if(input)
				{
					const std::string bytes((std::istreambuf_iterator<char>(input)),
						std::istreambuf_iterator<char>());
					JsonNode state(bytes.data(), bytes.size(), "Real VCMI LAN process settings");
					const JsonNode & current = state;
					if(resumed)
					{
						const JsonNode & temporary = current["presets"][LanModSessionState::temporaryPresetName()]["mods"];
						if(current["activePreset"].isString()
							&& current["activePreset"].String() == LanModSessionState::temporaryPresetName()
							&& current["lanSession"]["relaunchToken"].isNull()
							&& temporary.isVector()
							&& std::find(temporary.Vector().begin(), temporary.Vector().end(),
								JsonNode(requiredMod)) != temporary.Vector().end())
							return true;
					}
					else if(current["lanSession"].isNull()
						&& current["activePreset"].isString()
						&& current["activePreset"].String() == "personal"
						&& current["presets"]["personal"].toCompactString()
							== original["presets"]["personal"].toCompactString())
						return true;
				}
			}
			catch(const std::exception &)
			{
				// The other process might be atomically replacing this file.
			}
			int status = 0;
			if(::waitpid(pid, &status, WNOHANG) == pid)
			{
				pid = -1;
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}
		return false;
	};

	pid_t first = launch(true);
	ASSERT_GT(first, 0);
	const bool resumed = observe(first, true);
	stop(first);
	ASSERT_TRUE(resumed) << "vcmiclient failed to consume the one-shot token and retain the installed host mod";

	// A fresh client without a one-shot token must recover the exact personal
	// preset after the first client is forcibly terminated.
	pid_t second = launch(false);
	ASSERT_GT(second, 0);
	const bool recovered = observe(second, false);
	stop(second);
	if(!recovered)
	{
		std::ifstream finalFile(modSettings, std::ios::binary);
		const std::string finalBytes((std::istreambuf_iterator<char>(finalFile)),
			std::istreambuf_iterator<char>());
		ADD_FAILURE() << "vcmiclient failed to restore the personal preset after a crash. "
			<< "Final mod settings: " << finalBytes;
	}
}

TEST(LanModSyncTest, RealDesktopClientMountsCachedHostZipThenRecoversPersonalPreset)
{
	// Exercise the linked client with an actual cached ZIP and one-shot LAN mount.
	// The public fixture cannot parse all commercial H3Sprite data, so verify
	// the real mount before gameplay loading. Full data also verifies mod parsing.
	const char * requiredMod = std::getenv("VCMI_LAN_TEST_REQUIRED_MOD");
	if(!requiredMod || std::string(requiredMod) != "lan-ci-fixture")
		GTEST_SKIP() << "Requires the public LAN CI fixture";

	namespace fs = std::filesystem;
	const fs::path executable = fs::current_path() / "vcmiclient";
	if(!fs::is_regular_file(executable))
		GTEST_SKIP() << "Requires the linked desktop client";
	const bool fullGameData = std::getenv("VCMI_LAN_TEST_COMPLETE_H3_DATA") != nullptr;

	const fs::path sandbox = fs::temp_directory_path() / ("vcmi-lan-staged-process-" + LanRelaunchToken::generate());
	const fs::path dataHome = sandbox / "data";
	const fs::path userData = dataHome / "vcmi";
	const fs::path configHome = sandbox / "config";
	const fs::path modSettings = configHome / "vcmi" / "modSettings.json";
	ASSERT_TRUE(fs::create_directories(userData));
	ASSERT_TRUE(fs::create_directories(modSettings.parent_path()));
	pid_t first = -1;
	pid_t second = -1;
	const auto stop = [](pid_t & pid)
	{
		if(pid <= 0)
			return;
		::kill(pid, SIGKILL);
		int status = 0;
		while(::waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
		pid = -1;
	};
	const auto cleanup = vstd::makeScopeGuard([&]()
	{
		stop(first);
		stop(second);
		std::error_code error;
		fs::remove_all(sandbox, error);
	});

	// Keep the public fixture available without writing to the user's
	// installation. Only the temporary LAN cache lives in the sandbox.
	const fs::path installedData(VCMIDirs::get().userDataPath().native());
	for(const char * folder : {"Data", "Maps", "Mp3", "Mods"})
	{
		if(fs::is_directory(installedData / folder))
			fs::create_directory_symlink(fs::absolute(installedData / folder), userData / folder);
	}
	const std::string rootId = "lan-ci-staged";
	ASSERT_FALSE(fs::exists(userData / "Mods" / rootId));

	const fs::path archive = sandbox / "host.zip";
	zipFile writer = zipOpen(archive.string().c_str(), APPEND_STATUS_CREATE);
	ASSERT_NE(writer, nullptr);
	const std::string definition = R"({"name":"LAN staged startup test","author":"VCMI test","version":"1.0","modType":"Mechanics","settings":{}})";
	ASSERT_EQ(zipOpenNewFileInZip(writer, "lan-ci-staged/mod.json", nullptr,
		nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED, Z_DEFAULT_COMPRESSION), ZIP_OK);
	ASSERT_EQ(zipWriteInFileInZip(writer, definition.data(), static_cast<unsigned int>(definition.size())), ZIP_OK);
	ASSERT_EQ(zipCloseFileInZip(writer), ZIP_OK);
	ASSERT_EQ(zipClose(writer, nullptr), ZIP_OK);
	const auto digest = ModArchiveDigest::sha256File(archive, LanModArchiveCache::maxArchiveBytes);
	ASSERT_TRUE(digest);
	ASSERT_TRUE(LanModArchiveCache::publishVerifiedArchive(userData, rootId, *digest, archive));
	ASSERT_TRUE(LanModPackageStage::stageVerifiedArchive(userData, rootId, *digest));

	JsonNode original;
	original["activePreset"].String() = "personal";
	auto & personal = original["presets"]["personal"];
	personal["mods"].Vector().emplace_back("vcmi");
	personal["mods"].Vector().emplace_back("core");
	personal["settings"].Struct();
	personal["settings"]["lan-ci-extra"]["units"].Bool() = false;
	JsonNode session = original;
	LanModSessionState::begin(session);
	ModSyncPlan plan;
	plan.activate.insert(rootId);
	LanModSessionState::applyInstalledOnlyPlan(session, plan);
	LanModSessionState::setStagedMods(session, {{rootId, *digest}});
	LanModSessionState::setRejoinTarget(session, {"127.0.0.1", 3030, true, {"Guest"}, false, {}});
	const std::string token = LanRelaunchToken::generate();
	LanModSessionState::prepareControlledRelaunch(session, token);
	const std::string originalBytes = original.toCompactString();
	ASSERT_TRUE(AtomicModSettings::replace(modSettings, originalBytes));
	ASSERT_TRUE(LanModRecoveryJournal::persist(modSettings, originalBytes));
	ASSERT_TRUE(AtomicModSettings::replace(modSettings, session.toCompactString()));

	const fs::path resumedLog = sandbox / "controlled.log";
	const fs::path recoveryLog = sandbox / "recovered.log";
	const auto launch = [&](bool controlled, const fs::path & output) -> pid_t
	{
		const pid_t child = ::fork();
		if(child == 0)
		{
			if(::setenv("XDG_CONFIG_HOME", configHome.c_str(), 1) != 0
				|| ::setenv("XDG_DATA_HOME", dataHome.c_str(), 1) != 0
				|| ::setenv("XDG_CACHE_HOME", (sandbox / "cache").c_str(), 1) != 0)
				::_exit(126);
			::setenv("SDL_VIDEODRIVER", "dummy", 1);
			::setenv("SDL_AUDIODRIVER", "dummy", 1);
			FILE * log = std::freopen(output.c_str(), "w", stdout);
			if(!log || ::dup2(::fileno(log), STDERR_FILENO) < 0)
				::_exit(126);
			if(controlled)
				::execl(executable.c_str(), executable.c_str(), "--headless", "--nointro",
					"--lan-relaunch-token", token.c_str(), static_cast<char *>(nullptr));
			else
				::execl(executable.c_str(), executable.c_str(), "--headless", "--nointro",
					static_cast<char *>(nullptr));
			::_exit(127);
		}
		return child;
	};
	const auto logContains = [](const fs::path & path, const std::string & needle)
	{
		std::ifstream file(path, std::ios::binary);
		if(!file)
			return false;
		const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		return content.find(needle) != std::string::npos;
	};
	const auto settingsState = [&]() -> std::optional<JsonNode>
	{
		try
		{
			std::ifstream file(modSettings, std::ios::binary);
			const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			return JsonNode(bytes.data(), bytes.size(), "real staged LAN client settings");
		}
		catch(const std::exception &)
		{
			return std::nullopt; // A separate process may be replacing the file.
		}
	};
	const auto waitFor = [&](pid_t & pid, const std::function<bool()> & predicate)
	{
		for(int attempt = 0; attempt < 160; ++attempt)
		{
			if(predicate())
				return true;
			int status = 0;
			if(::waitpid(pid, &status, WNOHANG) == pid)
			{
				pid = -1;
				return predicate();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}
		return false;
	};

	first = launch(true, resumedLog);
	ASSERT_GT(first, 0);
	const bool loaded = waitFor(first, [&]()
	{
		const auto state = settingsState();
		return state && (*state)["activePreset"].isString()
			&& (*state)["activePreset"].String() == LanModSessionState::temporaryPresetName()
			&& (*state)["lanSession"]["relaunchToken"].isNull()
			&& logContains(resumedLog, "Mounted verified LAN host mod: " + rootId)
			&& (!fullGameData || logContains(resumedLog, "Loading mod: OK (" + rootId + ")"));
	});
	stop(first);
	std::ifstream controlledOutput(resumedLog, std::ios::binary);
	const std::string controlledLog((std::istreambuf_iterator<char>(controlledOutput)),
		std::istreambuf_iterator<char>());
	ASSERT_TRUE(loaded) << "Controlled vcmiclient failed to mount the cached host mod (or parse it with complete game data)"
		<< "\nControlled process output:\n" << controlledLog.substr(0, 16384);

	// A crashed client must recover exactly the previous preset on the next
	// ordinary startup. The ZIP stays cached, but must no longer be mounted.
	second = launch(false, recoveryLog);
	ASSERT_GT(second, 0);
	const bool recovered = waitFor(second, [&]()
	{
		const auto state = settingsState();
		return state && (*state)["lanSession"].isNull()
			&& (*state)["activePreset"].isString() && (*state)["activePreset"].String() == "personal"
			&& (*state)["presets"]["personal"].toCompactString() == original["presets"]["personal"].toCompactString()
			&& (!fullGameData || logContains(recoveryLog, "Loading mod: OK (vcmi)"));
	});
	stop(second);
	EXPECT_TRUE(recovered) << "Ordinary vcmiclient startup failed to restore the original preset";
	EXPECT_FALSE(logContains(recoveryLog, "Mounted verified LAN host mod: " + rootId))
		<< "Staged LAN mount leaked into the recovered personal process";
	EXPECT_FALSE(logContains(recoveryLog, "Loading mod: OK (" + rootId + ")"))
		<< "Staged LAN mod leaked into the recovered personal process";
	EXPECT_TRUE(LanModArchiveCache::findVerifiedArchive(userData, rootId, *digest));
	EXPECT_FALSE(fs::exists(userData / "Mods" / rootId));
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


TEST(LanModSyncTest, ActualTestPresetRoundTripsInstalledOnlyRelaunchThroughDisk)
{
	// Use VCMI's real test mod-settings file, not an in-memory JSON copy.
	ModsPresetState before(true);
	const auto personalPresetName = before.getActivePreset();
	const auto personalPreset = before.exportCurrentPreset().toCompactString();
	const auto personalPresets = before.getAllPresets();
	const auto personalActive = before.getActiveMods();
	if(std::find(personalActive.begin(), personalActive.end(), "roe-demo") != personalActive.end())
		GTEST_SKIP() << "The RoE demo mod is already active in this test environment";

	// Retain a recovery path if an assertion or simulated relaunch fails.
	auto restoreTestPreset = vstd::makeScopeGuard([]()
	{
		try
		{
			ModsPresetState recovery(true);
			recovery.restoreLanSession();
		}
		catch(const std::exception & e)
		{
			ADD_FAILURE() << "Could not recover on-disk LAN test preset: " << e.what();
		}
	});

	ModSyncPlan plan;
	plan.activate.insert("roe-demo"); // shipped with VCMI, normally disabled
	EXPECT_TRUE(plan.downloadOrUpdate.empty());
	const LanRejoinTarget host{"127.0.0.1", 3030, true, {"LAN guest"}, false};
	before.prepareInstalledOnlyLanSession(plan, host);
	EXPECT_EQ(before.getActivePreset(), LanModSessionState::temporaryPresetName());
	const auto stagedMods = before.getActiveMods();
	EXPECT_NE(std::find(stagedMods.begin(), stagedMods.end(), "roe-demo"), stagedMods.end());

	const auto token = LanRelaunchToken::generate();
	before.prepareControlledRelaunch(token);
	ModsPresetState restarted(true, token);
	EXPECT_EQ(restarted.getActivePreset(), LanModSessionState::temporaryPresetName());
	const auto resume = restarted.consumeLanRejoinTarget();
	EXPECT_TRUE(resume.has_value());
	if(resume)
	{
		EXPECT_EQ(resume->hostname, host.hostname);
		EXPECT_EQ(resume->port, host.port);
		EXPECT_EQ(resume->playerNames, host.playerNames);
	}
	const auto activeAfterRestart = restarted.getActiveMods();
	EXPECT_NE(std::find(activeAfterRestart.begin(), activeAfterRestart.end(), "roe-demo"), activeAfterRestart.end());

	restarted.restoreLanSession();
	ModsPresetState restored(true);
	EXPECT_EQ(restored.getActivePreset(), personalPresetName);
	EXPECT_EQ(restored.exportCurrentPreset().toCompactString(), personalPreset);
	EXPECT_EQ(restored.getAllPresets(), personalPresets);
	EXPECT_EQ(restored.getActiveMods(), personalActive);
}

static std::size_t countLanPlayableStartingHeroes(const CGameState & gameState)
{
	std::size_t ready = 0;
	for(int player = 0; player < 3; ++player)
	{
		const auto * hero = gameState.getMap().getHero(HeroTypeID(player));
		if(hero && hero->getOwner() == PlayerColor(player) && hero->hasStackAtSlot(SlotID(0)))
			++ready;
	}
	return ready;
}

TEST(LanModSyncTest, RealIndependentVCMIServerProvidesLegacyAndOptInLobbyPreview)
{
	// The workflow launches a separate real vcmiserver. No stub host is used.
	const char * portText = std::getenv("VCMI_LAN_TEST_SERVER_PORT");
	if(!portText)
		GTEST_SKIP() << "Requires an independent vcmiserver process";
	const int port = std::stoi(portText);
	ASSERT_GT(port, 0);
	ASSERT_LE(port, 65535);

	auto network = INetworkHandler::createHandler();
	struct HostClient final : INetworkClientListener
	{
		NetworkConnectionPtr connection;
		std::unique_ptr<GameConnection> peer;
		std::string error;
		bool joined = false;
		bool receivedStartPreparation = false;
		bool receivedInitializedStart = false;
		std::set<std::string> initializedHumanPlayers;
		std::size_t initializedPlayableHeroes = 0;
		std::vector<std::string> observedGuestNames;

		void onConnectionFailed(const std::string & reason) override { error = reason; }
		void onConnectionEstablished(const NetworkConnectionPtr & ready) override
		{
			connection = ready;
			peer = std::make_unique<GameConnection>(ready);
			peer->enterLobbyConnectionMode();
			LobbyClientConnected join;
			join.uuid = "00000000-0000-4000-8000-000000000001";
			join.names = {"LAN integration host"};
			join.mode = EStartMode::NEW_GAME;
			peer->sendPack(join);
		}
		void onDisconnected(const NetworkConnectionPtr &, const std::string & reason) override
		{
			if(!joined)
				error = reason;
		}
		void onPacketReceived(const NetworkConnectionPtr &, const std::vector<std::byte> & bytes) override
		{
			auto packet = peer->retrievePack(bytes);
			if(dynamic_cast<LobbyPrepareStartGame *>(packet.get()))
			{
				receivedStartPreparation = true;
				return;
			}
			if(const auto * started = dynamic_cast<LobbyStartGame *>(packet.get()))
			{
				receivedInitializedStart = started->initializedStartInfo && started->initializedGameState;
				if(receivedInitializedStart)
				{
					initializedHumanPlayers.clear();
					for(const auto & [color, player] : started->initializedStartInfo->playerInfos)
						if(player.isControlledByHuman())
							initializedHumanPlayers.insert(player.name);
					initializedPlayableHeroes = countLanPlayableStartingHeroes(*started->initializedGameState);
				}
				return;
			}
			if(const auto * accepted = dynamic_cast<LobbyClientConnected *>(packet.get()))
			{
				// LobbyClientConnected is also broadcast when OTHER guests join.
				// Only the first response assigns this host's own connection ID.
				if(joined)
				{
					// Observe real guests accepted by the server, including the
					// independently launched graphical client after LAN relaunch.
					if(accepted->clientId != peer->connectionID)
						observedGuestNames.insert(observedGuestNames.end(), accepted->names.begin(), accepted->names.end());
					return;
				}
				if(accepted->clientId == GameConnectionID::INVALID)
				{
					error = "Real VCMI server returned an invalid host connection ID";
					return;
				}
				joined = true;
				peer->connectionID = accepted->clientId;
				peer->setSerializationVersion(accepted->version);
			}
		}
	} host;

	network->connectToRemote(host, "127.0.0.1", static_cast<std::uint16_t>(port));
	network->getContext().run_for(std::chrono::seconds(4));
	ASSERT_TRUE(host.joined) << "Real VCMI host did not accept a lobby player: " << host.error;
	const auto originalHostId = host.peer->connectionID;

	struct PreviewGuest final : INetworkClientListener
	{
		explicit PreviewGuest(bool optIn) : wantsModSync(optIn) {}
		bool wantsModSync;
		std::string error;
		NetworkConnectionPtr connection;
		std::unique_ptr<GameConnection> peer;
		std::unique_ptr<CPack> response;

		void onConnectionFailed(const std::string & reason) override { error = reason; }
		void onConnectionEstablished(const NetworkConnectionPtr & ready) override
		{
			connection = ready;
			peer = std::make_unique<GameConnection>(ready);
			peer->enterLobbyConnectionMode();
			if(wantsModSync)
			{
				LobbyQueryModSync query;
				peer->sendPack(query);
			}
			else
			{
				LobbyQueryState query;
				peer->sendPack(query);
			}
		}
		void onDisconnected(const NetworkConnectionPtr &, const std::string & reason) override
		{
			if(!response)
				error = reason;
		}
		void onPacketReceived(const NetworkConnectionPtr &, const std::vector<std::byte> & bytes) override
		{
			if(!response)
				response = peer->retrievePack(bytes);
		}
	} legacy(false), optIn(true);

	network->getContext().restart();
	network->connectToRemote(legacy, "127.0.0.1", static_cast<std::uint16_t>(port));
	network->connectToRemote(optIn, "127.0.0.1", static_cast<std::uint16_t>(port));
	network->getContext().run_for(std::chrono::seconds(4));

	ASSERT_TRUE(legacy.error.empty()) << legacy.error;
	ASSERT_TRUE(optIn.error.empty()) << optIn.error;
	const auto * legacyMods = dynamic_cast<LobbyModsCheck *>(legacy.response.get());
	const auto * syncedMods = dynamic_cast<LobbyModSyncPreview *>(optIn.response.get());
	ASSERT_NE(legacyMods, nullptr);
	ASSERT_NE(syncedMods, nullptr);
	EXPECT_EQ(dynamic_cast<LobbyModSyncPreview *>(legacy.response.get()), nullptr);
	EXPECT_EQ(legacyMods->vcmiVersion, VCMI_VERSION_STRING);
	EXPECT_EQ(syncedMods->vcmiVersion, legacyMods->vcmiVersion);
	EXPECT_EQ(legacyMods->hostAccountDisplayName, "LAN integration host");
	EXPECT_EQ(syncedMods->hostAccountDisplayName, legacyMods->hostAccountDisplayName);
	EXPECT_EQ(legacyMods->participantNames, syncedMods->participantNames);
	ASSERT_EQ(legacyMods->participantNames.size(), 1);
	EXPECT_FALSE(legacyMods->mods.empty());
	EXPECT_EQ(syncedMods->mods.size(), legacyMods->mods.size());
	for(const auto & [id, mod] : legacyMods->mods)
	{
		const auto found = syncedMods->mods.find(id);
		ASSERT_NE(found, syncedMods->mods.end()) << id;
		EXPECT_EQ(found->second.version, mod.version);
		EXPECT_EQ(found->second.name, mod.name);
	}
	EXPECT_TRUE(syncedMods->acceptingPlayers);

	// CI supplies a genuinely active, redistributable host gameplay mod which is
	// installed but disabled in the guest test preset. Do not infer a host
	// requirement from a locally selected mod that the host never advertised.
	const char * requiredMod = std::getenv("VCMI_LAN_TEST_REQUIRED_MOD");
	const char * secondMod = std::getenv("VCMI_LAN_TEST_SECOND_MOD");
	const char * secondSubmod = std::getenv("VCMI_LAN_TEST_SECOND_SUBMOD");
	if(secondMod)
	{
		const auto second = syncedMods->mods.find(secondMod);
		ASSERT_NE(second, syncedMods->mods.end()) << "Host active preset omitted its second mod";
		EXPECT_TRUE(second->second.impactsGameplay);
		const auto statuses = ModVerificationInfo::verifyListAgainstLocalMods(syncedMods->mods);
		ASSERT_NE(statuses.find(secondMod), statuses.end());
		EXPECT_EQ(statuses.at(secondMod), ModVerificationStatus::DISABLED);
	}
	if(secondSubmod)
	{
		ASSERT_NE(secondMod, nullptr);
		const auto child = syncedMods->mods.find(secondSubmod);
		ASSERT_NE(child, syncedMods->mods.end()) << "Real host preview omitted active gameplay submod";
		EXPECT_EQ(child->second.parent, secondMod);
		EXPECT_TRUE(child->second.impactsGameplay);

		const auto statuses = ModVerificationInfo::verifyListAgainstLocalMods(syncedMods->mods);
		ASSERT_NE(statuses.find(secondSubmod), statuses.end());
		EXPECT_EQ(statuses.at(secondSubmod), ModVerificationStatus::DISABLED);

		// Use the real host's advertised child ID and the guest's actual
		// compatibility result to prepare an installed-only LAN preset.
		const ModListVerificationStatus childOnly = {
			{secondSubmod, statuses.at(secondSubmod)}
		};
		const auto plan = ModVerificationInfo::buildInstalledOnlyPlan(childOnly);
		ASSERT_TRUE(plan.has_value());
		EXPECT_TRUE(plan->activate.contains(secondSubmod));
		EXPECT_TRUE(plan->downloadOrUpdate.empty());
		EXPECT_TRUE(plan->reuseCachedArchive.empty());

		JsonNode guestPreset;
		guestPreset["activePreset"].String() = "personal";
		guestPreset["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
		guestPreset["presets"]["personal"]["mods"].Vector().emplace_back(secondMod);
		guestPreset["presets"]["personal"]["settings"][secondMod]["units"].Bool() = false;
		guestPreset["presets"]["other"]["mods"].Vector().emplace_back("vcmi");
		const auto originalPreset = guestPreset.toCompactString();
		LanModSessionState::begin(guestPreset);
		LanModSessionState::applyInstalledOnlyPlan(guestPreset, *plan);
		const JsonNode & temporary = guestPreset["presets"][LanModSessionState::temporaryPresetName()];
		EXPECT_TRUE(temporary["settings"][secondMod]["units"].Bool());
		EXPECT_FALSE(guestPreset["presets"]["personal"]["settings"][secondMod]["units"].Bool());
		EXPECT_TRUE(LanModSessionState::restore(guestPreset));
		EXPECT_EQ(guestPreset.toCompactString(), originalPreset);
	}
	if(requiredMod)
	{
		const auto required = syncedMods->mods.find(requiredMod);
		ASSERT_NE(required, syncedMods->mods.end()) << "Running host did not advertise the required LAN mod";
		EXPECT_TRUE(required->second.impactsGameplay);
		const auto statuses = ModVerificationInfo::verifyListAgainstLocalMods(syncedMods->mods);
		const auto requiredStatus = statuses.find(requiredMod);
		ASSERT_NE(requiredStatus, statuses.end());
		EXPECT_EQ(requiredStatus->second, ModVerificationStatus::DISABLED);
	}

	// A compatible guest must be able to join the same live server after
	// inspecting the opt-in host mod list. Preview-only sockets stay unregistered.
	struct JoinGuest final : INetworkClientListener
	{
		NetworkConnectionPtr connection;
		std::unique_ptr<GameConnection> peer;
		std::string error;
		bool joined = false;
		bool receivedStartPreparation = false;
		bool receivedInitializedStart = false;
		std::set<std::string> initializedHumanPlayers;
		std::size_t initializedPlayableHeroes = 0;
		bool archiveOnly = false;
		std::size_t otherJoinNotifications = 0;
		std::string uuid = "00000000-0000-4000-8000-000000000002";
		std::string playerName = "LAN integration guest";
		std::unique_ptr<LobbyLanModArchiveChunk> archiveChunk;
		GameConnectionID assignedClientId = GameConnectionID::INVALID;
		GameConnectionID assignedHostId = GameConnectionID::INVALID;

		void onConnectionFailed(const std::string & reason) override { error = reason; }
		void onConnectionEstablished(const NetworkConnectionPtr & ready) override
		{
			connection = ready;
			peer = std::make_unique<GameConnection>(ready);
			peer->enterLobbyConnectionMode();
			LobbyClientConnected join;
			join.uuid = archiveOnly ? "vcmi-lan-archive:" + uuid : uuid;
			if(!archiveOnly)
				join.names = {playerName};
			join.mode = EStartMode::NEW_GAME;
			peer->sendPack(join);
		}
		void onDisconnected(const NetworkConnectionPtr &, const std::string & reason) override
		{
			if(!joined)
				error = reason;
		}
		void onPacketReceived(const NetworkConnectionPtr &, const std::vector<std::byte> & bytes) override
		{
			auto packet = peer->retrievePack(bytes);
			if(dynamic_cast<LobbyPrepareStartGame *>(packet.get()))
			{
				receivedStartPreparation = true;
				return;
			}
			if(const auto * started = dynamic_cast<LobbyStartGame *>(packet.get()))
			{
				receivedInitializedStart = started->initializedStartInfo && started->initializedGameState;
				if(receivedInitializedStart)
				{
					initializedHumanPlayers.clear();
					for(const auto & [color, player] : started->initializedStartInfo->playerInfos)
						if(player.isControlledByHuman())
							initializedHumanPlayers.insert(player.name);
					initializedPlayableHeroes = countLanPlayableStartingHeroes(*started->initializedGameState);
				}
				return;
			}
			if(const auto * chunk = dynamic_cast<LobbyLanModArchiveChunk *>(packet.get()))
			{
				archiveChunk = std::make_unique<LobbyLanModArchiveChunk>(*chunk);
				return;
			}
			if(const auto * accepted = dynamic_cast<LobbyClientConnected *>(packet.get()))
			{
				// A downloader must not announce a phantom lobby guest to an
				// existing player or to another archive-only connection.
				if(joined)
				{
					if(accepted->clientId != assignedClientId)
						++otherJoinNotifications;
					return;
				}
				if(accepted->clientId == GameConnectionID::INVALID)
					return;
				assignedClientId = accepted->clientId;
				assignedHostId = accepted->hostClientId;
				peer->connectionID = accepted->clientId;
				peer->setSerializationVersion(accepted->version);
				joined = true;
			}
		}
	} guest;

	network->getContext().restart();
	network->connectToRemote(guest, "127.0.0.1", static_cast<std::uint16_t>(port));
	network->getContext().run_for(std::chrono::seconds(4));
	ASSERT_TRUE(guest.joined) << "Real VCMI server rejected guest after compatible LAN preview: " << guest.error;
	EXPECT_NE(guest.assignedClientId, host.peer->connectionID);
	EXPECT_EQ(guest.assignedHostId, originalHostId);
	EXPECT_EQ(host.peer->connectionID, originalHostId) << "Guest-join notification changed the host's identity";

	// Ask the independently running production server for its shared ZIP.
	// This crosses the real host lobby permission gate and packet handler,
	// unlike the loopback transfer fixture above.
	if(requiredMod)
	{
		namespace fs = std::filesystem;
		const auto offered = syncedMods->archives.find(requiredMod);
		ASSERT_NE(offered, syncedMods->archives.end()) << "Host did not advertise its explicitly shared ZIP";
		const auto & archive = offered->second;
		ASSERT_GT(archive.bytes, 64U * 1024U);
		const fs::path isolated = fs::temp_directory_path() / ("vcmi-real-host-zip-" + LanRelaunchToken::generate());
		ASSERT_TRUE(fs::create_directories(isolated));
		const auto cleanup = vstd::makeScopeGuard([&]()
		{
			std::error_code error;
			fs::remove_all(isolated, error);
		});
		// Two independently registered guests fetch the same host ZIP in interleaved
		// chunks. This uses the production server's per-connection transfer map,
		// rather than the in-process TCP fixture used by the other tests.
		JoinGuest firstTransfer;
		firstTransfer.archiveOnly = true;
		firstTransfer.uuid = "00000000-0000-4000-8000-000000000004";
		JoinGuest secondGuest;
		secondGuest.archiveOnly = true;
		secondGuest.uuid = "00000000-0000-4000-8000-000000000003";
		network->getContext().restart();
		network->connectToRemote(firstTransfer, "127.0.0.1", static_cast<std::uint16_t>(port));
		network->getContext().run_for(std::chrono::seconds(4));
		ASSERT_TRUE(firstTransfer.joined) << "Real VCMI server refused first archive-only connection: " << firstTransfer.error;
		network->getContext().restart();
		network->connectToRemote(secondGuest, "127.0.0.1", static_cast<std::uint16_t>(port));
		network->getContext().run_for(std::chrono::seconds(4));
		ASSERT_TRUE(secondGuest.joined) << "Real VCMI server refused second archive-only connection: " << secondGuest.error;
		EXPECT_NE(firstTransfer.assignedClientId, secondGuest.assignedClientId);
		EXPECT_NE(firstTransfer.assignedClientId, guest.assignedClientId);
		EXPECT_EQ(firstTransfer.assignedHostId, originalHostId);
		EXPECT_EQ(secondGuest.assignedHostId, originalHostId);
		EXPECT_EQ(guest.peer->connectionID, guest.assignedClientId)
			<< "Transfer connection's lobby broadcast changed the real guest's identity";
		EXPECT_EQ(guest.otherJoinNotifications, 0U)
			<< "Archive-only registration must not be announced as another player";
		EXPECT_EQ(firstTransfer.otherJoinNotifications, 0U)
			<< "A downloader must not receive unrelated lobby registrations";

		// A downloader can lose its connection after receiving the first chunk.
		// A new archive-only registration must be able to request the same data
		// without disrupting either ongoing transfer or the real player.
		JoinGuest interrupted;
		interrupted.archiveOnly = true;
		interrupted.uuid = "00000000-0000-4000-8000-000000000005";
		network->getContext().restart();
		network->connectToRemote(interrupted, "127.0.0.1", static_cast<std::uint16_t>(port));
		network->getContext().run_for(std::chrono::seconds(4));
		ASSERT_TRUE(interrupted.joined) << "Interrupted downloader could not register: " << interrupted.error;
		LobbyQueryLanModArchiveChunk firstRequest;
		firstRequest.rootId = requiredMod;
		firstRequest.sha256 = archive.sha256;
		firstRequest.offset = 0;
		interrupted.peer->sendPack(firstRequest);
		network->getContext().restart();
		network->getContext().run_for(std::chrono::seconds(2));
		ASSERT_NE(interrupted.archiveChunk, nullptr);
		ASSERT_TRUE(interrupted.archiveChunk->granted);
		ASSERT_FALSE(interrupted.archiveChunk->data.empty());
		const std::string expectedFirstChunk = interrupted.archiveChunk->data;
		interrupted.connection->close(); // No LobbyClientDisconnected packet.
		network->getContext().restart();
		network->getContext().run_for(std::chrono::seconds(2));

		JoinGuest retry;
		retry.archiveOnly = true;
		retry.uuid = interrupted.uuid;
		network->getContext().restart();
		network->connectToRemote(retry, "127.0.0.1", static_cast<std::uint16_t>(port));
		network->getContext().run_for(std::chrono::seconds(4));
		ASSERT_TRUE(retry.joined) << "Interrupted downloader could not reconnect: " << retry.error;
		retry.peer->sendPack(firstRequest);
		network->getContext().restart();
		network->getContext().run_for(std::chrono::seconds(2));
		ASSERT_NE(retry.archiveChunk, nullptr);
		ASSERT_TRUE(retry.archiveChunk->granted);
		EXPECT_EQ(retry.archiveChunk->offset, 0);
		EXPECT_EQ(retry.archiveChunk->sha256, archive.sha256);
		EXPECT_EQ(retry.archiveChunk->data, expectedFirstChunk);
		retry.connection->close();
		network->getContext().restart();
		network->getContext().run_for(std::chrono::seconds(2));

		// Registered archive-only connections must not consume game player slots.
		PreviewGuest duringTransfer(true);
		network->getContext().restart();
		network->connectToRemote(duringTransfer, "127.0.0.1", static_cast<std::uint16_t>(port));
		network->getContext().run_for(std::chrono::seconds(4));
		ASSERT_TRUE(duringTransfer.error.empty()) << duringTransfer.error;
		const auto * unchangedLobby = dynamic_cast<LobbyModSyncPreview *>(duringTransfer.response.get());
		ASSERT_NE(unchangedLobby, nullptr);
		EXPECT_EQ(unchangedLobby->participantNames.size(), 2);
		EXPECT_EQ(std::count(unchangedLobby->participantNames.begin(), unchangedLobby->participantNames.end(),
			"LAN integration host"), 1);
		EXPECT_EQ(std::count(unchangedLobby->participantNames.begin(), unchangedLobby->participantNames.end(),
			"LAN integration guest"), 1);
		duringTransfer.connection->close();

		const std::vector<JoinGuest *> transferGuests{&firstTransfer, &secondGuest};
		const std::vector<fs::path> receivedPaths{isolated / "first.zip", isolated / "second.zip"};
		const std::vector<fs::path> guestDataPaths{isolated / "guest-first", isolated / "guest-second"};
		std::vector<std::ofstream> outputs(transferGuests.size());
		std::vector<std::uint64_t> receivedBytes(transferGuests.size(), 0);
		for(std::size_t i = 0; i < transferGuests.size(); ++i)
		{
			outputs[i].open(receivedPaths[i], std::ios::binary);
			ASSERT_TRUE(outputs[i].good());
		}
		while(std::any_of(receivedBytes.begin(), receivedBytes.end(),
			[&](std::uint64_t count){ return count < archive.bytes; }))
		{
			// Issue each guest's next request before processing the network, so
			// both transfers really remain active against the same VCMI server.
			for(std::size_t i = 0; i < transferGuests.size(); ++i)
			{
				if(receivedBytes[i] == archive.bytes)
					continue;
				LobbyQueryLanModArchiveChunk request;
				request.rootId = requiredMod;
				request.sha256 = archive.sha256;
				request.offset = static_cast<std::int64_t>(receivedBytes[i]);
				transferGuests[i]->peer->sendPack(request);
			}
			network->getContext().restart();
			network->getContext().run_for(std::chrono::seconds(2));
			for(std::size_t i = 0; i < transferGuests.size(); ++i)
			{
				if(receivedBytes[i] == archive.bytes)
					continue;
				ASSERT_NE(transferGuests[i]->archiveChunk, nullptr)
					<< "Real VCMI host did not return guest " << i << "'s ZIP chunk";
				const auto & chunk = *transferGuests[i]->archiveChunk;
				ASSERT_TRUE(chunk.granted) << "Real VCMI host refused guest " << i;
				ASSERT_EQ(chunk.rootId, requiredMod);
				ASSERT_EQ(chunk.sha256, archive.sha256);
				ASSERT_EQ(chunk.offset, receivedBytes[i]);
				ASSERT_EQ(chunk.totalBytes, archive.bytes);
				ASSERT_FALSE(chunk.data.empty());
				ASSERT_LE(chunk.data.size(), 64U * 1024U);
				ASSERT_LE(chunk.data.size(), archive.bytes - receivedBytes[i]);
				outputs[i].write(chunk.data.data(), static_cast<std::streamsize>(chunk.data.size()));
				ASSERT_TRUE(outputs[i].good());
				receivedBytes[i] += chunk.data.size();
				transferGuests[i]->archiveChunk.reset();
			}
		}
		const ModListVerificationStatus missing = {{requiredMod, ModVerificationStatus::NOT_INSTALLED}};
		const ModArchiveHashes expected = {{requiredMod, archive.sha256}};
		for(std::size_t i = 0; i < transferGuests.size(); ++i)
		{
			outputs[i].close();
			ASSERT_EQ(receivedBytes[i], archive.bytes);
			ASSERT_TRUE(LanModArchiveCache::publishVerifiedArchive(
				guestDataPaths[i], requiredMod, archive.sha256, receivedPaths[i]));
			ASSERT_TRUE(LanModPackageStage::stageVerifiedArchive(
				guestDataPaths[i], requiredMod, archive.sha256));
			const auto nextJoin = ModVerificationInfo::buildSyncPlanFromPersistentCache(
				missing, expected, guestDataPaths[i]);
			EXPECT_TRUE(nextJoin.reuseCachedArchive.contains(requiredMod))
				<< "Guest " << i << " must reuse its own verified ZIP when rejoining";
			EXPECT_TRUE(nextJoin.downloadOrUpdate.empty());
		}

		// Reuse both archive-only TCP connections to fetch a different active
		// root from the SAME production host. This checks the real server's
		// transfer reset between packages, independent digest verification,
		// and a repeated join requiring zero bytes for BOTH cached roots.
		if(secondMod)
		{
			const auto secondOffer = syncedMods->archives.find(secondMod);
			ASSERT_NE(secondOffer, syncedMods->archives.end()) << "Second active mod was not shared";
			const auto & nextArchive = secondOffer->second;
			ASSERT_GT(nextArchive.bytes, 64U * 1024U);
			std::vector<std::uint64_t> secondBytes(transferGuests.size(), 0);
			std::vector<fs::path> secondPaths{isolated / "second-root-first.zip",
				isolated / "second-root-second.zip"};
			std::vector<std::ofstream> secondOutputs(transferGuests.size());
			for(std::size_t i = 0; i < transferGuests.size(); ++i)
			{
				secondOutputs[i].open(secondPaths[i], std::ios::binary);
				ASSERT_TRUE(secondOutputs[i].good());
			}
			while(std::any_of(secondBytes.begin(), secondBytes.end(),
				[&](std::uint64_t count){ return count < nextArchive.bytes; }))
			{
				for(std::size_t i = 0; i < transferGuests.size(); ++i)
				{
					if(secondBytes[i] == nextArchive.bytes)
						continue;
					LobbyQueryLanModArchiveChunk request;
					request.rootId = secondMod;
					request.sha256 = nextArchive.sha256;
					request.offset = static_cast<std::int64_t>(secondBytes[i]);
					transferGuests[i]->peer->sendPack(request);
				}
				network->getContext().restart();
				network->getContext().run_for(std::chrono::seconds(2));
				for(std::size_t i = 0; i < transferGuests.size(); ++i)
				{
					if(secondBytes[i] == nextArchive.bytes)
						continue;
					ASSERT_NE(transferGuests[i]->archiveChunk, nullptr)
						<< "Real VCMI host did not return guest " << i << "'s second mod";
					const auto & chunk = *transferGuests[i]->archiveChunk;
					ASSERT_TRUE(chunk.granted);
					EXPECT_EQ(chunk.rootId, secondMod);
					EXPECT_EQ(chunk.sha256, nextArchive.sha256);
					ASSERT_EQ(chunk.offset, secondBytes[i]);
					ASSERT_EQ(chunk.totalBytes, nextArchive.bytes);
					ASSERT_FALSE(chunk.data.empty());
					ASSERT_LE(chunk.data.size(), 64U * 1024U);
					ASSERT_LE(chunk.data.size(), nextArchive.bytes - secondBytes[i]);
					secondOutputs[i].write(chunk.data.data(), static_cast<std::streamsize>(chunk.data.size()));
					ASSERT_TRUE(secondOutputs[i].good());
					secondBytes[i] += chunk.data.size();
					transferGuests[i]->archiveChunk.reset();
				}
			}
			const ModListVerificationStatus bothMissing = {
				{requiredMod, ModVerificationStatus::NOT_INSTALLED},
				{secondMod, ModVerificationStatus::NOT_INSTALLED}
			};
			const ModArchiveHashes bothExpected = {
				{requiredMod, archive.sha256}, {secondMod, nextArchive.sha256}
			};
			for(std::size_t i = 0; i < transferGuests.size(); ++i)
			{
				secondOutputs[i].close();
				ASSERT_EQ(secondBytes[i], nextArchive.bytes);
				ASSERT_TRUE(LanModArchiveCache::publishVerifiedArchive(
					guestDataPaths[i], secondMod, nextArchive.sha256, secondPaths[i]));
				ASSERT_TRUE(LanModPackageStage::stageVerifiedArchive(
					guestDataPaths[i], secondMod, nextArchive.sha256));
				const auto repeated = ModVerificationInfo::buildSyncPlanFromPersistentCache(
					bothMissing, bothExpected, guestDataPaths[i]);
				EXPECT_TRUE(repeated.reuseCachedArchive.contains(requiredMod));
				EXPECT_TRUE(repeated.reuseCachedArchive.contains(secondMod));
				EXPECT_TRUE(repeated.downloadOrUpdate.empty())
					<< "Rejoining must not download either previously verified root";
			}

			// CI uses disposable, explicitly generated mod shares. Refresh one ZIP
			// while the production host stays online and verify that a new preview
			// requires only that root; the other root remains a zero-byte cache hit.
			if(const char * allowRefresh = std::getenv("VCMI_LAN_TEST_SHARE_REFRESH"); allowRefresh && std::string(allowRefresh) == "1"
				&& std::string(requiredMod) == "lan-ci-fixture"
				&& std::string(secondMod) == "lan-ci-extra")
			{
				const fs::path share = fs::path(VCMIDirs::get().userDataPath().native())
					/ "lan-mod-shares" / (std::string(requiredMod) + ".zip");
				const fs::path originalCopy = isolated / "host-original.zip";
				const fs::path replacement = share.parent_path()
					/ (std::string(requiredMod) + "-lan-test-" + LanRelaunchToken::generate() + ".zip");
				ASSERT_TRUE(fs::copy_file(share, originalCopy));
				const auto restoreShare = vstd::makeScopeGuard([&]()
				{
					std::error_code error;
					fs::copy_file(originalCopy, replacement, fs::copy_options::overwrite_existing, error);
					if(!error)
						fs::rename(replacement, share, error);
					EXPECT_FALSE(error) << "Could not restore CI host's original mod ZIP: " << error.message();
					fs::remove(replacement, error);
				});
				ASSERT_TRUE(fs::copy_file(share, replacement));
				zipFile zip = zipOpen(replacement.string().c_str(), APPEND_STATUS_ADDINZIP);
				ASSERT_NE(zip, nullptr);
				const std::string markerName = std::string(requiredMod) + "/assets/lan-refresh-fixture.txt";
				const std::string markerBytes = "VCMI LAN host archive changed without changing mod version";
				ASSERT_EQ(zipOpenNewFileInZip(zip, markerName.c_str(), nullptr,
					nullptr, 0, nullptr, 0, nullptr, 0, 0), ZIP_OK);
				EXPECT_EQ(zipWriteInFileInZip(zip, markerBytes.data(),
					static_cast<unsigned int>(markerBytes.size())), ZIP_OK);
				EXPECT_EQ(zipCloseFileInZip(zip), ZIP_OK);
				ASSERT_EQ(zipClose(zip, nullptr), ZIP_OK);
				fs::rename(replacement, share); // Linux atomic publication of the updated ZIP.

				PreviewGuest updatedPreview(true);
				network->getContext().restart();
				network->connectToRemote(updatedPreview, "127.0.0.1", static_cast<std::uint16_t>(port));
				network->getContext().run_for(std::chrono::seconds(4));
				ASSERT_TRUE(updatedPreview.error.empty()) << updatedPreview.error;
				const auto * updatedLobby = dynamic_cast<LobbyModSyncPreview *>(updatedPreview.response.get());
				ASSERT_NE(updatedLobby, nullptr);
				ASSERT_TRUE(updatedLobby->archives.contains(requiredMod));
				ASSERT_TRUE(updatedLobby->archives.contains(secondMod));
				const auto & changed = updatedLobby->archives.at(requiredMod);
				EXPECT_EQ(updatedLobby->mods.at(requiredMod).version, syncedMods->mods.at(requiredMod).version);
				EXPECT_NE(changed.sha256, archive.sha256);
				EXPECT_EQ(updatedLobby->archives.at(secondMod).sha256, nextArchive.sha256);
				const auto changedAvailability = ModVerificationInfo::inspectHostArchiveAvailability(
					bothMissing, updatedLobby->archives, guestDataPaths.front());
				ASSERT_EQ(changedAvailability.at(requiredMod), LanArchivePreviewState::OFFERED_BY_HOST);
				ASSERT_EQ(changedAvailability.at(secondMod), LanArchivePreviewState::VERIFIED_IN_CACHE);
				EXPECT_EQ(ModVerificationInfo::estimateUncachedArchiveBytes(
					changedAvailability, updatedLobby->archives), changed.bytes);
				const ModArchiveHashes changedHashes = {
					{requiredMod, changed.sha256}, {secondMod, nextArchive.sha256}
				};
				const auto changedPlan = ModVerificationInfo::buildSyncPlanFromPersistentCache(
					bothMissing, changedHashes, guestDataPaths.front());
				EXPECT_TRUE(changedPlan.downloadOrUpdate.contains(requiredMod));
				EXPECT_FALSE(changedPlan.downloadOrUpdate.contains(secondMod));
				EXPECT_TRUE(changedPlan.reuseCachedArchive.contains(secondMod));

				const fs::path changedPath = isolated / "updated-root.zip";
				std::ofstream changedOutput(changedPath, std::ios::binary);
				ASSERT_TRUE(changedOutput.good());
				std::uint64_t changedReceived = 0;
				while(changedReceived < changed.bytes)
				{
					LobbyQueryLanModArchiveChunk request;
					request.rootId = requiredMod;
					request.sha256 = changed.sha256;
					request.offset = static_cast<std::int64_t>(changedReceived);
					firstTransfer.peer->sendPack(request);
					network->getContext().restart();
					network->getContext().run_for(std::chrono::seconds(2));
					ASSERT_NE(firstTransfer.archiveChunk, nullptr);
					const auto & chunk = *firstTransfer.archiveChunk;
					ASSERT_TRUE(chunk.granted);
					ASSERT_EQ(chunk.sha256, changed.sha256);
					ASSERT_EQ(chunk.rootId, requiredMod);
					ASSERT_EQ(chunk.offset, changedReceived);
					ASSERT_EQ(chunk.totalBytes, changed.bytes);
					ASSERT_FALSE(chunk.data.empty());
					ASSERT_LE(chunk.data.size(), changed.bytes - changedReceived);
					changedOutput.write(chunk.data.data(), static_cast<std::streamsize>(chunk.data.size()));
					ASSERT_TRUE(changedOutput.good());
					changedReceived += chunk.data.size();
					firstTransfer.archiveChunk.reset();
				}
				changedOutput.close();
				ASSERT_EQ(changedReceived, changed.bytes);
				ASSERT_TRUE(LanModArchiveCache::publishVerifiedArchive(
					guestDataPaths.front(), requiredMod, changed.sha256, changedPath));
				const auto cachedAgain = ModVerificationInfo::buildSyncPlanFromPersistentCache(
					bothMissing, changedHashes, guestDataPaths.front());
				EXPECT_TRUE(cachedAgain.downloadOrUpdate.empty());
				EXPECT_TRUE(cachedAgain.reuseCachedArchive.contains(requiredMod));
				EXPECT_TRUE(cachedAgain.reuseCachedArchive.contains(secondMod));
				// Both content-addressed versions remain available for later sessions.
				const auto originalPlan = ModVerificationInfo::buildSyncPlanFromPersistentCache(
					bothMissing, bothExpected, guestDataPaths.front());
				EXPECT_TRUE(originalPlan.downloadOrUpdate.empty());
				updatedPreview.connection->close();
			}
		}
		// Remove both archive-only connections while keeping the real player.
		firstTransfer.connection->close();
		secondGuest.connection->close();
		network->getContext().restart();
		network->getContext().run_for(std::chrono::seconds(2));
	}

	PreviewGuest afterJoin(true);
	network->getContext().restart();
	network->connectToRemote(afterJoin, "127.0.0.1", static_cast<std::uint16_t>(port));
	network->getContext().run_for(std::chrono::seconds(4));
	ASSERT_TRUE(afterJoin.error.empty()) << afterJoin.error;
	const auto * occupiedLobby = dynamic_cast<LobbyModSyncPreview *>(afterJoin.response.get());
	ASSERT_NE(occupiedLobby, nullptr);
	EXPECT_TRUE(occupiedLobby->acceptingPlayers);
	EXPECT_EQ(occupiedLobby->mods.size(), syncedMods->mods.size());
	ASSERT_EQ(occupiedLobby->participantNames.size(), 2);
	EXPECT_NE(std::find(occupiedLobby->participantNames.begin(), occupiedLobby->participantNames.end(),
		"LAN integration host"), occupiedLobby->participantNames.end());
	EXPECT_NE(std::find(occupiedLobby->participantNames.begin(), occupiedLobby->participantNames.end(),
		"LAN integration guest"), occupiedLobby->participantNames.end());

	// Exercise the real-host reconnection path alongside VCMI's durable installed-only
	// mod preset. The independent server remains alive while the guest disconnects,
	// stages a previously disabled local mod, and loads its saved rejoin endpoint.
	guest.connection->close();
	ModsPresetState beforeRelaunch(true);
	const std::string guestOnlyMod = "roe-demo";
	const auto initialGuestMods = beforeRelaunch.getActiveMods();
	const bool guestOnlyInitiallyActive =
		std::find(initialGuestMods.begin(), initialGuestMods.end(), guestOnlyMod) != initialGuestMods.end();
	if(requiredMod)
	{
		// A real guest may have a private gameplay mod that the host does not use.
		// The LAN preset must disable it, then restore it after the guest leaves.
		ASSERT_EQ(syncedMods->mods.count(guestOnlyMod), 0);
		const auto & installed = LIBRARY->modh->getAllMods();
		ASSERT_NE(std::find(installed.begin(), installed.end(), guestOnlyMod), installed.end());
		beforeRelaunch.setModActive(guestOnlyMod, true);
		beforeRelaunch.saveConfigurationState();
	}
	const auto personalName = beforeRelaunch.getActivePreset();
	const auto personalPreset = beforeRelaunch.exportCurrentPreset().toCompactString();
	const auto personalPresets = beforeRelaunch.getAllPresets();
	const auto personalMods = beforeRelaunch.getActiveMods();
	const std::string stagedModId = requiredMod ? requiredMod : "roe-demo";
	ASSERT_EQ(std::find(personalMods.begin(), personalMods.end(), stagedModId), personalMods.end())
		<< "Integration fixture needs the host-required mod initially disabled";
	const auto restoreGuestSettings = vstd::makeScopeGuard([requiredMod, guestOnlyMod, guestOnlyInitiallyActive]()
	{
		try
		{
			ModsPresetState recovery(true);
			recovery.restoreLanSession();
			if(requiredMod)
			{
				// Undo only the test's personal-mod modification after checking that
				// the LAN recovery restored it exactly.
				recovery.setModActive(guestOnlyMod, guestOnlyInitiallyActive);
				recovery.saveConfigurationState();
			}
		}
		catch(const std::exception & e)
		{
			ADD_FAILURE() << "Could not restore actual guest mod settings after LAN test: " << e.what();
		}
	});

	ModSyncPlan localOnly;
	if(requiredMod)
	{
		// Build the complete plan for the live host, rather than testing just
		// one required mod. The guest-only mod is active in the personal preset.
		auto statuses = ModVerificationInfo::verifyListAgainstLocalMods(syncedMods->mods);
		const auto found = statuses.find(stagedModId);
		ASSERT_NE(found, statuses.end());
		// Test mode always injects vcmi-test into the resolved mod list, even
		// when a temporary LAN preset disables it. It is not a player mod.
		statuses.erase("vcmi-test");
		statuses[guestOnlyMod] = ModVerificationStatus::EXCESSIVE;
		const auto planFromHost = ModVerificationInfo::buildInstalledOnlyPlan(statuses);
		ASSERT_TRUE(planFromHost.has_value());
		localOnly = *planFromHost;
		EXPECT_EQ(localOnly.deactivate.count(guestOnlyMod), 1);
	}
	else
		localOnly.activate.insert(stagedModId);
	ASSERT_TRUE(localOnly.downloadOrUpdate.empty());
	ASSERT_EQ(localOnly.activate.count(stagedModId), 1);
	const LanRejoinTarget savedHost{"127.0.0.1", static_cast<std::uint16_t>(port),
		true, {"LAN integration guest"}, false,
		ModVerificationInfo::fingerprintLanHostOffer(syncedMods->mods, syncedMods->archives)};
	beforeRelaunch.prepareInstalledOnlyLanSession(localOnly, savedHost);
	const auto token = LanRelaunchToken::generate();
	beforeRelaunch.prepareControlledRelaunch(token);
	// Recreate the production mod manager, including dependency resolution, from
	// the persisted LAN preset. Reading the JSON alone cannot prove the game loads it.
	ModManager relaunchedGuest(JsonNode(), true, {}, token);
	const auto resumedTarget = relaunchedGuest.consumeLanRejoinTarget();
	ASSERT_TRUE(resumedTarget.has_value());
	EXPECT_EQ(resumedTarget->hostname, savedHost.hostname);
	EXPECT_EQ(resumedTarget->port, savedHost.port);
	EXPECT_EQ(resumedTarget->playerNames, savedHost.playerNames);
	EXPECT_EQ(resumedTarget->approvedHostModsSha256, savedHost.approvedHostModsSha256);
	const auto & activeAfterRelaunch = relaunchedGuest.getActiveMods();
	EXPECT_NE(std::find(activeAfterRelaunch.begin(), activeAfterRelaunch.end(), stagedModId), activeAfterRelaunch.end());
	if(requiredMod)
		EXPECT_EQ(std::find(activeAfterRelaunch.begin(), activeAfterRelaunch.end(), guestOnlyMod), activeAfterRelaunch.end())
			<< "LAN guest must not load private gameplay mods absent from the host";
	EXPECT_TRUE(localOnly.matchesResolvedHostMods(syncedMods->mods, activeAfterRelaunch))
		<< "Reloaded client did not resolve the gameplay mods required by the live host";

	// The saved endpoint must lead back to the actual running host, not a test
	// socket double. Preview again, then reconnect under the original player name.
	PreviewGuest resumedPreview(true);
	network->getContext().restart();
	network->connectToRemote(resumedPreview, resumedTarget->hostname, resumedTarget->port);
	network->getContext().run_for(std::chrono::seconds(4));
	ASSERT_TRUE(resumedPreview.error.empty()) << resumedPreview.error;
	const auto * freshLobby = dynamic_cast<LobbyModSyncPreview *>(resumedPreview.response.get());
	ASSERT_NE(freshLobby, nullptr);
	EXPECT_TRUE(freshLobby->acceptingPlayers);
	EXPECT_EQ(freshLobby->mods.size(), syncedMods->mods.size());
	EXPECT_EQ(ModVerificationInfo::fingerprintLanHostOffer(freshLobby->mods, freshLobby->archives),
		resumedTarget->approvedHostModsSha256);
	if(requiredMod)
	{
		const auto requiredAfterRestart = freshLobby->mods.find(stagedModId);
		ASSERT_NE(requiredAfterRestart, freshLobby->mods.end());
		EXPECT_EQ(requiredAfterRestart->second.version, syncedMods->mods.at(stagedModId).version);
	}

	JoinGuest resumedConnection;
	network->getContext().restart();
	network->connectToRemote(resumedConnection, resumedTarget->hostname, resumedTarget->port);
	network->getContext().run_for(std::chrono::seconds(4));
	ASSERT_TRUE(resumedConnection.joined) << "Relaunched guest could not rejoin live host: " << resumedConnection.error;
	EXPECT_EQ(resumedConnection.assignedHostId, originalHostId);
	EXPECT_EQ(host.peer->connectionID, originalHostId) << "Guest-rejoin notification changed the host's identity";
	EXPECT_NE(resumedConnection.assignedClientId, guest.assignedClientId);

	// An additional player registration must coexist with the guest that
	// relaunched with the host's installed gameplay mods. Archive-only sockets
	// above do not prove that VCMI admits two actual lobby players.
	JoinGuest concurrentPlayer;
	concurrentPlayer.uuid = "00000000-0000-4000-8000-000000000006";
	concurrentPlayer.playerName = "LAN concurrent guest";
	network->getContext().restart();
	network->connectToRemote(concurrentPlayer, resumedTarget->hostname, resumedTarget->port);
	network->getContext().run_for(std::chrono::seconds(4));
	ASSERT_TRUE(concurrentPlayer.joined) << "Second player could not join the independent VCMI server: " << concurrentPlayer.error;
	EXPECT_EQ(concurrentPlayer.assignedHostId, originalHostId);
	EXPECT_NE(concurrentPlayer.assignedClientId, resumedConnection.assignedClientId);

	PreviewGuest simultaneousPlayers(true);
	network->getContext().restart();
	network->connectToRemote(simultaneousPlayers, resumedTarget->hostname, resumedTarget->port);
	network->getContext().run_for(std::chrono::seconds(4));
	ASSERT_TRUE(simultaneousPlayers.error.empty()) << simultaneousPlayers.error;
	const auto * twoGuestLobby = dynamic_cast<LobbyModSyncPreview *>(simultaneousPlayers.response.get());
	ASSERT_NE(twoGuestLobby, nullptr);
	EXPECT_TRUE(twoGuestLobby->acceptingPlayers);
	EXPECT_EQ(twoGuestLobby->participantNames.size(), 3);
	for(const auto & name : {"LAN integration host", "LAN integration guest", "LAN concurrent guest"})
		EXPECT_EQ(std::count(twoGuestLobby->participantNames.begin(), twoGuestLobby->participantNames.end(), name), 1)
			<< "Live LAN lobby lost or duplicated player " << name;
	simultaneousPlayers.connection->close();

	// Move beyond lobby registration: the production host's pre-start signal
	// must reach both real players before either leaves the LAN room.
	LobbyPrepareStartGame prepareStart;
	host.peer->sendPack(prepareStart);
	network->getContext().restart();
	const auto prepareDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while((!host.receivedStartPreparation || !resumedConnection.receivedStartPreparation
		|| !concurrentPlayer.receivedStartPreparation) && std::chrono::steady_clock::now() < prepareDeadline)
		network->getContext().run_one_for(std::chrono::milliseconds(50));
	EXPECT_TRUE(host.receivedStartPreparation) << "Real host did not receive its own lobby pre-start signal";
	EXPECT_TRUE(resumedConnection.receivedStartPreparation) << "Relaunched LAN guest missed the host's pre-start signal";
	EXPECT_TRUE(concurrentPlayer.receivedStartPreparation) << "Second LAN guest missed the host's pre-start signal";

	// With an owner-supplied map, verify actual game-state delivery to both
	// headless clients after one guest prepared its disabled host mod.
	if(!std::getenv("VCMI_LAN_TEST_GUI_CLIENT"))
	{
		if(const char * mapResource = std::getenv("VCMI_LAN_TEST_START_MAP"); mapResource && *mapResource)
		{
			auto chosenMap = std::make_shared<CMapInfo>();
			ASSERT_NO_THROW(chosenMap->mapInit(mapResource));
			ASSERT_GE(chosenMap->amountOfHumanControllablePlayers, 3)
				<< "Select a map with three human-controllable player slots";
			LobbySetMap selectMap;
			selectMap.mapInfo = chosenMap;
			host.peer->sendPack(selectMap);
			// The real client prepares the selected map before starting gameplay.
			LobbyPrepareStartGame prepareSelectedMap;
			host.peer->sendPack(prepareSelectedMap);
			LobbyStartGame start;
			host.peer->sendPack(start);
			const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			network->getContext().restart();
			while((!host.receivedInitializedStart || !resumedConnection.receivedInitializedStart
				|| !concurrentPlayer.receivedInitializedStart)
				&& std::chrono::steady_clock::now() < startDeadline)
				network->getContext().run_one_for(std::chrono::milliseconds(100));
			EXPECT_TRUE(host.receivedInitializedStart) << "Production server did not initialize the LAN game";
			EXPECT_TRUE(resumedConnection.receivedInitializedStart)
				<< "Prepared LAN guest did not receive the initialized game state";
			EXPECT_TRUE(concurrentPlayer.receivedInitializedStart)
				<< "Second LAN guest did not receive the initialized game state";
			EXPECT_EQ(host.initializedPlayableHeroes, 3U);
			EXPECT_EQ(resumedConnection.initializedPlayableHeroes, 3U);
			EXPECT_EQ(concurrentPlayer.initializedPlayableHeroes, 3U)
				<< "Both LAN guests must receive their playable starting heroes";
			const std::set<std::string> expectedHumans = {
				"LAN integration host", "LAN integration guest", "LAN concurrent guest"};
			// HostClient and JoinGuest are different types, but both expose the
			// same set of human player names received from LobbyStartGame.
			for(const auto * playerNames : {&host.initializedHumanPlayers,
				&resumedConnection.initializedHumanPlayers, &concurrentPlayer.initializedHumanPlayers})
			{
				for(const auto & name : expectedHumans)
					EXPECT_TRUE(playerNames->contains(name))
						<< "Initialized multiplayer game did not assign a human slot to " << name;
			}
			concurrentPlayer.connection->close();
			resumedConnection.connection->close();
			relaunchedGuest.restoreLanSession();
			ModsPresetState restoredGuest(true);
			EXPECT_EQ(restoredGuest.getActivePreset(), personalName);
			EXPECT_EQ(restoredGuest.exportCurrentPreset().toCompactString(), personalPreset);
			EXPECT_EQ(restoredGuest.getAllPresets(), personalPresets);
			EXPECT_EQ(restoredGuest.getActiveMods(), personalMods);
			return;
		}
	}

	// Leaving the second player must not evict the first relaunched guest.
	concurrentPlayer.connection->close();
	network->getContext().restart();
	network->getContext().run_for(std::chrono::seconds(2));
	PreviewGuest remainingPlayers(true);
	network->getContext().restart();
	network->connectToRemote(remainingPlayers, resumedTarget->hostname, resumedTarget->port);
	network->getContext().run_for(std::chrono::seconds(4));
	ASSERT_TRUE(remainingPlayers.error.empty()) << remainingPlayers.error;
	const auto * remainingLobby = dynamic_cast<LobbyModSyncPreview *>(remainingPlayers.response.get());
	ASSERT_NE(remainingLobby, nullptr);
	EXPECT_EQ(remainingLobby->participantNames.size(), 2);
	EXPECT_EQ(std::count(remainingLobby->participantNames.begin(), remainingLobby->participantNames.end(),
		"LAN integration guest"), 1);
	EXPECT_EQ(std::count(remainingLobby->participantNames.begin(), remainingLobby->participantNames.end(),
		"LAN concurrent guest"), 0);
	remainingPlayers.connection->close();

	resumedConnection.connection->close();
	relaunchedGuest.restoreLanSession();
	ModsPresetState restoredGuest(true);
	EXPECT_EQ(restoredGuest.getActivePreset(), personalName);
	EXPECT_EQ(restoredGuest.exportCurrentPreset().toCompactString(), personalPreset);
	EXPECT_EQ(restoredGuest.getAllPresets(), personalPresets);
	EXPECT_EQ(restoredGuest.getActiveMods(), personalMods);

	// Join the same independent LAN host a second time. After the first match
	// the player's personal root mods and submods must not leak into this join,
	// and the previously installed host mod needs activation, not a download.
	ModsPresetState repeatedGuest(true);
	repeatedGuest.prepareInstalledOnlyLanSession(localOnly, savedHost);
	const std::string repeatedToken = LanRelaunchToken::generate();
	repeatedGuest.prepareControlledRelaunch(repeatedToken);
	ModManager repeatedRelaunch(JsonNode(), true, {}, repeatedToken);
	const auto repeatedTarget = repeatedRelaunch.consumeLanRejoinTarget();
	ASSERT_TRUE(repeatedTarget.has_value());
	EXPECT_EQ(repeatedTarget->approvedHostModsSha256, savedHost.approvedHostModsSha256);
	EXPECT_TRUE(localOnly.downloadOrUpdate.empty());
	EXPECT_TRUE(localOnly.matchesResolvedHostMods(syncedMods->mods, repeatedRelaunch.getActiveMods()))
		<< "Second LAN join loaded a different mod set";

	PreviewGuest repeatedPreview(true);
	network->getContext().restart();
	network->connectToRemote(repeatedPreview, repeatedTarget->hostname, repeatedTarget->port);
	network->getContext().run_for(std::chrono::seconds(4));
	ASSERT_TRUE(repeatedPreview.error.empty()) << repeatedPreview.error;
	const auto * repeatedHost = dynamic_cast<LobbyModSyncPreview *>(repeatedPreview.response.get());
	ASSERT_NE(repeatedHost, nullptr);
	EXPECT_EQ(ModVerificationInfo::fingerprintLanHostOffer(repeatedHost->mods, repeatedHost->archives),
		repeatedTarget->approvedHostModsSha256);

	JoinGuest repeatedConnection;
	network->getContext().restart();
	network->connectToRemote(repeatedConnection, repeatedTarget->hostname, repeatedTarget->port);
	network->getContext().run_for(std::chrono::seconds(4));
	ASSERT_TRUE(repeatedConnection.joined) << "Second join to independent LAN host failed: " << repeatedConnection.error;
	EXPECT_EQ(repeatedConnection.assignedHostId, originalHostId);
	repeatedConnection.connection->close();
	repeatedRelaunch.restoreLanSession();
	ModsPresetState restoredAgain(true);
	EXPECT_EQ(restoredAgain.getActivePreset(), personalName);
	EXPECT_EQ(restoredAgain.exportCurrentPreset().toCompactString(), personalPreset);
	EXPECT_EQ(restoredAgain.getAllPresets(), personalPresets);
	EXPECT_EQ(restoredAgain.getActiveMods(), personalMods);

#if defined(__linux__) && !defined(VCMI_MOBILE)
	if(std::getenv("VCMI_LAN_TEST_GUI_CLIENT"))
	{
		// Unlike the headless process test, this starts the real SDL client under
		// a virtual display and observes its automatic join on the live server.
		// The host is already registered, with lan-ci-fixture active.
		ASSERT_NE(requiredMod, nullptr);
		namespace fs = std::filesystem;
		const auto executable = fs::current_path() / "vcmiclient";
		ASSERT_TRUE(fs::is_regular_file(executable));
		const auto sandbox = fs::temp_directory_path() / ("vcmi-lan-gui-" + LanRelaunchToken::generate());
		const auto configRoot = sandbox / "config";
		const auto modSettings = configRoot / "vcmi" / "modSettings.json";
		ASSERT_TRUE(fs::create_directories(modSettings.parent_path()));
		const auto removeSandbox = vstd::makeScopeGuard([&]()
		{
			std::error_code error;
			fs::remove_all(sandbox, error);
		});

		JsonNode original;
		original["activePreset"].String() = "personal";
		original["presets"]["personal"]["mods"].Vector().emplace_back("vcmi");
		original["presets"]["personal"]["mods"].Vector().emplace_back("core");
		original["presets"]["personal"]["settings"].Struct();
		original["presets"]["personal"]["settings"]["lan-ci-extra"]["units"].Bool() = false;
		JsonNode staged = original;
		LanModSessionState::begin(staged);
		// The live host requires both active mod roots and its enabled submod.
		// A fixture that only activates requiredMod cannot pass the real lobby gate.
		const ModSyncPlan installed = localOnly;
		ASSERT_TRUE(installed.activate.contains(requiredMod));
		LanModSessionState::applyInstalledOnlyPlan(staged, installed);
		const std::string guiGuestName = "LAN GUI relaunch guest";
		LanModSessionState::setRejoinTarget(staged,
			{"127.0.0.1", static_cast<std::uint16_t>(port), true, {guiGuestName}, false,
				ModVerificationInfo::fingerprintLanHostOffer(syncedMods->mods, syncedMods->archives)});
		const std::string token = LanRelaunchToken::generate();
		LanModSessionState::prepareControlledRelaunch(staged, token);
		const std::string originalBytes = original.toCompactString();
		ASSERT_TRUE(AtomicModSettings::replace(modSettings, originalBytes));
		ASSERT_TRUE(LanModRecoveryJournal::persist(modSettings, originalBytes));
		ASSERT_TRUE(AtomicModSettings::replace(modSettings, staged.toCompactString()));

		// A second desktop process must connect to the same live host at the
		// same time, but with its own durable LAN preset and one-shot token.
		const std::string secondGuiGuestName = "LAN GUI second guest";
		const auto secondConfigRoot = sandbox / "second-config";
		const auto secondModSettings = secondConfigRoot / "vcmi" / "modSettings.json";
		const auto secondLogs = sandbox / "second-logs";
		ASSERT_TRUE(fs::create_directories(secondModSettings.parent_path()));
		ASSERT_TRUE(fs::create_directories(secondLogs));

		// Give each real GUI process an independent user-data and cache root.
		// Only locally owned game resources are linked; installed fixture mods
		// are copied, so the two guests cannot share a downloaded ZIP by accident.
		const auto sharedUserData = fs::absolute(fs::path(VCMIDirs::get().userDataPath().native()));
		const auto firstDataHome = sandbox / "first-data";
		const auto secondDataHome = sandbox / "second-data";
		const auto firstCacheHome = sandbox / "first-cache";
		const auto secondCacheHome = sandbox / "second-cache";
		const char * secondRequiredMod = std::getenv("VCMI_LAN_TEST_SECOND_MOD");
		ASSERT_NE(secondRequiredMod, nullptr);
		const auto stageIsolatedGuestData = [&](const fs::path & dataHome)
		{
			const auto userData = dataHome / "vcmi";
			fs::create_directories(userData / "Mods");
			for(const auto & entry : fs::directory_iterator(sharedUserData))
			{
				if(!entry.is_directory())
					continue;
				std::string name = entry.path().filename().string();
				std::transform(name.begin(), name.end(), name.begin(),
					[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
				if(name == "data" || name == "maps" || name == "mp3" || name == "video" || name == "music")
					fs::create_directory_symlink(entry.path(), userData / entry.path().filename());
			}
			for(const auto & modId : {std::string(requiredMod), std::string(secondRequiredMod)})
			{
				const auto source = sharedUserData / "Mods" / modId;
				if(!fs::is_directory(source))
					throw std::runtime_error("LAN GUI fixture is missing installed mod: " + modId);
				fs::copy(source, userData / "Mods" / modId, fs::copy_options::recursive);
			}
		};
		ASSERT_NO_THROW(stageIsolatedGuestData(firstDataHome));
		ASSERT_NO_THROW(stageIsolatedGuestData(secondDataHome));
		ASSERT_FALSE(fs::exists(firstDataHome / "vcmi" / "lan-mod-shares"));
		ASSERT_FALSE(fs::exists(secondDataHome / "vcmi" / "lan-mod-shares"));
		ASSERT_FALSE(fs::is_symlink(firstDataHome / "vcmi" / "Mods" / requiredMod));
		ASSERT_FALSE(fs::is_symlink(secondDataHome / "vcmi" / "Mods" / requiredMod));
		ASSERT_NE(fs::canonical(firstDataHome), fs::canonical(secondDataHome));

		JsonNode secondOriginal = original;
		secondOriginal["activePreset"].String() = "second-personal";
		secondOriginal["presets"]["second-personal"] = secondOriginal["presets"]["personal"];
		JsonNode secondStaged = secondOriginal;
		LanModSessionState::begin(secondStaged);
		LanModSessionState::applyInstalledOnlyPlan(secondStaged, installed);
		LanModSessionState::setRejoinTarget(secondStaged,
			{"127.0.0.1", static_cast<std::uint16_t>(port), true, {secondGuiGuestName}, false,
				ModVerificationInfo::fingerprintLanHostOffer(syncedMods->mods, syncedMods->archives)});
		const std::string secondToken = LanRelaunchToken::generate();
		LanModSessionState::prepareControlledRelaunch(secondStaged, secondToken);
		ASSERT_NE(secondToken, token);
		const std::string secondOriginalBytes = secondOriginal.toCompactString();
		ASSERT_TRUE(AtomicModSettings::replace(secondModSettings, secondOriginalBytes));
		ASSERT_TRUE(LanModRecoveryJournal::persist(secondModSettings, secondOriginalBytes));
		ASSERT_TRUE(AtomicModSettings::replace(secondModSettings, secondStaged.toCompactString()));

		pid_t guiPid = ::fork();
		if(guiPid == 0)
		{
			::setenv("XDG_CONFIG_HOME", configRoot.c_str(), 1);
			::setenv("XDG_DATA_HOME", firstDataHome.c_str(), 1);
			::setenv("XDG_CACHE_HOME", firstCacheHome.c_str(), 1);
			::setenv("SDL_AUDIODRIVER", "dummy", 1);
			::execl(executable.c_str(), executable.c_str(), "--nointro",
				"--logLocation", sandbox.c_str(), "--lan-relaunch-token", token.c_str(),
				static_cast<char *>(nullptr));
			::_exit(127);
		}
		ASSERT_GT(guiPid, 0);
		const auto stopClient = vstd::makeScopeGuard([&]()
		{
			if(guiPid > 0)
			{
				::kill(guiPid, SIGKILL);
				int status = 0;
				while(::waitpid(guiPid, &status, 0) == -1 && errno == EINTR) {}
				guiPid = -1;
			}
		});
		pid_t secondGuiPid = ::fork();
		if(secondGuiPid == 0)
		{
			::setenv("XDG_CONFIG_HOME", secondConfigRoot.c_str(), 1);
			::setenv("XDG_DATA_HOME", secondDataHome.c_str(), 1);
			::setenv("XDG_CACHE_HOME", secondCacheHome.c_str(), 1);
			::setenv("SDL_AUDIODRIVER", "dummy", 1);
			::execl(executable.c_str(), executable.c_str(), "--nointro",
				"--logLocation", secondLogs.c_str(), "--lan-relaunch-token", secondToken.c_str(),
				static_cast<char *>(nullptr));
			::_exit(127);
		}
		ASSERT_GT(secondGuiPid, 0);
		const auto stopSecondClient = vstd::makeScopeGuard([&]()
		{
			if(secondGuiPid > 0)
			{
				::kill(secondGuiPid, SIGKILL);
				int status = 0;
				while(::waitpid(secondGuiPid, &status, 0) == -1 && errno == EINTR) {}
				secondGuiPid = -1;
			}
		});
		bool guiJoined = false;
		bool secondGuiJoined = false;
		for(int attempt = 0; attempt < 120 && (!guiJoined || !secondGuiJoined); ++attempt)
		{
			network->getContext().restart();
			network->getContext().run_for(std::chrono::milliseconds(250));
			guiJoined = std::find(host.observedGuestNames.begin(), host.observedGuestNames.end(),
				guiGuestName) != host.observedGuestNames.end();
			secondGuiJoined = std::find(host.observedGuestNames.begin(), host.observedGuestNames.end(),
				secondGuiGuestName) != host.observedGuestNames.end();
			int status = 0;
			if(!guiJoined && ::waitpid(guiPid, &status, WNOHANG) == guiPid)
			{
				guiPid = -1;
				break;
			}
			if(!secondGuiJoined && ::waitpid(secondGuiPid, &status, WNOHANG) == secondGuiPid)
			{
				secondGuiPid = -1;
				break;
			}
		}
		if(!guiJoined)
		{
			const auto logFile = sandbox / "VCMI_Client_log.txt";
			std::ifstream log(logFile);
			if(log)
			{
				const std::string contents((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
				ADD_FAILURE() << "Real GUI LAN rejoin failed. Client log tail: "
					<< contents.substr(contents.size() > 2500 ? contents.size() - 2500 : 0);
			}
		}
		EXPECT_TRUE(guiJoined) << "First graphical vcmiclient did not join the live LAN host";
		EXPECT_TRUE(secondGuiJoined) << "Second independent graphical vcmiclient did not join the same host";
		if(!secondGuiJoined)
		{
			const auto logFile = secondLogs / "VCMI_Client_log.txt";
			std::ifstream log(logFile);
			if(log)
			{
				const std::string contents((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
				ADD_FAILURE() << "Second GUI LAN rejoin failed. Client log tail: "
					<< contents.substr(contents.size() > 2500 ? contents.size() - 2500 : 0);
			}
		}
		if(guiJoined && secondGuiJoined)
		{
			// A fresh real server preview proves that both graphical processes are
			// present in the lobby simultaneously, not merely that they connected once.
			PreviewGuest bothGraphicalGuests(true);
			network->getContext().restart();
			network->connectToRemote(bothGraphicalGuests, "127.0.0.1", static_cast<std::uint16_t>(port));
			network->getContext().run_for(std::chrono::seconds(4));
			ASSERT_TRUE(bothGraphicalGuests.error.empty()) << bothGraphicalGuests.error;
			const auto * liveRoom = dynamic_cast<LobbyModSyncPreview *>(bothGraphicalGuests.response.get());
			ASSERT_NE(liveRoom, nullptr);
			for(const auto & name : {guiGuestName, secondGuiGuestName})
				EXPECT_NE(std::find(liveRoom->participantNames.begin(), liveRoom->participantNames.end(), name),
					liveRoom->participantNames.end()) << "GUI guest is no longer in the production lobby: " << name;
			bothGraphicalGuests.connection->close();
		}
		if(guiJoined)
		{
			std::ifstream configFile(modSettings, std::ios::binary);
			const std::string bytes((std::istreambuf_iterator<char>(configFile)), std::istreambuf_iterator<char>());
			JsonNode state(bytes.data(), bytes.size(), "GUI LAN guest mod settings");
			EXPECT_EQ(state["activePreset"].String(), LanModSessionState::temporaryPresetName());
			const auto & tempMods = state["presets"][LanModSessionState::temporaryPresetName()]["mods"].Vector();
			EXPECT_NE(std::find(tempMods.begin(), tempMods.end(), JsonNode(requiredMod)), tempMods.end());
			EXPECT_TRUE(state["lanSession"]["relaunchToken"].isNull());
		}
		if(secondGuiJoined)
		{
			std::ifstream configFile(secondModSettings, std::ios::binary);
			const std::string bytes((std::istreambuf_iterator<char>(configFile)), std::istreambuf_iterator<char>());
			JsonNode state(bytes.data(), bytes.size(), "Second GUI LAN guest mod settings");
			EXPECT_EQ(state["activePreset"].String(), LanModSessionState::temporaryPresetName());
			const auto & tempMods = state["presets"][LanModSessionState::temporaryPresetName()]["mods"].Vector();
			EXPECT_NE(std::find(tempMods.begin(), tempMods.end(), JsonNode(requiredMod)), tempMods.end());
			EXPECT_TRUE(state["lanSession"]["relaunchToken"].isNull());
		}
		// With an owner-supplied map, the same two real GUI clients move from
		// the lobby into a production-initialized game. Public CI has no such map.
		if(guiJoined && secondGuiJoined)
		{
			if(const char * mapResource = std::getenv("VCMI_LAN_TEST_START_MAP"); mapResource && *mapResource)
			{
				auto chosenMap = std::make_shared<CMapInfo>();
				ASSERT_NO_THROW(chosenMap->mapInit(mapResource));
				ASSERT_GE(chosenMap->amountOfHumanControllablePlayers, 3)
					<< "Select an owned map with three human-controllable player slots";
				LobbySetMap selectMap;
				selectMap.mapInfo = chosenMap;
				host.peer->sendPack(selectMap);
				LobbyPrepareStartGame prepare;
				host.peer->sendPack(prepare);
				LobbyStartGame start;
				host.peer->sendPack(start);
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
				network->getContext().restart();
				while(!host.receivedInitializedStart && std::chrono::steady_clock::now() < deadline)
					network->getContext().run_one_for(std::chrono::milliseconds(100));
				ASSERT_TRUE(host.receivedInitializedStart)
					<< "The production server did not initialize and broadcast the game state";
				// Server initialization alone is not enough: both real GUI clients
				// must initialize their own human-controlled gameplay interfaces.
				const auto humanInterfaceColor = [](const fs::path & logDir) -> std::string
				{
					std::ifstream input(logDir / "VCMI_Client_log.txt");
					std::string line;
					while(std::getline(input, line))
					{
						const auto begin = line.find("Player ");
						if(begin == std::string::npos)
							continue;
						const auto end = line.find(" will be lead by human", begin + 7);
						if(end != std::string::npos && end > begin + 7)
							return line.substr(begin + 7, end - begin - 7);
					}
					return {};
				};
				std::string firstHuman;
				std::string secondHuman;
				const auto clientsDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
				while(std::chrono::steady_clock::now() < clientsDeadline)
				{
					firstHuman = humanInterfaceColor(sandbox);
					secondHuman = humanInterfaceColor(secondLogs);
					if(!firstHuman.empty() && !secondHuman.empty())
						break;
					network->getContext().restart();
					network->getContext().run_for(std::chrono::milliseconds(150));
				}
				EXPECT_FALSE(firstHuman.empty())
					<< "First GUI did not initialize a human gameplay interface after LobbyStartGame";
				EXPECT_FALSE(secondHuman.empty())
					<< "Second GUI did not initialize a human gameplay interface after LobbyStartGame";
				if(!firstHuman.empty() && !secondHuman.empty())
					EXPECT_NE(firstHuman, secondHuman)
						<< "Both LAN guests were assigned the same playable color";
				int firstStatus = 0;
				int secondStatus = 0;
				EXPECT_EQ(::waitpid(guiPid, &firstStatus, WNOHANG), 0)
					<< "First GUI client exited after the host started the game";
				EXPECT_EQ(::waitpid(secondGuiPid, &secondStatus, WNOHANG), 0)
					<< "Second GUI client exited after the host started the game";
			}
		}

		if(guiJoined && secondGuiJoined)
		{
			// Simulate both GUI clients crashing in the same lobby. A subsequent
			// ordinary launch must restore each distinct personal preset independently.
			const auto terminate = [](pid_t & pid)
			{
				if(pid <= 0)
					return;
				::kill(pid, SIGKILL);
				int status = 0;
				while(::waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
				pid = -1;
			};
			terminate(guiPid);
			terminate(secondGuiPid);
			const auto launchRecovery = [&](const fs::path & profile) -> pid_t
			{
				const pid_t child = ::fork();
				if(child == 0)
				{
					if(::setenv("XDG_CONFIG_HOME", profile.c_str(), 1) != 0)
						::_exit(126);
					const auto & dataHome = profile == configRoot ? firstDataHome : secondDataHome;
					const auto & cacheHome = profile == configRoot ? firstCacheHome : secondCacheHome;
					::setenv("XDG_DATA_HOME", dataHome.c_str(), 1);
					::setenv("XDG_CACHE_HOME", cacheHome.c_str(), 1);
					::setenv("SDL_VIDEODRIVER", "dummy", 1);
					::setenv("SDL_AUDIODRIVER", "dummy", 1);
					::execl(executable.c_str(), executable.c_str(), "--headless", "--nointro",
						static_cast<char *>(nullptr));
					::_exit(127);
				}
				return child;
			};
			pid_t recoveredFirstPid = launchRecovery(configRoot);
			ASSERT_GT(recoveredFirstPid, 0);
			const auto stopRecoveredFirst = vstd::makeScopeGuard([&](){ terminate(recoveredFirstPid); });
			pid_t recoveredSecondPid = launchRecovery(secondConfigRoot);
			ASSERT_GT(recoveredSecondPid, 0);
			const auto stopRecoveredSecond = vstd::makeScopeGuard([&](){ terminate(recoveredSecondPid); });
			const auto restored = [&](const fs::path & settings, const JsonNode & expected)
			{
				try
				{
					std::ifstream input(settings, std::ios::binary);
					if(!input)
						return false;
					const std::string bytes((std::istreambuf_iterator<char>(input)),
							std::istreambuf_iterator<char>());
					const JsonNode state(bytes.data(), bytes.size(), "Recovered GUI LAN guest settings");
					const auto & name = expected["activePreset"].String();
					return state["lanSession"].isNull()
						&& state["activePreset"].isString() && state["activePreset"].String() == name
						&& state["presets"][name].toCompactString() == expected["presets"][name].toCompactString()
						&& state["presets"][LanModSessionState::temporaryPresetName()].isNull();
				}
				catch(const std::exception &)
				{
					return false; // The recovery process may be replacing the settings file.
				}
			};
			bool restoredFirst = false;
			bool restoredSecond = false;
			for(int attempt = 0; attempt < 120 && (!restoredFirst || !restoredSecond); ++attempt)
			{
				restoredFirst = restored(modSettings, original);
				restoredSecond = restored(secondModSettings, secondOriginal);
				if(!restoredFirst || !restoredSecond)
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			EXPECT_TRUE(restoredFirst) << "First crashed GUI client did not recover its personal preset";
			EXPECT_TRUE(restoredSecond) << "Second crashed GUI client did not recover its distinct personal preset";
		}
		// Two independent GUI clients now exercise live lobby participation and
		// crash recovery. Actual gameplay and clean disconnect still need a match.
	}
#endif

	network->stop();
}
