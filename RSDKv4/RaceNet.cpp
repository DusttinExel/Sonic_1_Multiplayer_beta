#include "RetroEngine.hpp"

#if RETRO_USE_NETWORKING
#include "RaceNetCore.hpp"
#include "RaceNet.hpp"

int raceMode = 0;
char raceHostAddress[64] = "127.0.0.1";
int racePort = 7777;

static RaceNetCore net;
static bool started = false;
static int frameTick = 0;

// Finish tracking
static int lastStage = -1;                 // (list << 8) | position we were in last frame
static uint32_t finishMs  = 0;
static uint8_t finishedPos = 0xFF;
static int stageFirstFinisher[256];        // per stage position: id of whoever finished first (-1 none)
static uint32_t stageFirstTime[256];
static bool announced[RN_MAX_PLAYERS][256];

static int LocalStage() { return (activeStageList << 8) | (stageListPosition & 0xFF); }

void RaceNet_Init()
{
    for (int i = 0; i < 256; ++i) stageFirstFinisher[i] = -1;
    memset(announced, 0, sizeof(announced));

    if (raceMode != 1 && raceMode != 2)
        return;

    std::string err;
    if (!net.start(raceMode == 1 ? RN_HOST : RN_CLIENT, raceHostAddress, (uint16_t)racePort, &err)) {
        PrintLog("RaceNet: could not start (%s)", err.c_str());
        return;
    }
    started = true;
    if (raceMode == 1)
        PrintLog("RaceNet: hosting on UDP port %d", racePort);
    else
        PrintLog("RaceNet: joining %s:%d", raceHostAddress, racePort);
}

void RaceNet_Release()
{
    if (started)
        net.stop();
    started = false;
}

// Returns the entity slot of the local player, or -1.
static int LocalPlayerSlot()
{
    if (cameraTarget < 0 || cameraTarget >= ENTITY_COUNT)
        return -1;
    if (objectEntityList[cameraTarget].type == OBJ_TYPE_BLANKOBJECT)
        return -1;
    return cameraTarget;
}

static void CheckAnnouncements()
{
    // Whoever's finish for a stage has the lowest host time is the winner of that stage.
    const auto &remotes = net.getRemotes();
    for (int i = 0; i < RN_MAX_PLAYERS; ++i) {
        uint8_t pos;
        uint32_t t;
        if (i == net.getLocalId()) {
            pos = finishedPos;
            t   = finishMs;
        }
        else if (remotes[i].active) {
            pos = remotes[i].state.finishedPos;
            t   = remotes[i].state.finishMs;
        }
        else
            continue;

        if (pos == 0xFF || announced[i][pos])
            continue;
        announced[i][pos] = true;

        if (stageFirstFinisher[pos] < 0 || t < stageFirstTime[pos]) {
            bool firstEver = stageFirstFinisher[pos] < 0;
            stageFirstFinisher[pos] = i;
            stageFirstTime[pos]     = t;
            if (firstEver)
                PrintLog("RaceNet: player %d finished stage %d first (%.2fs)", i, pos, t / 1000.0f);
        }
    }
}

void RaceNet_Update()
{
    if (!started)
        return;

    net.poll();

    // The host has no id until a client joins; clients have none until welcomed.
    if (!net.connected())
        return;

    int slot = LocalPlayerSlot();
    if (slot < 0)
        return;

    // Detect "finished a stage": we moved on to the next position in the same list.
    int stage = LocalStage();
    if (lastStage >= 0 && stage != lastStage && (stage >> 8) == (lastStage >> 8) && (stage & 0xFF) == (lastStage & 0xFF) + 1) {
        finishedPos = (uint8_t)(lastStage & 0xFF);
        finishMs    = net.hostTimeMs();
        PrintLog("RaceNet: you finished stage %d", finishedPos);
    }
    lastStage = stage;

    if (++frameTick & 1) // 30 updates per second is plenty
        return;

    Entity *p = &objectEntityList[slot];
    RaceState s;
    s.stage       = (uint16_t)stage;
    s.xpos        = p->xpos;
    s.ypos        = p->ypos;
    s.rotation    = (int16_t)p->rotation;
    s.animation   = p->animation;
    s.frame       = p->frame;
    s.direction   = p->direction;
    s.flags       = p->visible ? RN_FLAG_VISIBLE : 0;
    s.finishMs    = finishMs;
    s.finishedPos = finishedPos;
    net.sendState(s);

    CheckAnnouncements();
}

void RaceNet_Draw()
{
    if (!started || !net.connected())
        return;

    int slot = LocalPlayerSlot();
    if (slot < 0)
        return;

    // Use the local player's animation data for everyone (same character everywhere).
    ObjectScript *script = &objectScriptList[objectEntityList[slot].type];
    if (!script->animFile)
        return;

    const int stage   = LocalStage();
    const auto &rem   = net.getRemotes();
    for (int i = 0; i < RN_MAX_PLAYERS; ++i) {
        if (i == net.getLocalId() || !rem[i].active)
            continue;
        const RaceState &r = rem[i].state;
        if (r.stage != stage || !(r.flags & RN_FLAG_VISIBLE))
            continue; // other player is in a different stage (or hidden)

        Entity ghost;
        memset(&ghost, 0, sizeof(ghost));
        ghost.xpos      = r.xpos;
        ghost.ypos      = r.ypos;
        ghost.rotation  = r.rotation;
        ghost.animation = r.animation;
        ghost.frame     = r.frame;
        ghost.direction = r.direction;
        ghost.visible   = 1;

        int x = (ghost.xpos >> 16) - xScrollOffset;
        int y = (ghost.ypos >> 16) - yScrollOffset;
        if (x < -64 || x > SCREEN_XSIZE + 64 || y < -64 || y > SCREEN_YSIZE + 64)
            continue;

        DrawObjectAnimation(script, &ghost, x, y);
    }
}

#endif
