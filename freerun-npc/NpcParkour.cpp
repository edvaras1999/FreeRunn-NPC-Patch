// NpcParkour.cpp - EXPERIMENTAL, step 1 of the NPC work. See NpcParkour.h.
//
// What a test does, in order (every step is written to the log):
//
//   1. aim      A short sphere cast from the player's chest, along the way the
//               player faces, looks for an NPC. The engine's own collidable ->
//               reference lookup (the one Havok.cpp already uses) names it.
//   2. align    The NPC is turned to the player's heading (optional), then left
//               alone for a moment so its character controller can catch up.
//   3. probe    The same ledge search the player uses runs for the NPC, from
//               the NPC's own position and facing. Parkour.cpp keeps its working
//               values in the player's globals; ProbeForActor borrows and
//               restores them.
//   4. start    The ledge type goes into the NPC's graph variable and the start
//               event is sent - exactly the protocol FreeRun speaks to the
//               player (see Graph.h).
//   5. watch    The NPC's own "ongoing" graph variable says whether the
//               behaviour patch took the event. The start mark is walked off
//               with the same per-tick correction the player gets, physics
//               simulation is switched off so the root motion can move the
//               body, and when "ongoing" drops the NPC is handed back.
//
// No hooks are installed: the NPC's end of the animation is read by polling its
// graph variable, not by listening to its animation events.

#include "NpcParkour.h"
#include "Engine.h"
#include "Graph.h"
#include "Havok.h"
#include "Parkour.h"
#include "Log.h"

#include "skse/GameReferences.h"

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    // ---- settings -----------------------------------------------------------

    const char * const kIni     = ".\\Data\\SKSE\\Plugins\\Freerun.ini";
    const char * const kSection = "Npc";

    bool    s_enabled       = true;
    UInt32  s_testKey       = 68;       // DirectInput scan code of F10
    bool    s_alignToPlayer = true;
    float   s_maxDistance   = 800.0f;
    bool    s_messages      = true;

    // Step 2: the tracked NPC (see TrackTick).
    UInt32  s_trackKey          = 87;       // DirectInput scan code of F11
    bool    s_autoTracked       = true;     // false: only log, never act on its own
    float   s_autoMinDistance   = 200.0f;   // game units from the player, at least
    float   s_autoMaxDistance   = 1500.0f;  // ... and at most
    float   s_autoStuckSec      = 2.0f;     // how long it must have stood still
    float   s_autoMaxMove       = 40.0f;    // moving less than this counts as standing still
    float   s_autoCooldownSec   = 6.0f;     // pause between two automatic moves
    float   s_autoDetourSec     = 3.0f;     // the window in which she must have come no closer
    float   s_autoMinApproach   = 30.0f;    // ... by less than this many units, to count as a detour
    float   s_autoMaxStartGap   = 30.0f;    // she must already be this close to the move's start mark
    float   s_dashSpeed         = 420.0f;   // game units a second: how fast she is brought to a start mark
    bool    s_trailEnabled      = true;     // the player's trail (step 3)
    float   s_trailMinHeight    = 120.0f;   // the player must be at least this far above her
    float   s_trailJoinRadius   = 350.0f;   // she joins the trail at a node at most this far from her
    float   s_trailSpeed        = 330.0f;   // game units a second along the trail
    float   s_trailEndRadius    = 220.0f;   // she has caught up when this close, and level
    float   s_trailMaxSec       = 45.0f;    // longest a replay may last
    float   s_trailSpacing      = 40.0f;    // a trail node every this many units
    bool    s_trailWalk         = false;    // true: she also WALKS the player's path (experimental); false: she only repeats his moves
    float   s_trailMoveRadius   = 120.0f;   // she repeats a move of the player's when she is this close to its start mark
    bool    s_callEnabled       = true;     // the player's move is the trigger: she goes to the same spot and does it too
    float   s_callRadius        = 500.0f;   // she answers a move from this far (flat) from where it was made
    float   s_callWindowSec     = 10.0f;    // ... for this long after he made it
    int     s_callAttempts      = 3;        // tries at one move if she does not end up on top
    bool    s_landing           = true;     // after a climb, rest the body on top of the edge
    float   s_landingForward    = 38.0f;    // ... this far past it
    int     s_landingGlideMs    = 280;      // ... getting there over this long (0: at once)
    bool    s_pathReplay        = true;     // she is carried along the path his body took, instead of left to the engine
    bool    s_animDriven        = false;    // EXPERIMENT: tell the engine she is animation driven for the length of a move
    float   s_strandedSec       = 15.0f;    // idle this long, far from the player: put back on his trail (0: never)
    float   s_strandedMinDistance = 450.0f; // ... and at least this far from him
    int     s_minMoveMs         = 900;      // a move of his that stopped sooner was interrupted, and is not repeated
    bool    s_manualToward      = true;     // F10: try the ledge between her and the player, from where she stands
    float   s_manualMaxGap      = 150.0f;   // F10: how far from the start mark she may be

    // ---- timing (milliseconds of real time) ---------------------------------

    const DWORD kRequestTtlMs    = 1000;    // a key press goes stale after this
    const DWORD kAlignMs         = 400;     // wait after turning the NPC
    const DWORD kStartTimeoutMs  = 1500;    // the behaviour must react within this
    const DWORD kPlanSettleMs    = 150;     // after turning her toward the player, before the move is sent
    const DWORD kActionTimeoutMs = 9000;    // hard stop for one move
    const DWORD kTraceEveryMs    = 250;

    // ---- state: one NPC at a time -------------------------------------------

    enum State { kIdle, kAligning, kWaitStart, kRunning, kTrail, kGlide };

    State           s_state         = kIdle;
    bool            s_requested     = false;
    DWORD           s_requestedAt   = 0;
    Actor *         s_npc           = NULL;
    DWORD           s_stateStart    = 0;
    DWORD           s_actionStart   = 0;
    DWORD           s_lastTrace     = 0;
    Parkour::Type   s_type          = Parkour::kNoLedge;
    NiPoint3        s_start;                // where the animation expects to begin

    // The tracked NPC. The pointer is never trusted: every use goes through
    // TrackedAlive, and the whole per-sample evaluation sits inside a guard.
    Actor *         s_tracked           = NULL;
    UInt32          s_trackedId         = 0;
    bool            s_trackRequested    = false;
    DWORD           s_trackRequestedAt  = 0;
    bool            s_manual            = true;     // the running move came from the key
    int             s_active            = -1;       // the slot of the tracked NPC that runs the move (-1: none, or a manual test)
    bool            s_trailRun          = false;    // a trail replay is running
    bool            s_trailCall         = false;    // ... and it answers a move of the player's
    DWORD           s_callNodeTime      = 0;        // the move being answered (by its time)
    DWORD           s_callAfterTime     = 0;        // moves made at or before this are answered
    UInt32          s_callAttemptsMade  = 0;
    DWORD           s_lastCallCheck     = 0;
    DWORD           s_lastCallWhyLog    = 0;
    DWORD           s_lastPathCheck     = 0;
    NiPoint3        s_planLedge;                    // the top of the edge of the move being made
    float           s_planYaw           = 0.0f;     // the heading the move is made with
    bool            s_walkInit          = false;    // the walk to a start mark has its own position, kept here
    float           s_walkPos[3]        = { 0.0f, 0.0f, 0.0f };
    float           s_walkYaw           = 0.0f;
    DWORD           s_approachDeadline  = 0;        // she must be at the start mark by then (0: no deadline)
    DWORD           s_lastTook          = 0;
    float           s_lastRise          = 0.0f;

    // A short, eased move to the spot on top of an edge, instead of a jump to it.
    struct Glide
    {
        bool    active;
        bool    isPlayer;
        Actor * who;
        float   from[3];
        float   to[3];
        DWORD   start;
        DWORD   ms;
    };

    Glide s_glide = { false, false, NULL, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0, 0 };

    // The path the player's body takes during one of his moves: where it is, a tick
    // at a time, relative to where it started (and with his own pull toward the
    // start mark taken out, which she does not need: she starts ON the mark).
    // A follower repeating the move is carried along that path, every tick, while
    // her own animation plays. The engine is then not what moves her, and there is
    // nothing for her AI or her physics to fight with: the logs showed her sliding
    // down the slope she started the climb on, at a steady 75 units a second, as
    // though her physics were running.
    enum { kPathMax = 6, kPathSamples = 240 };

    struct MovePath
    {
        DWORD   nodeTime;               // the trail node this belongs to
        UInt32  count;
        DWORD   t[kPathSamples];        // milliseconds since the move began
        float   x[kPathSamples];
        float   y[kPathSamples];
        float   z[kPathSamples];
    };

    MovePath    s_paths[kPathMax];
    UInt32      s_pathNext          = 0;
    MovePath *  s_recording         = NULL;     // the one being written, if one of his moves is under way
    DWORD       s_recordStart       = 0;
    UInt32      s_recordTicks       = 0;
    NiPoint3    s_recordOrigin;                 // where his body was when the move was decided
    NiPoint3    s_recordMissing;                // the gap to the start mark, which the engine closes over 30 ticks
    MovePath *  s_planPath          = NULL;     // the path the running move is carried along

    // One path for each kind of move, the latest of the player's. A move that is
    // not one of his - an NPC deciding by herself that there is something to
    // climb - has no path of its own to be carried along, and would be left to the
    // engine, which is where the fighting was. It is given his path for that kind
    // of move instead, turned to the heading she makes it with: the animation's
    // movement is the same wherever it is made. (The type numbers run 0 to 8.)
    enum { kTemplateTypes = 9 };
    MovePath    s_templates[kTemplateTypes];
    float       s_templateYaw[kTemplateTypes];
    bool        s_haveTemplate[kTemplateTypes];
    MovePath    s_tempPath;                     // the template turned to the heading of the move being made
    int             s_playerMoveType    = -1;       // the player's own last move, for his landing
    NiPoint3        s_playerMoveLedge;
    float           s_playerMoveYaw     = 0.0f;
    UInt32          s_trailIdx          = 0;        // the node she is heading for
    DWORD           s_trailStart        = 0;
    DWORD           s_trailLastTick     = 0;
    UInt32          s_trailMoves        = 0;
    DWORD           s_lastTrailCheck    = 0;
    DWORD           s_lastTrailWhyLog   = 0;
    bool            s_planValid         = false;    // a ledge was found a moment ago and is not searched for again
    Parkour::Type   s_planType          = Parkour::kNoLedge;
    NiPoint3        s_planStart;
    NiPoint3        s_anchor;                       // where it last was when it was moving
    DWORD           s_anchorTime        = 0;
    bool            s_anchorReset       = true;
    DWORD           s_lastSample        = 0;
    DWORD           s_lastProbe         = 0;
    DWORD           s_lastTrackTrace    = 0;
    DWORD           s_cooldownUntil     = 0;
    Parkour::Type   s_lastProbeType     = Parkour::kNoLedge;
    bool            s_haveProbe         = false;
    NiPoint3        s_posBefore;

    // The position correction. The animation starts from an exact mark, and she
    // is brought to it at running speed: the first logs showed a pull spread over
    // thirty ticks looking like a slow slide, and the moves that began from the
    // farthest marks were the ones that went wrong.
    struct Correction
    {
        bool    initialised;
        bool    done;
        DWORD   last;
        float   remaining;      // havok units still to cover
        float   dir[3];         // the direction of the original gap, normalised
    };

    Correction s_corr = { false, false, 0, 0.0f, { 0.0f, 0.0f, 0.0f } };

    void ResetCorrection()
    {
        s_corr.initialised  = false;
        s_corr.done         = false;
        s_corr.last         = 0;
        s_corr.remaining    = 0.0f;
        s_corr.dir[0] = s_corr.dir[1] = s_corr.dir[2] = 0.0f;
    }

    // ---- small helpers ------------------------------------------------------

    float ReadFloat(const char * key, float fallback)
    {
        char defaultText[32];
        sprintf_s(defaultText, sizeof(defaultText), "%f", fallback);
        char buffer[64];
        GetPrivateProfileStringA("Npc", key, defaultText, buffer, sizeof(buffer), ".\\Data\\SKSE\\Plugins\\Freerun.ini");
        return (float)atof(buffer);
    }

    const char * TypeName(Parkour::Type type)
    {
        switch(type)
        {
            case Parkour::kNoLedge:     return "none";
            case Parkour::kFailed:      return "failed (no stamina)";
            case Parkour::kGrab:        return "grab";
            case Parkour::kVault:       return "vault";
            case Parkour::kStepLow:     return "step low";
            case Parkour::kStepHigh:    return "step high";
            case Parkour::kLow:         return "climb low";
            case Parkour::kMedium:      return "climb medium";
            case Parkour::kHigh:        return "climb high";
            case Parkour::kHighest:     return "climb highest";
            default:                    return "unknown";
        }
    }

    // The engine's own message box, called the way Engine::MessageBox calls it.
    // It is spelled out here because <windows.h> turns the name MessageBox into
    // a macro, and the two spellings of Engine::MessageBox would then disagree
    // between files.
    void ShowText(const char * text)
    {
        if(!s_messages || !text) return;
        typedef void (__cdecl * fn_t)(const char *, void *, UInt32, UInt32, UInt32, void *, UInt32);
        ((fn_t)0x0087AC60)(text, NULL, 0, 4, 0x0A, (void *)0x01094A2C, 0);
    }

    // Is this still an actor the engine is simulating, in a loaded cell?
    bool NpcUsable(Actor * npc)
    {
        if(!npc) return false;
        if(Engine::LifeState(npc) != Engine::kLifeState_Alive) return false;
        if(!Engine::ParentCell(npc)) return false;
        if(!Engine::CharController(npc)) return false;
        if(!Engine::Get3D(npc, false)) return false;
        return true;
    }

    // The same question, with the access violation caught: for an NPC whose
    // pointer has been kept across frames.
    bool SafeUsable(Actor * npc)
    {
        __try
        {
            return NpcUsable(npc);
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Everything ParkourPossible asks of the player that makes sense for an NPC.
    bool NpcCanAct(Actor * npc, const char *& why, bool ignoreAnimationDriven)
    {
        if(Parkour::IsBeastForm(npc))           { why = "the NPC is in a beast form"; return false; }
        if(Parkour::IsKnockedOut(npc))          { why = "the NPC is knocked down"; return false; }
        if(!ignoreAnimationDriven && Parkour::IsAnimationDriven(npc)) { why = "the NPC is in a scripted or animation-driven state"; return false; }
        if(Parkour::IsStaggering(npc))          { why = "the NPC is staggering"; return false; }
        if(Parkour::IsInSyncedAnimation(npc))   { why = "the NPC is in a synced animation"; return false; }
        if(Parkour::IsSitting(npc))             { why = "the NPC is sitting or sleeping"; return false; }
        if(Parkour::IsInDrawSheath(npc))        { why = "the NPC is drawing or sheathing a weapon"; return false; }
        if(Parkour::IsAttacking(npc))           { why = "the NPC is attacking"; return false; }
        return true;
    }

    // ---- the per-tick position correction -----------------------------------

    void RunCorrection(Actor * npc)
    {
        void * ctrl = Engine::CharController(npc);
        if(!ctrl) return;

        __declspec(align(16)) float current[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Engine::ControllerGetPosition(ctrl, current, true);

        const float scale = Engine::HavokScale();
        const DWORD now = GetTickCount();
        float dt = 1.0f / 60.0f;

        if(!s_corr.initialised)
        {
            // The gap is measured ONCE, and what is covered is that gap along its
            // original direction. Measuring it again every tick would drag her
            // back toward the mark while the animation's own movement carries her
            // away from it, and that is exactly how seven of thirteen repeated
            // moves ended without lifting her at all.
            const float mx = s_start.x * scale - current[0];
            const float my = s_start.y * scale - current[1];
            const float mz = s_start.z * scale - current[2];
            const float distance = sqrtf(mx * mx + my * my + mz * mz);

            s_corr.initialised = true;
            s_corr.remaining   = distance;
            if(distance > 0.0001f)
            {
                s_corr.dir[0] = mx / distance;
                s_corr.dir[1] = my / distance;
                s_corr.dir[2] = mz / distance;
            }
            s_corr.done = (distance <= 2.0f * scale);

            _MESSAGE("NPC test: start mark is %.1f game units away", distance / scale);
        }
        else
        {
            dt = (float)(now - s_corr.last) / 1000.0f;
        }

        s_corr.last = now;
        if(dt > 0.1f) dt = 0.1f;
        if(dt <= 0.0f) dt = 1.0f / 60.0f;

        if(s_corr.done) return;

        float step = s_dashSpeed * scale * dt;
        if(step > s_corr.remaining) step = s_corr.remaining;

        __declspec(align(16)) float target[4];
        target[0] = current[0] + s_corr.dir[0] * step;
        target[1] = current[1] + s_corr.dir[1] * step;
        target[2] = current[2] + s_corr.dir[2] * step;
        target[3] = 0.0f;

        Engine::ControllerSetPosition(ctrl, target, true, false);

        s_corr.remaining -= step;
        if(s_corr.remaining <= 0.0001f)
            s_corr.done = true;
    }

    // ---- ending a test ------------------------------------------------------

    // Hands the NPC back to the game: physics on, graph variable cleared, and
    // the same unwinding the player gets when an action stops.
    void ReleaseNpc(Actor * npc, bool sendInterrupt)
    {
        if(!npc) return;

        void * ctrl = Engine::CharController(npc);
        if(ctrl)
        {
            Engine::ControllerFlags(ctrl) &= ~Engine::Controller::kFlag_NoSim;

            // Leaving the ground with whatever velocity the animation ended on
            // throws the actor across the room. Keep the direction, drop the
            // speed and the vertical part.
            if(Engine::ControllerState(ctrl) != Engine::Controller::kState_OnGround)
            {
                const NiPoint3 facing = Parkour::FacingFlat(npc);
                __declspec(align(16)) float velocity[4];
                velocity[0] = facing.x;
                velocity[1] = facing.y;
                velocity[2] = 0.0f;
                velocity[3] = 0.0f;
                Engine::ControllerSetLinearVelocity(ctrl, velocity);
            }
        }

        Graph::SetInt(npc, Graph::GetNames().ledge, Parkour::kNoLedge);
        if(s_animDriven)
            Graph::SetBool(npc, Graph::GetNames().animationDriven, false);
        if(sendInterrupt)
            Graph::Notify(npc, Graph::GetNames().interrupt);
    }

    void SafeReleaseNpc(Actor * npc)
    {
        __try
        {
            ReleaseNpc(npc, false);
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // touchNpc is false when the NPC may no longer exist.
    void EndTest(bool touchNpc, bool sendInterrupt)
    {
        if(touchNpc && s_npc)
            ReleaseNpc(s_npc, sendInterrupt);

        // A move the plugin started on its own is followed by a pause, so one
        // stuck NPC cannot be set off again and again.
        if(!s_manual)
        {
            s_cooldownUntil = GetTickCount() + (DWORD)(s_autoCooldownSec * 1000.0f);
            s_anchorReset   = true;
        }

        s_state     = kIdle;
        s_npc       = NULL;
        s_requested = false;
        s_planValid = false;
        s_planPath  = NULL;
        s_trailRun  = false;
        s_trailCall = false;
        ResetCorrection();
    }

    void Fail(const char * why, bool touchNpc)
    {
        _ERROR("NPC test FAILED: %s", why);

        EndTest(touchNpc, touchNpc);

        char text[512];
        sprintf_s(text, sizeof(text),
                  "FreeRun NPC test: it did not work.\n\n%s\n\n"
                  "The log has the details:\nDocuments\\My Games\\Skyrim\\SKSE\\FreerunLE.log", why);
        if(s_manual) ShowText(text);
    }

    // ---- 1. aim -------------------------------------------------------------

    // Where the player is aiming: where the camera looks in third person, where
    // the body faces otherwise. In third person the two are often different, and
    // the player looks at the NPC with the camera.
    NiPoint3 AimDirection(Actor * player)
    {
        void * state = Engine::CameraState();
        if(state && Engine::CameraStateID(state) == Engine::kCameraState_ThirdPerson)
        {
            const float yaw = *(float *)((UInt8 *)state + Engine::ThirdPerson::kTargetYaw);
            return NiPoint3(sinf(yaw), cosf(yaw), 0.0f);
        }
        return Parkour::FacingFlat(player);
    }

    Actor * FindTarget(Actor * player)
    {
        const NiPoint3 facing = AimDirection(player);
        if(facing.x == 0.0f && facing.y == 0.0f)
        {
            _MESSAGE("NPC test: the player has no facing yet");
            return NULL;
        }

        const NiPoint3 pos = Engine::Position(player);
        const NiPoint3 start(pos.x, pos.y, pos.z + 100.0f);

        _MESSAGE("NPC test: aiming (%.2f, %.2f) from (%.0f, %.0f, %.0f)", facing.x, facing.y, start.x, start.y, start.z);

        const Havok::Result r = Havok::ShapeCast(start, facing, s_maxDistance, 50.0f,
                                                 Havok::kLayer_Transparent, player);
        _MESSAGE("NPC test: the aim cast (%.0f units) found %u hit(s)", s_maxDistance, (unsigned int)r.hitCount);

        for(UInt32 h = 0; h < r.hitCount; ++h)
        {
            const Havok::Hit & hit = r.hits[h];
            TESObjectREFR * ref = hit.hasReference ? hit.ref : NULL;

            _MESSAGE("NPC test:   hit %u: layer %u, %.0f units, has reference %d, form type %u, reference %08X",
                     (unsigned int)h, (unsigned int)hit.layer, hit.distance,
                     hit.hasReference ? 1 : 0, (unsigned int)hit.formType,
                     (unsigned int)(ref ? ref->formID : 0));

            if(!ref || ref == (TESObjectREFR *)player) continue;
            if(hit.formType != kFormType_NPC) continue;

            return (Actor *)ref;
        }

        return NULL;
    }

    // ---- 2..4. align, probe, start ------------------------------------------

    // Turns her, for real. With her physics off, the character controller does not
    // follow a turn of the actor on its own: its facing stayed where it was for the
    // whole move, 44 to 175 degrees off in most of the repeated moves of the last
    // log, and the animation moves her along that facing. That is what climbing
    // "in the air, far from the right place" was. So the controller's own facing is
    // written too (it is stored pointing backwards).
    void SetHeading(Actor * npc, void * ctrl, float yaw)
    {
        Engine::Rotation(npc).z = yaw;

        if(ctrl)
        {
            float * fwd = (float *)((UInt8 *)ctrl + Engine::Controller::kForwardVec);
            fwd[0] = -sinf(yaw);
            fwd[1] = -cosf(yaw);
        }
    }

    // Holds her on the start mark, facing the way the move goes. While her physics
    // are off the engine still moves her (it is her AI, running), so this is done
    // every tick until the animation has taken over.
    void PinPlan(Actor * npc)
    {
        void * ctrl = Engine::CharController(npc);
        if(!ctrl) return;

        const float scale = Engine::HavokScale();
        __declspec(align(16)) float target[4];
        target[0] = s_planStart.x * scale;
        target[1] = s_planStart.y * scale;
        target[2] = s_planStart.z * scale;
        target[3] = 0.0f;
        Engine::ControllerSetPosition(ctrl, target, true, false);

        __declspec(align(16)) float stopped[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Engine::ControllerSetLinearVelocity(ctrl, stopped);

        Engine::ControllerFlags(ctrl) |= Engine::Controller::kFlag_NoSim;       // asserted every tick: nothing is to switch it back on
        SetHeading(npc, ctrl, s_planYaw);
    }

    // The recorded path, `elapsed` milliseconds into the move: how far the body has
    // gone from where it started. False if there is nothing to follow.
    bool PathAt(const MovePath * path, DWORD elapsed, NiPoint3 & out)
    {
        if(!path || path->count < 2) return false;

        const UInt32 last = path->count - 1;
        if(elapsed >= path->t[last])
        {
            out = NiPoint3(path->x[last], path->y[last], path->z[last]);
            return true;
        }

        UInt32 i = 0;
        while(i + 1 < last && path->t[i + 1] <= elapsed) ++i;

        const float span = (float)(path->t[i + 1] - path->t[i]);
        const float f = (span > 0.0f) ? (float)(elapsed - path->t[i]) / span : 0.0f;

        out = NiPoint3(path->x[i] + (path->x[i + 1] - path->x[i]) * f,
                       path->y[i] + (path->y[i + 1] - path->y[i]) * f,
                       path->z[i] + (path->z[i + 1] - path->z[i]) * f);
        return true;
    }

    // Carries her along the path, tick by tick, from the start mark.
    void ReplayPath(Actor * npc, DWORD now)
    {
        void * ctrl = Engine::CharController(npc);
        if(!ctrl) return;

        NiPoint3 d(0.0f, 0.0f, 0.0f);
        if(!PathAt(s_planPath, now - s_actionStart, d)) return;

        const float scale = Engine::HavokScale();
        __declspec(align(16)) float target[4];
        target[0] = (s_planStart.x + d.x) * scale;
        target[1] = (s_planStart.y + d.y) * scale;
        target[2] = (s_planStart.z + d.z) * scale;
        target[3] = 0.0f;
        Engine::ControllerSetPosition(ctrl, target, true, false);

        __declspec(align(16)) float stopped[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Engine::ControllerSetLinearVelocity(ctrl, stopped);

        Engine::ControllerFlags(ctrl) |= Engine::Controller::kFlag_NoSim;
        SetHeading(npc, ctrl, s_planYaw);
    }

    // The template for this kind of move, turned to `yaw`: a displacement turned by
    // the difference of the two headings (the heading runs clockwise from north,
    // so a heading h points along (sin h, cos h)).
    MovePath * BuildTemplatePath(int type, float yaw)
    {
        if(!s_pathReplay || type < 0 || type >= kTemplateTypes || !s_haveTemplate[type]) return NULL;

        const MovePath & source = s_templates[type];
        if(source.count < 4) return NULL;

        const float d = yaw - s_templateYaw[type];
        const float c = cosf(d);
        const float s = sinf(d);

        s_tempPath.nodeTime = 0;
        s_tempPath.count    = source.count;
        for(UInt32 i = 0; i < source.count; ++i)
        {
            s_tempPath.t[i] = source.t[i];
            s_tempPath.x[i] = source.x[i] * c + source.y[i] * s;
            s_tempPath.y[i] = -source.x[i] * s + source.y[i] * c;
            s_tempPath.z[i] = source.z[i];
        }
        return &s_tempPath;
    }

    // Starts a move on an NPC whose ledge is already known. When `turn` is set she
    // is turned to `yaw` (the way the engine reads it: clockwise from north).
    // Her physics are switched off at once, so that she cannot run on while the
    // turn settles; the first ticks of the move walk her to the start mark, and
    // ReleaseNpc gives her physics back when it ends.
    void StartOn(Actor * npc, bool turn, float yaw, bool manual, Parkour::Type type, const NiPoint3 & start, const NiPoint3 & ledge)
    {
        s_planLedge = ledge;
        s_planPath  = NULL;
        s_manual    = manual;
        s_npc       = npc;
        s_state     = kAligning;
        s_planValid = true;
        s_planType  = type;
        s_planStart = start;

        void * ctrl = Engine::CharController(npc);
        if(ctrl)
            Engine::ControllerFlags(ctrl) |= Engine::Controller::kFlag_NoSim;

        s_planYaw = turn ? yaw : Engine::Rotation(npc).z;
        SetHeading(npc, ctrl, s_planYaw);

        s_planPath = BuildTemplatePath((int)type, s_planYaw);
        if(s_planPath)
            _MESSAGE("NPC test: carried along a template path for a %s (%u samples, %u ms)",
                     TypeName(type), (unsigned int)s_planPath->count, (unsigned int)s_planPath->t[s_planPath->count - 1]);

        s_walkInit   = false;
        s_stateStart = GetTickCount();
    }

    void Begin(Actor * player)
    {
        if(Parkour::MenuOpen()) return;

        if(Parkour::InProgress())
        {
            _MESSAGE("NPC test: ignored, the player is in the middle of a parkour move");
            return;
        }

        s_manual = true;
        s_active = -1;
        _MESSAGE("NPC test: key pressed");

        Actor * npc = FindTarget(player);
        if(!npc)
        {
            Fail("No NPC found where you are looking.\n\nLook straight at the NPC (in third person the camera is what aims), closer than the configured distance.", false);
            return;
        }

        if(!NpcUsable(npc))
        {
            Fail("The NPC is not ready: no 3D, no character controller, or not alive.", false);
            return;
        }

        const char * why = NULL;
        if(!NpcCanAct(npc, why, false))
        {
            Fail(why ? why : "The NPC is busy.", false);
            return;
        }

        if(s_manualToward)
        {
            // The same decision the automatic mode takes: the ledge between her
            // and the player, from where she stands right now.
            const NiPoint3 here   = Engine::Position(npc);
            const NiPoint3 there  = Engine::Position(player);
            const float tx = there.x - here.x;
            const float ty = there.y - here.y;
            const float distance = sqrtf(tx * tx + ty * ty);
            if(distance < 1.0f)
            {
                Fail("The NPC is on top of you.", false);
                return;
            }

            const NiPoint3 toward(tx / distance, ty / distance, 0.0f);
            Parkour::Type type = Parkour::kNoLedge;
            NiPoint3 ledge(0.0f, 0.0f, 0.0f), start(0.0f, 0.0f, 0.0f);
            float scale = 1.0f;
            const bool ok = Parkour::ProbeForActor(npc, type, ledge, start, scale, &toward);

            _MESSAGE("NPC test: target %08X, %.0f units from you; toward you the ledge search says %s",
                     (unsigned int)npc->formID, distance, TypeName(type));

            if(!ok || type == Parkour::kNoLedge)
            {
                Fail("No ledge found between the NPC and you.\n\nShe has to be standing right at the foot of a climbable edge (a low wall, a rock, a crate), on the side facing you.", false);
                return;
            }

            if(type == Parkour::kFailed)
            {
                Fail("The NPC does not have enough stamina for this move.", false);
                return;
            }

            const float gx = start.x - here.x, gy = start.y - here.y, gz = start.z - here.z;
            const float gap = sqrtf(gx * gx + gy * gy + gz * gz);
            _MESSAGE("NPC test: %s, her start mark is %.0f away (limit %.0f)", TypeName(type), gap, s_manualMaxGap);

            if(gap > s_manualMaxGap)
            {
                char text[256];
                sprintf_s(text, sizeof(text),
                          "The NPC is %.0f units from where the move would start (limit %.0f).\n\nMove her closer to the edge.",
                          gap, s_manualMaxGap);
                Fail(text, false);
                return;
            }

            StartOn(npc, true, atan2f(tx, ty), true, type, start, ledge);
            return;
        }

        s_npc = npc;
        _MESSAGE("NPC test: target is %08X", (unsigned int)npc->formID);

        if(s_alignToPlayer)
        {
            const float before = Engine::Rotation(npc).z;
            Engine::Rotation(npc).z = Engine::Rotation(player).z;
            _MESSAGE("NPC test: NPC heading %.3f -> %.3f (the player's)", before, Engine::Rotation(npc).z);
        }

        s_state      = kAligning;
        s_stateStart = GetTickCount();
    }

    void ProbeAndStart()
    {
        Actor * npc = s_npc;
        const Graph::Names & n = Graph::GetNames();

        const NiPoint3 pos = Engine::Position(npc);
        const NiPoint3 facing = Parkour::FacingFlat(npc);
        _MESSAGE("NPC test: NPC at (%.1f, %.1f, %.1f), heading %.3f, controller facing (%.2f, %.2f)",
                 pos.x, pos.y, pos.z, Engine::Rotation(npc).z, facing.x, facing.y);

        Parkour::Type type = Parkour::kNoLedge;
        NiPoint3 ledge(0.0f, 0.0f, 0.0f);
        NiPoint3 start(0.0f, 0.0f, 0.0f);

        if(s_planValid)
        {
            // The ledge was found when the move was decided, and she has been
            // held in place since. Searching again would look from wherever the
            // AI has walked her to meanwhile - the first automatic tries failed
            // exactly like that. The start mark is a point in the world: it does
            // not move with her, and the first ticks walk her to it.
            type    = s_planType;
            start   = s_planStart;
            _MESSAGE("NPC test: using the ledge found a moment ago (%s), start mark (%.1f, %.1f, %.1f)",
                     TypeName(type), start.x, start.y, start.z);
        }
        else
        {
            if(facing.x == 0.0f && facing.y == 0.0f)
            {
                Fail("The NPC's character controller has no facing, so no ledge can be searched.", false);
                return;
            }

            float scale = 1.0f;
            const bool ok = Parkour::ProbeForActor(npc, type, ledge, start, scale);
            s_planLedge = ledge;
            _MESSAGE("NPC test: ledge search says %s (%d), actor scale %.2f", TypeName(type), (int)type, scale);

            if(!ok || type == Parkour::kNoLedge)
            {
                Fail("No ledge found in front of the NPC.\n\nIt has to stand within about 70 units of a climbable edge (a low wall, a crate, a fence), facing it.", false);
                return;
            }

            if(type == Parkour::kFailed)
            {
                Fail("The NPC does not have enough stamina for this move.", false);
                return;
            }

            _MESSAGE("NPC test: ledge at (%.1f, %.1f, %.1f), start mark (%.1f, %.1f, %.1f)",
                     ledge.x, ledge.y, ledge.z, start.x, start.y, start.z);
        }

        // What the NPC's graph says before anything is sent. If the behaviour
        // patch is in this graph at all, these variables exist.
        _MESSAGE("NPC test: before: SkyParkourTPPInstalled=%d, SkyParkourOngoing=%d, SkyParkourLedge=%d",
                 Graph::GetBool(npc, n.installedThird) ? 1 : 0,
                 Graph::GetBool(npc, n.ongoing) ? 1 : 0,
                 (int)Graph::GetInt(npc, n.ledge, -99));

        s_type      = type;
        s_start     = start;
        s_posBefore = pos;
        ResetCorrection();

        Graph::SetInt(npc, n.ledge, type);

        // The experiment: the engine is told that her movement is the animation's
        // for as long as the move lasts, which is what it already does for scripted
        // and synced animations. If it holds, the engine's own movement for her is
        // set aside by the engine itself, and nothing has to be fought.
        if(s_animDriven)
        {
            Graph::SetBool(npc, n.animationDriven, true);
            _MESSAGE("NPC test: bAnimationDriven set for the length of the move");
        }

        if(type == Parkour::kStepHigh || type == Parkour::kStepLow)
            Graph::SetBool(npc, n.lowerBody, Parkour::IsWeaponOut(npc));

        const bool sent = Graph::Notify(npc, n.notify);
        _MESSAGE("NPC test: start event sent, the graph answered %d", sent ? 1 : 0);

        s_state         = kWaitStart;
        s_stateStart    = GetTickCount();
        s_actionStart   = s_stateStart;
        s_lastTrace     = s_stateStart;
    }

    // ---- 5. watch -----------------------------------------------------------

    // ---- step 3: the player's trail ----------------------------------------
    //
    // What the first logs showed: sending a follower to "find a ledge where she
    // happens to be" gets her to climb the right thing only now and then, and
    // the failures line up with her being a long way from the move's start mark.
    // What the PLAYER did is a better guide, for a plain reason: it is known to
    // work. The player stood on those spots, ran that path, climbed those ledges
    // from those marks.
    //
    // So the player leaves a trail: a position every 400 ms or so while on the
    // ground and at least a step from the last one, and, whenever FreeRun makes
    // the player start a move, a node that carries the move itself - its type,
    // its start mark, the way the player faced, and where it ended.
    //
    // A tracked follower who is well below the player and not getting closer
    // joins the trail at the node nearest her and walks it: from position to
    // position at running speed, and at each move node, standing on its start
    // mark, she makes the same move. The trail is only followed if it is whole.
    // A plain jump leaves a gap in it, and she cannot copy a plain jump.
    //
    // While she is on the trail her physics are off and this module moves her,
    // so the AI cannot walk her somewhere else. Her own animation carries on as
    // if she were running. Whether that looks right is for the game to say.

    struct TrailNode
    {
        DWORD           time;
        bool            isMove;         // a FreeRun move the player made
        bool            hasEnd;         // its end position is known
        Parkour::Type   type;           // moves only
        float           yaw;            // moves only: the way the player faced
        NiPoint3        pos;            // a position; for a move, its start mark
        NiPoint3        end;            // moves only: where the player ended up
        NiPoint3        ledge;          // moves only: the top of the edge he climbed
        int             path;           // moves only: which recorded path (s_paths), or -1
    };

    enum { kTrailMax = 300 };

    TrailNode   s_trail[kTrailMax];
    UInt32      s_trailCount        = 0;
    DWORD       s_lastTrailSample   = 0;

    void TrailAppend(const TrailNode & node)
    {
        if(s_trailCount >= (UInt32)kTrailMax)
        {
            memmove(&s_trail[0], &s_trail[1], sizeof(TrailNode) * (kTrailMax - 1));
            s_trailCount = kTrailMax - 1;
            if(s_trailRun && s_trailIdx > 0) --s_trailIdx;      // keep a running replay on the same node
        }

        s_trail[s_trailCount++] = node;

        if(s_trailCount % 50 == 0 && s_trailCount < (UInt32)kTrailMax)
            _MESSAGE("NPC trail: %u nodes", (unsigned int)s_trailCount);
    }

    TrailNode MakeWalkNode(const NiPoint3 & pos, DWORD now)
    {
        TrailNode node;
        node.time   = now;
        node.isMove = false;
        node.hasEnd = false;
        node.type   = Parkour::kNoLedge;
        node.yaw    = 0.0f;
        node.pos    = pos;
        node.end    = pos;
        node.ledge  = pos;
        node.path   = -1;
        return node;
    }

    NiPoint3 TrailExit(const TrailNode & node)
    {
        return (node.isMove && node.hasEnd) ? node.end : node.pos;
    }

    // Can a body go from the end of one node to the start of the next on foot?
    bool TrailSegmentOk(const TrailNode & a, const TrailNode & b)
    {
        const NiPoint3 from = TrailExit(a);
        const float dx = b.pos.x - from.x;
        const float dy = b.pos.y - from.y;
        const float dz = fabsf(b.pos.z - from.z);
        return sqrtf(dx * dx + dy * dy) <= 350.0f && dz <= 90.0f;
    }

    // Called every tick: it is one comparison except when a sample is due.
    void TrailSample(Actor * player)
    {
        if(!s_enabled || !s_trailEnabled) return;

        const DWORD now = GetTickCount();
        if(now - s_lastTrailSample < 400) return;
        s_lastTrailSample = now;

        if(Parkour::InProgress()) return;               // a move writes itself
        if(Parkour::IsSwimming(player)) return;
        if(Engine::PlayerIsInMidair()) return;          // only places a body has stood on

        const NiPoint3 pos = Engine::Position(player);

        if(s_trailCount > 0)
        {
            const NiPoint3 from = TrailExit(s_trail[s_trailCount - 1]);
            const float dx = pos.x - from.x;
            const float dy = pos.y - from.y;
            const float dz = pos.z - from.z;

            if(sqrtf(dx * dx + dy * dy) > 3000.0f)      // fast travel, a teleport, somewhere new
            {
                _MESSAGE("NPC trail: the player moved %.0f units at once: the trail starts over", sqrtf(dx * dx + dy * dy));
                s_trailCount = 0;
            }
            else if(sqrtf(dx * dx + dy * dy + dz * dz) < s_trailSpacing)
            {
                return;
            }
        }

        TrailAppend(MakeWalkNode(pos, now));
    }

    bool GroundAt(Actor * npc, float x, float y, float z, float & outZ);

    // Is the way from `from` to `to` free of anything solid, at waist height? The
    // way is taken in pieces of at most 120 units, each at waist height over the
    // ground THERE: one level sweep from start to end, as this used to be, ran into
    // the rising ground at the foot of a slope and refused 46 of the player's moves.
    // A sphere is swept along each piece; characters, and ground a body can walk
    // on, are not counted.
    bool PathClear(Actor * npc, const NiPoint3 & from, const NiPoint3 & to)
    {
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        const float horizontal = sqrtf(dx * dx + dy * dy);
        if(horizontal < 5.0f) return true;

        int pieces = (int)(horizontal / 120.0f) + 1;
        if(pieces > 6) pieces = 6;

        NiPoint3 previous(from.x, from.y, from.z + 45.0f);

        for(int i = 1; i <= pieces; ++i)
        {
            const float f = (float)i / (float)pieces;
            const float px = from.x + dx * f;
            const float py = from.y + dy * f;
            float pz = from.z + (to.z - from.z) * f;

            float groundZ = 0.0f;
            if(GroundAt(npc, px, py, pz, groundZ)) pz = groundZ;

            const NiPoint3 next(px, py, pz + 45.0f);
            const float sx = next.x - previous.x;
            const float sy = next.y - previous.y;
            const float sz = next.z - previous.z;
            const float length = sqrtf(sx * sx + sy * sy + sz * sz);

            if(length > 1.0f)
            {
                const NiPoint3 direction(sx / length, sy / length, sz / length);
                const Havok::Result r = Havok::ShapeCast(previous, direction, length, 20.0f, Havok::kLayer_Transparent, npc);

                for(UInt32 h = 0; h < r.hitCount; ++h)
                {
                    const Havok::Hit & hit = r.hits[h];
                    if(hit.layer == Havok::kLayer_CharController || hit.layer == Havok::kLayer_Biped) continue;
                    if(hit.normal.z > 0.6f) continue;                       // ground to walk on
                    if(hit.distance < length - 10.0f) return false;
                }
            }

            previous = next;
        }

        return true;
    }

    // Where she joins the player's trail.
    //
    // Walking mode: the node nearest her, if the trail from there to the player
    // is whole. Moves-only mode (the default): the nearest MOVE of the player's
    // whose start mark is within reach, that would lift her, and with a clear way
    // to its start mark. She is not driven anywhere else: the AI brings her to
    // the foot of the wall on its own, since it is following the player there.
    bool FindTrailEntry(Actor * npc, const NiPoint3 & pos, UInt32 & entry, float & distance, const char *& why)
    {
        if(!s_trailWalk)
        {
            float best = 1.0e9f;
            UInt32 bestIndex = 0;
            bool found = false;

            for(UInt32 i = 0; i < s_trailCount; ++i)
            {
                const TrailNode & node = s_trail[i];
                if(!node.isMove || node.type == Parkour::kNoLedge) continue;

                const float rise = (node.hasEnd ? node.end.z : node.pos.z + 80.0f) - pos.z;
                if(rise < 40.0f) continue;                      // it would not lift her

                const float dx = pos.x - node.pos.x;
                const float dy = pos.y - node.pos.y;
                const float dz = pos.z - node.pos.z;
                const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if(d < best) { best = d; bestIndex = i; found = true; }
            }

            if(!found || best > s_trailMoveRadius)
            {
                why = "no move of the player's starts within her reach (or none would lift her)";
                return false;
            }

            if(!PathClear(npc, pos, s_trail[bestIndex].pos))
            {
                why = "something solid is between her and that move's start mark";
                return false;
            }

            entry    = bestIndex;
            distance = best;
            return true;
        }

        float best = 1.0e9f;
        UInt32 bestIndex = 0;

        for(UInt32 i = 0; i < s_trailCount; ++i)
        {
            const float dx = pos.x - s_trail[i].pos.x;
            const float dy = pos.y - s_trail[i].pos.y;
            const float dz = pos.z - s_trail[i].pos.z;
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if(d < best) { best = d; bestIndex = i; }
        }

        if(best > s_trailJoinRadius)
        {
            why = "no point of the player's path is within her reach";
            return false;
        }

        for(UInt32 i = bestIndex; i + 1 < s_trailCount; ++i)
        {
            if(!TrailSegmentOk(s_trail[i], s_trail[i + 1]))
            {
                why = "the player's path has a gap from there on (a plain jump or a fall)";
                return false;
            }
        }

        entry    = bestIndex;
        distance = best;
        return true;
    }

    void StartTrail(Actor * npc, UInt32 entry, float entryDistance, float heightGap, float distance)
    {
        const DWORD now = GetTickCount();

        UInt32 moves = 0;
        for(UInt32 i = entry; i < s_trailCount; ++i)
            if(s_trail[i].isMove) ++moves;

        if(s_trailWalk)
            _MESSAGE("NPC track %08X: the player is %.0f above her, %.0f away, and she is not getting closer: "
                     "following his path from node %u of %u (%.0f units from her), %u moves ahead",
                     (unsigned int)s_trackedId, heightGap, distance, (unsigned int)entry,
                     (unsigned int)s_trailCount, entryDistance, (unsigned int)moves);
        else
            _MESSAGE("NPC track %08X: the player is %.0f above her, %.0f away, and she is not getting closer: "
                     "his %s (node %u of %u) starts %.0f units from her: going to its start mark to repeat it",
                     (unsigned int)s_trackedId, heightGap, distance, TypeName(s_trail[entry].type),
                     (unsigned int)entry, (unsigned int)s_trailCount, entryDistance);

        s_manual        = false;
        s_npc           = npc;
        s_state         = kTrail;
        s_planValid     = false;
        s_trailRun      = true;
        s_trailCall     = false;
        s_callAttemptsMade = 0;
        s_walkInit      = false;
        s_approachDeadline = 0;
        s_trailIdx      = entry;
        s_trailStart    = now;
        s_trailLastTick = now;
        s_trailMoves    = 0;

        void * ctrl = Engine::CharController(npc);
        if(ctrl)
            Engine::ControllerFlags(ctrl) |= Engine::Controller::kFlag_NoSim;
    }

    void TrailEnd(Actor * npc, const char * why, bool release)
    {
        _MESSAGE("NPC trail: %s after %lu ms, %u moves repeated",
                 why, (unsigned long)(GetTickCount() - s_trailStart), (unsigned int)s_trailMoves);

        if(release && npc)
            ReleaseNpc(npc, false);

        s_trailRun      = false;
        s_trailCall     = false;
        s_state         = kIdle;
        s_npc           = NULL;
        s_planValid     = false;
        s_cooldownUntil = GetTickCount() + (DWORD)(s_autoCooldownSec * 1000.0f);
        s_anchorReset   = true;
        ResetCorrection();
    }

    // The ground under (x, y), near height z, from a ray cast straight down.
    bool GroundAt(Actor * npc, float x, float y, float z, float & outZ)
    {
        const NiPoint3 above(x, y, z + 80.0f);
        const NiPoint3 down(0.0f, 0.0f, -1.0f);
        const Havok::Result r = Havok::RayCast(above, down, 240.0f, Havok::kLayer_Transparent, npc);

        for(UInt32 h = 0; h < r.hitCount; ++h)
        {
            const Havok::Hit & hit = r.hits[h];
            if(hit.layer == Havok::kLayer_CharController || hit.layer == Havok::kLayer_Biped) continue;
            outZ = hit.position.z;
            return true;
        }
        return false;
    }

    // One step toward `target` (game coordinates) at `speed` game units a second.
    // Returns how far is left, FLAT, after the step.
    //
    // The position is her own: kept in s_walkPos, advanced by the step, and written
    // to the controller every tick. It is not read back from the controller, because
    // her AI keeps moving her too, and a step taken from wherever the AI had put her
    // last made the path a tangle: the spinning, the sliding, the 25 seconds spent
    // circling a mark she never reached. She is also turned along her way by her
    // own heading, only while she still has some way to go (a heading taken from
    // a mark she is standing on spins), at a limited rate. With `snap` set she is
    // kept on the ground under her.
    float TrailMoveToward(Actor * npc, void * ctrl, const NiPoint3 & target, float dt, float speed, bool snap)
    {
        const float scale = Engine::HavokScale();

        if(!s_walkInit)
        {
            __declspec(align(16)) float current[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            Engine::ControllerGetPosition(ctrl, current, true);
            s_walkPos[0] = current[0];
            s_walkPos[1] = current[1];
            s_walkPos[2] = current[2];
            s_walkYaw    = Engine::Rotation(npc).z;
            s_walkInit   = true;
        }

        const float mx = target.x * scale - s_walkPos[0];
        const float my = target.y * scale - s_walkPos[1];
        const float mz = target.z * scale - s_walkPos[2];
        const float flat = sqrtf(mx * mx + my * my);
        const float flatGame = flat / scale;

        const float stepGame = speed * dt;
        const float k = (flatGame <= stepGame || flat < 0.0001f) ? 1.0f : stepGame / flatGame;

        s_walkPos[0] += mx * k;
        s_walkPos[1] += my * k;

        float groundZ = 0.0f;
        if(snap && GroundAt(npc, s_walkPos[0] / scale, s_walkPos[1] / scale, s_walkPos[2] / scale, groundZ))
            s_walkPos[2] = groundZ * scale;
        else
            s_walkPos[2] += mz * k;

        __declspec(align(16)) float next[4];
        next[0] = s_walkPos[0];
        next[1] = s_walkPos[1];
        next[2] = s_walkPos[2];
        next[3] = 0.0f;
        Engine::ControllerSetPosition(ctrl, next, true, false);

        if(flatGame > 12.0f)
        {
            const float want = atan2f(mx, my);
            float diff = want - s_walkYaw;
            while(diff >  3.14159265f) diff -= 6.28318531f;
            while(diff < -3.14159265f) diff += 6.28318531f;

            const float maxTurn = 5.0f * dt;                    // radians a second
            if(diff >  maxTurn) diff =  maxTurn;
            if(diff < -maxTurn) diff = -maxTurn;
            s_walkYaw += diff;
        }
        SetHeading(npc, ctrl, s_walkYaw);

        return (flatGame <= stepGame) ? 0.0f : flatGame - stepGame;
    }

    // ---- step 4: landing, and answering the player's moves -------------------

    bool IsClimbType(int type)
    {
        return type == Parkour::kLow || type == Parkour::kMedium ||
               type == Parkour::kHigh || type == Parkour::kHighest;
    }

    // Begins the eased move to `to` (game coordinates).
    void StartGlide(Actor * who, void * ctrl, const NiPoint3 & to)
    {
        const float scale = Engine::HavokScale();

        __declspec(align(16)) float current[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Engine::ControllerGetPosition(ctrl, current, true);

        s_glide.who      = who;
        s_glide.isPlayer = (who == (Actor *)Engine::Player());
        s_glide.from[0]  = current[0];
        s_glide.from[1]  = current[1];
        s_glide.from[2]  = current[2];
        s_glide.to[0]    = to.x * scale;
        s_glide.to[1]    = to.y * scale;
        s_glide.to[2]    = to.z * scale;
        s_glide.start    = GetTickCount();
        s_glide.ms       = (DWORD)s_landingGlideMs;
        s_glide.active   = true;
    }

    // Once per tick, while a glide is running.
    void TickGlide(DWORD now)
    {
        if(!s_glide.active) return;

        if(!s_glide.isPlayer && !SafeUsable(s_glide.who))
        {
            s_glide.active = false;
            return;
        }

        void * ctrl = Engine::CharController(s_glide.who);
        if(!ctrl)
        {
            s_glide.active = false;
            return;
        }

        float s = (s_glide.ms == 0) ? 1.0f : (float)(now - s_glide.start) / (float)s_glide.ms;
        if(s > 1.0f) s = 1.0f;
        const float e = s * s * (3.0f - 2.0f * s);              // eased in and out

        __declspec(align(16)) float target[4];
        for(int i = 0; i < 3; ++i)
            target[i] = s_glide.from[i] + (s_glide.to[i] - s_glide.from[i]) * e;
        target[3] = 0.0f;
        Engine::ControllerSetPosition(ctrl, target, true, false);

        __declspec(align(16)) float stopped[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Engine::ControllerSetLinearVelocity(ctrl, stopped);

        if(s >= 1.0f)
            s_glide.active = false;
    }

    // Rests someone who has just finished a climb ON the surface: a step past the
    // edge, on the ground there, reached with a short glide rather than a jump. The
    // animation ends at the lip, and a body left at the lip tends to slide back
    // down - which is what the player sees, and what made the follower give up and
    // take the long way round instead of trying again. The spot is checked before
    // anyone is moved: there has to be ground there at the height of the edge, the
    // way to it has to be free of anything solid, and it has to be close to where
    // the body already is. Returns true if a move was begun; `outLanding` is where
    // it goes.
    bool ApplyLanding(Actor * who, const NiPoint3 & ledge, float yaw, NiPoint3 * outLanding)
    {
        if(!s_landing || !who) return false;

        void * ctrl = Engine::CharController(who);
        if(!ctrl) return false;

        const NiPoint3 here = Engine::Position(who);
        if(here.z < ledge.z - 50.0f) return false;              // not up yet: a move that was interrupted

        const float fx = sinf(yaw);
        const float fy = cosf(yaw);
        const float tries[2] = { s_landingForward, s_landingForward * 0.5f };

        for(int t = 0; t < 2; ++t)
        {
            const float lx = ledge.x + fx * tries[t];
            const float ly = ledge.y + fy * tries[t];

            float groundZ = 0.0f;
            if(!GroundAt(who, lx, ly, ledge.z, groundZ)) continue;
            if(fabsf(groundZ - ledge.z) > 40.0f) continue;      // not the same surface

            const NiPoint3 landing(lx, ly, groundZ + 1.0f);

            const float dx = landing.x - here.x;
            const float dy = landing.y - here.y;
            const float dz = landing.z - here.z;
            const float moveBy = sqrtf(dx * dx + dy * dy + dz * dz);

            if(moveBy > 80.0f) continue;                        // nowhere near where it should be
            if(moveBy < 4.0f) return false;                     // already there
            if(!PathClear(who, here, landing)) continue;        // something solid in the way

            StartGlide(who, ctrl, landing);
            if(outLanding) *outLanding = landing;

            _MESSAGE("NPC landing: moving %.0f units over %d ms to rest on top of the edge (%.0f past it)",
                     moveBy, s_landingGlideMs, tries[t]);
            return true;
        }

        _MESSAGE("NPC landing: no safe spot past the edge: left where the animation ended");
        return false;
    }

    // The first of the player's moves that is still waiting for an answer, if she
    // can answer it. Moves are answered in the order he made them. A move she has
    // already got past, or that would not lift her, counts as answered.
    bool FindCall(Actor * npc, const NiPoint3 & pos, DWORD now, UInt32 & index, float & markDistance, const char *& why)
    {
        why = NULL;

        for(UInt32 i = 0; i < s_trailCount; ++i)
        {
            const TrailNode & node = s_trail[i];
            if(!node.isMove || node.type == Parkour::kNoLedge) continue;       // (kNoLedge: it was interrupted)
            if(node.time <= s_callAfterTime) continue;                          // answered
            if(now - node.time > (DWORD)(s_callWindowSec * 1000.0f)) continue;  // too long ago
            if(node.type == Parkour::kStepLow || node.type == Parkour::kStepHigh) continue;     // she steps up by herself

            const float dx = pos.x - node.pos.x;
            const float dy = pos.y - node.pos.y;
            const float horizontal = sqrtf(dx * dx + dy * dy);

            // Already past the point he made the move from, along the way he went.
            const float along = dx * sinf(node.yaw) + dy * cosf(node.yaw);
            if(along > 60.0f) { s_callAfterTime = node.time; continue; }

            // Already as high as that climb took him.
            if(IsClimbType(node.type) && node.hasEnd && node.end.z - pos.z < 40.0f)
            {
                s_callAfterTime = node.time;
                continue;
            }

            if(horizontal > s_callRadius)
            {
                why = "she is too far from where the player made his move";
                return false;
            }

            if(!PathClear(npc, pos, node.pos))
            {
                why = "something solid is between her and the place he made it";
                return false;
            }

            index           = i;
            markDistance    = horizontal;
            return true;
        }

        return false;
    }

    void StartCall(Actor * npc, UInt32 index, float markDistance, float playerDistance)
    {
        const DWORD now = GetTickCount();
        const TrailNode & node = s_trail[index];

        _MESSAGE("NPC track %08X: the player made a %s %.1f s ago, %.0f units from her (%.0f from him): "
                 "she goes to the same spot and does it too",
                 (unsigned int)s_trackedId, TypeName(node.type), (float)(now - node.time) / 1000.0f,
                 markDistance, playerDistance);

        s_manual            = false;
        s_npc               = npc;
        s_state             = kTrail;
        s_planValid         = false;
        s_trailRun          = true;
        s_trailCall         = true;
        s_trailIdx          = index;
        s_callNodeTime      = node.time;
        s_callAttemptsMade  = 0;
        s_trailStart        = now;
        s_trailLastTick     = now;
        s_lastPathCheck     = now;
        s_trailMoves        = 0;
        s_walkInit          = false;
        s_approachDeadline  = now + (DWORD)((markDistance / s_trailSpeed) * 1000.0f + 4000.0f);

        void * ctrl = Engine::CharController(npc);
        if(ctrl)
            Engine::ControllerFlags(ctrl) |= Engine::Controller::kFlag_NoSim;
    }

    void TrailStep(Actor * npc, DWORD now)
    {
        Actor * player = (Actor *)Engine::Player();
        if(!player)
        {
            TrailEnd(npc, "there is no player", true);
            return;
        }

        if(now - s_trailStart > (DWORD)((s_trailCall ? 25.0f : s_trailMaxSec) * 1000.0f))
        {
            TrailEnd(npc, "took too long, stopped", true);
            return;
        }

        if(Parkour::IsKnockedOut(npc) || Parkour::IsStaggering(npc))
        {
            TrailEnd(npc, "she was knocked down, stopped", true);
            return;
        }

        if(Parkour::IsWeaponOut(npc))
        {
            TrailEnd(npc, "she drew her weapon: a fight is left to the AI", true);
            return;
        }

        void * ctrl = Engine::CharController(npc);
        if(!ctrl)
        {
            TrailEnd(npc, "she has no character controller", false);
            return;
        }

        float dt = (float)(now - s_trailLastTick) / 1000.0f;
        s_trailLastTick = now;
        if(dt > 0.1f) dt = 0.1f;
        if(dt <= 0.0f) return;

        // Done when she is level with the player and close.
        const NiPoint3 here  = Engine::Position(npc);
        const NiPoint3 there = Engine::Position(player);
        const float px = there.x - here.x;
        const float py = there.y - here.y;
        // (Not when answering a move: next to a fence, level with him, she still has to vault it.)
        if(!s_trailCall && sqrtf(px * px + py * py) <= s_trailEndRadius && fabsf(there.z - here.z) <= 100.0f)
        {
            TrailEnd(npc, "she caught up with the player", true);
            return;
        }

        if(s_trailIdx >= s_trailCount)
        {
            TrailEnd(npc, "the end of the player's path", true);
            return;
        }

        const TrailNode node = s_trail[s_trailIdx];

        // Answering a move she may have a way to go to its start mark, on foot: the
        // way is checked again every so often, and she is kept on the ground.
        if(s_trailCall && now - s_lastPathCheck >= 400)
        {
            s_lastPathCheck = now;
            if(!PathClear(npc, here, node.pos))
            {
                s_callAfterTime = s_callNodeTime;
                TrailEnd(npc, "the way to the start mark is blocked", true);
                return;
            }
        }

        // She has a limited time to get to the mark: a way that took 25 seconds
        // meant she was not going to.
        if(s_trailCall && s_approachDeadline != 0 && now > s_approachDeadline)
        {
            s_callAfterTime = s_callNodeTime;
            TrailEnd(npc, "could not get to the start mark in time", true);
            return;
        }

        const bool onFoot = s_trailCall || s_trailWalk;
        const float left = TrailMoveToward(npc, ctrl, node.pos, dt, onFoot ? s_trailSpeed : s_dashSpeed,
                                           s_trailCall || (s_trailWalk && !node.isMove));

        if(!node.isMove)
        {
            if(left <= 15.0f) ++s_trailIdx;
            return;
        }

        // A move is made from its start mark and nowhere else.
        if(left <= 8.0f)
        {
            ++s_trailMoves;
            _MESSAGE("NPC trail: at the start mark of the player's %s (node %u): repeating it",
                     TypeName(node.type), (unsigned int)s_trailIdx);

            s_planValid     = true;
            s_planType      = node.type;
            s_planStart     = node.pos;
            s_planLedge     = node.ledge;
            s_planYaw       = node.yaw;
            s_planPath      = NULL;
            if(s_pathReplay && node.path >= 0 && node.path < (int)kPathMax &&
               s_paths[node.path].nodeTime == node.time && s_paths[node.path].count > 3)
            {
                s_planPath = &s_paths[node.path];
                _MESSAGE("NPC trail: carried along the path his body took (%u samples, %u ms)",
                         (unsigned int)s_planPath->count, (unsigned int)s_planPath->t[s_planPath->count - 1]);
            }
            else
            {
                s_planPath = BuildTemplatePath((int)node.type, node.yaw);
                if(s_planPath)
                    _MESSAGE("NPC trail: no path of his own for that one: carried along the template for a %s",
                             TypeName(node.type));
            }
            s_walkInit      = false;
            s_state         = kAligning;
            s_stateStart    = now;
            // s_trailIdx moves on when the move ends (Succeed)
        }
    }

    // What follows a move that is over and, if it was a climb, landed.
    void FinishSuccess(Actor * npc, DWORD now)
    {
        if(s_trailRun && !s_trailWalk)
        {
            // Did she end up on top? If not, she goes back to the start mark and
            // tries again instead of being left to the AI, which sends her the
            // long way round.
            bool good = true;
            if(IsClimbType(s_type))
            {
                const NiPoint3 final = Engine::Position(npc);
                if(s_planLedge.x != 0.0f || s_planLedge.y != 0.0f)
                    good = final.z >= s_planLedge.z - 30.0f;
                else
                    good = (final.z - s_posBefore.z) >= 60.0f;
            }

            if(!good && (int)s_callAttemptsMade + 1 < s_callAttempts)
            {
                ++s_callAttemptsMade;
                _MESSAGE("NPC trail: she did not end up on top: back to the start mark to try again (attempt %u of %d)",
                         (unsigned int)(s_callAttemptsMade + 1), s_callAttempts);
                s_state             = kTrail;
                s_planValid         = false;
                s_trailLastTick     = now;
                s_walkInit          = false;
                s_approachDeadline  = now + 8000;
                ResetCorrection();
                return;
            }

            // One move repeated is the whole job. The AI takes her from here, and
            // the next move of the player's gets its own entry, after a short pause.
            const bool answered = s_trailCall;
            if(answered)
                s_callAfterTime = s_callNodeTime;

            TrailEnd(npc, good ? "repeated the player's move" : "gave up on the player's move after trying", true);
            s_cooldownUntil = GetTickCount() + (answered ? 500 : 2000);
            return;
        }

        if(s_trailRun)
        {
            ++s_trailIdx;
            s_state         = kTrail;
            s_trailLastTick = now;
            s_planValid     = false;
            s_walkInit      = false;
            ResetCorrection();
            return;
        }

        const Parkour::Type type = s_type;

        EndTest(true, true);

        char text[512];
        sprintf_s(text, sizeof(text),
                  "FreeRun NPC test: finished.\n\nMove: %s\nTime: %lu ms\nHeight change: %.0f units\n\n"
                  "If it did not look right, send FreerunLE.log.",
                  TypeName(type), (unsigned long)s_lastTook, s_lastRise);
        if(s_manual) ShowText(text);
    }

    void Succeed(Actor * npc, DWORD now)
    {
        const NiPoint3 after = Engine::Position(npc);
        s_lastTook = now - s_actionStart;
        s_lastRise = after.z - s_posBefore.z;

        _MESSAGE("NPC test: finished after %lu ms, moved from (%.0f, %.0f, %.0f) to (%.0f, %.0f, %.0f)",
                 (unsigned long)s_lastTook, s_posBefore.x, s_posBefore.y, s_posBefore.z, after.x, after.y, after.z);

        // The animation ends at the lip of the edge: she is eased onto the top of
        // it, and what follows waits until she has got there.
        if(s_landing && IsClimbType(s_type))
        {
            if(ApplyLanding(npc, s_planLedge, Engine::Rotation(npc).z, NULL))
            {
                s_state = kGlide;
                return;
            }
        }

        FinishSuccess(npc, now);
    }

    void Step()
    {
        Actor * npc = s_npc;

        if(!SafeUsable(npc))
        {
            _ERROR("NPC test: the NPC is gone, dead or no longer loaded: stopping without touching it");
            EndTest(false, false);
            return;
        }

        const Graph::Names & n = Graph::GetNames();
        const DWORD now = GetTickCount();

        switch(s_state)
        {
            case kAligning:
                if(s_planValid)
                {
                    // On the mark, facing the way the move goes: written every tick,
                    // and the move is only sent once her facing really matches.
                    PinPlan(npc);

                    const NiPoint3 f = Parkour::FacingFlat(npc);
                    const bool faces = (f.x * sinf(s_planYaw) + f.y * cosf(s_planYaw)) >= 0.985f;     // about ten degrees
                    const DWORD waited = now - s_stateStart;

                    if((faces && waited >= 50) || waited >= 600)
                    {
                        if(!faces)
                            _MESSAGE("NPC test: her facing never matched the move's heading: going on anyway");
                        ProbeAndStart();
                    }
                    return;
                }

                if(now - s_stateStart >= kAlignMs)
                    ProbeAndStart();
                return;

            case kGlide:
                if(!s_glide.active)
                    FinishSuccess(npc, now);
                return;

            case kTrail:
                TrailStep(npc, now);
                return;

            case kWaitStart:
            {
                if(s_planPath)          ReplayPath(npc, now);       // carried along his path from the first tick
                else if(s_planValid)    PinPlan(npc);               // held on the mark until the animation takes over
                else                    RunCorrection(npc);

                if(Graph::GetBool(npc, n.ongoing))
                {
                    _MESSAGE("NPC test: the behaviour took the event, the move started after %lu ms",
                             (unsigned long)(now - s_stateStart));

                    // From here the animation's root motion moves the body.
                    void * ctrl = Engine::CharController(npc);
                    if(ctrl)
                        Engine::ControllerFlags(ctrl) |= Engine::Controller::kFlag_NoSim;

                    s_state      = kRunning;
                    s_stateStart = now;
                    return;
                }

                if(now - s_stateStart >= kStartTimeoutMs)
                    Fail("The NPC's animation graph never entered the parkour state.\n\n"
                         "Either FreeRun's behaviour patch does not apply to NPCs, or this NPC's behaviour ignores the event. "
                         "The log says what the graph reported.", true);
                return;
            }

            case kRunning:
            {
                if(s_planPath)          ReplayPath(npc, now);
                else if(!s_planValid)   RunCorrection(npc);     // (with a plan, she is already on the mark)

                if(Parkour::IsKnockedOut(npc) || Parkour::IsStaggering(npc))
                {
                    Fail("The NPC was knocked down or staggered during the move.", true);
                    return;
                }

                if(now - s_lastTrace >= kTraceEveryMs)
                {
                    const NiPoint3 p = Engine::Position(npc);
                    void * ctrl = Engine::CharController(npc);
                    _MESSAGE("NPC test:   %lu ms: at (%.0f, %.0f, %.0f), controller state %d",
                             (unsigned long)(now - s_actionStart), p.x, p.y, p.z,
                             ctrl ? (int)Engine::ControllerState(ctrl) : -1);
                    s_lastTrace = now;
                }

                if(!Graph::GetBool(npc, n.ongoing))
                {
                    Succeed(npc, now);
                    return;
                }

                if(now - s_actionStart >= kActionTimeoutMs)
                    Fail("The move did not finish in time, so it was stopped.", true);
                return;
            }

            default:
                return;
        }
    }

    // ---- step 2: the tracked NPC --------------------------------------------
    //
    // One NPC is watched - the follower the player pointed at with the track
    // key. There is no list of the actors around to pick from, and a ray cast
    // around the player would miss anything not squarely in its path, so the
    // NPC is chosen once, by aiming, and followed from then on.
    //
    // WHAT THE FIRST LOG TAUGHT. A follower that cannot reach the player does
    // not stand still against the obstacle: the navigation mesh sends her the
    // long way round, so she RUNS - the animation's own speed stays at about 1.0
    // - while the distance to the player grows, for ten seconds at a time. The
    // first version waited for "standing still" and never saw it. Two things
    // are watched now:
    //
    //   detour   she is running (relative speed 0.5 or more) and has come no
    //            closer to the player in the last few seconds;
    //   pushing  she is running on the spot: still for a while, speed up.
    //
    // While either holds, the ledge search is run twice, twice a second: once
    // the way she is facing, and once straight toward the player. The second is
    // the one that matters for a shortcut: the first obstacle between them, if
    // it is something the player's own search would climb and she is already at
    // its foot, is the shortcut.

    enum { kHistory = 40 };                 // 4 s of distances at 10 samples a second

    DWORD   s_histTime[kHistory];
    float   s_histDist[kHistory];
    UInt32  s_histCount     = 0;
    UInt32  s_histNext      = 0;

    Parkour::Type   s_probeToward       = Parkour::kNoLedge;    // the last search toward the player
    float           s_probeTowardGap    = 0.0f;                 // how far she is from its start mark
    bool            s_haveProbeToward   = false;

    void HistoryClear()
    {
        s_histCount = 0;
        s_histNext  = 0;
    }

    void HistoryPush(DWORD now, float distance)
    {
        s_histTime[s_histNext] = now;
        s_histDist[s_histNext] = distance;
        s_histNext = (s_histNext + 1) % kHistory;
        if(s_histCount < (UInt32)kHistory) ++s_histCount;
    }

    // The distance `ms` ago, if the history reaches back that far.
    bool HistoryAgo(DWORD now, DWORD ms, float & out)
    {
        for(UInt32 i = 0; i < s_histCount; ++i)
        {
            const UInt32 index = (s_histNext + kHistory - s_histCount + i) % kHistory;
            if(now - s_histTime[index] <= ms)
            {
                if(i == 0) return false;                                // the history is shorter than the window
                if(now - s_histTime[index] < ms - ms / 5) return false; // and not close to a full window
                out = s_histDist[index];
                return true;
            }
        }
        return false;
    }

    // ---- several tracked NPCs ------------------------------------------------
    //
    // Up to three NPCs can be tracked. What belongs to one NPC - where she was
    // last, how long she has stood still, her history of distances, her pauses,
    // which of the player's moves she has answered - lives in a slot, and the
    // code that works on "the tracked NPC" keeps working on the globals above:
    // the slot is copied in before her turn and back out after it. Only one of
    // them runs a move at a time, and the others wait their turn: he made the
    // move once, and they follow one after another.

    enum { kMaxTracked = 3 };

    struct Tracked
    {
        Actor *         tracked;
        UInt32          trackedId;
        NiPoint3        anchor;
        DWORD           anchorTime;
        bool            anchorReset;
        DWORD           lastSample;
        DWORD           lastProbe;
        DWORD           lastTrackTrace;
        DWORD           cooldownUntil;
        Parkour::Type   lastProbeType;
        bool            haveProbe;
        Parkour::Type   probeToward;
        float           probeTowardGap;
        bool            haveProbeToward;
        DWORD           histTime[kHistory];
        float           histDist[kHistory];
        UInt32          histCount;
        UInt32          histNext;
        DWORD           callAfterTime;
        DWORD           lastCallCheck;
        DWORD           lastCallWhyLog;
        DWORD           lastTrailCheck;
        DWORD           lastTrailWhyLog;
    };

    Tracked s_slots[kMaxTracked];

    void LoadSlot(int i)
    {
        const Tracked & s = s_slots[i];
        s_tracked = s.tracked;                  s_trackedId = s.trackedId;
        s_anchor = s.anchor;                    s_anchorTime = s.anchorTime;        s_anchorReset = s.anchorReset;
        s_lastSample = s.lastSample;            s_lastProbe = s.lastProbe;          s_lastTrackTrace = s.lastTrackTrace;
        s_cooldownUntil = s.cooldownUntil;
        s_lastProbeType = s.lastProbeType;      s_haveProbe = s.haveProbe;
        s_probeToward = s.probeToward;          s_probeTowardGap = s.probeTowardGap;    s_haveProbeToward = s.haveProbeToward;
        memcpy(s_histTime, s.histTime, sizeof(s_histTime));
        memcpy(s_histDist, s.histDist, sizeof(s_histDist));
        s_histCount = s.histCount;              s_histNext = s.histNext;
        s_callAfterTime = s.callAfterTime;      s_lastCallCheck = s.lastCallCheck;  s_lastCallWhyLog = s.lastCallWhyLog;
        s_lastTrailCheck = s.lastTrailCheck;    s_lastTrailWhyLog = s.lastTrailWhyLog;
    }

    void SaveSlot(int i)
    {
        Tracked & s = s_slots[i];
        s.tracked = s_tracked;                  s.trackedId = s_trackedId;
        s.anchor = s_anchor;                    s.anchorTime = s_anchorTime;        s.anchorReset = s_anchorReset;
        s.lastSample = s_lastSample;            s.lastProbe = s_lastProbe;          s.lastTrackTrace = s_lastTrackTrace;
        s.cooldownUntil = s_cooldownUntil;
        s.lastProbeType = s_lastProbeType;      s.haveProbe = s_haveProbe;
        s.probeToward = s_probeToward;          s.probeTowardGap = s_probeTowardGap;    s.haveProbeToward = s_haveProbeToward;
        memcpy(s.histTime, s_histTime, sizeof(s_histTime));
        memcpy(s.histDist, s_histDist, sizeof(s_histDist));
        s.histCount = s_histCount;              s.histNext = s_histNext;
        s.callAfterTime = s_callAfterTime;      s.lastCallCheck = s_lastCallCheck;  s.lastCallWhyLog = s_lastCallWhyLog;
        s.lastTrailCheck = s_lastTrailCheck;    s.lastTrailWhyLog = s_lastTrailWhyLog;
    }

    int SlotOf(Actor * npc)
    {
        for(int i = 0; i < kMaxTracked; ++i)
            if(s_slots[i].tracked == npc) return i;
        return -1;
    }

    // ---- stranded ------------------------------------------------------------
    //
    // The last log had a follower standing on a rock for 98 seconds, her speed at
    // 0.00 - her AI not asking her to move at all - while the player went 3,500
    // units away. Nothing in what this module does explains an AI that has simply
    // stopped, and the likeliest reason is the plainest: the top of a rock is not
    // on the navigation mesh, so there is no path from where she stands to anyone.
    // As a last resort, after she has stood idle for long enough and far enough
    // from the player, she is put back on the player's own trail a little way
    // behind him: a place he stood on, so a place the AI can path from.

    bool FindRescueSpot(const NiPoint3 & playerPos, NiPoint3 & out)
    {
        for(UInt32 k = s_trailCount; k > 0; --k)
        {
            const TrailNode & node = s_trail[k - 1];
            if(node.isMove) continue;

            const float dx = node.pos.x - playerPos.x;
            const float dy = node.pos.y - playerPos.y;
            const float flat = sqrtf(dx * dx + dy * dy);
            if(flat < 180.0f || flat > 450.0f) continue;
            if(fabsf(node.pos.z - playerPos.z) > 150.0f) continue;

            out = node.pos;
            return true;
        }
        return false;
    }

    void RescueTeleport(Actor * npc, const NiPoint3 & spot, float stillFor, float distance)
    {
        void * ctrl = Engine::CharController(npc);
        if(!ctrl) return;

        const float scale = Engine::HavokScale();
        __declspec(align(16)) float target[4];
        target[0] = spot.x * scale;
        target[1] = spot.y * scale;
        target[2] = spot.z * scale;
        target[3] = 0.0f;
        Engine::ControllerSetPosition(ctrl, target, true, false);

        __declspec(align(16)) float stopped[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Engine::ControllerSetLinearVelocity(ctrl, stopped);
        Engine::ControllerFlags(ctrl) &= ~Engine::Controller::kFlag_NoSim;

        _MESSAGE("NPC track %08X: stranded: she has stood still for %.0f s, %.0f units from the player, her AI not asking "
                 "her to move: put back on his trail, a little way behind him", (unsigned int)s_trackedId, stillFor, distance);

        s_cooldownUntil = GetTickCount() + 20000;
        s_anchorReset   = true;
    }

    bool TrackedAlive(Actor * npc, UInt32 id)
    {
        __try
        {
            if(!npc) return false;
            if(npc->formID != id) return false;
            if(npc->formType != kFormType_Character) return false;
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void StopTracking(const char * why)
    {
        if(s_tracked)
            _MESSAGE("NPC track: stopped tracking %08X (%s)", (unsigned int)s_trackedId, why);
        s_tracked   = NULL;
        s_trackedId = 0;
    }

    void HandleTrack(Actor * player)
    {
        if(Parkour::MenuOpen()) return;

        Actor * npc = FindTarget(player);
        if(!npc)
        {
            _MESSAGE("NPC track: no NPC found in front of the player");
            ShowText("FreeRun NPC: no NPC found where you are looking.\n\nLook straight at the NPC you want to track (the camera aims in third person).");
            return;
        }

        const int existing = SlotOf(npc);
        if(existing >= 0)
        {
            _MESSAGE("NPC track: stopped tracking %08X (asked to stop)", (unsigned int)s_slots[existing].trackedId);
            s_slots[existing].tracked   = NULL;
            s_slots[existing].trackedId = 0;
            ShowText("FreeRun NPC: no longer tracking this NPC.");
            return;
        }

        int slot = -1;
        for(int i = 0; i < kMaxTracked; ++i)
            if(!s_slots[i].tracked) { slot = i; break; }

        if(slot < 0)
        {
            ShowText("FreeRun NPC: three NPCs are already tracked.\n\nPress the key facing one of them to stop tracking it first.");
            return;
        }

        Tracked & s = s_slots[slot];
        s.tracked           = npc;
        s.trackedId         = npc->formID;
        s.anchor            = Engine::Position(npc);
        s.anchorTime        = GetTickCount();
        s.anchorReset       = true;
        s.lastSample        = 0;
        s.lastProbe         = 0;
        s.lastTrackTrace    = 0;
        s.cooldownUntil     = 0;
        s.lastProbeType     = Parkour::kNoLedge;
        s.haveProbe         = false;
        s.probeToward       = Parkour::kNoLedge;
        s.probeTowardGap    = 0.0f;
        s.haveProbeToward   = false;
        s.histCount         = 0;
        s.histNext          = 0;
        s.callAfterTime     = GetTickCount();       // what he did before she was tracked is not hers to answer
        s.lastCallCheck     = 0;
        s.lastCallWhyLog    = 0;
        s.lastTrailCheck    = 0;
        s.lastTrailWhyLog   = 0;

        int count = 0;
        for(int i = 0; i < kMaxTracked; ++i)
            if(s_slots[i].tracked) ++count;

        _MESSAGE("NPC track: now tracking %08X (%d of %d; automatic moves %s)",
                 (unsigned int)s.trackedId, count, (int)kMaxTracked, s_autoTracked ? "on" : "off, log only");

        char text[256];
        sprintf_s(text, sizeof(text),
                  "FreeRun NPC: tracking this NPC (%d of %d).\n\n%s\n\nPress the same key facing it to stop.",
                  count, (int)kMaxTracked,
                  s_autoTracked ? "When you climb or vault something, she will go there and do it too."
                                : "Log only: it will not move on its own.");
        ShowText(text);
    }

    void TrackTick(Actor * player)
    {
        const DWORD now = GetTickCount();
        if(now - s_lastSample < 100) return;
        s_lastSample = now;

        Actor * npc = s_tracked;
        if(!TrackedAlive(npc, s_trackedId))
        {
            StopTracking("the actor is gone");
            return;
        }

        if(!NpcUsable(npc))
        {
            s_anchorReset = true;               // not loaded right now: start over when it is
            HistoryClear();
            return;
        }

        const NiPoint3 pos          = Engine::Position(npc);
        const NiPoint3 playerPos    = Engine::Position(player);

        if(s_anchorReset)
        {
            s_anchor        = pos;
            s_anchorTime    = now;
            s_anchorReset   = false;
            HistoryClear();
        }

        const float ax = pos.x - s_anchor.x;
        const float ay = pos.y - s_anchor.y;
        const float az = pos.z - s_anchor.z;
        const float moved = sqrtf(ax * ax + ay * ay + az * az);
        if(moved > s_autoMaxMove)
        {
            s_anchor        = pos;
            s_anchorTime    = now;
        }
        const float stillFor = (float)(now - s_anchorTime) / 1000.0f;

        const float tx = playerPos.x - pos.x;
        const float ty = playerPos.y - pos.y;
        const float distance = sqrtf(tx * tx + ty * ty);

        HistoryPush(now, distance);

        const NiPoint3 facing = Parkour::FacingFlat(npc);
        float dot = 0.0f;
        if(distance > 1.0f) dot = (facing.x * tx + facing.y * ty) / distance;

        const float speed = Parkour::RelativeSpeed(npc);
        const bool wantsToMove = speed >= 0.5f;
        const bool inRange = distance >= s_autoMinDistance && distance <= s_autoMaxDistance;

        float before = 0.0f;
        const bool haveWindow = HistoryAgo(now, (DWORD)(s_autoDetourSec * 1000.0f), before);
        const float approach = haveWindow ? before - distance : 0.0f;       // positive: she got closer

        const bool detour  = inRange && wantsToMove && haveWindow && approach < s_autoMinApproach;
        const bool pushing = inRange && wantsToMove && stillFor >= s_autoStuckSec;

        // Being "animation driven" is allowed while only looking: the first log
        // showed it on during ordinary running, and it must not hide the data.
        // It still stops her from being set off.
        const char * why = NULL;
        // "Animation driven" is not a reason to hold back here: the first logs
        // showed it on exactly while she was running into things, which is when
        // she needs the help.
        const bool canAct       = NpcCanAct(npc, why, true);
        const bool canLook      = canAct;

        // ---- stranded: her AI is not asking her to move, and he is far -------
        if(s_strandedSec > 0.0f && s_autoTracked && stillFor >= s_strandedSec && speed < 0.15f &&
           distance >= s_strandedMinDistance && canAct && !Parkour::IsWeaponOut(npc) &&
           now >= s_cooldownUntil && !Parkour::MenuOpen() && s_trailCount >= 3)
        {
            NiPoint3 spot(0.0f, 0.0f, 0.0f);
            if(FindRescueSpot(playerPos, spot))
            {
                RescueTeleport(npc, spot, stillFor, distance);
                return;
            }
        }

        // ---- the player's move: she answers it ------------------------------
        //
        // He climbed or vaulted something. If she is behind him and close enough,
        // she goes to the same spot and does the same, instead of walking round.
        if(s_autoTracked && s_trailEnabled && s_callEnabled && canAct && !Parkour::IsWeaponOut(npc) &&
           !Parkour::MenuOpen() && now >= s_cooldownUntil && now - s_lastCallCheck >= 200)
        {
            s_lastCallCheck = now;

            UInt32 callIndex = 0;
            float markDistance = 0.0f;
            const char * noCall = NULL;

            if(FindCall(npc, pos, now, callIndex, markDistance, noCall))
            {
                StartCall(npc, callIndex, markDistance, distance);
                return;
            }

            if(noCall && now - s_lastCallWhyLog >= 2000)
            {
                s_lastCallWhyLog = now;
                _MESSAGE("NPC track %08X: the player made a move but she does not answer it: %s",
                         (unsigned int)s_trackedId, noCall);
            }
        }

        // ---- the player's trail: the way he went up -------------------------
        //
        // The player is well above her and she is not getting closer, or is
        // running on the spot: if his path from where she is to where he is
        // can be walked, she walks it, and makes the moves he made.
        const float heightGap = playerPos.z - pos.z;
        const bool trouble = wantsToMove && ((haveWindow && approach < s_autoMinApproach) || stillFor >= s_autoStuckSec);

        if(s_autoTracked && s_trailEnabled && trouble && heightGap >= s_trailMinHeight &&
           distance <= s_autoMaxDistance && s_trailCount >= 3 && now >= s_cooldownUntil &&
           now - s_lastTrailCheck >= 500)
        {
            s_lastTrailCheck = now;

            if(canAct && !Parkour::IsWeaponOut(npc) && !Parkour::InProgress() && !Parkour::MenuOpen())
            {
                UInt32 entry = 0;
                float entryDistance = 0.0f;
                const char * noTrail = NULL;

                if(FindTrailEntry(npc, pos, entry, entryDistance, noTrail))
                {
                    StartTrail(npc, entry, entryDistance, heightGap, distance);
                    return;
                }

                if(now - s_lastTrailWhyLog >= 3000)
                {
                    s_lastTrailWhyLog = now;
                    _MESSAGE("NPC track %08X: the player is %.0f above her and she is not getting closer, "
                             "but there is no trail to follow: %s",
                             (unsigned int)s_trackedId, heightGap, noTrail ? noTrail : "?");
                }
            }
        }

        // The ledge search is the expensive part: twice a second at most, and only
        // while she is in trouble.
        bool probedNow = false;
        Parkour::Type typeOwn = Parkour::kNoLedge, typeToward = Parkour::kNoLedge;
        NiPoint3 ledgeOwn(0.0f, 0.0f, 0.0f), startOwn(0.0f, 0.0f, 0.0f);
        NiPoint3 ledgeToward(0.0f, 0.0f, 0.0f), startToward(0.0f, 0.0f, 0.0f);
        float towardGap = 0.0f;

        if((detour || pushing) && canLook && now - s_lastProbe >= 500)
        {
            float scale = 1.0f;

            // 1. the way she is facing (only meaningful when it is roughly toward the player)
            if(dot > 0.0f)
                Parkour::ProbeForActor(npc, typeOwn, ledgeOwn, startOwn, scale, NULL);

            // 2. straight toward the player
            if(distance > 1.0f)
            {
                const NiPoint3 toward(tx / distance, ty / distance, 0.0f);
                Parkour::ProbeForActor(npc, typeToward, ledgeToward, startToward, scale, &toward);

                if(typeToward != Parkour::kNoLedge && typeToward != Parkour::kFailed)
                {
                    const float gx = startToward.x - pos.x;
                    const float gy = startToward.y - pos.y;
                    const float gz = startToward.z - pos.z;
                    towardGap = sqrtf(gx * gx + gy * gy + gz * gz);
                }
            }

            s_lastProbe         = now;
            s_lastProbeType     = typeOwn;
            s_haveProbe         = true;
            s_probeToward       = typeToward;
            s_probeTowardGap    = towardGap;
            s_haveProbeToward   = true;
            probedNow           = true;
        }

        if(now - s_lastTrackTrace >= 1000)
        {
            s_lastTrackTrace = now;
            _MESSAGE("NPC track %08X: distance %.0f (%+.0f in %.0f s), still %.1f s (moved %.0f), facing %.2f, speed %.2f, "
                     "%s%s%s%s",
                     (unsigned int)s_trackedId, distance, -approach, s_autoDetourSec, stillFor, moved, dot, speed,
                     detour ? "DETOUR " : "", pushing ? "PUSHING " : "",
                     canAct ? "free to act" : (why ? why : "busy"),
                     "");
            if(s_haveProbe || s_haveProbeToward)
                _MESSAGE("NPC track %08X:   last searches: facing %s, toward the player %s (start mark %.0f away)",
                         (unsigned int)s_trackedId,
                         s_haveProbe ? TypeName(s_lastProbeType) : "-",
                         s_haveProbeToward ? TypeName(s_probeToward) : "-", s_probeTowardGap);
        }

        if(!probedNow) return;

        // Which of the two searches, if any, is a shortcut worth taking.
        Parkour::Type chosen = Parkour::kNoLedge;
        float chosenGap = 0.0f, chosenLedgeZ = 0.0f;
        NiPoint3 chosenStart(0.0f, 0.0f, 0.0f);
        NiPoint3 chosenLedge(0.0f, 0.0f, 0.0f);
        bool turn = false;
        const char * source = "";

        if(typeToward != Parkour::kNoLedge && typeToward != Parkour::kFailed)
        {
            chosen = typeToward; chosenGap = towardGap; chosenLedgeZ = ledgeToward.z; chosenStart = startToward; chosenLedge = ledgeToward;
            turn = true; source = "toward the player";
        }
        else if(typeOwn != Parkour::kNoLedge && typeOwn != Parkour::kFailed && dot > 0.0f)
        {
            const float gx = startOwn.x - pos.x, gy = startOwn.y - pos.y, gz = startOwn.z - pos.z;
            chosen = typeOwn; chosenGap = sqrtf(gx * gx + gy * gy + gz * gz); chosenLedgeZ = ledgeOwn.z; chosenStart = startOwn; chosenLedge = ledgeOwn;
            turn = false; source = "the way she faces";
        }

        if(chosen == Parkour::kNoLedge) return;

        // Climbing must not take her farther from the player's height than she
        // already is: up to a ledge toward someone above, never onto a roof
        // for someone on the street.
        const float heightNow   = fabsf(playerPos.z - pos.z);
        const float heightAfter = fabsf(playerPos.z - chosenLedgeZ);
        const bool heightOk = heightAfter <= heightNow + 20.0f;
        const bool gapOk    = chosenGap <= s_autoMaxStartGap;

        _MESSAGE("NPC track %08X: a ledge %s (%s): her start mark is %.0f away (limit %.0f), height to the player %.0f -> %.0f; %s%s%s",
                 (unsigned int)s_trackedId, TypeName(chosen), source, chosenGap, s_autoMaxStartGap, heightNow, heightAfter,
                 !s_autoTracked ? "automatic moves are off" : "",
                 (s_autoTracked && !gapOk) ? "too far from the start mark" : "",
                 (s_autoTracked && gapOk && !heightOk) ? "it would take her away from the player's height" : "");

        if(!s_autoTracked || !gapOk || !heightOk) return;
        if(!canAct) { _MESSAGE("NPC track %08X: not now: %s", (unsigned int)s_trackedId, why ? why : "busy"); return; }
        if(now < s_cooldownUntil) return;
        if(Parkour::InProgress() || Parkour::MenuOpen()) return;

        _MESSAGE("NPC track %08X: %s, %.0f units from the player: starting the move on its own",
                 (unsigned int)s_trackedId, detour ? "running round it" : "pushing against it", distance);

        StartOn(npc, turn, atan2f(tx, ty), false, chosen, chosenStart, chosenLedge);
    }

    // The access violation caught, so that whatever is wrong with a pointer
    // kept across frames costs the tracking and not the game.
    void SafeTrackTick(Actor * player)
    {
        __try
        {
            TrackTick(player);
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            _ERROR("NPC track: an exception while evaluating %08X: tracking stopped", (unsigned int)s_trackedId);
            s_tracked   = NULL;
            s_trackedId = 0;
        }
    }

    void TickInner(Actor * player)
    {
        TrailSample(player);
        TickGlide(GetTickCount());

        if(s_state == kIdle)
        {
            if(s_requested)
            {
                s_requested = false;
                if(GetTickCount() - s_requestedAt <= kRequestTtlMs)     // stale: a menu held the tick back
                    Begin(player);
                return;
            }

            if(s_trackRequested)
            {
                s_trackRequested = false;
                if(GetTickCount() - s_trackRequestedAt <= kRequestTtlMs)
                    HandleTrack(player);
                return;
            }

            for(int i = 0; i < kMaxTracked; ++i)
            {
                if(!s_slots[i].tracked) continue;

                LoadSlot(i);
                SafeTrackTick(player);
                SaveSlot(i);

                if(s_state != kIdle)            // she has started a move: the others wait
                {
                    s_active = i;
                    break;
                }
            }
            return;
        }

        if(s_active >= 0) LoadSlot(s_active);
        Step();
        if(s_active >= 0) SaveSlot(s_active);
    }
}

namespace NpcParkour
{
    void LoadSettings()
    {
        s_enabled       = GetPrivateProfileIntA(kSection, "bEnabled",       s_enabled ? 1 : 0,       kIni) != 0;
        s_testKey       = (UInt32)GetPrivateProfileIntA(kSection, "iTestKey", (int)s_testKey,        kIni);
        s_alignToPlayer = GetPrivateProfileIntA(kSection, "bAlignToPlayer", s_alignToPlayer ? 1 : 0, kIni) != 0;
        s_messages      = GetPrivateProfileIntA(kSection, "bMessages",      s_messages ? 1 : 0,      kIni) != 0;

        char buffer[64];
        GetPrivateProfileStringA(kSection, "fMaxDistance", "800.0", buffer, sizeof(buffer), kIni);
        s_maxDistance = (float)atof(buffer);
        if(s_maxDistance < 100.0f)  s_maxDistance = 100.0f;
        if(s_maxDistance > 3000.0f) s_maxDistance = 3000.0f;

        s_trackKey          = (UInt32)GetPrivateProfileIntA(kSection, "iTrackKey", (int)s_trackKey, kIni);
        s_autoTracked       = GetPrivateProfileIntA(kSection, "bAutoTracked", s_autoTracked ? 1 : 0, kIni) != 0;
        s_autoMinDistance   = ReadFloat("fAutoMinDistance", s_autoMinDistance);
        s_autoMaxDistance   = ReadFloat("fAutoMaxDistance", s_autoMaxDistance);
        s_autoStuckSec      = ReadFloat("fAutoStuckSec", s_autoStuckSec);
        s_autoMaxMove       = ReadFloat("fAutoMaxMove", s_autoMaxMove);
        s_autoCooldownSec   = ReadFloat("fAutoCooldownSec", s_autoCooldownSec);
        s_autoDetourSec     = ReadFloat("fAutoDetourSec", s_autoDetourSec);
        s_autoMinApproach   = ReadFloat("fAutoMinApproach", s_autoMinApproach);
        s_autoMaxStartGap   = ReadFloat("fAutoMaxStartGap", s_autoMaxStartGap);
        s_dashSpeed         = ReadFloat("fDashSpeed", s_dashSpeed);
        s_trailEnabled      = GetPrivateProfileIntA(kSection, "bTrail", s_trailEnabled ? 1 : 0, kIni) != 0;
        s_trailMinHeight    = ReadFloat("fTrailMinHeight", s_trailMinHeight);
        s_trailJoinRadius   = ReadFloat("fTrailJoinRadius", s_trailJoinRadius);
        s_trailSpeed        = ReadFloat("fTrailSpeed", s_trailSpeed);
        s_trailEndRadius    = ReadFloat("fTrailEndRadius", s_trailEndRadius);
        s_trailMaxSec       = ReadFloat("fTrailMaxSec", s_trailMaxSec);
        s_trailSpacing      = ReadFloat("fTrailSpacing", s_trailSpacing);
        s_trailWalk         = GetPrivateProfileIntA(kSection, "bTrailWalk", s_trailWalk ? 1 : 0, kIni) != 0;
        s_callEnabled       = GetPrivateProfileIntA(kSection, "bTrailCall", s_callEnabled ? 1 : 0, kIni) != 0;
        s_callRadius        = ReadFloat("fCallRadius", s_callRadius);
        s_callWindowSec     = ReadFloat("fCallWindowSec", s_callWindowSec);
        s_callAttempts      = GetPrivateProfileIntA(kSection, "iCallAttempts", s_callAttempts, kIni);
        s_landing           = GetPrivateProfileIntA(kSection, "bLandingAssist", s_landing ? 1 : 0, kIni) != 0;
        s_landingForward    = ReadFloat("fLandingForward", s_landingForward);
        s_landingGlideMs    = GetPrivateProfileIntA(kSection, "iLandingGlideMs", s_landingGlideMs, kIni);
        s_pathReplay        = GetPrivateProfileIntA(kSection, "bPathReplay", s_pathReplay ? 1 : 0, kIni) != 0;
        s_animDriven        = GetPrivateProfileIntA(kSection, "bAnimDriven", s_animDriven ? 1 : 0, kIni) != 0;
        s_strandedSec       = ReadFloat("fStrandedSec", s_strandedSec);
        s_strandedMinDistance = ReadFloat("fStrandedMinDistance", s_strandedMinDistance);
        s_minMoveMs         = GetPrivateProfileIntA(kSection, "iMinMoveMs", s_minMoveMs, kIni);
        if(s_strandedSec < 0.0f)        s_strandedSec = 0.0f;
        if(s_strandedSec > 0.0f && s_strandedSec < 5.0f) s_strandedSec = 5.0f;
        if(s_minMoveMs < 0)             s_minMoveMs = 0;
        if(s_minMoveMs > 3000)          s_minMoveMs = 3000;
        if(s_landingGlideMs < 0)        s_landingGlideMs = 0;
        if(s_landingGlideMs > 800)      s_landingGlideMs = 800;
        if(s_callRadius < 100.0f)       s_callRadius = 100.0f;
        if(s_callRadius > 1500.0f)      s_callRadius = 1500.0f;
        if(s_callWindowSec < 2.0f)      s_callWindowSec = 2.0f;
        if(s_callWindowSec > 30.0f)     s_callWindowSec = 30.0f;
        if(s_callAttempts < 1)          s_callAttempts = 1;
        if(s_callAttempts > 6)          s_callAttempts = 6;
        if(s_landingForward < 10.0f)    s_landingForward = 10.0f;
        if(s_landingForward > 80.0f)    s_landingForward = 80.0f;
        s_trailMoveRadius   = ReadFloat("fTrailMoveRadius", s_trailMoveRadius);
        if(s_trailMoveRadius < 30.0f)   s_trailMoveRadius = 30.0f;
        if(s_trailMoveRadius > 300.0f)  s_trailMoveRadius = 300.0f;
        if(s_dashSpeed < 100.0f)        s_dashSpeed = 100.0f;
        if(s_trailSpeed < 100.0f)       s_trailSpeed = 100.0f;
        if(s_trailSpeed > 700.0f)       s_trailSpeed = 700.0f;
        if(s_trailSpacing < 20.0f)      s_trailSpacing = 20.0f;
        if(s_trailSpacing > 120.0f)     s_trailSpacing = 120.0f;
        s_manualToward      = GetPrivateProfileIntA(kSection, "bManualTowardPlayer", s_manualToward ? 1 : 0, kIni) != 0;
        s_manualMaxGap      = ReadFloat("fManualMaxStartGap", s_manualMaxGap);
        if(s_autoDetourSec < 1.0f)      s_autoDetourSec = 1.0f;
        if(s_autoDetourSec > 3.5f)      s_autoDetourSec = 3.5f;
        if(s_autoStuckSec < 0.5f)       s_autoStuckSec = 0.5f;
        if(s_autoCooldownSec < 1.0f)    s_autoCooldownSec = 1.0f;

        _MESSAGE("NPC track: key %u, automatic moves %s, from %.0f to %.0f units away, pushing = still %.1f s (moves under %.0f), "
                 "detour = %.0f units closer or less in %.1f s, start mark within %.0f, pause %.1f s",
                 (unsigned int)s_trackKey, s_autoTracked ? "on" : "off", s_autoMinDistance, s_autoMaxDistance,
                 s_autoStuckSec, s_autoMaxMove, s_autoMinApproach, s_autoDetourSec, s_autoMaxStartGap, s_autoCooldownSec);

        _MESSAGE("NPC test: %s, key %u, align to player %d, aim distance %.0f, messages %d",
                 s_enabled ? "on" : "off", (unsigned int)s_testKey, s_alignToPlayer ? 1 : 0,
                 s_maxDistance, s_messages ? 1 : 0);
    }

    bool    Enabled()   { return s_enabled; }
    UInt32  TestKey()   { return s_testKey; }
    UInt32  TrackKey()  { return s_trackKey; }

    void RequestTest()
    {
        if(!s_enabled) return;
        if(s_state != kIdle) return;            // a test is already running

        s_requested     = true;
        s_requestedAt   = GetTickCount();
    }

    void RecordPlayerMove(int type, const NiPoint3 & startMark, const NiPoint3 & ledge, const NiPoint3 & facing)
    {
        if(!s_enabled) return;

        // The landing is the player's own, and does not need the trail.
        s_playerMoveType    = type;
        s_playerMoveLedge   = ledge;
        s_playerMoveYaw     = atan2f(facing.x, facing.y);
        s_recording         = NULL;

        if(!s_trailEnabled) return;

        // A grab is made in the air after a jump, and she cannot copy a jump.
        if(type == Parkour::kNoLedge || type == Parkour::kFailed || type == Parkour::kGrab) return;

        TrailNode node = MakeWalkNode(startMark, GetTickCount());
        node.isMove = true;
        node.type   = (Parkour::Type)type;
        node.yaw    = s_playerMoveYaw;
        node.ledge  = ledge;

        // The path of his body during the move: this node's, from now until it ends.
        if(s_pathReplay && type != Parkour::kStepLow && type != Parkour::kStepHigh)
        {
            const int slot = (int)s_pathNext;
            s_pathNext = (s_pathNext + 1) % kPathMax;

            MovePath & path = s_paths[slot];
            path.nodeTime   = node.time;
            path.count      = 0;

            const NiPoint3 origin = Engine::Position((Actor *)Engine::Player());
            s_recordOrigin  = origin;
            s_recordMissing = NiPoint3(startMark.x - origin.x, startMark.y - origin.y, startMark.z - origin.z);
            s_recordStart   = GetTickCount();
            s_recordTicks   = 0;
            s_recording     = &path;
            node.path       = slot;

            path.t[0] = 0; path.x[0] = 0.0f; path.y[0] = 0.0f; path.z[0] = 0.0f;
            path.count = 1;
        }

        TrailAppend(node);

        _MESSAGE("NPC trail: recorded the player's %s from (%.0f, %.0f, %.0f), heading %.2f (%u nodes)",
                 TypeName((Parkour::Type)type), startMark.x, startMark.y, startMark.z, node.yaw, (unsigned int)s_trailCount);
    }

    // One sample of the body, every tick of one of the player's moves. The engine
    // closes the gap to the start mark over thirty ticks, a thirtieth of it a tick
    // from the second tick on; that part is taken out, so that what is kept is the
    // movement of the animation itself, from where the body began.
    void RecordPlayerMoveSample(const NiPoint3 & position)
    {
        if(!s_recording || s_recording->count >= (UInt32)kPathSamples) return;

        ++s_recordTicks;
        const float done = (s_recordTicks > 1) ? (float)(s_recordTicks - 1) / 30.0f : 0.0f;
        const float share = (done > 1.0f) ? 1.0f : done;

        const UInt32 i = s_recording->count++;
        s_recording->t[i] = GetTickCount() - s_recordStart;
        s_recording->x[i] = position.x - s_recordOrigin.x - s_recordMissing.x * share;
        s_recording->y[i] = position.y - s_recordOrigin.y - s_recordMissing.y * share;
        s_recording->z[i] = position.z - s_recordOrigin.z - s_recordMissing.z * share;
    }

    void RecordPlayerMoveEnd(Actor * player)
    {
        if(!s_enabled || !player) return;

        DWORD duration = 0;
        MovePath * recorded = s_recording;
        const int movedType = s_playerMoveType;
        if(s_recording)
        {
            duration = s_recording->count ? s_recording->t[s_recording->count - 1] : 0;
            _MESSAGE("NPC trail: the path of his body: %u samples over %u ms",
                     (unsigned int)s_recording->count, (unsigned int)duration);
            s_recording = NULL;
        }

        // First the landing, so that what is recorded is where he really ends up.
        NiPoint3 landing(0.0f, 0.0f, 0.0f);
        bool landed = false;
        if(IsClimbType(s_playerMoveType))
            landed = ApplyLanding(player, s_playerMoveLedge, s_playerMoveYaw, &landing);
        s_playerMoveType = -1;

        if(!s_trailEnabled || s_trailCount == 0) return;

        TrailNode & last = s_trail[s_trailCount - 1];
        if(!last.isMove || last.hasEnd) return;

        // A move that stopped far too soon was interrupted - he fell, or cancelled
        // it. She would play the whole animation for nothing, and be held still for
        // all of it by a path a few ticks long.
        if(last.path >= 0 && duration < (DWORD)s_minMoveMs)
        {
            _MESSAGE("NPC trail: that move stopped after %u ms: it was interrupted, so it is not repeated", (unsigned int)duration);
            last.type = Parkour::kNoLedge;
        }

        const NiPoint3 endPosition = landed ? landing : Engine::Position(player);
        last.end    = endPosition;
        last.hasEnd = true;

        // A complete move of his is the template for that kind of move.
        if(last.type != Parkour::kNoLedge && recorded && movedType >= 0 && movedType < (int)kTemplateTypes &&
           duration >= (DWORD)s_minMoveMs && recorded->count >= 4)
        {
            s_templates[movedType]      = *recorded;
            s_templates[movedType].nodeTime = 0;
            s_templateYaw[movedType]    = s_playerMoveYaw;
            s_haveTemplate[movedType]   = true;
            _MESSAGE("NPC trail: kept as the template for a %s (%u samples, %u ms)",
                     TypeName((Parkour::Type)movedType), (unsigned int)recorded->count, (unsigned int)duration);
        }

        // The walk carries on from where the move ended.
        TrailAppend(MakeWalkNode(endPosition, GetTickCount()));
    }

    void RequestTrack()
    {
        if(!s_enabled) return;
        if(s_state != kIdle) return;

        s_trackRequested    = true;
        s_trackRequestedAt  = GetTickCount();
    }

    // Once per animation update of the player. The common case - nothing
    // requested, nothing running, nothing tracked - is a handful of comparisons.
    //
    // The whole module runs inside an exception guard. This code is called from
    // an AI worker thread, while other threads change the very actors it looks
    // at, so a pointer that was good a moment ago can stop being good. Whatever
    // goes wrong here ends the NPC module's work and not the game.
    void OnTick(Actor * player)
    {
        __try
        {
            TickInner(player);
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            _ERROR("NPC: an exception inside the NPC module: its work is stopped here and the game goes on");

            // Whoever was being moved gets her physics back, if she can still be reached.
            if(s_state != kIdle && s_npc)
                SafeReleaseNpc(s_npc);
            for(int i = 0; i < kMaxTracked; ++i)
            {
                s_slots[i].tracked   = NULL;
                s_slots[i].trackedId = 0;
            }

            s_state             = kIdle;
            s_npc               = NULL;
            s_requested         = false;
            s_trackRequested    = false;
            s_tracked           = NULL;
            s_trackedId         = 0;
            ResetCorrection();
        }
    }

    void Reset()
    {
        // A graph rebuild or a load in the middle of a test: if the NPC is
        // still a live actor, give its physics back before forgetting it.
        if(s_state != kIdle && s_npc && SafeUsable(s_npc))
            ReleaseNpc(s_npc, false);

        s_state     = kIdle;
        s_requested = false;
        s_npc       = NULL;
        s_planValid   = false;
        s_trailRun    = false;
        s_trailCall   = false;
        s_glide.active = false;
        s_planPath    = NULL;
        s_anchorReset = true;                   // the tracked NPCs, if any, are kept
        for(int i = 0; i < kMaxTracked; ++i) s_slots[i].anchorReset = true;
        ResetCorrection();
    }

    // A load or a new game: the actors of the old world are gone, and a pointer
    // kept across that is a pointer to freed memory.
    void Forget()
    {
        Reset();
        for(int i = 0; i < kMaxTracked; ++i)
        {
            if(s_slots[i].tracked)
                _MESSAGE("NPC track: %08X forgotten (a load or a new game)", (unsigned int)s_slots[i].trackedId);
            s_slots[i].tracked   = NULL;
            s_slots[i].trackedId = 0;
        }
        s_tracked   = NULL;
        s_trackedId = 0;
        s_active    = -1;
        s_trailCount = 0;                       // the old world's trail means nothing in the new one
        s_callAfterTime = 0;
        s_playerMoveType = -1;
        s_glide.active = false;
        s_recording = NULL;
        for(int i = 0; i < kPathMax; ++i) s_paths[i].nodeTime = 0;
    }
}
