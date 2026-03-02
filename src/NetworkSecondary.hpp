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

#ifndef __NETWORKSECONDARY_HPP_INCLUDED__
#define __NETWORKSECONDARY_HPP_INCLUDED__

#include "Network.hpp"

#include <string>
#include <deque>

#include <enet/enet.h>

//Forward declarations
class SimulationModel;

class NetworkSecondary : public Network
{
public:
    NetworkSecondary(int port, OperatingMode::Mode mode, irr::IrrlichtDevice* dev);
    ~NetworkSecondary();

    void connectToServer(std::string hostnames);
    void getScenarioFromNetwork(std::string& dataString);
    void setModel(SimulationModel* model);
    void update();
    int getPort();
    void shutdownAllSecondaries(void);
    void sendChatMessage(const std::string& text) override;
    bool hasPendingChat() override;
    std::deque<ChatMessage> getPendingChatMessages() override;

private:
    SimulationModel* model;
    irr::IrrlichtDevice* device;

    float accelAdjustment;
    float previousTimeError;

    ENetHost * server; // Also used as client host in MultiplayerClient mode
    ENetPeer * hubPeer; // Peer connection to hub (MultiplayerClient mode only)
    ENetEvent event;
    OperatingMode::Mode mode;

    void receiveMessage();

    // Chat message queue
    std::deque<ChatMessage> pendingChatMessages;
};

#endif

