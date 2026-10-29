/*
 * NetworkDiscovery.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"
#include "NetworkDiscovery.h"
#include "NetworkInterface.h"
#include "NetworkDiscoveryProtocol.h"

ServerDiscovery::ServerDiscovery(NetworkContext & context, IServerDiscoveryObserver & observer)
	: context(context), observer(observer)
{
}

ServerDiscovery::~ServerDiscovery()
{
	abort();
}

void ServerDiscovery::abort()
{
	if(sendTimer)
	{
#if BOOST_VERSION >= 108700
		sendTimer->cancel();
#else
		boost::system::error_code ec;
		sendTimer->cancel(ec);
#endif
	}
	if(socket)
	{
		boost::system::error_code ec;
		socket->close(ec);
		socket.reset();
	}
}

void ServerDiscovery::start()
{
	auto self = shared_from_this();
	socket = std::make_shared<boost::asio::ip::udp::socket>(context);
	socket->open(boost::asio::ip::udp::v4());
	socket->set_option(boost::asio::socket_base::broadcast(true));

	auto recvBuf = std::make_shared<std::array<char, 1024>>();
	auto senderEndpoint = std::make_shared<boost::asio::ip::udp::endpoint>();
	sendTimer = std::make_shared<boost::asio::steady_timer>(context);
	
	// Receive handler for incoming server responses
	auto receiveHandler = std::make_shared<std::function<void(const boost::system::error_code&, std::size_t)>>();
	*receiveHandler = [self, socket = this->socket, recvBuf, senderEndpoint, receiveHandler](const boost::system::error_code& ec, std::size_t len)
	{
		if(ec == boost::asio::error::operation_aborted)
			return;
		
		if(!ec && len > 0)
		{
			std::string resp(recvBuf->data(), recvBuf->data() + len);
			if(const auto reply = LanModDiscovery::parseReply(resp))
			{
				DiscoveredServer server;
				server.address = senderEndpoint->address().to_string();
				server.port = reply->port;
				server.lanModSyncPreview = reply->supportsModPreview;

				const auto serverKey = std::make_pair(server.address, server.port);
				auto [known, inserted] = self->discoveredServers.try_emplace(serverKey, server.lanModSyncPreview);
				const bool retainedPreview = LanModDiscovery::retainPreviewCapability(known->second, server.lanModSyncPreview);
				if(inserted || retainedPreview != known->second)
				{
					known->second = retainedPreview;
					logGlobal->info("Discovered server: %s", resp.c_str());
					self->observer.onServerDiscovered(server);
				}
			}
		}
		socket->async_receive_from(boost::asio::buffer(*recvBuf), *senderEndpoint, *receiveHandler);
	};
	
	// Send handler for broadcasting discovery message
	auto sendHandler = std::make_shared<std::function<void(const boost::system::error_code&)>>();
	*sendHandler = [self, socket = this->socket, sendHandler](const boost::system::error_code& ec)
	{
		if(ec == boost::asio::error::operation_aborted || ec == boost::asio::error::bad_descriptor)
			return;
		
		if(ec)
		{
			logGlobal->error("Discovery send error: %s", ec.message());
			return;
		}
		
		const boost::asio::ip::udp::endpoint broadcastEndpoint(boost::asio::ip::address_v4::broadcast(), 3030);

		// Probe upgraded hosts and preserve the exact legacy probe for older hosts.
		// Keep both UDP buffers alive until their asynchronous sends complete.
		auto previewMessage = std::make_shared<std::string>(LanModDiscovery::previewProbe);
		socket->async_send_to(boost::asio::buffer(*previewMessage), broadcastEndpoint,
			[previewMessage](const boost::system::error_code & sendError, std::size_t)
			{
				if(sendError && sendError != boost::asio::error::operation_aborted && sendError != boost::asio::error::bad_descriptor)
					logGlobal->warn("Optional LAN preview discovery failed: %s", sendError.message());
			});

		auto legacyMessage = std::make_shared<std::string>(LanModDiscovery::legacyProbe);
		socket->async_send_to(boost::asio::buffer(*legacyMessage), broadcastEndpoint,
			[self, sendHandler, legacyMessage](const boost::system::error_code & sendError, std::size_t)
			{
				if(sendError && sendError != boost::asio::error::operation_aborted && sendError != boost::asio::error::bad_descriptor)
					logGlobal->error("Discovery send error: %s", sendError.message());

				if(sendError != boost::asio::error::operation_aborted && sendError != boost::asio::error::bad_descriptor)
				{
					self->sendTimer->expires_after(std::chrono::seconds(1));
					self->sendTimer->async_wait(*sendHandler);
				}
			});
	};
	
	// Start receiving
	socket->async_receive_from(boost::asio::buffer(*recvBuf), *senderEndpoint, *receiveHandler);
	
	// Send first broadcast immediately
	(*sendHandler)(boost::system::error_code());
}

std::vector<std::string> ServerDiscovery::ipAddresses()
{
    std::vector<std::string> addresses;

    try
	{
        boost::asio::io_context io;
        boost::asio::ip::udp::socket socket(io);

        socket.connect({
            boost::asio::ip::make_address("8.8.8.8"), 53
        });

        auto addr = socket.local_endpoint().address();
        if (addr.is_v4())
            addresses.push_back(addr.to_string());
    }
    catch (const std::exception& e)
	{
        logGlobal->error("IP address retrieval error: %s", e.what());
    }

    return addresses;
}

ServerDiscoveryListener::ServerDiscoveryListener(NetworkContext & context, IServerDiscoveryAnnouncer & announcer, uint16_t port)
	: context(context), announcer(announcer), port(port)
{
}

ServerDiscoveryListener::~ServerDiscoveryListener()
{
	stop();
}

void ServerDiscoveryListener::start()
{
	if(socket)
		return;

	boost::asio::ip::udp::endpoint listenEndpoint(boost::asio::ip::udp::v4(), port);

	try
	{
		socket = std::make_shared<boost::asio::ip::udp::socket>(context, listenEndpoint);
	}
	catch(const boost::system::system_error & e)
	{
		if(e.code() == boost::asio::error::address_in_use)
		{
			logGlobal->warn("ServerDiscoveryListener: port %d already in use, skipping", port);
			return;
		}
		throw;
	}
	asyncReceive();
}

void ServerDiscoveryListener::stop()
{
	if(socket)
	{
		boost::system::error_code ec;
		socket->close(ec);
		socket.reset();
	}
}

void ServerDiscoveryListener::asyncReceive()
{
	auto self = shared_from_this();
	auto socketPtr = socket;
	if(!socketPtr)
		return;

	socketPtr->async_receive_from(
		boost::asio::buffer(recvBuffer),
		remoteEndpoint,
		[self, socketPtr, this](const boost::system::error_code & ec, std::size_t len)
		{
			if(ec == boost::asio::error::operation_aborted)
				return;

			if(!ec && len > 0)
			{
				std::string msg(recvBuffer.data(), recvBuffer.data() + len);

				if(self->announcer.isInLobby())
				{
					// Extended replies are sent only to a client explicitly asking
					// for preview capability; old clients always get the old format.
					const std::string response = LanModDiscovery::replyForProbe(msg, self->announcer.getPort());
					if(!response.empty())
						socketPtr->send_to(boost::asio::buffer(response), remoteEndpoint);
				}
			}

			self->asyncReceive();
		}
	);
}
