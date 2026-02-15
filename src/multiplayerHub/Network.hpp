/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2016 James Packer

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

#ifndef __NETWORK_HPP_INCLUDED__
#define __NETWORK_HPP_INCLUDED__

#include <string>
#include <vector>
#include <deque>
#include <utility>

#include <enet/enet.h>

class Network
{
public:
    Network(int port, bool serverMode = false);
    ~Network();

    // Server mode: start listening for connections
    void startServer(int maxPlayers = 32);

    // Legacy client mode: connect out to peers
    void connectToServer(std::string hostnames);

    unsigned int getNumberOfPeers();          // Total peer slots (including disconnected)
    unsigned int getNumberOfConnectedPeers(); // Only currently connected peers
    bool isPeerConnected(unsigned int peerNumber);

    void sendString(std::string stringToSend, bool reliable, unsigned int peerNumber);
    void listenForMessages();
    std::string getLatestMessage(unsigned int peerNumber);
    std::string getPeerAddress(unsigned int peerNumber) const;

    // Event queries (drain pending events since last call)
    std::vector<unsigned int> getNewConnections();
    std::vector<unsigned int> getNewDisconnections();

    // Chat message queue (pair of peerIndex, message string)
    std::deque<std::pair<unsigned int, std::string>> getPendingChatMessages();

private:
    int port;
    bool isServer;

    ENetHost* host;
    ENetEvent event;
    std::vector<ENetPeer*> peers;
    std::vector<std::string> latestMessageFromPeer;
    std::vector<bool> peerConnected;

    // Pending event queues
    std::vector<unsigned int> pendingConnections;
    std::vector<unsigned int> pendingDisconnections;

    // Pending chat messages (peerIndex, raw message string)
    std::deque<std::pair<unsigned int, std::string>> pendingChatMessages;
};

#endif
