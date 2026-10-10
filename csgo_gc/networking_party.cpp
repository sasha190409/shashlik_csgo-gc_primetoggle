#include "stdafx.h"
#include "networking_party.h"
#include "networking_shared.h"
#include "gc_client.h"
#include "platform.h"

NetworkingParty::NetworkingParty(ISteamNetworkingMessages *networkingMessages)
    : m_networkingMessages{ networkingMessages }
    , m_sessionRequest{ this, &NetworkingParty::OnSessionRequest }
    , m_sessionFailed{ this, &NetworkingParty::OnSessionFailed }
{
}

void NetworkingParty::Update(ClientGC *gc)
{
    SteamNetworkingMessage_t *message;
    while (m_networkingMessages->ReceiveMessagesOnChannel(PartyChannel, &message, 1))
    {
        const uint64_t fromSteamId = message->m_identityPeer.GetSteamID64();
        const uint32_t fromAccountId = static_cast<uint32_t>(fromSteamId & 0xffffffff);

        GCMessageRead read{ 0, message->GetData(), static_cast<uint32_t>(message->GetSize()) };
        if (!read.IsValid() || read.IsProtobuf())
        {
            message->Release();
            continue;
        }

        const uint32_t type = read.TypeUnmasked();

        switch (type)
        {
        case k_EMsgPartyInviteRemote:
        {
            const auto *p = static_cast<const PartyInvitePayload *>(
                read.ReadData(sizeof(PartyInvitePayload)));
            if (p)
            {
                gc->OnRemotePartyInvite(p->fromAccountId, p->lobbyId, p->gameType);
            }
            break;
        }
        case k_EMsgPartyJoinRelayRemote:
        {
            const auto *p = static_cast<const PartyJoinRelayPayload *>(
                read.ReadData(sizeof(PartyJoinRelayPayload)));
            if (p)
            {
                gc->OnRemoteJoinRelay(p->fromAccountId, p->lobbyId);
            }
            break;
        }
        case k_EMsgPartyLobbyUpdateRemote:
        {
            const auto *header = static_cast<const uint32_t *>(
                read.ReadData(sizeof(uint32_t) * 2));
            if (!header) break;

            const uint32_t lobbyId = header[0];
            const uint32_t count = header[1];
            if (count == 0 || count > 64) break; // sanity

            const auto *members = static_cast<const uint32_t *>(
                read.ReadData(count * sizeof(uint32_t)));
            if (!members) break;

            std::vector<uint32_t> memberVec(members, members + count);
            gc->OnRemoteLobbyUpdate(fromAccountId, lobbyId, memberVec);
            break;
        }
        default:
            Platform::Print("NetworkingParty: unknown P2P msg %u from %u\n",
                type, fromAccountId);
            break;
        }

        message->Release();
    }
}

void NetworkingParty::SendInvite(uint32_t targetAccountId, uint32_t fromAccountId,
                                 uint32_t lobbyId, uint32_t gameType)
{
    PartyInvitePayload payload{ lobbyId, gameType, fromAccountId };

    GCMessageWrite write{ k_EMsgPartyInviteRemote };
    write.WriteData(&payload, sizeof(payload));

    SteamNetworkingIdentity identity;
    identity.SetSteamID64(CSteamID(targetAccountId, k_EUniversePublic,
        k_EAccountTypeIndividual).ConvertToUint64());

    m_networkingMessages->SendMessageToUser(identity, write.Data(), write.Size(),
        k_nSteamNetworkingSend_Reliable, PartyChannel);
}

void NetworkingParty::SendJoinRelay(uint32_t hostAccountId, uint32_t fromAccountId,
                                    uint32_t lobbyId)
{
    PartyJoinRelayPayload payload{ lobbyId, fromAccountId };

    GCMessageWrite write{ k_EMsgPartyJoinRelayRemote };
    write.WriteData(&payload, sizeof(payload));

    SteamNetworkingIdentity identity;
    identity.SetSteamID64(CSteamID(hostAccountId, k_EUniversePublic,
        k_EAccountTypeIndividual).ConvertToUint64());

    m_networkingMessages->SendMessageToUser(identity, write.Data(), write.Size(),
        k_nSteamNetworkingSend_Reliable, PartyChannel);
}

void NetworkingParty::SendLobbyUpdate(uint32_t targetAccountId, uint32_t lobbyId,
                                      const std::vector<uint32_t> &members)
{
    GCMessageWrite write{ k_EMsgPartyLobbyUpdateRemote };
    write.WriteUint32(lobbyId);
    write.WriteUint32(static_cast<uint32_t>(members.size()));
    if (!members.empty())
    {
        write.WriteData(members.data(),
            static_cast<uint32_t>(members.size() * sizeof(uint32_t)));
    }

    SteamNetworkingIdentity identity;
    identity.SetSteamID64(CSteamID(targetAccountId, k_EUniversePublic,
        k_EAccountTypeIndividual).ConvertToUint64());

    m_networkingMessages->SendMessageToUser(identity, write.Data(), write.Size(),
        k_nSteamNetworkingSend_Reliable, PartyChannel);
}

void NetworkingParty::OnSessionRequest(SteamNetworkingMessagesSessionRequest_t *param)
{
    // accept p2p sessions from everyone - party is party
    m_networkingMessages->AcceptSessionWithUser(param->m_identityRemote);
}

void NetworkingParty::OnSessionFailed(SteamNetworkingMessagesSessionFailed_t *param)
{
    Platform::Print("NetworkingParty session failed: %s\n", param->m_info.m_szEndDebug);
}
