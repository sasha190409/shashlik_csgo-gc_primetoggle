#pragma once

#include "config.h"
#include "gc_shared.h"
#include "inventory.h"
#include "keyvalue.h"

#include <steam/steam_api_common.h>

struct PartyLobby
{
    uint32_t lobbyId{};
    uint32_t hostAccountId{};
    uint32_t gameType{};
    uint32_t ver{};
    uint32_t launcher{};
    std::vector<uint32_t> memberAccountIds;
    bool active{ false };
};

class ClientGC final : public SharedGC
{
public:
    ClientGC(uint64_t steamId);
    ~ClientGC();
    void CheckFileReloads();

    uint64_t GetSteamId() const { return m_steamId; }
    uint32_t GetAccountId() const { return m_steamId & 0xffffffff; }

    // P2P party entry points (called from NetworkingParty)
    void OnRemotePartyInvite(uint32_t fromAccountId, uint32_t lobbyId, uint32_t gameType);
    void OnRemoteJoinRelay(uint32_t fromAccountId, uint32_t lobbyId);
    void OnRemoteLobbyUpdate(uint32_t fromAccountId, uint32_t lobbyId,
        const std::vector<uint32_t> &members);

    const PartyLobby &GetPartyLobby() const { return m_partyLobby; }

    // convenience accessor for the stored privacy settings
    const std::unordered_map<uint32_t, uint32_t> &GetPrivacySettings() const { return m_privacySettings; }

private:
    KeyValue m_priceSheet;
    KeyValue m_passes;
    KeyValue m_unusualLootLists;

    void HandleEvent(GCEvent type, uint64_t id, const std::vector<uint8_t> &buffer) override;
    bool m_isSearching{ false };

    void HandleMessage(uint32_t type, const void *data, uint32_t size);
    void HandleNetMessage(const void *data, uint32_t size);
    void HandleSOCacheRequest();

    void SendMessageToGame(bool sendToGameServer, uint32_t type,
        const google::protobuf::MessageLite &message, uint64_t jobId = JobIdInvalid);

    void OnClientHello(GCMessageRead &messageRead);
    void AdjustItemEquippedState(GCMessageRead &messageRead);
    void AdjustItemEquippedStateMulti(GCMessageRead &messageRead);
    void ClientPlayerDecalSign(GCMessageRead &messageRead);
    void UseItemRequest(GCMessageRead &messageRead);
    void ClientRequestJoinServerData(GCMessageRead &messageRead);
    void SetItemPositions(GCMessageRead &messageRead);
    void IncrementKillCountAttribute(GCMessageRead &messageRead);
    void ApplySticker(GCMessageRead &messageRead);
    void StoreGetUserData(GCMessageRead &messageRead);
    void StorePurchaseInit(GCMessageRead &messageRead);
    void StorePurchaseFinalize(GCMessageRead &messageRead);
    void PartySearch(GCMessageRead &messageRead);
    void RequestCoPlays(GCMessageRead &messageRead);
    void ClientRequestPlayersProfile(GCMessageRead &messageRead);
    void CasketItemLoadContents(GCMessageRead &messageRead);
    void CasketItemAdd(GCMessageRead &messageRead);
    void CasketItemExtract(GCMessageRead &messageRead);
    void StatTrakSwap(GCMessageRead &messageRead);

    void DeleteItem(GCMessageRead &messageRead);
    void UnlockCrate(GCMessageRead &messageRead);
    void NameItem(GCMessageRead &messageRead);
    void NameBaseItem(GCMessageRead &messageRead);
    void RemoveItemName(GCMessageRead &messageRead);

    // Secure mode / validation
    void OnClientInitSystemResponse(GCMessageRead &messageRead);
    void SendInitSystem();

    // Privacy
    void OnAccountPrivacySettings(GCMessageRead &messageRead);

    // Souvenir
    void OnClientRequestSouvenir(GCMessageRead &messageRead);

    // NEW
    void OnAcknowledgePenalty(GCMessageRead &messageRead);
    void OnSetPlayerLeaderboardSafeName(GCMessageRead &messageRead);
    void OnClientReportPlayer(GCMessageRead &messageRead);
    void OnClientReportServer(GCMessageRead &messageRead);
    void OnClientCommendPlayer(GCMessageRead &messageRead);
    void OnSetMyActivityInfo(GCMessageRead &messageRead);
    void OnGlobalChatSubscribe(GCMessageRead &messageRead);
    void OnGlobalChatUnsubscribe(GCMessageRead &messageRead);
    void OnClientToGCChat(GCMessageRead &messageRead);

    // MatchList / Watch (empty responses)
    void OnMatchListRequestCurrentLiveGames(GCMessageRead &messageRead);
    void OnMatchListRequestRecentUserGames(GCMessageRead &messageRead);
    void OnMatchListRequestLiveGameForUser(GCMessageRead &messageRead);
    void OnMatchListRequestFullGameInfo(GCMessageRead &messageRead);
    void OnMatchListRequestTournamentGames(GCMessageRead &messageRead);

    void SendEmptyMatchList(uint32_t requestId, uint32_t accountId);

    void BuildMatchmakingHello(CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &message);
    void BuildClientWelcome(CMsgClientWelcome &message, const CMsgCStrike15Welcome &csWelcome,
        const CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &matchmakingHello);
    void SendRankUpdate();
    void OnMatchmakingPing(GCMessageRead &messageRead);
    void OnMatchmakingStart(GCMessageRead &messageRead);
    void OnMatchmakingStop(GCMessageRead &messageRead);
    void SendMatchmakingUpdate();

    const uint64_t m_steamId;
    void ProcessGiftUse(uint64_t giftId);
    Inventory m_inventory;

    // microtransactions, we only have one going at a time
    uint64_t m_transactionId{};
    std::vector<uint64_t> m_transactionItemIds;

    void SendInventoryUpdate();
    void ReloadInventory();
    void ReloadConfig();
    void ReloadPriceSheet();
    void ReloadPasses();
    void ReloadUnusualLootLists();

    // Account privacy settings, indexed by setting_type
    std::unordered_map<uint32_t, uint32_t> m_privacySettings;

    // Fatal error handling: if the config has "error", we send
    // ClientLogonFatalError and stop. This flag prevents duplicate sends.
    bool m_fatalErrorSent{ false };

    // set by SetPlayerLeaderboardSafeName
    std::string m_leaderboardSafeName;

    void SendMatchmakingHelloUpdate();
    uint32_t AccountId() const { return m_steamId & 0xffffffff; }
    uint32_t EffectiveAccountId() const;
    std::chrono::steady_clock::time_point m_matchmakingStartTime;
    bool m_matchmakingReservationSent = false;
    uint64_t m_matchmakingReservationId = 0;
    void SendMatchmakingReservation();
    bool m_isCooldownActive{ false };
    std::chrono::steady_clock::time_point m_cooldownEndTime;
    void SendCompetitiveCooldown();
    void UpdateCooldown();

    // ===== Party =====
    PartyLobby m_partyLobby;

    void OnPartyRegister(GCMessageRead &messageRead);
    void OnPartyUnregister(GCMessageRead &messageRead);
    void OnPartyInvite(GCMessageRead &messageRead);
    void OnClientPartyJoinRelay(GCMessageRead &messageRead);
    void OnClientPartyWarning(GCMessageRead &messageRead);

    void OnClientReportValidation(GCMessageRead &messageRead);
    void OnGetEventFavorites(GCMessageRead &messageRead);
    void OnClientRequestPrestigeCoin(GCMessageRead &messageRead);
};
