#include "stdafx.h"
#include "gc_client.h"
#include "graffiti.h"
#include "keyvalue.h"
#include <filesystem>
#include "case_opening.h"
#include "random.h"

ClientGC::ClientGC(uint64_t steamId)
    : m_steamId{ steamId }
    , m_inventory{ m_steamId }
{
    Graffiti::Initialize();
    StartThread();

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
            m_isCooldownActive = false;
        }
    }

    CMsgGCCStrike15_v2_ServerNotificationForUserPenalty penalty;
    penalty.set_account_id(EffectiveAccountId());
    penalty.set_reason(0);
    penalty.set_seconds(seconds);
    penalty.set_communication_cooldown(false);
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ServerNotificationForUserPenalty, penalty);
}

void ClientGC::OnMatchmakingPing(GCMessageRead &messageRead)
{
    SendMatchmakingUpdate();
}

void ClientGC::SendMatchmakingUpdate()
{
    CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate update;
    update.set_matchmaking(m_isSearching ? 1 : 0);

    auto* stats = update.mutable_global_stats();
    stats->set_players_searching(1000);
    stats->set_search_time_avg(60);

    auto* detail = stats->add_search_statistics();
    detail->set_game_type(0);
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

        // 1) Tell the client to stop the search animation.
        SendMatchmakingUpdate();

        // 2) Refresh MatchmakingHello so penalty_seconds/penalty_reason
        //    reflect the just-started cooldown. Without this, the main menu
        //    keeps the Play button enabled and the client never knows the
        //    account is on cooldown.
        SendMatchmakingHelloUpdate();

        // 3) Push the popup notification with the initial cooldown length.
        SendCompetitiveCooldown();

        Platform::Print("Blocked matchmaking: Cooldown active (%u seconds)\n", cooldown);
        return;
    }

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
    reserve.set_serverid(0x12345678);
    reserve.set_direct_udp_ip(0xC0A8000E);
    reserve.set_direct_udp_port(27019);
    reserve.set_reservationid(++m_matchmakingReservationId);
    reserve.set_map("de_dust2");
    reserve.set_server_address("192.168.0.14:27019");
    CMsgGCCStrike15_v2_MatchmakingGC2ServerReserve *sub = reserve.mutable_reservation();
    (void)sub;

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

        case k_EMsgGCAdjustItemEquippedStateMulti:
            AdjustItemEquippedStateMulti(messageRead);
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

        case k_EMsgGCCStrike15_v2_ClientReportValidation:
            OnClientReportValidation(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_GC2ClientInitSystem_Response:
            OnClientInitSystemResponse(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_AccountPrivacySettings:
            OnAccountPrivacySettings(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientRequestSouvenir:
            OnClientRequestSouvenir(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_AcknowledgePenalty:
            OnAcknowledgePenalty(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_SetPlayerLeaderboardSafeName:
            OnSetPlayerLeaderboardSafeName(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientReportPlayer:
            OnClientReportPlayer(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientReportServer:
            OnClientReportServer(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientCommendPlayer:
            OnClientCommendPlayer(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_SetMyActivityInfo:
            OnSetMyActivityInfo(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_GlobalChat:
            OnClientToGCChat(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_GlobalChat_Subscribe:
            OnGlobalChatSubscribe(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_GlobalChat_Unsubscribe:
            OnGlobalChatUnsubscribe(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchListRequestCurrentLiveGames:
            OnMatchListRequestCurrentLiveGames(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchListRequestRecentUserGames:
            OnMatchListRequestRecentUserGames(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchListRequestLiveGameForUser:
            OnMatchListRequestLiveGameForUser(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchListRequestFullGameInfo:
            OnMatchListRequestFullGameInfo(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchListRequestTournamentGames:
            OnMatchListRequestTournamentGames(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_GetEventFavorites_Request:
            OnGetEventFavorites(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Party_Register:
            OnPartyRegister(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Party_Unregister:
            OnPartyUnregister(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Party_Invite:
            OnPartyInvite(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientPartyJoinRelay:
            OnClientPartyJoinRelay(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientPartyWarning:
            OnClientPartyWarning(messageRead);
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

        // Tell the client that the cooldown is over: Hello without
        // penalty_seconds + zero-length notification.
        SendMatchmakingHelloUpdate();
        SendCompetitiveCooldown();

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

    CMsgSOMultipleObjects updateMultiple;
    CMsgSOSingleObject destroy;
    CMsgGCItemCustomizationNotification notification;
    notification.set_request(k_EGCItemCustomizationNotification_Gift);

    CaseOpening caseOpening(m_inventory.GetItemSchema(), m_inventory.GetRandom());

    for (int i = 0; i < numItems; i++)
    {
        CSOEconItem newItemProto;
        if (!caseOpening.SelectItemFromLootList(*lootList, newItemProto))
        {
            Platform::Print("Failed to select item from loot list\n");
            continue;
        }

        CSOEconItem &createdItem = m_inventory.CreateItem(newItemProto);
        m_inventory.AddToMultipleObjects(updateMultiple, SOTypeItem, createdItem);
        notification.add_item_id(createdItem.id());
    }

    if (GetConfig().DestroyUsedItems())
    {
        m_inventory.RemoveItem(giftId, destroy);
    }

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
    message.set_store_item_hash(136617352);
    message.set_timeplayedconsecutively(0);
    message.set_time_first_played(1329845773);
    message.set_last_time_played(1680260376);
    message.set_last_ip_address(MakeAddress(127, 0, 0, 1));
}

void ClientGC::BuildMatchmakingHello(CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &message)
{
    message.set_account_id(EffectiveAccountId());

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
    detail->set_game_type(6);
    detail->set_search_time_avg(60);
    detail->set_players_searching(1000);

    message.mutable_global_stats()->set_main_post_url("");

    message.mutable_global_stats()->set_required_appid_version(13857);
    message.mutable_global_stats()->set_pricesheet_version(1680057676);
    message.mutable_global_stats()->set_twitch_streams_version(2);
    message.mutable_global_stats()->set_active_tournament_eventid(20);
    message.mutable_global_stats()->set_active_survey_id(0);
    message.mutable_global_stats()->set_required_appid_version2(13862);

    message.set_vac_banned(GetConfig().VacBanned());
    message.mutable_commendation()->set_cmd_friendly(GetConfig().CommendedFriendly());
    message.mutable_commendation()->set_cmd_teaching(GetConfig().CommendedTeaching());
    message.mutable_commendation()->set_cmd_leader(GetConfig().CommendedLeader());
    message.set_player_level(GetConfig().Level());
    message.set_player_cur_xp(GetConfig().Xp());

    // Cooldown state. This is what actually makes the client disable the
    // Play button and show the "cooldown" UI. The ServerNotificationForUserPenalty
    // only fires the initial popup; the persistent state lives here.
    if (m_isCooldownActive)
    {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            m_cooldownEndTime - std::chrono::steady_clock::now()).count();
        if (remaining > 0)
        {
            message.set_penalty_seconds(static_cast<uint32_t>(remaining));
            // reason 6 = abandon (as seen in the wild). Change here if a
            // different cooldown reason is needed.
            message.set_penalty_reason(6);
        }
    }
    if (GetConfig().HasPrime() && GetConfig().CompetitiveRank() != RankNone)
    {
        PlayerRankingInfo *ranking = message.mutable_ranking();
        ranking->set_account_id(EffectiveAccountId());
        ranking->set_rank_id(GetConfig().CompetitiveRank());
        ranking->set_wins(GetConfig().CompetitiveWins());
        ranking->set_rank_type_id(RankTypeCompetitive);
    }
}

void ClientGC::BuildClientWelcome(CMsgClientWelcome &message, const CMsgCStrike15Welcome &csWelcome,
    const CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &matchmakingHello)
{
    message.set_version(0);
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

// ============================================================================
// #9 Secure mode / validation
// ============================================================================

void ClientGC::SendInitSystem()
{
    CMsgGCCStrike15_v2_GC2ClientInitSystem init;
    init.set_load(false);
    init.set_name("");
    init.set_outputname("");
    init.set_cookie(0);
    init.set_manifest("");
    init.set_load_system(false);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_GC2ClientInitSystem, init);
}

void ClientGC::OnClientInitSystemResponse(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_GC2ClientInitSystem_Response message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing GC2ClientInitSystem_Response failed, ignoring\n");
        return;
    }

    Platform::Print("[SecureMode] 👍👍👍👍👍 response=%d einit_result=%d err1=%d err2=%d\n",
        message.response(),
        message.einit_result(),
        message.error_code1(),
        message.error_code2());
}

// ============================================================================
// #10 Privacy settings
// ============================================================================

void ClientGC::OnAccountPrivacySettings(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_AccountPrivacySettings message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing AccountPrivacySettings failed, ignoring\n");
        return;
    }

    m_privacySettings.clear();
    for (int i = 0; i < message.settings_size(); i++)
    {
        const auto &setting = message.settings(i);
        m_privacySettings[setting.setting_type()] = setting.setting_value();
        Platform::Print("Privacy setting %u = %u\n",
            setting.setting_type(), setting.setting_value());
    }

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_AccountPrivacySettings, message);
}

// ============================================================================
// #13 Souvenir
// ============================================================================

void ClientGC::OnClientRequestSouvenir(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientRequestSouvenir message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing ClientRequestSouvenir failed, ignoring\n");
        return;
    }

    Platform::Print("ClientRequestSouvenir: itemid=%llu matchid=%llu eventid=%d\n",
        static_cast<unsigned long long>(message.itemid()),
        static_cast<unsigned long long>(message.matchid()),
        message.eventid());

    CMsgGCCStrike15_v2_MatchEndRewardDropsNotification notification;
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchEndRewardDropsNotification, notification);
}

// ============================================================================
// #14 Multi-equip
// ============================================================================

void ClientGC::AdjustItemEquippedStateMulti(GCMessageRead &messageRead)
{
    CMsgAdjustItemEquippedStateMulti message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgAdjustItemEquippedStateMulti failed, ignoring\n");
        return;
    }

    Platform::Print("AdjustItemEquippedStateMulti: T=%d CT=%d noteam=%d\n",
        message.t_equips_size(),
        message.ct_equips_size(),
        message.noteam_equips_size());

    CMsgSOMultipleObjects update;

    for (int i = 0; i < message.t_equips_size(); i++)
    {
        uint64_t itemId = message.t_equips(i);
        const CSOEconItem *item = m_inventory.GetItem(itemId);
        if (!item)
        {
            Platform::Print("AdjustItemEquippedStateMulti: unknown T item %llu\n",
                static_cast<unsigned long long>(itemId));
            continue;
        }
        uint32_t slot = 0;
        for (const auto &eq : item->equipped_state())
        {
            if (eq.new_class() == 2) { slot = eq.new_slot(); break; }
        }
        if (slot == 0) continue;
        m_inventory.EquipItem(itemId, 2, slot, update);
    }

    for (int i = 0; i < message.ct_equips_size(); i++)
    {
        uint64_t itemId = message.ct_equips(i);
        const CSOEconItem *item = m_inventory.GetItem(itemId);
        if (!item)
        {
            Platform::Print("AdjustItemEquippedStateMulti: unknown CT item %llu\n",
                static_cast<unsigned long long>(itemId));
            continue;
        }
        uint32_t slot = 0;
        for (const auto &eq : item->equipped_state())
        {
            if (eq.new_class() == 3) { slot = eq.new_slot(); break; }
        }
        if (slot == 0) continue;
        m_inventory.EquipItem(itemId, 3, slot, update);
    }

    for (int i = 0; i < message.noteam_equips_size(); i++)
    {
        uint64_t itemId = message.noteam_equips(i);
        const CSOEconItem *item = m_inventory.GetItem(itemId);
        if (!item)
        {
            Platform::Print("AdjustItemEquippedStateMulti: unknown noteam item %llu\n",
                static_cast<unsigned long long>(itemId));
            continue;
        }
        uint32_t slot = 0;
        for (const auto &eq : item->equipped_state())
        {
            if (eq.new_class() == 0) { slot = eq.new_slot(); break; }
        }
        if (slot == 0) continue;
        m_inventory.EquipItem(itemId, 0, slot, update);
    }

    if (update.objects_modified_size() > 0)
    {
        SendMessageToGame(true, k_ESOMsg_UpdateMultiple, update);
    }
}

// ============================================================================
// AcknowledgePenalty
// ============================================================================

void ClientGC::OnAcknowledgePenalty(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_AcknowledgePenalty message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing AcknowledgePenalty failed, ignoring\n");
        return;
    }

    Platform::Print("AcknowledgePenalty: acknowledged=%d\n", message.acknowledged());
}

// ============================================================================
// SetPlayerLeaderboardSafeName
// ============================================================================

void ClientGC::OnSetPlayerLeaderboardSafeName(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_SetPlayerLeaderboardSafeName message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing SetPlayerLeaderboardSafeName failed, ignoring\n");
        return;
    }

    m_leaderboardSafeName = message.leaderboard_safe_name();
    Platform::Print("SetPlayerLeaderboardSafeName: %s\n", m_leaderboardSafeName.c_str());
}

// ============================================================================
// Reports / Commend
// ============================================================================

static uint64_t MakeReportConfirmationId()
{
    return Random{}.Integer<uint64_t>();
}

void ClientGC::OnClientReportPlayer(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientReportPlayer message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing ClientReportPlayer failed, ignoring\n");
        return;
    }

    Platform::Print("ClientReportPlayer: account=%u aimbot=%u wallhack=%u speedhack=%u "
                    "teamharm=%u textabuse=%u voiceabuse=%u match=%llu demo=%d\n",
        message.account_id(),
        message.rpt_aimbot(), message.rpt_wallhack(), message.rpt_speedhack(),
        message.rpt_teamharm(), message.rpt_textabuse(), message.rpt_voiceabuse(),
        static_cast<unsigned long long>(message.match_id()),
        message.report_from_demo());

    CMsgGCCStrike15_v2_ClientReportResponse response;
    response.set_confirmation_id(MakeReportConfirmationId());
    response.set_account_id(EffectiveAccountId());
    response.set_response_type(0);
    response.set_response_result(0);
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientReportResponse, response);
}

void ClientGC::OnClientReportServer(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientReportServer message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing ClientReportServer failed, ignoring\n");
        return;
    }

    Platform::Print("ClientReportServer: poorperf=%u abusivemodels=%u badmotd=%u "
                    "listingabuse=%u inventoryabuse=%u match=%llu\n",
        message.rpt_poorperf(),
        message.rpt_abusivemodels(),
        message.rpt_badmotd(),
        message.rpt_listingabuse(),
        message.rpt_inventoryabuse(),
        static_cast<unsigned long long>(message.match_id()));

    CMsgGCCStrike15_v2_ClientReportResponse response;
    response.set_confirmation_id(MakeReportConfirmationId());
    response.set_account_id(EffectiveAccountId());
    response.set_response_type(1);
    response.set_response_result(0);
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientReportResponse, response);
}

void ClientGC::OnClientCommendPlayer(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientCommendPlayer message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing ClientCommendPlayer failed, ignoring\n");
        return;
    }

    Platform::Print("ClientCommendPlayer: account=%u match=%llu tokens=%u "
                    "friendly=%u teaching=%u leader=%u\n",
        message.account_id(),
        static_cast<unsigned long long>(message.match_id()),
        message.tokens(),
        message.commendation().cmd_friendly(),
        message.commendation().cmd_teaching(),
        message.commendation().cmd_leader());
}

// ============================================================================
// SetMyActivityInfo
// ============================================================================

void ClientGC::OnSetMyActivityInfo(GCMessageRead &messageRead)
{
    (void)messageRead;
    Platform::Print("ClientGC: received SetMyActivityInfo (unparsed)\n");
}

// ============================================================================
// Global chat
// ============================================================================

void ClientGC::OnGlobalChatSubscribe(GCMessageRead &messageRead)
{
    (void)messageRead;
    Platform::Print("GlobalChat: subscribe\n");
}

void ClientGC::OnGlobalChatUnsubscribe(GCMessageRead &messageRead)
{
    (void)messageRead;
    Platform::Print("GlobalChat: unsubscribe\n");
}

void ClientGC::OnClientToGCChat(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientToGCChat message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing ClientToGCChat failed, ignoring\n");
        return;
    }

    Platform::Print("GlobalChat: %s\n", message.text().c_str());

    CMsgGCCStrike15_v2_GCToClientChat response;
    response.set_account_id(EffectiveAccountId());
    response.set_text(message.text());
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_GlobalChat, response);
}

// ============================================================================
// MatchList / Watch — empty responses
// ============================================================================

void ClientGC::SendEmptyMatchList(uint32_t requestId, uint32_t accountId)
{
    CMsgGCCStrike15_v2_MatchList message;
    message.set_msgrequestid(requestId);
    message.set_accountid(accountId);
    message.set_servertime(static_cast<uint32_t>(time(nullptr)));
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchList, message);
}

void ClientGC::OnMatchListRequestCurrentLiveGames(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchListRequestCurrentLiveGames request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::Print("Parsing MatchListRequestCurrentLiveGames failed, ignoring\n");
        return;
    }

    Platform::Print("MatchList: request current live games\n");
    SendEmptyMatchList(0, EffectiveAccountId());
}

void ClientGC::OnMatchListRequestRecentUserGames(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchListRequestRecentUserGames request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::Print("Parsing MatchListRequestRecentUserGames failed, ignoring\n");
        return;
    }

    Platform::Print("MatchList: recent user games for %u\n", request.accountid());
    SendEmptyMatchList(0, request.accountid());
}

void ClientGC::OnMatchListRequestLiveGameForUser(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchListRequestLiveGameForUser request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::Print("Parsing MatchListRequestLiveGameForUser failed, ignoring\n");
        return;
    }

    Platform::Print("MatchList: live game for %u\n", request.accountid());
    SendEmptyMatchList(0, request.accountid());
}

void ClientGC::OnMatchListRequestFullGameInfo(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchListRequestFullGameInfo request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::Print("Parsing MatchListRequestFullGameInfo failed, ignoring\n");
        return;
    }

    Platform::Print("MatchList: full game info for match=%llu\n",
        static_cast<unsigned long long>(request.matchid()));
    SendEmptyMatchList(0, EffectiveAccountId());
}

void ClientGC::OnMatchListRequestTournamentGames(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchListRequestTournamentGames request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::Print("Parsing MatchListRequestTournamentGames failed, ignoring\n");
        return;
    }

    Platform::Print("MatchList: tournament games for event=%d\n", request.eventid());
    SendEmptyMatchList(0, EffectiveAccountId());
}

// ============================================================================
// Existing handlers
// ============================================================================

void ClientGC::OnClientReportValidation(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientReportValidation message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Failed to parse ClientReportValidation\n");
        return;
    }

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

    // If config has "error", reply with ClientLogonFatalError and stop.
    const std::string &err = GetConfig().Error();
    if (!err.empty() && !m_fatalErrorSent)
    {
        m_fatalErrorSent = true;

        CMsgGCCStrike15_v2_ClientLogonFatalError logonError;
        logonError.set_errorcode(1);
        logonError.set_message(err);
        logonError.set_country(GetConfig().Country());

        SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientLogonFatalError, logonError);
        Platform::Print("Sent ClientLogonFatalError: %s\n", err.c_str());
        return;
    }

    CMsgCStrike15Welcome csWelcome;
    BuildCSWelcome(csWelcome);

    CMsgGCCStrike15_v2_MatchmakingGC2ClientHello mmHello;
    BuildMatchmakingHello(mmHello);

    CMsgClientWelcome clientWelcome;
    BuildClientWelcome(clientWelcome, csWelcome, mmHello);

    SendMessageToGame(false, k_EMsgGCClientWelcome, clientWelcome);
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientHello, mmHello);

    SendRankUpdate();

    SendInitSystem();
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
        assert(false);
        return;
    }

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

    if (m_matchmakingReservationSent) {
        response.mutable_res()->set_serverid(0x12345678);
        response.mutable_res()->set_direct_udp_ip(0xC0A8000E);
        response.mutable_res()->set_direct_udp_port(27019);
        response.mutable_res()->set_reservationid(m_matchmakingReservationId);
        response.mutable_res()->set_server_address("192.168.0.14:27019");
    } else {
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
        if (m_inventory.ScrapeSticker(message, update, destroy, notification))
        {
            if (destroy.has_type_id())
            {
                SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
            }

            if (update.has_type_id())
            {
                SendMessageToGame(true, k_ESOMsg_Update, update);
            }

            if (notification.has_request())
            {
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
    response.set_price_sheet_version(1729);
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

    uint64_t transactionId = Random{}.Integer<uint64_t>();

    assert(!m_transactionId);
    m_transactionId = transactionId;
    m_transactionItemIds.reserve(message.line_items_size());

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

    char url[128];
    snprintf(url, sizeof(url), "https://checkout.steampowered.com/checkout/approvetxn/%llu/?returnurl=steam", transactionId);

    CMsgGCStorePurchaseInitResponse response;
    response.set_result(1);
    response.set_txn_id(transactionId);
    response.set_url(url);
    response.mutable_item_ids()->Assign(m_transactionItemIds.begin(), m_transactionItemIds.end());

    SendMessageToGame(false, k_EMsgGCStorePurchaseInitResponse, response, messageRead.JobId());

    for (auto &newItem : inventoryUpdate)
    {
        SendMessageToGame(true, k_ESOMsg_Create, newItem);
    }

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
    response.set_result(1);
    response.mutable_item_ids()->Assign(m_transactionItemIds.begin(), m_transactionItemIds.end());
    SendMessageToGame(false, k_EMsgGCStorePurchaseFinalizeResponse, response, messageRead.JobId());

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

    const bool     prime    = GetConfig().HasPrime();
    const uint32_t rank     = static_cast<uint32_t>(GetConfig().CompetitiveRank());
    const uint32_t gameType = message.game_type();
    const uint32_t launcher = message.launcher();

    // Self lobby id: use the id assigned by our own Party_Register, or
    // fall back to the account id if we never registered one.
    const uint32_t selfLobbyId = (m_partyLobby.active && m_partyLobby.lobbyId)
        ? m_partyLobby.lobbyId
        : AccountId();

    // ------------------------------------------------------------------
    // 1. Register a lobby session for every entry we're about to send.
    //    PartyBrowser's GetPartyMemberXuid / GetPartySessionSetting only
    //    consult the engine's lobby-session registry, which is populated
    //    exclusively by Party_Register (9189). SearchResults alone is not
    //    enough - the client will show an empty leader tile and request
    //    a profile for account 0.
    // ------------------------------------------------------------------
    auto registerLobby = [&](uint32_t lobbyId, uint32_t leaderAccountId)
    {
        CMsgGCCStrike15_v2_Party_Register reg;
        reg.set_id(lobbyId);
        reg.set_ver(1);
        reg.set_apr(prime ? 1 : 0);
        reg.set_ark(rank);
        reg.set_nby(0);
        reg.set_grp(3);
        reg.set_slots(5);
        reg.set_launcher(launcher);
        reg.set_game_type(gameType);
        SendMessageToGame(false, k_EMsgGCCStrike15_v2_Party_Register, reg);

        Platform::Print("PartySearch: registered lobby=%u leader=%u (apr=%u ark=%u)\n",
            lobbyId, leaderAccountId, prime ? 1u : 0u, rank);
    };

    // registerLobby(selfLobbyId, EffectiveAccountId());

    for (uint32_t friendId : GetConfig().GetFriends())
    {
        if (friendId == AccountId())
            continue;
        registerLobby(friendId, friendId);
    }

    // ------------------------------------------------------------------
    // 2. Emit the search results that reference the same lobby ids.
    // ------------------------------------------------------------------
    CMsgGCCStrike15_v2_Party_SearchResults response;

    auto addEntry = [&](uint32_t lobbyId, uint32_t accountId)
    {
        CMsgGCCStrike15_v2_Party_SearchResults::Entry *entry = response.add_entries();
        entry->set_id(lobbyId);
        entry->set_grp(3);
        entry->set_game_type(gameType);
        entry->set_apr(prime ? 1 : 0);
        entry->set_ark(rank);
        entry->set_loc(30066);
        entry->set_accountid(accountId);
    };

    // addEntry(selfLobbyId, EffectiveAccountId());

    for (uint32_t friendId : GetConfig().GetFriends())
    {
        if (friendId == AccountId())
            continue;
        addEntry(friendId, friendId);
    }

    Platform::Print("PartySearch: sending %d entries (self lobby=%u, game_type=%u)\n",
        response.entries_size(), selfLobbyId, gameType);

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

    CMsgGCCStrike15_v2_Account_RequestCoPlays_Player *player = message.add_players();
    player->set_accountid(EffectiveAccountId());
    player->set_online(true);
    player->set_rtcoplay(1771263169);

    for (uint32_t player_id : GetConfig().GetFriends())
    {
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

    CMsgGCCStrike15_v2_MatchmakingGC2ClientHello* mmHello = response.add_account_profiles();
    mmHello->set_account_id(message.account_id());
    mmHello->mutable_commendation()->set_cmd_friendly(GetConfig().CommendedFriendly());
    mmHello->mutable_commendation()->set_cmd_teaching(GetConfig().CommendedTeaching());
    mmHello->mutable_commendation()->set_cmd_leader(GetConfig().CommendedLeader());
    mmHello->set_player_level(GetConfig().Level());
    mmHello->set_player_cur_xp(GetConfig().Xp());

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
    uint64_t itemId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::Print("DeleteItem: parsing CMsgGCDelete failed, ignoring\n");
        return;
    }

    Platform::Print("DeleteItem: requested for item %llu\n",
        static_cast<unsigned long long>(itemId));

    CMsgSOSingleObject destroyed;
    if (!m_inventory.RemoveItem(itemId, destroyed))
    {
        Platform::Print("DeleteItem: item %llu not found in inventory\n",
            static_cast<unsigned long long>(itemId));
        return;
    }

    SendMessageToGame(true, k_ESOMsg_Destroy, destroyed);
    SendInventoryUpdate();

    Platform::Print("DeleteItem: removed %llu, sent destroy + resync\n",
        static_cast<unsigned long long>(itemId));
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
    messageRead.ReadData(1);
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
    messageRead.ReadData(1);
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
    SendMatchmakingHelloUpdate();
}

void ClientGC::ReloadPriceSheet()
{
    m_priceSheet.Clear();
    if (!m_priceSheet.ParseFromFile("csgo_gc/price_sheet.txt"))
    {
        Platform::Print("ReloadPriceSheet: failed to load price_sheet.txt\n");
        return;
    }
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

uint32_t ClientGC::EffectiveAccountId() const
{
    return AccountId();
}

void ClientGC::OnPartyRegister(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_Party_Register message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Failed to parse Party_Register\n");
        return;
    }

    // Клиент шлёт Party_Register с id=0 при создании лобби.
    // Нам нужно присвоить ненулевой id и вернуть эхо обратно.
    uint32_t lobbyId = message.id();
    if (lobbyId == 0)
    {
        // Генерируем случайный id лобби. Нельзя 0 — клиент считает это
        // «не зарегистрирован».
        do { lobbyId = Random{}.Integer<uint32_t>(); } while (lobbyId == 0);
    }

    m_partyLobby.lobbyId    = lobbyId;
    m_partyLobby.hostAccountId = EffectiveAccountId();
    m_partyLobby.gameType   = message.game_type();
    m_partyLobby.ver        = message.ver();
    m_partyLobby.launcher   = message.launcher();
    m_partyLobby.active     = true;

    // Отправляем клиенту эхо с валидным id и минимальными настройками.
    CMsgGCCStrike15_v2_Party_Register response;
    response.set_id(lobbyId);
    response.set_ver(message.ver());
    response.set_apr(GetConfig().HasPrime() ? 1 : 0);
    response.set_ark(static_cast<uint32_t>(GetConfig().CompetitiveRank()) * 10);
    response.set_grp(3);
    response.set_slots(5);                 // максимум для MM
    response.set_launcher(message.launcher());
    response.set_game_type(message.game_type());

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_Party_Register, response);

    Platform::Print("Party_Register: assigned lobby=%u host=%u game_type=%u\n",
        lobbyId, m_partyLobby.hostAccountId, m_partyLobby.gameType);
}

void ClientGC::OnPartyUnregister(GCMessageRead &messageRead)
{
    Platform::Print("Party_Unregister: lobby=%u\n", m_partyLobby.lobbyId);
    m_partyLobby = PartyLobby{};
}

void ClientGC::OnPartyInvite(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_Party_Invite message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Failed to parse Party_Invite\n");
        return;
    }

    uint32_t lobbyId = message.lobbyid();
    PostToHost(HostEvent::PartyInvite, message.accountid(), &lobbyId, sizeof(lobbyId));

    Platform::Print("Party_Invite → account %u (lobby %u)\n",
        message.accountid(), message.lobbyid());
}

void ClientGC::OnClientPartyJoinRelay(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientPartyJoinRelay message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Failed to parse ClientPartyJoinRelay\n");
        return;
    }

    uint32_t lobbyId = static_cast<uint32_t>(message.lobbyid());
    PostToHost(HostEvent::PartyJoinRelay, message.accountid(), &lobbyId, sizeof(lobbyId));

    Platform::Print("ClientPartyJoinRelay → host account %u\n", message.accountid());
}

void ClientGC::OnClientPartyWarning(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientPartyWarning message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Failed to parse ClientPartyWarning\n");
        return;
    }

    for (int i = 0; i < message.entries_size(); i++)
    {
        const auto &e = message.entries(i);
        Platform::Print("PartyWarning: account %u warntype %u\n", e.accountid(), e.warntype());
    }
}

void ClientGC::OnRemotePartyInvite(uint32_t fromAccountId, uint32_t lobbyId, uint32_t gameType)
{
    CMsgInvitationCreated notification;
    notification.set_group_id(lobbyId);
    notification.set_steam_id(CSteamID(fromAccountId, k_EUniversePublic, k_EAccountTypeIndividual).ConvertToUint64());

    SendMessageToGame(false, k_EMsgGCInvitationCreated, notification);

    Platform::Print("Remote invite from %u to lobby %u (game_type %u)\n",
        fromAccountId, lobbyId, gameType);
}

void ClientGC::OnRemoteJoinRelay(uint32_t fromAccountId, uint32_t lobbyId)
{
    if (!m_partyLobby.active || m_partyLobby.lobbyId != lobbyId)
    {
        Platform::Print("Remote join relay for unknown lobby %u (mine %u)\n",
            lobbyId, m_partyLobby.lobbyId);
        return;
    }

    if (std::find(m_partyLobby.memberAccountIds.begin(),
                  m_partyLobby.memberAccountIds.end(),
                  fromAccountId) == m_partyLobby.memberAccountIds.end())
    {
        m_partyLobby.memberAccountIds.push_back(fromAccountId);
        Platform::Print("Added member %u to lobby %u\n", fromAccountId, lobbyId);
    }

    std::vector<uint32_t> members = m_partyLobby.memberAccountIds;
    for (uint32_t member : members)
    {
        if (member == EffectiveAccountId()) continue;
        PostToHost(HostEvent::PartyLobbyUpdate, member, members.data(), members.size() * sizeof(uint32_t));
    }
}

void ClientGC::OnRemoteLobbyUpdate(uint32_t /*fromAccountId*/, uint32_t lobbyId,
    const std::vector<uint32_t> &members)
{
    if (!m_partyLobby.active || m_partyLobby.lobbyId != lobbyId)
    {
        return;
    }
    m_partyLobby.memberAccountIds = members;
    Platform::Print("Lobby %u updated: %zu members\n", lobbyId, members.size());
}
