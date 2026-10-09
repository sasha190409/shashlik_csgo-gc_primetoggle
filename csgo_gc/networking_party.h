#pragma once

#include "networking_shared.h"
#include <steam/isteamnetworkingmessages.h>
#include <cstdint>
#include <vector>

class ClientGC;

// Отдельный канал от игрового (NetMessageChannel=7), чтобы не мешать.
constexpr int PartyChannel = 8;

// Внутренние типы сообщений для P2P-party.
enum EPartyInternalMsg : uint32_t
{
    k_EMsgPartyInviteRemote      = 0x60000001,
    k_EMsgPartyJoinRelayRemote   = 0x60000002,
    k_EMsgPartyLobbyUpdateRemote = 0x60000003,
};

struct PartyInvitePayload
{
    uint32_t lobbyId;
    uint32_t gameType;
    uint32_t fromAccountId;
};

struct PartyJoinRelayPayload
{
    uint32_t lobbyId;
    uint32_t fromAccountId;
};

class NetworkingParty
{
public:
    explicit NetworkingParty(ISteamNetworkingMessages *networkingMessages);

    // Вызывается из RunCallbacks.
    void Update(ClientGC *gc);

    void SendInvite(uint32_t targetAccountId, uint32_t fromAccountId,
                    uint32_t lobbyId, uint32_t gameType);
    void SendJoinRelay(uint32_t hostAccountId, uint32_t fromAccountId, uint32_t lobbyId);
    void SendLobbyUpdate(uint32_t targetAccountId, uint32_t lobbyId,
                         const std::vector<uint32_t> &members);

    STEAM_CALLBACK(NetworkingParty, OnSessionRequest,
        SteamNetworkingMessagesSessionRequest_t, m_sessionRequest);
    STEAM_CALLBACK(NetworkingParty, OnSessionFailed,
        SteamNetworkingMessagesSessionFailed_t, m_sessionFailed);

private:
    ISteamNetworkingMessages *const m_networkingMessages;
};
