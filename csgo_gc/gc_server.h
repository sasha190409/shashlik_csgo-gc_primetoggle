// gc_server.h
#pragma once

#include "gc_shared.h"
#include <unordered_map>
#include <atomic>

// Forward declaration to avoid cyclic include
class NetworkingServer;

class ServerGC final : public SharedGC
{
public:
    ServerGC();
    ~ServerGC();

    bool CanHandleNetMessages() const { return m_receivedHello.load(std::memory_order_acquire); }

    void SetNetworking(NetworkingServer* net) { m_networking = net; }
    void CheckPendingReservations();          // called from the main loop

private:
    void HandleEvent(GCEvent type, uint64_t id, const std::vector<uint8_t> &buffer) override;

    void HandleMessage(uint32_t type, const void *data, uint32_t size);
    void HandleNetMessage(uint64_t steamId, const void *data, uint32_t size);
    void HandleClientSOCacheUnsubscribe(uint64_t steamId);

    // handshake / connection
    void OnServerHello(GCMessageRead &messageRead);
    void OnServerAvailable(GCMessageRead &messageRead);              // 4506
    void OnClientConnectionStatus(GCMessageRead &messageRead);       // 4009
    void OnServerConnectionStatus(GCMessageRead &messageRead);       // 4010

    // matchmaking
    void OnMatchmakingClient2ServerPing(GCMessageRead &messageRead); // 9103
    void OnMatchmakingServerReservationResponse(GCMessageRead &messageRead); // 9106
    void OnMatchmakingGC2ClientReserve(GCMessageRead &messageRead);  // 9107
    void OnMatchmakingGC2ClientAbandon(GCMessageRead &messageRead);  // 9112
    void OnMatchmakingGC2ServerConfirm(GCMessageRead &messageRead);  // 9114
    void OnGC2ServerReservationUpdate(GCMessageRead &messageRead);   // 9142

    // misc
    void OnServerVarValueNotificationInfo(GCMessageRead &messageRead); // 9150
    void OnServer2GCClientValidate(GCMessageRead &messageRead);      // 9153
    void OnServerVersionUpdated(GCMessageRead &messageRead);         // 2522
    void OnGameServerInfo(GCMessageRead &messageRead);               // 4508

    // item plumbing
    void IncrementKillCountAttribute(GCMessageRead &messageRead);

    struct PendingReservation {
        uint64_t exchange;
        uint32_t token;
        uint32_t stamp;
    };

    std::unordered_map<uint64_t, PendingReservation> m_pendingReservations;
    NetworkingServer* m_networking = nullptr;
    std::atomic_bool m_receivedHello{};

    void SendConfirmToClient(uint64_t clientId, const PendingReservation& res);
};