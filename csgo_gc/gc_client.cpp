#include "stdafx.h"
#include "gc_client.h"
#include "graffiti.h"
#include "keyvalue.h"
#include <filesystem>
#include "case_opening.h"
#include <steam/isteamhttp.h>
#include "steam_hook.h"   // for RecreateClientGC()

ClientGC::ClientGC(uint64_t steamId)
    : m_steamId{ steamId }
    , m_inventory{ m_steamId }
    , m_httpCallback(this, &ClientGC::OnOverwatchHTTPResponse)
{
    Graffiti::Initialize();
    StartThread();
    FetchOverwatchCases();

    Platform::Print("ClientGC spawned for user %llu\n", m_steamId);
}

ClientGC::~ClientGC()
{
    StopThread();
    Platform::Print("ClientGC destroyed\n");
}

void ClientGC::HandleEvent(GCEvent type, uint64_t id, const std::vector<uint8_t> &buffer)
{
    switch (type)
    {
    case GCEvent::Message:
        HandleMessage(static_cast<uint32_t>(id), buffer.data(), static_cast<uint32_t>(buffer.size()));
        break;

    case GCEvent::NetMessage:
        HandleNetMessage(buffer.data(), static_cast<uint32_t>(buffer.size()));
        break;

    case GCEvent::SOCacheRequest:
        HandleSOCacheRequest();
        break;

    default:
        assert(false);
        break;
    }
}

void ClientGC::SendCompetitiveCooldown()
{
    uint32_t seconds = 0;
    if (m_isCooldownActive) {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            m_cooldownEndTime - std::chrono::steady_clock::now()).count();
        seconds = (remaining > 0) ? static_cast<uint32_t>(remaining) : 0;
        if (seconds == 0) {
            // expired during the check
            m_isCooldownActive = false;
        }
    }

    CMsgGCCStrike15_v2_ServerNotificationForUserPenalty penalty;
    penalty.set_account_id(EffectiveAccountId());
    penalty.set_reason(0); // competitive cooldown
    penalty.set_seconds(seconds);
    penalty.set_communication_cooldown(false);
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ServerNotificationForUserPenalty, penalty);
}

void ClientGC::OnMatchmakingPing(GCMessageRead &messageRead)
{
    // Просто отправляем актуальное состояние (в зависимости от флага)
    SendMatchmakingUpdate();
}

void ClientGC::SendMatchmakingUpdate()
{
    CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate update;
    update.set_matchmaking(m_isSearching ? 1 : 0);

    auto* stats = update.mutable_global_stats();
    stats->set_players_searching(1000);
    stats->set_search_time_avg(60);   // keep top-level for fallback

    // Add per-game-type statistics (choose the appropriate game_type)
    auto* detail = stats->add_search_statistics();
    detail->set_game_type(0); // or whatever game type you're emulating (e.g., 6 for competitive)
    detail->set_search_time_avg(60);
    detail->set_players_searching(1000);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate, update);
}

void ClientGC::OnMatchmakingStart(GCMessageRead &messageRead)
{
    uint32_t cooldown = GetConfig().CompetitiveCooldownSeconds();
    if (cooldown > 0)
    {
        m_isSearching = false;
        m_isCooldownActive = true;
        m_cooldownEndTime = std::chrono::steady_clock::now() + std::chrono::seconds(cooldown);

        SendMatchmakingUpdate(); // stops the searching animation
        SendCompetitiveCooldown(); // sends initial cooldown
        Platform::Print("Blocked matchmaking: Cooldown active (%u seconds)\n", cooldown);
        return;
    }

    // Original code continues...
    m_isSearching = true;
    SendMatchmakingUpdate();
}

void ClientGC::OnMatchmakingStop(GCMessageRead &messageRead)
{
    m_isSearching = false;
    SendMatchmakingUpdate();
}

void ClientGC::SendMatchmakingReservation()
{
    CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve reserve;
    reserve.set_serverid(0x12345678);  // any non‑zero value
    reserve.set_direct_udp_ip(0xC0A8000E); // 192.168.0.14
    reserve.set_direct_udp_port(27019);
    reserve.set_reservationid(++m_matchmakingReservationId); // unique
    reserve.set_map("de_dust2");
    reserve.set_server_address("192.168.0.14:27019");
    CMsgGCCStrike15_v2_MatchmakingGC2ServerReserve *sub = reserve.mutable_reservation();

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve, reserve);
    m_matchmakingReservationSent = true;

    Platform::Print("Sent matchmaking reservation to %s:%d\n", "192.168.0.14", 27019);
}

void ClientGC::HandleMessage(uint32_t type, const void *data, uint32_t size)
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
        case k_EMsgGCClientHello:
            OnClientHello(messageRead);
            break;

        case k_EMsgGCAdjustItemEquippedState:
            AdjustItemEquippedState(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientPlayerDecalSign:
            ClientPlayerDecalSign(messageRead);
            break;

        case k_EMsgGCUseItemRequest:
            UseItemRequest(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientRequestJoinServerData:
            ClientRequestJoinServerData(messageRead);
            break;

        case k_EMsgGCSetItemPositions:
            SetItemPositions(messageRead);
            break;

        case k_EMsgGCApplySticker:
            ApplySticker(messageRead);
            break;

        case k_EMsgGCStoreGetUserData:
            StoreGetUserData(messageRead);
            break;

        case k_EMsgGCStorePurchaseInit:
            StorePurchaseInit(messageRead);
            break;

        case k_EMsgGCStorePurchaseFinalize:
            StorePurchaseFinalize(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Party_Search:
            PartySearch(messageRead);
            break;
            
        case k_EMsgGCCStrike15_v2_Account_RequestCoPlays:
            RequestCoPlays(messageRead);
            break;
        
        case k_EMsgGCCStrike15_v2_ClientRequestPlayersProfile:
            ClientRequestPlayersProfile(messageRead);
            break;

        case k_EMsgGCCasketItemLoadContents:
            CasketItemLoadContents(messageRead);
            break;

        case k_EMsgGCCasketItemAdd:
            CasketItemAdd(messageRead);
            break;

        case k_EMsgGCCasketItemExtract:
            CasketItemExtract(messageRead);
            break;

        case k_EMsgGCStatTrakSwap:
            StatTrakSwap(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchmakingClient2ServerPing:
            OnMatchmakingPing(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchmakingStart:
            OnMatchmakingStart(messageRead);
            break;
        
        case k_EMsgGCCStrike15_v2_MatchmakingStop:
            OnMatchmakingStop(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_PlayerOverwatchCaseStatus:
            OnOverwatchCaseStatus(messageRead);
            break;
        
        case k_EMsgGCCStrike15_v2_PlayerOverwatchCaseUpdate:
            OnOverwatchCaseUpdate(messageRead);
            break;
        
        case k_EMsgGCCStrike15_v2_ClientReportValidation:
            OnClientReportValidation(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_GetEventFavorites_Request:
            OnGetEventFavorites(messageRead);
            break;

        default:
            Platform::Print("ClientGC::HandleMessage: unhandled protobuf message %s\n",
                MessageName(messageRead.TypeUnmasked()));
            break;
        }
    }
    else
    {
        switch (messageRead.TypeUnmasked())
        {
        case k_EMsgGCDelete:
            DeleteItem(messageRead);
            break;

        case k_EMsgGCUnlockCrate:
            UnlockCrate(messageRead);
            break;

        case k_EMsgGCNameItem:
            NameItem(messageRead);
            break;

        case k_EMsgGCNameBaseItem:
            NameBaseItem(messageRead);
            break;

        case k_EMsgGCRemoveItemName:
            RemoveItemName(messageRead);
            break;

        default:
            Platform::Print("ClientGC::HandleMessage: unhandled struct message %s\n",
                MessageName(messageRead.TypeUnmasked()));
            break;
        }
    }
}

void ClientGC::SendMatchmakingHelloUpdate()
{
    CMsgGCCStrike15_v2_MatchmakingGC2ClientHello mmHello;
    BuildMatchmakingHello(mmHello);
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientHello, mmHello);
}

void ClientGC::UpdateCooldown()
{
    if (!m_isCooldownActive)
        return;
    auto now = std::chrono::steady_clock::now();
    if (now >= m_cooldownEndTime) {
        m_isCooldownActive = false;
        SendCompetitiveCooldown(); // sends 0
        Platform::Print("Competitive cooldown expired.\n");
    }
}

void ClientGC::ProcessGiftUse(uint64_t giftId)
{
    const CSOEconItem *giftItem = m_inventory.GetItem(giftId);
    if (!giftItem)
    {
        Platform::Print("Gift item not found\n");
        return;
    }

    uint32_t defIndex = giftItem->def_index();

    // Determine number of recipients (items to give)
    int numItems = 1;
    if (defIndex == 1210)
        numItems = 1;
    else if (defIndex == 1211)
        numItems = 9;
    else if (defIndex == 1215)
        numItems = 25;
    else {
        Platform::Print("Unknown gift type\n");
        return;
    }

    // Get the "set supply crate series" attribute value via Inventory's ItemSchema
    uint32_t series = 0;
    uint32_t attrDef = m_inventory.GetItemSchema().GetAttributeDefIndex("set supply crate series");
    if (attrDef)
    {
        for (const auto &attr : giftItem->attribute())
        {
            if (attr.def_index() == attrDef)
            {
                series = m_inventory.GetItemSchema().AttributeUint32(&attr);
                break;
            }
        }
    }

    if (series == 0)
    {
        Platform::Print("Gift has no supply crate series\n");
        return;
    }

    const LootList *lootList = m_inventory.GetItemSchema().GetLootListBySeries(series);
    if (!lootList)
    {
        Platform::Print("No loot list for series %u\n", series);
        return;
    }

    // Prepare updates
    CMsgSOMultipleObjects updateMultiple;
    CMsgSOSingleObject destroy;
    CMsgGCItemCustomizationNotification notification;
    notification.set_request(k_EGCItemCustomizationNotification_Gift); // from protobuf enum

    // Use Inventory's schema and random generator
    CaseOpening caseOpening(m_inventory.GetItemSchema(), m_inventory.GetRandom());

    for (int i = 0; i < numItems; i++)
    {
        CSOEconItem newItemProto;
        if (!caseOpening.SelectItemFromLootList(*lootList, newItemProto))
        {
            Platform::Print("Failed to select item from loot list\n");
            continue;
        }

        // Create actual item in inventory
        CSOEconItem &createdItem = m_inventory.CreateItem(newItemProto);
        // AddToMultipleObjects is now public and accepts the SOTypeId
        m_inventory.AddToMultipleObjects(updateMultiple, SOTypeItem, createdItem);
        notification.add_item_id(createdItem.id());
    }

    // Destroy the gift if configured
    if (GetConfig().DestroyUsedItems())
    {
        m_inventory.RemoveItem(giftId, destroy);
    }

    // Send updates to game server and client
    if (updateMultiple.objects_modified_size() > 0)
    {
        SendMessageToGame(true, k_ESOMsg_UpdateMultiple, updateMultiple);
        SendMessageToGame(false, k_ESOMsg_UpdateMultiple, updateMultiple);
    }

    if (destroy.has_type_id())
    {
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        SendMessageToGame(false, k_ESOMsg_Destroy, destroy);
    }

    if (notification.item_id_size() > 0)
    {
        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
}


void ClientGC::HandleNetMessage(const void *data, uint32_t size)
{
    // pass 0 as type so it gets parsed from the message
    GCMessageRead messageRead{ 0, data, size };
    if (!messageRead.IsValid())
    {
        assert(false);
        return;
    }

    if (messageRead.IsProtobuf())
    {
        switch (messageRead.TypeUnmasked())
        {
        case k_EMsgGC_IncrementKillCountAttribute:
            IncrementKillCountAttribute(messageRead);
            return;
        }
    }

    Platform::Print("ClientGC::HandleNetMessage: unhandled protobuf message %s\n",
        MessageName(messageRead.TypeUnmasked()));
}

void ClientGC::HandleSOCacheRequest()
{
    CMsgSOCacheSubscribed message;
    m_inventory.BuildCacheSubscription(message, GetConfig().Level(), true);

    GCMessageWrite messageWrite{ k_ESOMsg_CacheSubscribed, message };
    PostToHost(HostEvent::NetMessage, 0, messageWrite.Data(), messageWrite.Size());
}

void ClientGC::SendMessageToGame(bool sendToGameServer, uint32_t type,
    const google::protobuf::MessageLite &message, uint64_t jobId)
{
    GCMessageWrite messageWrite{ type, message, jobId };

    if (sendToGameServer)
    {
        PostToHost(HostEvent::NetMessage, 0, messageWrite.Data(), messageWrite.Size());
    }

    PostToHost(HostEvent::Message, messageWrite.TypeMasked(), messageWrite.Data(), messageWrite.Size());
}

constexpr uint32_t MakeAddress(uint32_t v1, uint32_t v2, uint32_t v3, uint32_t v4)
{
    return v4 | (v3 << 8) | (v2 << 16) | (v1 << 24);
}

static void BuildCSWelcome(CMsgCStrike15Welcome &message)
{
    // mikkotodo cleanup dox
    message.set_store_item_hash(136617352);
    message.set_timeplayedconsecutively(0);
    message.set_time_first_played(1329845773);
    message.set_last_time_played(1680260376);
    message.set_last_ip_address(MakeAddress(127, 0, 0, 1));
}

void ClientGC::BuildMatchmakingHello(CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &message)
{
    message.set_account_id(EffectiveAccountId());

    // this is the state of csgo matchmaking in 2024
    message.mutable_global_stats()->set_players_online(10000);
    message.mutable_global_stats()->set_servers_online(10000);
    message.mutable_global_stats()->set_players_searching(1000);
    message.mutable_global_stats()->set_servers_available(1000);
    message.mutable_global_stats()->set_ongoing_matches(999);
    message.mutable_global_stats()->set_search_time_avg(60);

    auto* stats = message.mutable_global_stats();
    stats->set_players_searching(1000);
    stats->set_servers_available(1000);
    stats->set_search_time_avg(60);

    auto* detail = stats->add_search_statistics();
    detail->set_game_type(6); // competitive, see below
    detail->set_search_time_avg(60);
    detail->set_players_searching(1000);

    // don't write search_statistics

    message.mutable_global_stats()->set_main_post_url("");

    // bullshit
    message.mutable_global_stats()->set_required_appid_version(13857);
    message.mutable_global_stats()->set_pricesheet_version(1680057676); // mikkotodo revisit
    message.mutable_global_stats()->set_twitch_streams_version(2);
    message.mutable_global_stats()->set_active_tournament_eventid(20);
    message.mutable_global_stats()->set_active_survey_id(0);
    message.mutable_global_stats()->set_required_appid_version2(13862); // csgo s2

    message.set_vac_banned(GetConfig().VacBanned());
    message.mutable_commendation()->set_cmd_friendly(GetConfig().CommendedFriendly());
    message.mutable_commendation()->set_cmd_teaching(GetConfig().CommendedTeaching());
    message.mutable_commendation()->set_cmd_leader(GetConfig().CommendedLeader());
    message.set_player_level(GetConfig().Level());
    message.set_player_cur_xp(GetConfig().Xp());
}

void ClientGC::BuildClientWelcome(CMsgClientWelcome &message, const CMsgCStrike15Welcome &csWelcome,
    const CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &matchmakingHello)
{
    // mikkotodo remove dox
    message.set_version(0); // this is accurate
    message.set_game_data(csWelcome.SerializeAsString());
    m_inventory.BuildCacheSubscription(*message.add_outofdate_subscribed_caches(), GetConfig().Level(), false);
    message.mutable_location()->set_latitude(65.0133006f);
    message.mutable_location()->set_longitude(25.4646212f);
    message.mutable_location()->set_country(GetConfig().Country());
    message.set_game_data2(matchmakingHello.SerializeAsString());
    message.set_rtime32_gc_welcome_timestamp(static_cast<uint32_t>(time(nullptr)));
    message.set_currency(GetConfig().Currency());
    message.set_txn_country_code(GetConfig().Country());
}

void ClientGC::SendRankUpdate()
{
    CMsgGCCStrike15_v2_ClientGCRankUpdate message;

    PlayerRankingInfo *rank = message.add_rankings();
    rank->set_account_id(EffectiveAccountId());
    rank->set_rank_id(GetConfig().CompetitiveRank());
    rank->set_wins(GetConfig().CompetitiveWins());
    rank->set_rank_type_id(RankTypeCompetitive);

    rank = message.add_rankings();
    rank->set_account_id(EffectiveAccountId());
    rank->set_rank_id(GetConfig().WingmanRank());
    rank->set_wins(GetConfig().WingmanWins());
    rank->set_rank_type_id(RankTypeWingman);

    rank = message.add_rankings();
    rank->set_account_id(AccountId());
    rank->set_rank_id(GetConfig().DangerZoneRank());
    rank->set_wins(GetConfig().DangerZoneWins());
    rank->set_rank_type_id(RankTypeDangerZone);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientGCRankUpdate, message);
}

void ClientGC::OnClientReportValidation(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientReportValidation message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Failed to parse ClientReportValidation\n");
        return;
    }

    // Локальная лямбда: печатает длинную строку, разбивая её на куски по 252 символа.
    // Префикс повторяется на каждой строке, чтобы понимать, где продолжение.
    auto PrintLongString = [](const char *prefix, std::string_view str)
    {
        constexpr size_t MaxLine = 252;

        if (str.empty())
        {
            Platform::Print("%s<empty>\n", prefix);
            return;
        }

        size_t offset = 0;
        while (offset < str.size())
        {
            size_t chunk = std::min(MaxLine, str.size() - offset);
            Platform::Print("%.*s%.*s\n",
                static_cast<int>(strlen(prefix)), prefix,
                static_cast<int>(chunk), str.data() + offset);
            offset += chunk;
        }
    };

    Platform::Print("[Validation] ============================================\n");
    Platform::Print("[Validation] status_id: %u | total_files: %u | client_version: %u | trust_time: %u\n",
        message.status_id(),
        message.total_files(),
        message.clientreportversion(),
        message.trust_time());

    Platform::Print("[Validation] internal_error: %u | count_pending: %u | count_completed: %u | process_id: %u\n",
        message.internal_error(),
        message.count_pending(),
        message.count_completed(),
        message.process_id());

    Platform::Print("[Validation] osversion: %d | report_count: %u | client_time: %llu\n",
        message.osversion(),
        message.report_count(),
        static_cast<unsigned long long>(message.client_time()));

    Platform::Print("[Validation] diagnostic1: %u | diagnostic2: %llu | diagnostic3: %llu\n",
        message.diagnostic1(),
        static_cast<unsigned long long>(message.diagnostic2()),
        static_cast<unsigned long long>(message.diagnostic3()));

    Platform::Print("[Validation] diagnostic4: %llu | diagnostic5: %llu\n",
        static_cast<unsigned long long>(message.diagnostic4()),
        static_cast<unsigned long long>(message.diagnostic5()));

    if (!message.command_line().empty())
    {
        PrintLongString("[Validation] command_line: ", message.command_line());
    }

    if (!message.last_launch_data().empty())
    {
        PrintLongString("[Validation] last_launch_data: ", message.last_launch_data());
    }

    if (!message.file_report().empty())
    {
        Platform::Print("[Validation] file_report (size=%zu):\n", message.file_report().size());
        PrintLongString("[Validation]   ", message.file_report());
    }

    for (int i = 0; i < message.diagnostics_size(); i++)
    {
        const CVDiagnostic &diag = message.diagnostics(i);
        Platform::Print("[Validation] Diag[%d] id: %u | extended: %u | value: %llu\n",
            i,
            diag.id(),
            diag.extended(),
            static_cast<unsigned long long>(diag.value()));

        if (!diag.string_value().empty())
        {
            PrintLongString("[Validation]   string: ", diag.string_value());
        }
    }

    Platform::Print("[Validation] ============================================\n");
}
void ClientGC::OnGetEventFavorites(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_GetEventFavorites_Request request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::Print("Failed to parse GetEventFavorites_Request\n");
        return;
    }

    // Клиент ждёт ответа, иначе UI турниров/Pick'Em будет висеть или выдаст ошибку.
    // Отправляем пустые JSON-массивы, чтобы клиент не пытался парсить null.
    CMsgGCCStrike15_v2_GetEventFavorites_Response response;
    response.set_all_events(request.all_events());
    response.set_json_favorites("[]");
    response.set_json_featured("[]");

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_GetEventFavorites_Response, response);
    
    Platform::Print("Sent empty GetEventFavorites_Response (all_events: %d)\n", request.all_events());
}

void ClientGC::OnClientHello(GCMessageRead &messageRead)
{
    CMsgClientHello hello;
    if (!messageRead.ReadProtobuf(hello))
    {
        Platform::Print("Parsing CMsgClientHello failed, ignoring\n");
        return;
    }

    // we don't care about anything in this message, just reply
    CMsgCStrike15Welcome csWelcome;
    BuildCSWelcome(csWelcome);

    CMsgGCCStrike15_v2_MatchmakingGC2ClientHello mmHello;
    BuildMatchmakingHello(mmHello);

    CMsgClientWelcome clientWelcome;
    BuildClientWelcome(clientWelcome, csWelcome, mmHello);

    SendMessageToGame(false, k_EMsgGCClientWelcome, clientWelcome);

    // the real gc sends this a bit later when it has more info to put on it
    // however we have everything at our fingertips so send it right away
    // mikkotodo is this even needed? k_EMsgGCClientWelcome should have it all already
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientHello, mmHello);

    // send all ranks here as well, it's a bit back and forth with real gc
    SendRankUpdate();
    
    uint32_t cooldown = GetConfig().CompetitiveCooldownSeconds();
    if (cooldown > 0) {
        SendCompetitiveCooldown();
    }
}

void ClientGC::AdjustItemEquippedState(GCMessageRead &messageRead)
{
    CMsgAdjustItemEquippedState message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgAdjustItemEquippedState failed, ignoring\n");
        return;
    }

    CMsgSOMultipleObjects update;
    if (!m_inventory.EquipItem(message.item_id(), message.new_class(), message.new_slot(), update))
    {
        // no change
        assert(false);
        return;
    }

    // let the gameserver know, too
    SendMessageToGame(true, k_ESOMsg_UpdateMultiple, update);
}

void ClientGC::ClientPlayerDecalSign(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientPlayerDecalSign message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCCStrike15_v2_ClientPlayerDecalSign failed, ignoring\n");
        return;
    }

    if (!Graffiti::SignMessage(*message.mutable_data()))
    {
        Platform::Print("Could not sign graffiti! it won't appear\n");
        return;
    }

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientPlayerDecalSign, message);
}

void ClientGC::UseItemRequest(GCMessageRead &messageRead)
{
    CMsgUseItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgUseItem failed, ignoring\n");
        return;
    }

    uint64_t itemId = message.item_id();

    // Check if it's a gift
    const CSOEconItem *giftItem = m_inventory.GetItem(itemId);
    if (giftItem)
    {
        uint32_t defIndex = giftItem->def_index();
        if (defIndex == 1210 || 
            defIndex == 1211 || 
            defIndex == 1215)
        {
            ProcessGiftUse(itemId);
            return;
        }
    }

    // Normal use handling
    CMsgSOSingleObject destroy;
    CMsgSOMultipleObjects updateMultiple;
    CMsgGCItemCustomizationNotification notification;

    if (m_inventory.UseItem(itemId, destroy, updateMultiple, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        SendMessageToGame(true, k_ESOMsg_UpdateMultiple, updateMultiple);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
}

static void AddressString(uint32_t ip, uint32_t port, char *buffer, size_t bufferSize)
{
    snprintf(buffer, bufferSize,
        "%u.%u.%u.%u:%u\n",
        (ip >> 24) & 0xff,
        (ip >> 16) & 0xff,
        (ip >> 8) & 0xff,
        ip & 0xff,
        port);
}

void ClientGC::ClientRequestJoinServerData(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientRequestJoinServerData request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::Print("Parsing CMsgGCCStrike15_v2_ClientRequestJoinServerData failed, ignoring\n");
        return;
    }

    CMsgGCCStrike15_v2_ClientRequestJoinServerData response = request;

    // If we have sent a reservation, use our fixed server details
    if (m_matchmakingReservationSent) {
        response.mutable_res()->set_serverid(0x12345678);
        response.mutable_res()->set_direct_udp_ip(0xC0A8000E);   // 192.168.0.14
        response.mutable_res()->set_direct_udp_port(27019);
        response.mutable_res()->set_reservationid(m_matchmakingReservationId);
        response.mutable_res()->set_server_address("192.168.0.14:27019");
    } else {
        // Fallback to the original echo behaviour
        response.mutable_res()->set_serverid(request.version());
        response.mutable_res()->set_direct_udp_ip(request.server_ip());
        response.mutable_res()->set_direct_udp_port(request.server_port());
        response.mutable_res()->set_reservationid(GameServerCookieId);
        char addressString[32];
        AddressString(request.server_ip(), request.server_port(), addressString, sizeof(addressString));
        response.mutable_res()->set_server_address(addressString);
    }

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientRequestJoinServerData, response);
}

void ClientGC::SetItemPositions(GCMessageRead &messageRead)
{
    CMsgSetItemPositions message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgSetItemPositions failed, ignoring\n");
        return;
    }

    std::vector<CMsgItemAcknowledged> acknowledgements;
    acknowledgements.reserve(message.item_positions_size());

    CMsgSOMultipleObjects update;
    if (m_inventory.SetItemPositions(message, acknowledgements, update))
    {
        for (const CMsgItemAcknowledged &acknowledgement : acknowledgements)
        {
            // send these to the server only
            GCMessageWrite messageWrite{ k_EMsgGCItemAcknowledged, acknowledgement };
            PostToHost(HostEvent::NetMessage, 0, messageWrite.Data(), messageWrite.Size());
        }

        SendMessageToGame(true, k_ESOMsg_UpdateMultiple, update);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::IncrementKillCountAttribute(GCMessageRead &messageRead)
{
    CMsgIncrementKillCountAttribute message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgIncrementKillCountAttribute failed, ignoring\n");
        return;
    }

    assert(message.event_type() == 0);

    CMsgSOSingleObject update;
    if (m_inventory.IncrementKillCountAttribute(message.item_id(), message.amount(), update))
    {
        SendMessageToGame(true, k_ESOMsg_Update, update);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::ApplySticker(GCMessageRead &messageRead)
{
    CMsgApplySticker message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgApplySticker failed, ignoring\n");
        return;
    }

    assert(!message.item_item_id() != !message.baseitem_defidx());

    CMsgSOSingleObject update, destroy;
    CMsgGCItemCustomizationNotification notification;

    if (!message.sticker_item_id())
    {
        // scrape
        if (m_inventory.ScrapeSticker(message, update, destroy, notification))
        {
            if (destroy.has_type_id())
            {
                // destroying a default item
                SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
            }

            if (update.has_type_id())
            {
                // if the item got removed (handled above), nothing gets updated
                SendMessageToGame(true, k_ESOMsg_Update, update);
            }

            if (notification.has_request())
            {
                // might get a k_EGCItemCustomizationNotification_RemoveSticker
                SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
            }
        }
        else
        {
            assert(false);
        }
    }
    else if (m_inventory.ApplySticker(message, update, destroy, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        SendMessageToGame(true, k_ESOMsg_Update, update);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::StoreGetUserData(GCMessageRead &messageRead)
{
    CMsgStoreGetUserData message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgStoreGetUserData failed, ignoring\n");
        return;
    }

    // If price sheet hasn't been loaded yet, load it now.
    if (m_priceSheet.IsEmpty())
    {
        ReloadPriceSheet();
        if (m_priceSheet.IsEmpty())
        {
            Platform::Print("StoreGetUserData: price sheet is empty, cannot respond\n");
            return;
        }
    }

    std::string binaryString;
    binaryString.reserve(1 << 17);
    m_priceSheet.BinaryWriteToString(binaryString);

    CMsgStoreGetUserDataResponse response;
    response.set_result(1);
    response.set_price_sheet_version(1729); // or store a version from the file
    *response.mutable_price_sheet() = std::move(binaryString);

    SendMessageToGame(false, k_EMsgGCStoreGetUserDataResponse, response);
}

void ClientGC::StorePurchaseInit(GCMessageRead &messageRead)
{
    CMsgGCStorePurchaseInit message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCStorePurchaseInit failed, ignoring\n");
        return;
    }

    // value doesn't matter
    uint64_t transactionId = Random{}.Integer<uint64_t>();

    assert(!m_transactionId);
    m_transactionId = transactionId;
    m_transactionItemIds.reserve(message.line_items_size()); // rough approx

    // inventory update response
    std::vector<CMsgSOSingleObject> inventoryUpdate;

    for (const auto &item : message.line_items())
    {
        for (uint32_t i = 0; i < item.quantity(); i++)
        {
            uint64_t itemId = m_inventory.PurchaseItem(item.item_def_id(), inventoryUpdate);
            if (!itemId)
            {
                assert(false);
            }
            else
            {
                m_transactionItemIds.push_back(itemId);
            }
        }
    }

    char url[128]; // url doesn't matter, but it needs to be set
    snprintf(url, sizeof(url), "https://checkout.steampowered.com/checkout/approvetxn/%llu/?returnurl=steam", transactionId);

    CMsgGCStorePurchaseInitResponse response;
    response.set_result(1); // success
    response.set_txn_id(transactionId);
    response.set_url(url);
    response.mutable_item_ids()->Assign(m_transactionItemIds.begin(), m_transactionItemIds.end());

    SendMessageToGame(false, k_EMsgGCStorePurchaseInitResponse, response, messageRead.JobId());

    // FIXME: why would the server care???
    for (auto &newItem : inventoryUpdate)
    {
        SendMessageToGame(true, k_ESOMsg_Create, newItem);
    }

    // this will run the steam callback
    PostToHost(HostEvent::MicroTransactionResponse, 0, nullptr, 0);
}

void ClientGC::StorePurchaseFinalize(GCMessageRead &messageRead)
{
    CMsgGCStorePurchaseFinalize message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCStorePurchaseFinalize failed, ignoring\n");
        return;
    }

    assert(m_transactionId);

    CMsgGCStorePurchaseFinalizeResponse response;
    response.set_result(1); // success
    response.mutable_item_ids()->Assign(m_transactionItemIds.begin(), m_transactionItemIds.end());
    SendMessageToGame(false, k_EMsgGCStorePurchaseFinalizeResponse, response, messageRead.JobId());

    // done with this one
    m_transactionId = 0;
}

void ClientGC::PartySearch(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_Party_Search message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCCStrike15_v2_Party_Search failed, ignoring\n");
        return;
    }

    CMsgGCCStrike15_v2_Party_SearchResults response;

    // adding self
    CMsgGCCStrike15_v2_Party_SearchResults::Entry *entry = response.add_entries();
    entry->set_id(AccountId());
    entry->set_grp(3);
    entry->set_game_type(message.game_type());
    entry->set_apr(1);
    entry->set_ark(std::rand() % 18 + 1);
    entry->set_loc(30066);
    entry->set_accountid(EffectiveAccountId());

    for (uint32_t player_id : GetConfig().GetFriends())
    {
        // dont make self duplicate
        if (AccountId() == player_id)
            continue;

        entry = response.add_entries();
        entry->set_id(player_id);
        entry->set_grp(3);
        entry->set_game_type(message.game_type());
        entry->set_apr(std::rand() % 40 + 1);
        entry->set_ark(std::rand() % 18 + 1);
        entry->set_loc(30066);
        entry->set_accountid(player_id);
    }

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_Party_Search, response);
}

void ClientGC::RequestCoPlays(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_Account_RequestCoPlays message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCCStrike15_v2_Account_RequestCoPlays failed, ignoring\n");
        return;
    }
    
    // adding self
    CMsgGCCStrike15_v2_Account_RequestCoPlays_Player *player = message.add_players();
    player->set_accountid(EffectiveAccountId());
    player->set_online(true);
    player->set_rtcoplay(1771263169);

    for (uint32_t player_id : GetConfig().GetFriends())
    {
        // dont make self duplicate
        if (AccountId() == player_id)
            continue;

        player = message.add_players();
        player->set_accountid(player_id);
        player->set_online(true);
        player->set_rtcoplay(1771262169);
    }

    message.set_servertime(1771263169);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_Account_RequestCoPlays, message);
}

void ClientGC::ClientRequestPlayersProfile(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientRequestPlayersProfile message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCCStrike15_v2_ClientRequestPlayersProfile failed, ignoring\n");
        return;
    }

    Platform::Print("Requested accountId: %u\n", message.account_id());

    CMsgGCCStrike15_v2_PlayersProfile response;
    response.set_request_id(message.account_id());

    // --- Добавляем базовый профиль для запрошенного аккаунта ---
    CMsgGCCStrike15_v2_MatchmakingGC2ClientHello* mmHello = response.add_account_profiles();
    mmHello->set_account_id(message.account_id());
    mmHello->mutable_commendation()->set_cmd_friendly(GetConfig().CommendedFriendly());
    mmHello->mutable_commendation()->set_cmd_teaching(GetConfig().CommendedTeaching());
    mmHello->mutable_commendation()->set_cmd_leader(GetConfig().CommendedLeader());
    mmHello->set_player_level(GetConfig().Level());
    mmHello->set_player_cur_xp(GetConfig().Xp());

    // --- Получаем список друзей из конфига (уже есть в GCConfig) ---
    std::vector<int> friends = GetConfig().GetFriends();

    // Добавляем запрошенный аккаунт в список для рангов, если его там нет
    bool requestedInFriends = false;
    for (int id : friends) {
        if (static_cast<uint32_t>(id) == message.account_id()) {
            requestedInFriends = true;
            break;
        }
    }
    if (!requestedInFriends) {
        friends.push_back(static_cast<int>(message.account_id()));
    }

    // Примечание: CMsgGCCStrike15_v2_PlayersProfile не содержит поля rankings,
    // поэтому добавление рангов для друзей удалено. Если потребуется,
    // необходимо использовать другое сообщение или расширить протокол.

    // --- Отправляем ответ клиенту ---
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_PlayersProfile, response);
}
void ClientGC::CasketItemLoadContents(GCMessageRead &messageRead)
{
    CMsgCasketItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CasketItemLoadContents::CMsgCasketItem failed, ignoring\n");
        return;
    }

    // wha
    CMsgGCItemCustomizationNotification notification;
    notification.set_request(k_EGCItemCustomizationNotification_CasketContents);
    notification.add_item_id(message.casket_item_id());

    SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
}

void ClientGC::CasketItemAdd(GCMessageRead &messageRead)
{
    CMsgCasketItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CasketItemAdd::CMsgCasketItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject updateItem, updateCasket;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.CasketItemAdd(message.casket_item_id(), message.item_item_id(), updateItem, updateCasket, notification))
    {
        SendMessageToGame(false, k_ESOMsg_Update, updateItem);
        SendMessageToGame(false, k_ESOMsg_Update, updateCasket);
        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
}

void ClientGC::CasketItemExtract(GCMessageRead &messageRead)
{
    CMsgCasketItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CasketItemExtract::CMsgCasketItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject updateItem, updateCasket;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.CasketItemRemove(message.casket_item_id(), message.item_item_id(), updateItem, updateCasket, notification))
    {
        SendMessageToGame(false, k_ESOMsg_Update, updateItem);
        SendMessageToGame(false, k_ESOMsg_Update, updateCasket);
        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
}

void ClientGC::StatTrakSwap(GCMessageRead &messageRead)
{
    CMsgApplyStatTrakSwap message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing StatTrakSwap::CMsgApplyStatTrakSwap failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject destroy, updateItem1, updateItem2;
    CMsgGCItemCustomizationNotification notification;

    // ugh
    if (m_inventory.StatTrakSwap(
            message.tool_item_id(),
            message.item_1_item_id(),
            message.item_2_item_id(),
            destroy,
            updateItem1,
            updateItem2,
            notification))
    {
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        SendMessageToGame(true, k_ESOMsg_Update, updateItem1);
        SendMessageToGame(true, k_ESOMsg_Update, updateItem2);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
}

void ClientGC::DeleteItem(GCMessageRead &messageRead)
{
    // there is data after this, but i don't know what it is
    uint64_t itemId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCDelete failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject destroyed;
    if (m_inventory.RemoveItem(itemId, destroyed))
    {
        // mikkotodo what does the server want to know
        SendMessageToGame(true, k_ESOMsg_Destroy, destroyed);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::UnlockCrate(GCMessageRead &messageRead)
{
    uint64_t keyId = messageRead.ReadUint64();
    uint64_t crateId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCUnlockCrate failed, ignoring\n");
        return;
    }

    Platform::Print("CASE OPENING %llu with %llu\n", crateId, keyId);

    CMsgSOSingleObject destroyCrate, destroyKey, newItem;
    CMsgGCItemCustomizationNotification notification;

    if (m_inventory.UnlockCrate(
            crateId,
            keyId,
            destroyCrate,
            destroyKey,
            newItem,
            notification))
    {
        // mikkotodo what does the server want to know
        SendMessageToGame(true, k_ESOMsg_Destroy, destroyCrate);
        SendMessageToGame(true, k_ESOMsg_Destroy, destroyKey);
        SendMessageToGame(true, k_ESOMsg_Create, newItem);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::NameItem(GCMessageRead &messageRead)
{
    uint64_t nameTagId = messageRead.ReadUint64();
    uint64_t itemId = messageRead.ReadUint64();
    messageRead.ReadData(1); // skip the sentinel
    std::string_view name = messageRead.ReadString();

    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCNameItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject update, destroy;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.NameItem(nameTagId, itemId, name, update, destroy, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Update, update);
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::NameBaseItem(GCMessageRead &messageRead)
{
    uint64_t nameTagId = messageRead.ReadUint64();
    uint32_t defIndex = messageRead.ReadUint32();
    messageRead.ReadData(1); // skip the sentinel
    std::string_view name = messageRead.ReadString();

    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCNameBaseItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject create, destroy;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.NameBaseItem(nameTagId, defIndex, name, create, destroy, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Create, create);
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::RemoveItemName(GCMessageRead &messageRead)
{
    uint64_t itemId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCRemoveItemName failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject update, destroy;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.RemoveItemName(itemId, update, destroy, notification))
    {
        if (update.has_type_id())
        {
            SendMessageToGame(true, k_ESOMsg_Update, update);
        }

        if (destroy.has_type_id())
        {
            SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        }

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::SendInventoryUpdate()
{
    CMsgSOCacheSubscribed message;
    m_inventory.BuildCacheSubscription(message, GetConfig().Level(), false);
    SendMessageToGame(false, k_ESOMsg_CacheSubscribed, message);
}


void ClientGC::CheckFileReloads()
{
    namespace fs = std::filesystem;

    struct WatchedFile {
        fs::path path;
        fs::file_time_type& lastWrite;
        void (ClientGC::*reloadFn)();
    };

    // Static variables to remember the last modification time of each file
    static fs::file_time_type lastInventoryWrite = fs::file_time_type::min();
    static fs::file_time_type lastConfigWrite = fs::file_time_type::min();
    static fs::file_time_type lastPriceSheetWrite = fs::file_time_type::min();
    static fs::file_time_type lastPassesWrite = fs::file_time_type::min();
    static fs::file_time_type lastUnusualLootWrite = fs::file_time_type::min();

    WatchedFile files[] = {
        { "csgo_gc/inventory.txt",         lastInventoryWrite,   &ClientGC::ReloadInventory      },
        { "csgo_gc/config.txt",            lastConfigWrite,      &ClientGC::ReloadConfig         },
        { "csgo_gc/price_sheet.txt",       lastPriceSheetWrite,  &ClientGC::ReloadPriceSheet     },
        { "csgo_gc/passes.txt",            lastPassesWrite,      &ClientGC::ReloadPasses         },
        { "csgo_gc/unusual_loot_lists.txt",lastUnusualLootWrite, &ClientGC::ReloadUnusualLootLists }
    };

    for (auto& file : files) {
        if (!fs::exists(file.path))
            continue;

        auto currentWrite = fs::last_write_time(file.path);
        if (currentWrite != file.lastWrite) {
            file.lastWrite = currentWrite;
            Platform::Print("%s changed – reloading...\n", file.path.filename().string().c_str());
            (this->*file.reloadFn)();
        }
    }
     // --- Matchmaking timer ---
    if (m_isSearching && !m_matchmakingReservationSent) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_matchmakingStartTime).count();
        if (false) {}
    }
    UpdateCooldown();
}

void ClientGC::ReloadInventory()
{
    m_inventory.ReloadFromFile();
    SendInventoryUpdate();
}

void ClientGC::ReloadConfig()
{
    GetConfig().ReloadFromFile();
    SendRankUpdate();
}

void ClientGC::ReloadPriceSheet()
{
    m_priceSheet.Clear();
    if (!m_priceSheet.ParseFromFile("csgo_gc/price_sheet.txt"))
    {
        Platform::Print("ReloadPriceSheet: failed to load price_sheet.txt\n");
        return;
    }
    // Optionally store the version from the file if needed.
    // m_priceSheetVersion = ...; // if you track a version number
    Platform::Print("ReloadPriceSheet: price sheet reloaded\n");
}

void ClientGC::ReloadPasses()
{
    m_passes.Clear();
    if (!m_passes.ParseFromFile("csgo_gc/passes.txt"))
    {
        Platform::Print("ReloadPasses: failed to load passes.txt\n");
        return;
    }
    Platform::Print("ReloadPasses: pass definitions reloaded\n");
}

void ClientGC::ReloadUnusualLootLists()
{
    m_unusualLootLists.Clear();
    if (!m_unusualLootLists.ParseFromFile("csgo_gc/unusual_loot_lists.txt"))
    {
        Platform::Print("ReloadUnusualLootLists: failed to load unusual_loot_lists.txt\n");
        return;
    }
    Platform::Print("ReloadUnusualLootLists: unusual loot lists reloaded\n");
}


void ClientGC::FetchOverwatchCases()
{
    ISteamHTTP *http = SteamHTTP();
    if (!http) return;

    HTTPRequestHandle hRequest = http->CreateHTTPRequest(k_EHTTPMethodGET, "https://sasha190409.github.io/csgo/overwatch");
    if (hRequest == k_uAPICallInvalid) return;

    http->SetHTTPRequestHeaderValue(hRequest, "User-Agent", "csgo_gc/1.0");

    // Send the request – the callback will be triggered via m_httpCallback
    SteamAPICall_t hCall;
    if (!http->SendHTTPRequest(hRequest, &hCall))
        Platform::Print("Overwatch: failed to send request\n");
    // No need to store hCall; the callback system handles it.
}
void ClientGC::OnOverwatchHTTPResponse(HTTPRequestCompleted_t *pCallback)
{
    if (!pCallback->m_bRequestSuccessful || pCallback->m_eStatusCode != k_EHTTPStatusCode200OK)
    {
        Platform::Print("Overwatch: HTTP request failed (status %d, success %d)\n",
                        pCallback->m_eStatusCode, pCallback->m_bRequestSuccessful);
        return;
    }

    ISteamHTTP *http = SteamHTTP();
    if (!http) return;

    uint32_t bodySize;
    if (!http->GetHTTPResponseBodySize(pCallback->m_hRequest, &bodySize) || bodySize == 0)
        return;

    std::vector<uint8_t> body(bodySize + 1, 0);  // use uint8_t
    if (!http->GetHTTPResponseBodyData(pCallback->m_hRequest, body.data(), bodySize))
        return;

    std::string json(reinterpret_cast<char*>(body.data()), bodySize);
    Platform::Print("Overwatch: received JSON: %s\n", json.c_str());


    // Parse JSON: {"case1":"STEAM_0:1:562118442", ...}
    // Simple manual parser for this specific format.
    std::vector<uint32_t> suspects;
    size_t pos = 0;
    // Find first '{'
    size_t start = json.find('{');
    if (start == std::string::npos) return;
    // Find last '}'
    size_t end = json.rfind('}');
    if (end == std::string::npos || end <= start) return;
    std::string content = json.substr(start + 1, end - start - 1);

    // Split by ','
    size_t comma = 0;
    while (comma != std::string::npos)
    {
        size_t next = content.find(',', comma);
        std::string pair = content.substr(comma, (next == std::string::npos) ? std::string::npos : next - comma);
        // Trim spaces
        pair.erase(0, pair.find_first_not_of(" \t\n\r"));
        pair.erase(pair.find_last_not_of(" \t\n\r") + 1);
        if (pair.empty()) break;

        // Find colon
        size_t colon = pair.find(':');
        if (colon == std::string::npos) continue;
        std::string key = pair.substr(0, colon);
        std::string value = pair.substr(colon + 1);
        // Trim quotes
        auto trimQuotes = [](std::string& s) {
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
                s = s.substr(1, s.size() - 2);
        };
        trimQuotes(key);
        trimQuotes(value);
        // key is "caseN", value is "STEAM_0:..."
        if (key.find("case") == 0 && !value.empty())
        {
            uint32_t accId = SteamIDStringToAccountId(value);
            if (accId != 0)
                suspects.push_back(accId);
        }

        comma = (next == std::string::npos) ? std::string::npos : next + 1;
    }

    // Store in member
    {
        std::lock_guard<std::mutex> lock(m_overwatchMutex);
        m_overwatchSuspects = std::move(suspects);
        m_nextOverwatchIndex = 0;
        Platform::Print("Overwatch: loaded %zu suspects\n", m_overwatchSuspects.size());
    }
}
uint32_t ClientGC::SteamIDStringToAccountId(const std::string &str)
{
    unsigned int x, y;
    if (sscanf(str.c_str(), "STEAM_%*u:%u:%u", &x, &y) != 2)
        return 0;
    return y * 2 + x;
}

void ClientGC::SendOverwatchCaseAssignment(uint32_t suspectAccountId)
{
    CMsgGCCStrike15_v2_PlayerOverwatchCaseAssignment assignment;
    assignment.set_caseid(m_nextCaseId++);
    assignment.set_suspectid(suspectAccountId);
    assignment.set_fractionid(0);
    assignment.set_numrounds(8);
    assignment.set_fractionrounds(8);
    assignment.set_streakconvictions(0);
    assignment.set_reason(0);
    assignment.set_verdict(0);
    assignment.set_timestamp(static_cast<uint32_t>(time(nullptr)));
    assignment.set_throttleseconds(0);

    std::string url = "https://sasha190409.github.io/csgo/demos/imposter_"
                    + std::to_string(suspectAccountId) + ".dem";
    assignment.set_caseurl(url);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_PlayerOverwatchCaseAssignment, assignment);

    Platform::Print("Overwatch: assigned case %llu for suspect %u, URL: %s\n",
                    assignment.caseid(), suspectAccountId, url.c_str());
}

void ClientGC::OnOverwatchCaseStatus(GCMessageRead& messageRead)
{
    CMsgGCCStrike15_v2_PlayerOverwatchCaseStatus msg;
    if (!messageRead.ReadProtobuf(msg))
    {
        Platform::Print("Failed to parse OverwatchCaseStatus\n");
        return;
    }

    if (msg.caseid() == 0)   // client requests a new case
    {
        std::lock_guard<std::mutex> lock(m_overwatchMutex);
        if (m_overwatchSuspects.empty())
        {
            Platform::Print("Overwatch: no suspects available\n");
            return;
        }
        uint32_t suspect = m_overwatchSuspects[m_nextOverwatchIndex];
        m_nextOverwatchIndex = (m_nextOverwatchIndex + 1) % m_overwatchSuspects.size();
        SendOverwatchCaseAssignment(suspect);
    }
    else
    {
        Platform::Print("Overwatch: status check for case %llu (status %u)\n",
                        msg.caseid(), msg.statusid());
    }
}

void ClientGC::OnOverwatchCaseUpdate(GCMessageRead& messageRead)
{
    CMsgGCCStrike15_v2_PlayerOverwatchCaseUpdate msg;
    if (!messageRead.ReadProtobuf(msg)) {
        Platform::Print("Failed to parse OverwatchCaseUpdate\n");
        return;
    }

    Platform::Print("Overwatch: verdict for case %llu, suspect %u, reason %u\n",
                    msg.caseid(), msg.suspectid(), msg.reason());

    // Send to Cloudflare Worker
    SendVerdictToCloudflare(msg);
}

void ClientGC::SendVerdictToCloudflare(const CMsgGCCStrike15_v2_PlayerOverwatchCaseUpdate &msg)
{
    ISteamHTTP *http = SteamHTTP();
    if (!http) return;

    std::string json = "{"
        "\"caseid\":" + std::to_string(msg.caseid()) + ","
        "\"suspectid\":" + std::to_string(msg.suspectid()) + ","
        "\"reason\":" + std::to_string(msg.reason()) + ","
        "\"timestamp\":" + std::to_string(time(nullptr)) +
    "}";

    HTTPRequestHandle hRequest = http->CreateHTTPRequest(k_EHTTPMethodPOST, "https://still-dawn-e090.ivanhihlov4.workers.dev/verdict");
    if (hRequest == k_uAPICallInvalid) return;

    http->SetHTTPRequestHeaderValue(hRequest, "Content-Type", "application/json");

    // Convert to non-const uint8_t*
    std::vector<uint8_t> postData(json.begin(), json.end());
    http->SetHTTPRequestRawPostBody(hRequest, "application/json", postData.data(), static_cast<uint32_t>(postData.size()));

    // Send asynchronously – we ignore the callback
    http->SendHTTPRequest(hRequest, nullptr);  // or &someCall if you care
}

uint32_t ClientGC::EffectiveAccountId() const
{
    return AccountId(); // TODO: remove this function (im too lazy)
}
