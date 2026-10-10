#include "stdafx.h"
#include "gc_server.h"
#include "gc_const.h"
#include "gc_const_csgo.h"
#include "graffiti.h"

// yuck!! needed for CSteamID (construct full id from account id)
#include "steam/steamclientpublic.h"
#include "networking_server.h"

ServerGC::ServerGC()
{
    // also called from ClientGC's constructor
    Graffiti::Initialize();

    StartThread();

    Platform::Print("ServerGC spawned\n");
}

ServerGC::~ServerGC()
{
    StopThread();
    Platform::Print("ServerGC destroyed\n");
}

void ServerGC::HandleEvent(GCEvent type, uint64_t id, const std::vector<uint8_t> &buffer)
{
    switch (type)
    {
    case GCEvent::Message:
        HandleMessage(static_cast<uint32_t>(id), buffer.data(), static_cast<uint32_t>(buffer.size()));
        break;

    case GCEvent::NetMessage:
        HandleNetMessage(id, buffer.data(), static_cast<uint32_t>(buffer.size()));
        break;

    case GCEvent::ClientSOCacheUnsubscribe:
        HandleClientSOCacheUnsubscribe(id);
        break;

    default:
        assert(false);
        break;
    }
}

void ServerGC::HandleMessage(uint32_t type, const void *data, uint32_t size)
{
    GCMessageRead messageRead{ type, data, size };
    if (!messageRead.IsValid())
    {
        assert(false);
        return;
    }

    if (messageRead.IsProtobuf())
    {
        switch (messageRead.TypeUnmasked())
        {
        // handshake / connection
        case k_EMsgGCServerHello:
            OnServerHello(messageRead);
            break;

        case k_EMsgGCServerAvailable:                    // 4506
            OnServerAvailable(messageRead);
            break;

        case k_EMsgGCClientConnectionStatus:             // 4009
            OnClientConnectionStatus(messageRead);
            break;

        case k_EMsgGCServerConnectionStatus:             // 4010
            OnServerConnectionStatus(messageRead);
            break;

        // matchmaking
        case k_EMsgGCCStrike15_v2_MatchmakingClient2ServerPing:      // 9103
            OnMatchmakingClient2ServerPing(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchmakingServerReservationResponse: // 9106
            OnMatchmakingServerReservationResponse(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve:       // 9107
            OnMatchmakingGC2ClientReserve(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchmakingGC2ClientAbandon:       // 9112
            OnMatchmakingGC2ClientAbandon(messageRead);
            break;

        case 9114: /* k_EMsgGCCStrike15_v2_MatchmakingGC2ServerConfirm */
            OnMatchmakingGC2ServerConfirm(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_GC2ServerReservationUpdate:        // 9142
            OnGC2ServerReservationUpdate(messageRead);
            break;

        // misc
        case k_EMsgGCCStrike15_v2_ServerVarValueNotificationInfo:    // 9150
            OnServerVarValueNotificationInfo(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Server2GCClientValidate:           // 9153
            OnServer2GCClientValidate(messageRead);
            break;

        case k_EMsgGCServerVersionUpdated:                           // 2522
            OnServerVersionUpdated(messageRead);
            break;

        case k_EMsgGCGameServerInfo:                                 // 4508
            OnGameServerInfo(messageRead);
            break;

        // items
        case k_EMsgGC_IncrementKillCountAttribute:
            IncrementKillCountAttribute(messageRead);
            break;

        default:
            Platform::Print("ServerGC::HandleMessage: unhandled protobuf message %s)\n",
                MessageName(messageRead.TypeUnmasked()));
            break;
        }
    }
    else
    {
        // no non-protobuf messages are expected server-side, but log anyway
        Platform::Print("ServerGC::HandleMessage: unhandled non-protobuf message %s\n",
            MessageName(messageRead.TypeUnmasked()));
    }
}

void ServerGC::HandleClientSOCacheUnsubscribe(uint64_t steamId)
{
    Platform::Print("HandleClientSOCacheUnsubscribe: %llu\n", steamId);

    CMsgSOCacheUnsubscribed message;
    message.mutable_owner_soid()->set_type(SoIdTypeSteamId);
    message.mutable_owner_soid()->set_id(steamId);

    GCMessageWrite write{ k_ESOMsg_CacheUnsubscribed, message };
    PostToHost(HostEvent::Message, write.TypeMasked(), write.Data(), write.Size());
}

template<typename T>
static bool ValidateMessageOwnerSOID(GCMessageRead &messageRead, uint64_t steamId)
{
    T message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("ValidateMessageOwnerSOID %llu: parsing failed\n", steamId);
        return false;
    }

    if (message.owner_soid().type() != SoIdTypeSteamId
        || message.owner_soid().id() != steamId)
    {
        Platform::Print("ValidateMessageOwnerSOID %llu: steam id mismatch (message has %llu)\n",
            steamId, message.owner_soid().id());
        return false;
    }

    return true;
}

void ServerGC::SendConfirmToClient(uint64_t clientId, const PendingReservation& res)
{
    CMsgGCCStrike15_v2_MatchmakingGC2ServerConfirm confirm;
    confirm.set_token(res.token);
    confirm.set_stamp(res.stamp);
    confirm.set_exchange(res.exchange);

    GCMessageWrite write{ 9114, confirm };   // k_EMsgGCCStrike15_v2_MatchmakingGC2ServerConfirm
    PostToHost(HostEvent::NetMessage, clientId, write.Data(), write.Size());
    Platform::Print("ServerGC: sent confirm to client %llu (res %llu)\n", clientId, res.exchange);
}

void ServerGC::CheckPendingReservations()
{
    if (m_pendingReservations.empty())
        return;

    auto it = m_pendingReservations.begin();
    while (it != m_pendingReservations.end())
    {
        uint64_t clientId = it->first;
        if (!m_networking->HasClient(clientId))
        {
            SendConfirmToClient(clientId, it->second);
            it = m_pendingReservations.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// handshake / connection

void ServerGC::OnServerHello(GCMessageRead &messageRead)
{
    CMsgServerHello hello;
    if (!messageRead.ReadProtobuf(hello))
    {
        Platform::Print("Parsing CMsgServerHello failed, ignoring\n");
        return;
    }

    Platform::Print("ServerGC: ServerHello version=%u\n", hello.version());

    CMsgCStrike15Welcome csWelcome;
    csWelcome.set_gscookieid(GameServerCookieId);

    CMsgClientWelcome welcome;
    welcome.set_version(0);
    welcome.set_game_data(csWelcome.SerializeAsString());
    welcome.set_rtime32_gc_welcome_timestamp(static_cast<uint32_t>(time(nullptr)));

    GCMessageWrite write{ k_EMsgGCServerWelcome, welcome };
    PostToHost(HostEvent::Message, write.TypeMasked(), write.Data(), write.Size());

    m_receivedHello.store(true, std::memory_order_release);
}

void ServerGC::OnServerAvailable(GCMessageRead &messageRead)
{
    CMsgServerAvailable message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgServerAvailable failed, ignoring\n");
        return;
    }

    Platform::Print("ServerGC: ServerAvailable received, replying with ServerConnectionStatus\n");

    CMsgConnectionStatus response;
    response.set_status(GCConnectionStatus_HAVE_SESSION);
    response.set_client_session_need(0);
    response.set_queue_position(0);
    response.set_queue_size(0);
    response.set_wait_seconds(0);
    response.set_estimated_wait_seconds_remaining(0);

    GCMessageWrite write{ k_EMsgGCServerConnectionStatus, response };
    PostToHost(HostEvent::Message, write.TypeMasked(), write.Data(), write.Size());
}

void ServerGC::OnClientConnectionStatus(GCMessageRead &messageRead)
{
    CMsgConnectionStatus message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing ClientConnectionStatus failed, ignoring\n");
        return;
    }

    Platform::Print("ServerGC: ClientConnectionStatus status=%d queue=%d/%d wait=%d est=%d\n",
        message.status(),
        message.queue_position(),
        message.queue_size(),
        message.wait_seconds(),
        message.estimated_wait_seconds_remaining());
}

void ServerGC::OnServerConnectionStatus(GCMessageRead &messageRead)
{
    CMsgConnectionStatus message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing ServerConnectionStatus failed, ignoring\n");
        return;
    }

    Platform::Print("ServerGC: ServerConnectionStatus status=%d queue=%d/%d wait=%d est=%d\n",
        message.status(),
        message.queue_position(),
        message.queue_size(),
        message.wait_seconds(),
        message.estimated_wait_seconds_remaining());
}

// matchmaking

void ServerGC::OnMatchmakingClient2ServerPing(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchmakingClient2ServerPing request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::Print("Parsing MatchmakingClient2ServerPing failed, ignoring\n");
        return;
    }

    Platform::Print("ServerGC: MatchmakingClient2ServerPing gameserverpings=%d datacenterpings=%d offset=%d final=%d\n",
        request.gameserverpings_size(),
        request.data_center_pings_size(),
        request.offset_index(),
        request.final_batch());

    // mirror the client behaviour: reply with a MatchmakingGC2ClientUpdate
    // so the game stops waiting on the batch.
    CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate response;
    response.set_matchmaking(0);

    GCMessageWrite write{ k_EMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate, response, messageRead.JobId() };
    PostToHost(HostEvent::Message, write.TypeMasked(), write.Data(), write.Size());
}

void ServerGC::OnMatchmakingServerReservationResponse(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchmakingServerReservationResponse response;
    if (!messageRead.ReadProtobuf(response))
    {
        Platform::Print("Failed to parse MatchmakingServerReservationResponse\n");
        return;
    }

    // the response has no account id, use the reservationid as a key
    // mikkotodo: a proper impl would map reservationid -> client steamid
    uint64_t clientId = response.reservationid();

    Platform::Print("ServerGC: received reservation response for res %llu, map %s, client %llu\n",
                    response.reservationid(), response.map().c_str(), clientId);

    if (!m_networking->HasClient(clientId))
    {
        PendingReservation pending;
        pending.exchange = response.reservationid();
        pending.token = 0x12345678;
        pending.stamp = static_cast<uint32_t>(time(nullptr));
        m_pendingReservations[clientId] = pending;
        Platform::Print("ServerGC: pending reservation for client %llu\n", clientId);
        return;
    }

    SendConfirmToClient(clientId, { response.reservationid(), 0x12345678, static_cast<uint32_t>(time(nullptr)) });
}

void ServerGC::OnMatchmakingGC2ClientReserve(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing MatchmakingGC2ClientReserve failed, ignoring\n");
        return;
    }

    Platform::Print("ServerGC: MatchmakingGC2ClientReserve serverid=%llu res=%llu map=%s addr=%s\n",
        static_cast<unsigned long long>(message.serverid()),
        static_cast<unsigned long long>(message.reservationid()),
        message.map().c_str(),
        message.server_address().c_str());

    // this is nominally a GC->client message; if it happens to arrive at the
    // server, just forward it into the game's GC queue so the client sees it.
    GCMessageWrite write{ k_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve, message };
    PostToHost(HostEvent::Message, write.TypeMasked(), write.Data(), write.Size());
}

void ServerGC::OnMatchmakingGC2ClientAbandon(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchmakingGC2ClientAbandon message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing MatchmakingGC2ClientAbandon failed, ignoring\n");
        return;
    }

    Platform::Print("ServerGC: MatchmakingGC2ClientAbandon account=%u penalty=%u reason=%u\n",
        message.account_id(),
        message.penalty_seconds(),
        message.penalty_reason());

    GCMessageWrite write{ k_EMsgGCCStrike15_v2_MatchmakingGC2ClientAbandon, message };
    PostToHost(HostEvent::Message, write.TypeMasked(), write.Data(), write.Size());
}

void ServerGC::OnMatchmakingGC2ServerConfirm(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchmakingGC2ServerConfirm message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing MatchmakingGC2ServerConfirm failed, ignoring\n");
        return;
    }

    Platform::Print("ServerGC: MatchmakingGC2ServerConfirm token=%u stamp=%u exchange=%llu retry=%u\n",
        message.token(),
        message.stamp(),
        static_cast<unsigned long long>(message.exchange()),
        message.retry());
}

void ServerGC::OnGC2ServerReservationUpdate(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_GC2ServerReservationUpdate message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing GC2ServerReservationUpdate failed, ignoring\n");
        return;
    }

    Platform::Print("ServerGC: GC2ServerReservationUpdate viewers_total=%u viewers_steam=%u\n",
        message.viewers_external_total(),
        message.viewers_external_steam());
}

// misc

void ServerGC::OnServerVarValueNotificationInfo(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ServerVarValueNotificationInfo message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing ServerVarValueNotificationInfo failed, ignoring\n");
        return;
    }

    Platform::Print("ServerGC: ServerVarValueNotificationInfo account=%u type=%u viewangles=%d userdata=%d\n",
        message.accountid(),
        message.type(),
        message.viewangles_size(),
        message.userdata_size());
}

void ServerGC::OnServer2GCClientValidate(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_Server2GCClientValidate message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing Server2GCClientValidate failed, ignoring\n");
        return;
    }

    // the server doesn't want a response.
    Platform::Print("ServerGC: Server2GCClientValidate account=%u (ignored)\n",
        message.accountid());
}

void ServerGC::OnServerVersionUpdated(GCMessageRead &messageRead)
{
    CMsgGCServerVersionUpdated message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing ServerVersionUpdated failed, ignoring\n");
        return;
    }

    Platform::Print("ServerGC: ServerVersionUpdated server_version=%u\n",
        message.server_version());
}

void ServerGC::OnGameServerInfo(GCMessageRead &messageRead)
{
    CMsgGameServerInfo message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing GameServerInfo failed, ignoring\n");
        return;
    }

    Platform::Print("ServerGC: GameServerInfo ip=%u port=%u tv_port=%u type=%d region=%u load=%f\n",
        message.server_public_ip_addr(),
        message.server_port(),
        message.server_tv_port(),
        static_cast<int>(message.server_type()),
        message.server_region(),
        message.server_loadavg());
}

// items

void ServerGC::IncrementKillCountAttribute(GCMessageRead &messageRead)
{
    CMsgIncrementKillCountAttribute message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgIncrementKillCountAttribute failed, ignoring\n");
        return;
    }

    // just forward it to the killer
    GCMessageWrite messageWrite{ k_EMsgGC_IncrementKillCountAttribute, message };
    CSteamID killerId{ message.killer_account_id(), k_EUniversePublic, k_EAccountTypeIndividual };
    PostToHost(HostEvent::NetMessage, killerId.ConvertToUint64(), messageWrite.Data(), messageWrite.Size());
}

// net messages (client <-> server over P2P)

void ServerGC::HandleNetMessage(uint64_t steamId, const void *data, uint32_t size)
{
    assert(CanHandleNetMessages());

    GCMessageRead validate{ 0, data, size };
    if (!validate.IsValid())
    {
        assert(false);
        return;
    }

    if (!validate.IsProtobuf())
    {
        // all the allowed messages are protobuf based
        Platform::Print("ServerGC: ignoring non protobuf message %u from %llu\n",
            validate.TypeUnmasked(), steamId);
        return;
    }

    // validate the type and contents
    bool isValid = false;

    switch (validate.TypeUnmasked())
    {
    // single object SO messages: all carry owner_soid
    case k_ESOMsg_Create:
    case k_ESOMsg_Update:
    case k_ESOMsg_Destroy:
        isValid = ValidateMessageOwnerSOID<CMsgSOSingleObject>(validate, steamId);
        break;

    // multi object SO messages
    case k_ESOMsg_UpdateMultiple:
        isValid = ValidateMessageOwnerSOID<CMsgSOMultipleObjects>(validate, steamId);
        break;

    // cache subscription family
    case k_ESOMsg_CacheSubscribed:
        isValid = ValidateMessageOwnerSOID<CMsgSOCacheSubscribed>(validate, steamId);
        break;

    case k_ESOMsg_CacheUnsubscribed:
        isValid = ValidateMessageOwnerSOID<CMsgSOCacheUnsubscribed>(validate, steamId);
        break;

    case k_ESOMsg_CacheSubscriptionCheck:
        isValid = ValidateMessageOwnerSOID<CMsgSOCacheSubscriptionCheck>(validate, steamId);
        break;

    case k_ESOMsg_CacheSubscriptionRefresh:
        isValid = ValidateMessageOwnerSOID<CMsgSOCacheSubscriptionRefresh>(validate, steamId);
        break;

    // acknowledged items: no owner, just forward
    case k_EMsgGCItemAcknowledged:
        isValid = true;
        break;

    default:
        Platform::Print("ServerGC: unknown net message %u from %llu (dropping)\n",
            validate.TypeUnmasked(), steamId);
        return;
    }

    if (!isValid)
    {
        Platform::Print("ServerGC: ignoring net message %u from %llu\n",
            validate.TypeUnmasked(), steamId);
        return;
    }

    PostToHost(HostEvent::Message, validate.TypeMasked(), data, size);
}