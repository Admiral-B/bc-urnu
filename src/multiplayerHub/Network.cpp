/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2014 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation

     This program is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY Or FITNESS For A PARTICULAR PURPOSE.  See the
     GNU General Public License For more details.

     You should have received a copy of the GNU General Public License along
     with this program; if not, write to the Free Software Foundation, Inc.,
     51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include "Network.hpp"

#include "../Utilities.hpp"
#include "../Constants.hpp"
#include <iostream>
#include <cstdio>
#include <vector>

Network::Network(int port, bool serverMode)
{
    this->port = port;
    this->isServer = serverMode;

    if (enet_initialize() != 0) {
        exit(EXIT_FAILURE);
    }

    host = nullptr;

    if (!serverMode) {
        // Legacy client mode: create client host immediately
        host = enet_host_create(NULL, 32, 0, 0, 0);
        if (host == NULL) {
            std::cout << "An error occurred while trying to create an ENet client host." << std::endl;
            exit(EXIT_FAILURE);
        }
        std::cout << "Started enet (client mode)" << std::endl;
    } else {
        std::cout << "ENet initialized (server mode, call startServer to begin listening)" << std::endl;
    }
}

Network::~Network()
{
    // Disconnect all peers gracefully
    for (unsigned int i = 0; i < peers.size(); i++) {
        if (peers[i] && peerConnected[i]) {
            enet_peer_disconnect(peers[i], 0);
        }
    }
    if (host) {
        // Flush any pending disconnect packets
        enet_host_flush(host);
        enet_host_destroy(host);
    }
    enet_deinitialize();
    std::cout << "Shut down enet" << std::endl;
}

void Network::startServer(int maxPlayers)
{
    if (!isServer) {
        std::cout << "startServer called but not in server mode" << std::endl;
        return;
    }

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;

    host = enet_host_create(&address, maxPlayers, 2, 0, 0);
    if (host == NULL) {
        std::cout << "Failed to create ENet server on port " << port << std::endl;
        return;
    }
    std::cout << "ENet server listening on port " << port << std::endl;
}

void Network::connectToServer(std::string hostnames)
{
    if (isServer) {
        std::cout << "connectToServer called but in server mode" << std::endl;
        return;
    }

    // hostname may be multiple comma separated names
    std::vector<std::string> multipleHostnames = Utilities::split(hostnames, ',');

    if (multipleHostnames.size() < 1) {
        multipleHostnames.push_back("");
    }

    for (int i = 0; i < (int)multipleHostnames.size(); i++) {
        ENetAddress address;
        ENetPeer* peer;

        std::string thisHostname = Utilities::trim(multipleHostnames.at(i));

        if (thisHostname.find(':') != std::string::npos) {
            std::vector<std::string> splitHostname = Utilities::split(thisHostname, ':');
            if (splitHostname.size() == 2) {
                thisHostname = splitHostname.at(0);
                address.port = Utilities::lexical_cast<enet_uint16>(splitHostname.at(1));
            } else {
                address.port = port;
            }
        } else {
            address.port = port;

            for (unsigned int j = 0; j < (unsigned int)i; j++) {
                if (thisHostname.compare(multipleHostnames.at(j)) == 0) {
                    address.port++;
                }
            }
        }

        enet_address_set_host(&address, thisHostname.c_str());

        peer = enet_host_connect(host, &address, ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT, 0);

        if (peer == NULL) {
            std::cout << "No available peers for initiating an ENet connection." << std::endl;
            exit(EXIT_FAILURE);
        }
        if (enet_host_service(host, &event, 1000) > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
            std::cout << "ENet connection succeeded to: " << thisHostname << std::endl;
            peers.push_back(peer);
            latestMessageFromPeer.push_back("");
            peerConnected.push_back(true);
        } else {
            enet_peer_reset(peer);
            std::cout << "ENet connection failed to:" << thisHostname << std::endl;
        }
    }
}

unsigned int Network::getNumberOfPeers()
{
    return (unsigned int)peers.size();
}

unsigned int Network::getNumberOfConnectedPeers()
{
    unsigned int count = 0;
    for (unsigned int i = 0; i < peerConnected.size(); i++) {
        if (peerConnected[i]) count++;
    }
    return count;
}

bool Network::isPeerConnected(unsigned int peerNumber)
{
    return peerNumber < peerConnected.size() && peerConnected[peerNumber];
}

void Network::sendString(std::string stringToSend, bool reliable, unsigned int peerNumber)
{
    if (peerNumber < peers.size() && peerConnected[peerNumber] && peers[peerNumber]) {
        int reliableFlag = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;

        if (stringToSend.length() > 0) {
            ENetPacket* packet = enet_packet_create(
                stringToSend.c_str(),
                strlen(stringToSend.c_str()) + 1,
                reliableFlag);

            enet_peer_send(peers.at(peerNumber), 0, packet);
            enet_host_flush(host);
        }
    }
}

void Network::listenForMessages()
{
    while (enet_host_service(host, &event, 10) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            // New peer connected
            std::cout << "New peer connected from "
                      << (event.peer->address.host & 0xFF) << "."
                      << ((event.peer->address.host >> 8) & 0xFF) << "."
                      << ((event.peer->address.host >> 16) & 0xFF) << "."
                      << ((event.peer->address.host >> 24) & 0xFF) << ":"
                      << event.peer->address.port << std::endl;
            peers.push_back(event.peer);
            latestMessageFromPeer.push_back("");
            peerConnected.push_back(true);
            pendingConnections.push_back((unsigned int)peers.size() - 1);
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            char tempString[8192];
            snprintf(tempString, 8192, "%s", event.packet->data);
            std::string receivedString(tempString);

            for (unsigned int i = 0; i < peers.size(); i++) {
                if (event.peer == peers.at(i)) {
                    if (receivedString.length() > 5 && receivedString.substr(0, 5) == "CHAT#") {
                        pendingChatMessages.push_back(std::make_pair(i, receivedString));
                    } else if (i < latestMessageFromPeer.size()) {
                        latestMessageFromPeer.at(i) = receivedString;
                    }
                }
            }
            enet_packet_destroy(event.packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            // Find which peer disconnected - mark as disconnected, don't erase
            for (unsigned int i = 0; i < peers.size(); i++) {
                if (event.peer == peers.at(i)) {
                    std::cout << "Peer " << i << " disconnected." << std::endl;
                    peerConnected[i] = false;
                    peers[i] = nullptr;
                    latestMessageFromPeer[i] = "";
                    pendingDisconnections.push_back(i);
                    break;
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

std::string Network::getLatestMessage(unsigned int peerNumber)
{
    if (peerNumber < latestMessageFromPeer.size()) {
        return latestMessageFromPeer.at(peerNumber);
    } else {
        return "";
    }
}

std::vector<unsigned int> Network::getNewConnections()
{
    std::vector<unsigned int> result;
    result.swap(pendingConnections);
    return result;
}

std::vector<unsigned int> Network::getNewDisconnections()
{
    std::vector<unsigned int> result;
    result.swap(pendingDisconnections);
    return result;
}

std::deque<std::pair<unsigned int, std::string>> Network::getPendingChatMessages()
{
    std::deque<std::pair<unsigned int, std::string>> result;
    result.swap(pendingChatMessages);
    return result;
}

std::string Network::getPeerAddress(unsigned int peerNumber) const
{
    if (peerNumber < peers.size() && peers[peerNumber]) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u",
                 peers[peerNumber]->address.host & 0xFF,
                 (peers[peerNumber]->address.host >> 8) & 0xFF,
                 (peers[peerNumber]->address.host >> 16) & 0xFF,
                 (peers[peerNumber]->address.host >> 24) & 0xFF,
                 peers[peerNumber]->address.port);
        return std::string(buf);
    }
    return "";
}
