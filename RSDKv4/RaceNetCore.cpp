#include "RaceNetCore.hpp"

using asio::ip::udp;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- wire format
// 29 bytes, little endian:
//  0  u32 magic   4 u8 version   5 u8 type   6 u8 playerId   7 u8 flags
//  8  u16 stage   10 i32 x       14 i32 y    18 i16 rotation
//  20 u8 anim     21 u8 frame    22 u8 dir   23 u8 role       24 u32 finishMs
//  28 u8 finishedPos

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xFF;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void RaceNetCore::encode(uint8_t type, const RaceState &s, uint8_t *o)
{
    memset(o, 0, PACKET_BYTES);
    put32(o + 0, RN_MAGIC);
    o[4] = RN_VERSION;
    o[5] = type;
    o[6] = s.playerId;
    o[7] = s.flags;
    put16(o + 8, s.stage);
    put32(o + 10, (uint32_t)s.xpos);
    put32(o + 14, (uint32_t)s.ypos);
    put16(o + 18, (uint16_t)s.rotation);
    o[20] = s.animation;
    o[21] = s.frame;
    o[22] = s.direction;
    o[23] = s.role;
    put32(o + 24, s.finishMs);
    o[28] = s.finishedPos;
}

bool RaceNetCore::decode(const uint8_t *in, size_t len, uint8_t *type, RaceState *s)
{
    if (len != PACKET_BYTES || get32(in) != RN_MAGIC || in[4] != RN_VERSION)
        return false;
    *type        = in[5];
    s->playerId  = in[6];
    s->flags     = in[7];
    s->stage     = get16(in + 8);
    s->xpos      = (int32_t)get32(in + 10);
    s->ypos      = (int32_t)get32(in + 14);
    s->rotation  = (int16_t)get16(in + 18);
    s->animation = in[20];
    s->frame     = in[21];
    s->direction = in[22];
    s->role      = in[23];
    s->finishMs  = get32(in + 24);
    s->finishedPos = in[28];
    return s->playerId < RN_MAX_PLAYERS;
}

// ---------------------------------------------------------------- lifecycle
bool RaceNetCore::start(RaceNetRole r, const std::string &host, uint16_t port, std::string *err)
{
    stop();
    try {
        asio::error_code ec;
        if (r == RN_HOST) {
            socket.open(udp::v4());
            socket.bind(udp::endpoint(udp::v4(), port));
            localId = 0;
        }
        else if (r == RN_CLIENT) {
            udp::resolver resolver(io);
            auto results = resolver.resolve(udp::v4(), host, std::to_string(port), ec);
            if (ec || results.empty()) {
                if (err)
                    *err = "could not resolve host '" + host + "'";
                return false;
            }
            hostEndpoint = *results.begin();
            socket.open(udp::v4());
            socket.bind(udp::endpoint(udp::v4(), 0));
            localId = -1;
        }
        else {
            return false;
        }
        socket.non_blocking(true);
    } catch (std::exception &e) {
        if (err)
            *err = e.what();
        if (socket.is_open()) {
            asio::error_code ignore;
            socket.close(ignore);
        }
        return false;
    }

    role      = r;
    rejected  = false;
    startTime = Clock::now();
    lastHello    = startTime - std::chrono::seconds(1); // send HELLO on the first poll
    lastHostSeen = startTime;
    for (auto &rm : remotes) rm = RaceRemote();
    return true;
}

void RaceNetCore::stop()
{
    if (role == RN_OFF)
        return;

    // Best effort goodbye so the others drop our ghost immediately.
    RaceState bye;
    bye.playerId = (uint8_t)(localId < 0 ? 0 : localId);
    uint8_t buf[PACKET_BYTES];
    encode(RNP_BYE, bye, buf);
    if (role == RN_CLIENT && localId >= 0) {
        send(buf, hostEndpoint);
    }
    else if (role == RN_HOST) {
        for (auto &rm : remotes)
            if (rm.active)
                send(buf, rm.endpoint);
    }

    asio::error_code ignore;
    socket.close(ignore);
    role    = RN_OFF;
    localId = -1;
    for (auto &rm : remotes) rm = RaceRemote();
}

uint32_t RaceNetCore::hostTimeMs() const
{
    int64_t local = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startTime).count();
    return (uint32_t)(local + clockOffsetMs);
}

// ---------------------------------------------------------------- io
void RaceNetCore::send(const uint8_t *buf, const udp::endpoint &to)
{
    asio::error_code ec;
    socket.send_to(asio::buffer(buf, PACKET_BYTES), to, 0, ec); // UDP: drops are fine, never block
}

int RaceNetCore::findClient(const udp::endpoint &ep) const
{
    for (int i = 1; i < RN_MAX_PLAYERS; ++i)
        if (remotes[i].active && remotes[i].endpoint == ep)
            return i;
    return -1;
}

void RaceNetCore::sendState(const RaceState &in)
{
    if (role == RN_OFF || !connected())
        return;

    RaceState s  = in;
    s.playerId   = (uint8_t)localId;
    uint8_t buf[PACKET_BYTES];
    encode(RNP_STATE, s, buf);

    if (role == RN_CLIENT)
        send(buf, hostEndpoint);
    else
        for (int i = 1; i < RN_MAX_PLAYERS; ++i)
            if (remotes[i].active)
                send(buf, remotes[i].endpoint);
}

void RaceNetCore::handle(const uint8_t *buf, size_t len, const udp::endpoint &from)
{
    uint8_t type = 0;
    RaceState s;
    if (!decode(buf, len, &type, &s))
        return;

    const auto now = Clock::now();

    if (role == RN_HOST) {
        int id = findClient(from);
        switch (type) {
            case RNP_HELLO: {
                if (id < 0) {
                    for (int i = 1; i < RN_MAX_PLAYERS; ++i) {
                        if (!remotes[i].active) {
                            id                  = i;
                            remotes[i]          = RaceRemote();
                            remotes[i].active   = true;
                            remotes[i].endpoint = from;
                            remotes[i].state.playerId = (uint8_t)i;
                            break;
                        }
                    }
                }
                RaceState reply;
                uint8_t out[PACKET_BYTES];
                if (id < 0) {
                    encode(RNP_FULL, reply, out);
                }
                else {
                    remotes[id].lastSeen = now;
                    reply.playerId       = (uint8_t)id;
                    reply.finishMs       = hostTimeMs();
                    encode(RNP_WELCOME, reply, out);
                }
                send(out, from);
                break;
            }
            case RNP_STATE: {
                if (id < 0)
                    break; // not welcomed yet
                s.playerId           = (uint8_t)id; // never trust the id the client claims
                remotes[id].state    = s;
                remotes[id].lastSeen = now;
                uint8_t out[PACKET_BYTES];
                encode(RNP_STATE, s, out);
                for (int i = 1; i < RN_MAX_PLAYERS; ++i)
                    if (i != id && remotes[i].active)
                        send(out, remotes[i].endpoint);
                break;
            }
            case RNP_BYE: {
                if (id < 0)
                    break;
                remotes[id].active = false;
                RaceState gone;
                gone.playerId = (uint8_t)id;
                uint8_t out[PACKET_BYTES];
                encode(RNP_BYE, gone, out);
                for (int i = 1; i < RN_MAX_PLAYERS; ++i)
                    if (remotes[i].active)
                        send(out, remotes[i].endpoint);
                break;
            }
        }
    }
    else if (role == RN_CLIENT) {
        if (from != hostEndpoint)
            return;
        lastHostSeen = now;
        switch (type) {
            case RNP_WELCOME:
                clockOffsetMs = (int64_t)s.finishMs
                                - std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
                localId = s.playerId;
                if (localId == 0)
                    localId = -1; // host id is reserved, a valid WELCOME is never 0
                break;
            case RNP_FULL: rejected = true; break;
            case RNP_STATE:
                if ((int)s.playerId == localId)
                    break;
                remotes[s.playerId].active   = true;
                remotes[s.playerId].state    = s;
                remotes[s.playerId].lastSeen = now;
                break;
            case RNP_BYE: remotes[s.playerId].active = false; break;
        }
    }
}

void RaceNetCore::poll()
{
    if (role == RN_OFF)
        return;

    const auto now = Clock::now();

    // Drain the socket.
    uint8_t buf[256];
    for (int guard = 0; guard < 512; ++guard) {
        udp::endpoint from;
        asio::error_code ec;
        size_t n = socket.receive_from(asio::buffer(buf), from, 0, ec);
        if (ec == asio::error::would_block || ec == asio::error::try_again)
            break;
        if (ec)
            continue; // e.g. ICMP "connection reset" on Windows, ignore
        handle(buf, n, from);
    }

    // Client: knock until welcomed, then keep knocking once a second as a keepalive
    // (the host drops clients that go quiet, e.g. while sitting in a menu).
    if (role == RN_CLIENT && !rejected
        && now - lastHello >= std::chrono::milliseconds(localId < 0 ? 500 : 1000)) {
        lastHello = now;
        RaceState hello;
        uint8_t out[PACKET_BYTES];
        encode(RNP_HELLO, hello, out);
        send(out, hostEndpoint);
    }

    if (role == RN_CLIENT && localId >= 0 && now - lastHostSeen > std::chrono::milliseconds(TIMEOUT_MS)) {
        localId = -1; // host went away, go back to knocking
        for (auto &rm : remotes) rm.active = false;
    }

    // Time out silent peers.
    for (int i = 0; i < RN_MAX_PLAYERS; ++i) {
        RaceRemote &rm = remotes[i];
        if (!rm.active)
            continue;
        if (now - rm.lastSeen > std::chrono::milliseconds(TIMEOUT_MS))
            rm.active = false;
    }
}
