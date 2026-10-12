#ifndef RACENET_H
#define RACENET_H

// Engine glue for the Sonic 1 / RSDKv4 multiplayer (race mode).
// Every player runs their own copy of the stage; the other players are drawn as
// "ghosts" using the local player's own animation data, so no game scripts need changing.

#if RETRO_USE_NETWORKING

extern int raceMode;           // 0 = off, 1 = host, 2 = join   (settings.ini [Multiplayer] Mode)
extern char raceHostAddress[64]; // address to join                (settings.ini [Multiplayer] Address)
extern int racePort;           // UDP port                        (settings.ini [Multiplayer] Port)

void RaceNet_Init();      // call once after settings are loaded
void RaceNet_Update();    // call once per frame, right after ProcessObjects() in a normal stage step
void RaceNet_Draw();      // call at the end of the stage draw, draws the other players
void RaceNet_Release();   // call on shutdown

#else
inline void RaceNet_Init() {}
inline void RaceNet_Update() {}
inline void RaceNet_Draw() {}
inline void RaceNet_Release() {}
#endif

#endif
