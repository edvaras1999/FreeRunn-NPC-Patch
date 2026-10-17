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

    enum State { kIdle, kAligning, kWaitStart, kRunning, kTrail };

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
    bool            s_trailRun          = false;    // a trail replay is running
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
    };

    Correction s_corr = { false, false, 0 };

    void ResetCorrection()
    {
        s_corr.initialised  = false;
        s_corr.done         = false;
        s_corr.last         = 0;
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
        const float mx = s_start.x * scale - current[0];
        const float my = s_start.y * scale - current[1];
        const float mz = s_start.z * scale - current[2];
        const float distance = sqrtf(mx * mx + my * my + mz * mz);

        const DWORD now = GetTickCount();
        float dt = 1.0f / 60.0f;
        if(s_corr.initialised)
            dt = (float)(now - s_corr.last) / 1000.0f;
        if(dt > 0.1f) dt = 0.1f;
        if(dt <= 0.0f) dt = 1.0f / 60.0f;
        s_corr.last = now;

        if(!s_corr.initialised)
        {
            s_corr.initialised = true;
            _MESSAGE("NPC test: start mark is %.1f game units away", distance / scale);
        }

        if(s_corr.done) return;

        if(distance <= 2.0f * scale)
        {
            s_corr.done = true;
            return;
        }

        const float maxStep = s_dashSpeed * scale * dt;
        const float k = (distance <= maxStep) ? 1.0f : maxStep / distance;

        __declspec(align(16)) float target[4];
        target[0] = current[0] + mx * k;
        target[1] = current[1] + my * k;
        target[2] = current[2] + mz * k;
        target[3] = 0.0f;

        Engine::ControllerSetPosition(ctrl, target, true, false);
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
        if(sendInterrupt)
            Graph::Notify(npc, Graph::GetNames().interrupt);
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
        s_trailRun  = false;
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

    // Starts a move on an NPC whose ledge is already known. When `turn` is set she
    // is turned to `yaw` (the way the engine reads it: clockwise from north).
    // Her physics are switched off at once, so that she cannot run on while the
    // turn settles; the first ticks of the move walk her to the start mark, and
    // ReleaseNpc gives her physics back when it ends.
    void StartOn(Actor * npc, bool turn, float yaw, bool manual, Parkour::Type type, const NiPoint3 & start)
    {
        s_manual    = manual;
        s_npc       = npc;
        s_state     = kAligning;
        s_planValid = true;
        s_planType  = type;
        s_planStart = start;

        void * ctrl = Engine::CharController(npc);
        if(ctrl)
            Engine::ControllerFlags(ctrl) |= Engine::Controller::kFlag_NoSim;

        if(turn)
            Engine::Rotation(npc).z = yaw;

        s_stateStart = GetTickCount() - (kAlignMs - kPlanSettleMs);
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

            StartOn(npc, true, atan2f(tx, ty), true, type, start);
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

        if(s_trailCount % 50 == 0)
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

    // The node nearest her, if the trail from there to the player is whole.
    bool FindTrailEntry(const NiPoint3 & pos, UInt32 & entry, float & distance, const char *& why)
    {
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

        _MESSAGE("NPC track %08X: the player is %.0f above her, %.0f away, and she is not getting closer: "
                 "following his path from node %u of %u (%.0f units from her), %u moves ahead",
                 (unsigned int)s_trackedId, heightGap, distance, (unsigned int)entry,
                 (unsigned int)s_trailCount, entryDistance, (unsigned int)moves);

        s_manual        = false;
        s_npc           = npc;
        s_state         = kTrail;
        s_planValid     = false;
        s_trailRun      = true;
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
        s_state         = kIdle;
        s_npc           = NULL;
        s_planValid     = false;
        s_cooldownUntil = GetTickCount() + (DWORD)(s_autoCooldownSec * 1000.0f);
        s_anchorReset   = true;
        ResetCorrection();
    }

    // One step toward `target` (game coordinates) at `speed` game units a second.
    // Returns how far is left after the step.
    float TrailMoveToward(Actor * npc, void * ctrl, const NiPoint3 & target, float dt, float speed)
    {
        __declspec(align(16)) float current[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Engine::ControllerGetPosition(ctrl, current, true);

        const float scale = Engine::HavokScale();
        const float mx = target.x * scale - current[0];
        const float my = target.y * scale - current[1];
        const float mz = target.z * scale - current[2];
        const float distance = sqrtf(mx * mx + my * my + mz * mz);
        if(distance < 0.0001f) return 0.0f;

        const float left = distance / scale;                    // game units
        const float stepGame = speed * dt;
        const float k = (left <= stepGame) ? 1.0f : stepGame / left;

        __declspec(align(16)) float next[4];
        next[0] = current[0] + mx * k;
        next[1] = current[1] + my * k;
        next[2] = current[2] + mz * k;
        next[3] = 0.0f;
        Engine::ControllerSetPosition(ctrl, next, true, false);

        if(fabsf(mx) + fabsf(my) > 2.0f * scale)                // face where she is going
            Engine::Rotation(npc).z = atan2f(mx, my);

        return (left <= stepGame) ? 0.0f : left - stepGame;
    }

    void TrailStep(Actor * npc, DWORD now)
    {
        Actor * player = (Actor *)Engine::Player();
        if(!player)
        {
            TrailEnd(npc, "there is no player", true);
            return;
        }

        if(now - s_trailStart > (DWORD)(s_trailMaxSec * 1000.0f))
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
        if(sqrtf(px * px + py * py) <= s_trailEndRadius && fabsf(there.z - here.z) <= 100.0f)
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
        const float left = TrailMoveToward(npc, ctrl, node.pos, dt, s_trailSpeed);

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
            Engine::Rotation(npc).z = node.yaw;
            s_state         = kAligning;
            s_stateStart    = now - (kAlignMs - kPlanSettleMs);
            // s_trailIdx moves on when the move ends (Succeed)
        }
    }

    void Succeed(Actor * npc, DWORD now)
    {
        const NiPoint3 after = Engine::Position(npc);
        const DWORD took = now - s_actionStart;

        _MESSAGE("NPC test: finished after %lu ms, moved from (%.0f, %.0f, %.0f) to (%.0f, %.0f, %.0f)",
                 (unsigned long)took, s_posBefore.x, s_posBefore.y, s_posBefore.z, after.x, after.y, after.z);

        // On the trail she is not handed back yet: the next node is waiting.
        if(s_trailRun)
        {
            ++s_trailIdx;
            s_state         = kTrail;
            s_trailLastTick = now;
            s_planValid     = false;
            ResetCorrection();
            return;
        }

        const float rise = after.z - s_posBefore.z;
        const Parkour::Type type = s_type;

        EndTest(true, true);

        char text[512];
        sprintf_s(text, sizeof(text),
                  "FreeRun NPC test: finished.\n\nMove: %s\nTime: %lu ms\nHeight change: %.0f units\n\n"
                  "If it did not look right, send FreerunLE.log.",
                  TypeName(type), (unsigned long)took, rise);
        if(s_manual) ShowText(text);
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
                if(now - s_stateStart >= kAlignMs)
                    ProbeAndStart();
                return;

            case kTrail:
                TrailStep(npc, now);
                return;

            case kWaitStart:
            {
                RunCorrection(npc);

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
                RunCorrection(npc);

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

        if(npc == s_tracked)
        {
            StopTracking("asked to stop");
            ShowText("FreeRun NPC: no longer tracking this NPC.");
            return;
        }

        s_tracked           = npc;
        s_trackedId         = npc->formID;
        s_anchorReset       = true;
        s_cooldownUntil     = 0;
        s_haveProbe         = false;
        s_haveProbeToward   = false;
        HistoryClear();

        _MESSAGE("NPC track: now tracking %08X (automatic moves %s)",
                 (unsigned int)s_trackedId, s_autoTracked ? "on" : "off, log only");

        char text[256];
        sprintf_s(text, sizeof(text),
                  "FreeRun NPC: tracking this NPC.\n\n%s\n\nPress the same key facing it to stop.",
                  s_autoTracked ? "When she runs round an obstacle she could climb, she will try to climb it."
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

                if(FindTrailEntry(pos, entry, entryDistance, noTrail))
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
        bool turn = false;
        const char * source = "";

        if(typeToward != Parkour::kNoLedge && typeToward != Parkour::kFailed)
        {
            chosen = typeToward; chosenGap = towardGap; chosenLedgeZ = ledgeToward.z; chosenStart = startToward;
            turn = true; source = "toward the player";
        }
        else if(typeOwn != Parkour::kNoLedge && typeOwn != Parkour::kFailed && dot > 0.0f)
        {
            const float gx = startOwn.x - pos.x, gy = startOwn.y - pos.y, gz = startOwn.z - pos.z;
            chosen = typeOwn; chosenGap = sqrtf(gx * gx + gy * gy + gz * gz); chosenLedgeZ = ledgeOwn.z; chosenStart = startOwn;
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

        StartOn(npc, turn, atan2f(tx, ty), false, chosen, chosenStart);
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

            if(s_tracked)
                SafeTrackTick(player);
            return;
        }

        Step();
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

    void RecordPlayerMove(int type, const NiPoint3 & startMark, const NiPoint3 & facing)
    {
        if(!s_enabled || !s_trailEnabled) return;

        // A grab is made in the air after a jump, and she cannot copy a jump.
        if(type == Parkour::kNoLedge || type == Parkour::kFailed || type == Parkour::kGrab) return;

        TrailNode node = MakeWalkNode(startMark, GetTickCount());
        node.isMove = true;
        node.type   = (Parkour::Type)type;
        node.yaw    = atan2f(facing.x, facing.y);

        TrailAppend(node);

        _MESSAGE("NPC trail: recorded the player's %s from (%.0f, %.0f, %.0f), heading %.2f (%u nodes)",
                 TypeName((Parkour::Type)type), startMark.x, startMark.y, startMark.z, node.yaw, (unsigned int)s_trailCount);
    }

    void RecordPlayerMoveEnd(const NiPoint3 & endPosition)
    {
        if(!s_enabled || !s_trailEnabled || s_trailCount == 0) return;

        TrailNode & last = s_trail[s_trailCount - 1];
        if(!last.isMove || last.hasEnd) return;

        last.end    = endPosition;
        last.hasEnd = true;

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
        s_anchorReset = true;                   // the tracked NPC, if any, is kept
        ResetCorrection();
    }

    // A load or a new game: the actors of the old world are gone, and a pointer
    // kept across that is a pointer to freed memory.
    void Forget()
    {
        Reset();
        if(s_tracked)
            _MESSAGE("NPC track: %08X forgotten (a load or a new game)", (unsigned int)s_trackedId);
        s_tracked   = NULL;
        s_trackedId = 0;
        s_trailCount = 0;                       // the old world's trail means nothing in the new one
    }
}
