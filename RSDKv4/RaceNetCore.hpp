#ifndef RACENET_CORE_H
#define RACENET_CORE_H

// Engine-independent UDP layer for the race / hide-and-seek multiplayer.
// Topology: one host + up to (RN_MAX_PLAYERS - 1) clients. The host relays every
// client's state to all the other clients. Polling is non-blocking and meant to be
// called once per game frame from the main thread (no extra threads, no locks).

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

#define RN_MAX_PLAYERS 8
#define RN_MAGIC       0x4E435253u // "SRCN"
#define RN_VERSION     1

enum RaceNetRole { RN_OFF = 0, RN_HOST = 1, RN_CLIENT = 2 };

enum RaceNetPacketType : uint8_t {
    RNP_HELLO   = 1, // client -> host
    RNP_WELCOME = 2, // host   -> client (playerId assigned)
    RNP_STATE   = 3, // both ways (host relays)
    RNP_BYE     = 4, // both ways
    RNP_FULL    = 5, // host   -> client (room is full)
};

// What we send about one player every few frames.
struct RaceState {
    uint8_t playerId   = 0;
    uint8_t flags      = 0; // bit1 = visible
    uint16_t stage     = 0; // (stageList << 8) | stagePosition
    int32_t xpos       = 0; // 16.16 fixed point, same as Entity
    int32_t ypos       = 0;
    int16_t rotation   = 0;
    uint8_t animation  = 0;
    uint8_t frame      = 0;
    uint8_t direction  = 0;
    uint8_t role       = 0; // game-mode specific (0 = runner, 1 = seeker ...)
    uint32_t finishMs  = 0; // host-clock time of the last finish
    uint8_t finishedPos = 0xFF; // stage position of that last finish, 0xFF = none yet
};

enum { RN_FLAG_VISIBLE = 2 };

struct RaceRemote {
    bool active = false;
    RaceState state;
    std::chrono::steady_clock::time_point lastSeen;
    asio::ip::udp::endpoint endpoint; // host side only
};

class RaceNetCore
{
public:
    RaceNetCore() : socket(io) {}
    ~RaceNetCore() { stop(); }

    // Returns false (and fills err) if the socket could not be set up.
    bool start(RaceNetRole role, const std::string &host, uint16_t port, std::string *err = nullptr);
    void stop();

    // Call once per frame. Receives everything pending, handles timeouts and hello retries.
    void poll();

    // Queue our own state (host: relayed to clients, client: sent to host).
    void sendState(const RaceState &s);

    bool running() const { return role != RN_OFF; }
    RaceNetRole getRole() const { return role; }
    int getLocalId() const { return localId; }                 // -1 until a client is welcomed
    bool connected() const { return role == RN_HOST || localId >= 0; }
    bool wasRejected() const { return rejected; }
    const std::array<RaceRemote, RN_MAX_PLAYERS> &getRemotes() const { return remotes; }

    // Host clock in ms (clients estimate it from the WELCOME), used to order finishers fairly.
    uint32_t hostTimeMs() const;

    static constexpr int TIMEOUT_MS = 5000;

private:
    static constexpr size_t PACKET_BYTES = 29;
    static void encode(uint8_t type, const RaceState &s, uint8_t *out);
    static bool decode(const uint8_t *in, size_t len, uint8_t *type, RaceState *s);

    void send(const uint8_t *buf, const asio::ip::udp::endpoint &to);
    void handle(const uint8_t *buf, size_t len, const asio::ip::udp::endpoint &from);
    int findClient(const asio::ip::udp::endpoint &ep) const;

    asio::io_context io;
    asio::ip::udp::socket socket;
    asio::ip::udp::endpoint hostEndpoint;

    RaceNetRole role = RN_OFF;
    int localId      = -1;
    bool rejected    = false;
    std::array<RaceRemote, RN_MAX_PLAYERS> remotes;

    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point lastHello;
    std::chrono::steady_clock::time_point lastHostSeen;
    int64_t clockOffsetMs = 0; // client: hostClock - ourClock, learned from WELCOME
};

#endif
