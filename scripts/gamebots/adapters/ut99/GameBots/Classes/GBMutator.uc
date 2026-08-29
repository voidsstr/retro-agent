//=============================================================================
// GBMutator -- the UT99 gamebots adapter.
//
// OFF BY DEFAULT (bEnabled=False in the generated GameBots.ini). Loading this
// mutator changes nothing on its own -- an admin has to say so, either by
// editing GameBots.ini or at runtime with the console command
// `mutate gb_enable 1` (`mutate gb_enable 0` to stop; `mutate gb_status` to
// print the current state). This mirrors the Quake III adapter's
// `gb_enable 0` cvar as closely as UT99's admin surface allows -- UnrealScript
// mutators don't have cvars, but `Mutate()` is the standard console-command
// hook every UT99 admin already knows.
//
// WHAT IT DOES NOT DO: intercept the server's own `addbot`/auto-added bots.
// Those stay ordinary Botpack.Bot, driven by the built-in AI alone. This
// mutator spawns and owns its OWN roster of GBBot pawns (see SpawnBots) so
// there is never any ambiguity about which bots are "ours" -- important for
// the control experiment the README documents (same bots, only the wiring
// to the policy server toggled).
//
// NEVER BLOCKS THE SERVER: TcpLink (GBLink) is asynchronous end to end, and
// this class starts an exchange and moves on -- there is no code path here
// that waits for policyd. A round-trip that does not complete by the next
// policy tick just means this tick's action is skipped (GBBot's Super.Tick()
// output stands); GBLink's own timeout closes a truly stuck connection so a
// hung policy server costs one dropped connection, not a stalled game.
//=============================================================================
class GBMutator extends Mutator config(GameBots);

const MAX_BOTS = 16;

// Every GBSchema.* value below is fetched through a static function call --
// `class'GBSchema'.static.NAME()` -- never a bare `GBSchema.NAME`. A bare
// cross-class `const` reference compiled in some positions and not others
// with no discoverable pattern (a lone array subscript sometimes worked; the
// identical constant as a loop bound, a function argument, or combined with
// `+`/`&&` in the same expression usually did not, "Bad or missing
// expression"). The static-function form hit none of those failures in any
// position tried. Full account in gen_gbschema.py's module docstring and the
// adapter README's honest-verdict section.

// --- config, all editable in GameBots.ini without recompiling -------------
var config bool   bEnabled;          // off by default -- see header comment
var config int    NumBots;           // how many GBBot pawns to spawn (<=MAX_BOTS)
var config float  TickRate;          // policy decisions/sec, NOT the server's
                                      // own tick rate -- see README for why
                                      // 10 Hz is the default
var config string PolicyHost;        // policyd's --tcp-listen host
var config int    PolicyPort;        // policyd's --udp-listen port
var config float  ResponseTimeout;   // seconds to wait for one exchange
var config bool   bDebug;            // per-bot action logging

// --- runtime state ----------------------------------------------------
var GBLink Link;
var GBBot  Bots[16];                 // = MAX_BOTS; literal size, see README
var int    NumActiveBots;

var float  TickAccum;
var int    WireTick;

var byte   ActHave  [16];    // 0/1 -- UnrealScript does not allow bool arrays
var int    ActButtons[16];
var float  ActPitch [16];
var float  ActYaw   [16];
var float  ActFwd   [16];
var float  ActSide  [16];
var byte   ActWeapon[16];

var int    ReportedState;            // -1 unknown, 0 fallback, 1 driving --
                                      // log only on change (see gb_adapter.c's
                                      // gb_reported_state -- a line per tick
                                      // at 10Hz is still 36,000 lines/hour)

// Normalisation constants. Same role as gb_adapter.c's GB_FAR_PLANE etc --
// scale factors so a value means the same thing on every map, not physics.
const FAR_PLANE  = 2000.0;
const EYE_Z      = 44.0;             // approx eye height above Pawn.Location
                                      // for a standing human-sized UT pawn;
                                      // there is no DEFAULT_VIEWHEIGHT-style
                                      // engine constant exposed to script, so
                                      // this is a documented estimate, not a
                                      // measured constant -- see README

// ---------------------------------------------------------------- lifecycle

function PostBeginPlay()
{
    Super.PostBeginPlay();
    ReportedState = -1;
    if (bEnabled)
        StartGB();
}

function StartGB()
{
    if (Link != None)
        return;                       // already running
    Link = Spawn(class'GBLink', Self);
    Link.Init(Self);
    SpawnBots();
    Log("gamebots(ut99): starting -- schema " $ class'GBSchema'.static.SCHEMA_HASH()
        $ " obs_dim " $ class'GBSchema'.static.OBS_DIM() $ " policy tcp://" $ PolicyHost $ ":"
        $ PolicyPort $ " tickrate " $ TickRate
        $ " -- 'mutate gb_enable 0' to stop, 'mutate gb_status' to check");
}

function StopGB()
{
    local int i;
    if (Link != None)
    {
        // UdpLink has no Close() (that's a TcpLink-only function -- a
        // connectionless socket has nothing to close); destroying the actor
        // releases its bound port.
        Link.Destroy();
        Link = None;
    }
    for (i = 0; i < NumActiveBots; i++)
    {
        if (Bots[i] != None)
            Bots[i].Brain = None;     // release control; the bot keeps
    }                                  // playing on its own built-in AI
    ReportedState = -1;
    Log("gamebots(ut99): stopped -- bots released to their own AI");
}

function SpawnBots()
{
    local NavigationPoint Start;
    local GBBot NewBot;
    local int i, want;

    want = NumBots;
    if (want > MAX_BOTS)
        want = MAX_BOTS;

    for (i = 0; i < want; i++)
    {
        Start = Level.Game.FindPlayerStart(None);
        if (Start == None)
        {
            Log("gamebots(ut99): FindPlayerStart returned None, stopped at "
                $ NumActiveBots $ "/" $ want $ " bots");
            break;
        }
        NewBot = Spawn(class'GBBot',,, Start.Location, Start.Rotation);
        if (NewBot == None)
        {
            Log("gamebots(ut99): Spawn(GBBot) failed for slot " $ i);
            continue;
        }
        NewBot.Brain = Self;
        NewBot.MyIndex = NumActiveBots;
        Level.Game.RestartPlayer(NewBot);
        Bots[NumActiveBots] = NewBot;
        NumActiveBots++;
    }
    Log("gamebots(ut99): spawned " $ NumActiveBots $ "/" $ want $ " GBBot(s)");
}

// ------------------------------------------------------------- admin hook

function Mutate(string MutateString, PlayerPawn Sender)
{
    if (MutateString == "gb_enable 1")
    {
        bEnabled = True;
        SaveConfig();
        StartGB();
    }
    else if (MutateString == "gb_enable 0")
    {
        bEnabled = False;
        SaveConfig();
        StopGB();
    }
    else if (MutateString == "gb_debug 1")
    {
        bDebug = True;
        SaveConfig();
    }
    else if (MutateString == "gb_debug 0")
    {
        bDebug = False;
        SaveConfig();
    }
    else if (MutateString == "gb_status")
    {
        // UDP is connectionless -- there is no IsConnected() to ask.
        // "addrKnown" reflects whether PolicyHost resolved; "state" reflects
        // whether a reply has arrived recently enough (see Tick()).
        Log("gamebots(ut99): enabled=" $ bEnabled $ " bots=" $ NumActiveBots
            $ " addrKnown=" $ (Link != None && Link.HasServerAddr())
            $ " state=" $ ReportedState);
    }
    Super.Mutate(MutateString, Sender);
}

// -------------------------------------------------------------- main loop

event Tick(float DeltaTime)
{
    Super.Tick(DeltaTime);

    if (!bEnabled || Link == None)
        return;

    TickAccum += DeltaTime;
    if (TickAccum < (1.0 / TickRate))
        return;
    TickAccum = 0.0;
    WireTick++;

    if (!Link.HasServerAddr())
    {
        ReportFallback();
        return;
    }

    // UDP is connectionless and fire-and-forget: send this tick's batch and
    // move on. Whether it was answered is judged separately, below, from
    // how long it has been since the last GOOD reply -- there is no
    // "in flight" state to track and nothing here can block waiting for one.
    SendObservations();

    if (Level.TimeSeconds - Link.LastGoodReplyTime > ResponseTimeout)
    {
        // Stale or never-answered: relinquish control rather than freezing
        // every bot on its last known action forever. GBBot's Super.Tick()
        // then drives again, same as if the mutator were never enabled.
        ClearActions();
        ReportFallback();
    }
    else
    {
        ReportDriving();
    }
}

function ClearActions()
{
    local int i;
    for (i = 0; i < NumActiveBots; i++)
        ActHave[i] = 0;
}

function ReportFallback()
{
    if (ReportedState != 0)
    {
        ReportedState = 0;
        Log("gamebots(ut99): policy server unavailable -- bots are on their own AI");
    }
}

function ReportDriving()
{
    if (ReportedState != 1)
    {
        ReportedState = 1;
        Log("gamebots(ut99): policy server answering, driving " $ NumActiveBots $ " bot(s)");
    }
}

// ------------------------------------------------------- GBLink callbacks
//
// UdpLink has no Opened()/Closed() -- UDP is connectionless, there is
// nothing to open or close. "Connected" is judged in Tick() from how long
// it has been since the last good reply (Link.LastGoodReplyTime).

function OnAction(int BotId, int Buttons, float Pitch, float Yaw, float Fwd, float Side, byte Weapon)
{
    // Clamp here, not in GBBot -- the policy is not trusted, same distrust
    // gb_client.c's gb_clamp() applies on every other engine. Also absorbs
    // GBMath's un-decoded NaN/Inf case (see GBMath.uc): a garbage bit pattern
    // decodes to *some* float, finite or not, and FClamp below cannot pass a
    // NaN through silently because we test for it explicitly first.
    if (Pitch != Pitch) Pitch = 0.0;    // NaN != NaN is the only defined NaN test
    if (Yaw   != Yaw)   Yaw   = 0.0;
    if (Fwd   != Fwd)   Fwd   = 0.0;
    if (Side  != Side)  Side  = 0.0;

    Pitch = FClamp(Pitch, -class'GBSchema'.static.MAX_PITCH_DELTA_DEG(), class'GBSchema'.static.MAX_PITCH_DELTA_DEG());
    Yaw   = FClamp(Yaw,   -class'GBSchema'.static.MAX_YAW_DELTA_DEG(),   class'GBSchema'.static.MAX_YAW_DELTA_DEG());
    Fwd   = FClamp(Fwd,   -1.0, 1.0);
    Side  = FClamp(Side,  -1.0, 1.0);

    if (BotId < 0 || BotId >= NumActiveBots)
        return;

    ActHave  [BotId] = 1;
    ActButtons[BotId] = Buttons;
    ActPitch [BotId] = Pitch;
    ActYaw   [BotId] = Yaw;
    ActFwd   [BotId] = Fwd;
    ActSide  [BotId] = Side;
    ActWeapon[BotId] = Weapon;

    if (BotId == 0)
        ReportDriving();   // one representative bot is enough to flag the transition
}

// ------------------------------------------------------------ observation

function SendObservations()
{
    local int i;
    local float Obs[144];   // = class'GBSchema'.static.OBS_DIM(); literal, see README on array sizes

    Link.BeginRequest(NumActiveBots, WireTick);
    for (i = 0; i < NumActiveBots; i++)
    {
        if (Bots[i] == None || Bots[i].bDeleteMe)
            continue;
        BuildObservation(Bots[i], i, Obs);
        Link.WriteBotObs(i, Obs);
    }
    Link.EndRequest();
}

function BuildObservation(GBBot B, int Index, out float Obs[144])
{
    local vector Fwd, Right, Up, Eye, Rel;
    local rotator ViewRot, YawOnly;
    local int i;
    local float DamageTaken;
    local bool Alive;

    for (i = 0; i < class'GBSchema'.static.OBS_DIM(); i++)
        Obs[i] = 0.0;

    Alive = (B.Health > 0);

    // Yaw-only body frame -- pitch reported separately (OBS_PITCH_NORM), same
    // reasoning as gb_adapter.c: folding pitch into the frame would make "an
    // enemy above me" indistinguishable from "an enemy ahead while I look up".
    ViewRot = B.ViewRotation;
    YawOnly.Yaw = ViewRot.Yaw;
    YawOnly.Pitch = 0;
    YawOnly.Roll = 0;
    GetAxes(YawOnly, Fwd, Right, Up);
    Eye = B.Location;
    Eye.Z += EYE_Z;

    // --- self ---
    Obs[class'GBSchema'.static.OBS_HEALTH_FRAC()] = FClamp(float(B.Health) / 100.0, 0.0, 1.0);
    // OBS_AMMO_FRAC, OBS_AMMO_RESERVE_FRAC and OBS_RELOADING all stay ZERO.
    // The ammo count for the equipped weapon lives on a separate `Ammo`
    // inventory actor (`AmmoAmount` is a property of Ammo, not a Weapon
    // function -- confirmed via `ucc packagedump Engine`, Group: Export =
    // Ammo, an IntProperty), reached via `Pawn.FindInventoryType(class<
    // Inventory>)`. That call's exact parameter shape did not compile here
    // ("type mismatch in parameter 1") and, per this project's rule for
    // every other engine adapter, an unresolved field is left zero and
    // documented rather than guessed. See the README.
    Obs[class'GBSchema'.static.OBS_WEAPON_ID_NORM()] = 0.0;   // no stable cross-map weapon index
                                                // exposed on the base Pawn/Weapon
                                                // API -- see README

    Rel = TransformToLocal(B.Velocity, Fwd, Right, Up);
    Obs[class'GBSchema'.static.OBS_VEL_LOCAL() + 0] = FClamp(Rel.X / B.GroundSpeed, -2.0, 2.0);
    Obs[class'GBSchema'.static.OBS_VEL_LOCAL() + 1] = FClamp(Rel.Y / B.GroundSpeed, -2.0, 2.0);
    Obs[class'GBSchema'.static.OBS_VEL_LOCAL() + 2] = FClamp(Rel.Z / B.GroundSpeed, -2.0, 2.0);
    Obs[class'GBSchema'.static.OBS_SPEED_FRAC()] = FClamp(VSize(B.Velocity) / B.GroundSpeed, 0.0, 2.0);
    Obs[class'GBSchema'.static.OBS_PITCH_NORM()] = FClamp(NormalizedPitch(ViewRot.Pitch) / 90.0, -1.0, 1.0);
    // Every one of these four was, at one point while developing this file,
    // a one-line ternary ("Obs[X] = (cond) ? 1.0 : 0.0;") that reported
    // "Type mismatch in '='" -- moving the error to a DIFFERENT one of the
    // four each time a preceding line was rewritten, the same parser-desync
    // signature documented on BuildGameContext and BuildEntities. All four
    // are if/else now rather than chased individually. See the README.
    if (B.Physics == PHYS_Walking)
        Obs[class'GBSchema'.static.OBS_ON_GROUND()] = 1.0;
    else
        Obs[class'GBSchema'.static.OBS_ON_GROUND()] = 0.0;

    if (B.bIsCrouching)
        Obs[class'GBSchema'.static.OBS_CROUCHING()] = 1.0;
    else
        Obs[class'GBSchema'.static.OBS_CROUCHING()] = 0.0;

    if (B.HeadRegion.Zone != None && B.HeadRegion.Zone.bWaterZone)
        Obs[class'GBSchema'.static.OBS_IN_WATER()] = 1.0;
    else
        Obs[class'GBSchema'.static.OBS_IN_WATER()] = 0.0;

    if (Alive)
        Obs[class'GBSchema'.static.OBS_ALIVE()] = 1.0;
    else
        Obs[class'GBSchema'.static.OBS_ALIVE()] = 0.0;

    // --- local geometry: 16 rays around us, plus up and down ---
    BuildRays(B, Eye, ViewRot.Yaw, Obs);

    // --- other players ---
    BuildEntities(B, Eye, Fwd, Right, Up, Obs);

    // --- what just happened ---
    DamageTaken = B.PrevHealth - B.Health;
    if (DamageTaken > 0.0)
        Obs[class'GBSchema'.static.OBS_TOOK_DAMAGE()] = FClamp(DamageTaken / 100.0, 0.0, 1.0);
    // No per-attacker hit direction is read here -- UT99's Pawn does not
    // expose "who last hurt me" the way Q3's g_entities[].client->lasthurt_
    // client does on the base API this adapter uses, so OBS_DAMAGE_DIR stays
    // ZERO. Documented, not invented -- see README.
    if (B.PlayerReplicationInfo != None && B.PlayerReplicationInfo.Score > B.PrevScore)
        Obs[class'GBSchema'.static.OBS_KILLED_SOMEONE()] = 1.0;
    if (B.PrevAlive && !Alive)
        Obs[class'GBSchema'.static.OBS_DIED()] = 1.0;

    B.PrevHealth = B.Health;
    if (B.PlayerReplicationInfo != None)
        B.PrevScore = B.PlayerReplicationInfo.Score;
    B.PrevAlive = Alive;

    // --- match context ---
    BuildGameContext(B, Obs);

    // intent[] stays zero -- policyd injects the planner's vector, same
    // contract as every other engine adapter.
}

function vector TransformToLocal(vector World, vector Fwd, vector Right, vector Up)
{
    local vector Out;
    Out.X = World Dot Fwd;
    Out.Y = World Dot Right;
    Out.Z = World Dot Up;
    return Out;
}

function float NormalizedPitch(int Pitch)
{
    // UnrealScript rotator components are 0..65535 per full turn; convert to
    // signed degrees the way gb_adapter.c's AngleNormalize180 does for Q3.
    local float Deg;
    Deg = (float(Pitch) / 65536.0) * 360.0;
    if (Deg > 180.0)
        Deg -= 360.0;
    return Deg;
}

function BuildRays(GBBot B, vector Eye, int YawBase, out float Obs[144])
{
    local int i;
    local rotator R;
    local vector HitLoc, HitNorm, TraceEnd, Dir;
    local Actor Hit;

    for (i = 0; i < class'GBSchema'.static.NUM_RAYS_H(); i++)
    {
        R.Yaw = YawBase + (65536 * i) / class'GBSchema'.static.NUM_RAYS_H();
        R.Pitch = 0;
        R.Roll = 0;
        Dir = vector(R);
        TraceEnd = Eye + Dir * FAR_PLANE;
        Hit = B.Trace(HitLoc, HitNorm, TraceEnd, Eye, True);
        Obs[class'GBSchema'.static.OBS_RAY_H() + i] = RayFraction(Hit, Eye, HitLoc);
    }

    // up
    TraceEnd = Eye;
    TraceEnd.Z += FAR_PLANE;
    Hit = B.Trace(HitLoc, HitNorm, TraceEnd, Eye, True);
    Obs[class'GBSchema'.static.OBS_RAY_UP()] = RayFraction(Hit, Eye, HitLoc);

    // down
    TraceEnd = Eye;
    TraceEnd.Z -= FAR_PLANE;
    Hit = B.Trace(HitLoc, HitNorm, TraceEnd, Eye, True);
    Obs[class'GBSchema'.static.OBS_RAY_DOWN()] = RayFraction(Hit, Eye, HitLoc);
}

function float RayFraction(Actor Hit, vector Eye, vector HitLoc)
{
    if (Hit == None)
        return 1.0;
    return FClamp(VSize(HitLoc - Eye) / FAR_PLANE, 0.0, 1.0);
}

function BuildEntities(GBBot B, vector Eye, vector Fwd, vector Right, vector Up, out float Obs[144])
{
    local Pawn P;
    local float CandDist[16];
    local byte  CandVis[16];     // 0/1 -- UnrealScript does not allow bool arrays
    local byte  CandTeam[16];    // 0/1
    local Pawn  CandPawn[16];
    local int nCand, i, j, base;
    local vector OtherEye, Delta, Dir, RelVel;
    local float Dist, TmpDist;
    local Actor Hit;
    local vector HitLoc, HitNorm;
    local bool TmpVisB, TmpTeamB;
    local byte TmpVis, TmpTeam;
    local Pawn TmpPawn;
    local int TeammatesAlive, EnemiesAlive;
    local float FlagF;   // see the two `if/else` blocks below

    foreach B.VisibleCollidingActors(class'Pawn', P, FAR_PLANE, Eye)
    {
        if (P == B || P.Health <= 0)
            continue;
        OtherEye = P.Location;
        OtherEye.Z += EYE_Z;
        Delta = OtherEye - Eye;
        Dist = VSize(Delta);
        if (Dist < 0.001)
            continue;

        if (P.PlayerReplicationInfo != None && B.PlayerReplicationInfo != None)
            TmpTeamB = (P.PlayerReplicationInfo.Team == B.PlayerReplicationInfo.Team)
                && (Level.Game.GameReplicationInfo.bTeamGame);
        else
            TmpTeamB = False;
        if (TmpTeamB) TmpTeam = 1; else TmpTeam = 0;

        Hit = B.Trace(HitLoc, HitNorm, OtherEye, Eye, True);
        TmpVisB = (Hit == None) || (Hit == P);
        if (TmpVisB) TmpVis = 1; else TmpVis = 0;

        if (TmpTeamB)
            TeammatesAlive++;
        else
            EnemiesAlive++;

        if (nCand < 16)
        {
            CandPawn[nCand] = P;
            CandDist[nCand] = Dist;
            CandVis[nCand]  = TmpVis;
            CandTeam[nCand] = TmpTeam;
            nCand++;
        }
    }

    // Selection-sort the (small, <=16) candidate list: visible enemies
    // first, then by distance -- same ordering rule as gb_adapter.c's
    // GB_CandCompare, and for the same reason: the slot order IS
    // information, so it has to be stable and meaningful.
    for (i = 0; i < nCand - 1; i++)
    {
        for (j = i + 1; j < nCand; j++)
        {
            if (ShouldSwap(CandVis[i], CandTeam[i], CandDist[i], CandVis[j], CandTeam[j], CandDist[j]))
            {
                TmpPawn = CandPawn[i]; CandPawn[i] = CandPawn[j]; CandPawn[j] = TmpPawn;
                TmpDist = CandDist[i]; CandDist[i] = CandDist[j]; CandDist[j] = TmpDist;
                TmpVis  = CandVis[i];  CandVis[i]  = CandVis[j];  CandVis[j]  = TmpVis;
                TmpTeam = CandTeam[i]; CandTeam[i] = CandTeam[j]; CandTeam[j] = TmpTeam;
            }
        }
    }

    // `i < X() && i < nCand` as one compound condition reported "Bad or
    // missing expression after '<'"; splitting the cap into its own
    // `if...break` avoids it. See the README's honest-verdict section.
    for (i = 0; i < nCand; i++)
    {
        if (i >= class'GBSchema'.static.MAX_ENTITIES())
            break;
        base = class'GBSchema'.static.OBS_E0_PRESENT() + i * class'GBSchema'.static.ENT_SLOT_STRIDE();
        OtherEye = CandPawn[i].Location;
        OtherEye.Z += EYE_Z;
        Delta = OtherEye - Eye;
        Dir = TransformToLocal(Normal(Delta), Fwd, Right, Up);

        Obs[base + class'GBSchema'.static.ENT_PRESENT()]  = 1.0;
        // `(CandTeam[i] != 0) ? 1.0 : 0.0` assigned straight into Obs[]
        // reported "Type mismatch in '='" -- a byte-array element compared
        // against an int literal as a ternary condition, unlike every other
        // (bool-typed) ternary condition in this file. Split into if/else
        // rather than chased further.
        if (CandTeam[i] != 0) FlagF = 1.0; else FlagF = 0.0;
        Obs[base + class'GBSchema'.static.ENT_TEAMMATE()] = FlagF;
        Obs[base + class'GBSchema'.static.ENT_DIR() + 0]  = FClamp(Dir.X, -1.0, 1.0);
        Obs[base + class'GBSchema'.static.ENT_DIR() + 1]  = FClamp(Dir.Y, -1.0, 1.0);
        Obs[base + class'GBSchema'.static.ENT_DIR() + 2]  = FClamp(Dir.Z, -1.0, 1.0);
        Obs[base + class'GBSchema'.static.ENT_DIST()]     = FClamp(CandDist[i] / FAR_PLANE, 0.0, 1.0);
        RelVel = TransformToLocal(CandPawn[i].Velocity, Fwd, Right, Up);
        Obs[base + class'GBSchema'.static.ENT_RELVEL() + 0] = FClamp(RelVel.X / B.GroundSpeed, -2.0, 2.0);
        Obs[base + class'GBSchema'.static.ENT_RELVEL() + 1] = FClamp(RelVel.Y / B.GroundSpeed, -2.0, 2.0);
        Obs[base + class'GBSchema'.static.ENT_HEALTH()]   = FClamp(float(CandPawn[i].Health) / 100.0, 0.0, 1.0);
        if (CandVis[i] != 0) FlagF = 1.0; else FlagF = 0.0;
        Obs[base + class'GBSchema'.static.ENT_VISIBLE()] = FlagF;
    }

    if (NumActiveBots > 0)
    {
        Obs[class'GBSchema'.static.OBS_TEAMMATES_ALIVE_FRAC()] = FClamp(float(TeammatesAlive) / float(NumActiveBots), 0.0, 1.0);
        Obs[class'GBSchema'.static.OBS_ENEMIES_ALIVE_FRAC()]   = FClamp(float(EnemiesAlive)   / float(NumActiveBots), 0.0, 1.0);
    }
}

function bool ShouldSwap(byte VisA, byte TeamA, float DistA, byte VisB, byte TeamB, float DistB)
{
    if (VisA != VisB)
        return (VisB != 0) && (VisA == 0);   // visible first
    if (TeamA != TeamB)
        return (TeamA != 0) && (TeamB == 0); // enemies (Team=0) before teammates
    return DistB < DistA;                     // then nearest first
}

function BuildGameContext(GBBot B, out float Obs[144])
{
    // TimeLimit/FragLimit live on Botpack.DeathMatchPlus, NOT the more
    // generic TournamentGameInfo -- confirmed via `ucc packagedump Botpack`
    // (Group: Export[3] = DeathMatchPlus for both), not assumed. A team or
    // CTF gametype that does not derive from DeathMatchPlus leaves
    // round_time_frac/score_diff_norm at zero rather than guessing.
    local DeathMatchPlus TGI;
    local float TotalSecs, RemainSecs, Frac, ScoreFrac;

    TGI = DeathMatchPlus(Level.Game);
    if (TGI == None)
        return;

    if (TGI.TimeLimit > 0 && Level.Game.GameReplicationInfo != None)
    {
        TotalSecs = float(TGI.TimeLimit) * 60.0;
        RemainSecs = float(Level.Game.GameReplicationInfo.RemainingTime);
        Frac = 1.0 - (RemainSecs / TotalSecs);
        Obs[class'GBSchema'.static.OBS_ROUND_TIME_FRAC()] = FClamp(Frac, 0.0, 1.0);
    }
    if (TGI.FragLimit > 0 && B.PlayerReplicationInfo != None)
    {
        ScoreFrac = B.PlayerReplicationInfo.Score / float(TGI.FragLimit);
        Obs[class'GBSchema'.static.OBS_SCORE_DIFF_NORM()] = FClamp(ScoreFrac, -1.0, 1.0);
    }
    // OBJECTIVE (bomb/flag state) stays zero -- this adapter only reads the
    // generic DeathMatchPlus surface, not CTF/Domination specifics.
}

// -------------------------------------------------------------- action out

function ApplyAction(GBBot B, int Index, float DeltaTime)
{
    local vector Fwd, Right, Up, WishDir;
    local rotator NewView, YawOnly;

    if (Index < 0 || Index >= NumActiveBots || ActHave[Index] == 0)
        return;                       // no fresh answer -- Super.Tick()'s own
                                        // AI output stands, untouched

    // Overwriting Acceleration alone is not enough: Botpack's bot AI moves
    // via native pathfinding toward Pawn.MoveTarget (a NavigationPoint), not
    // purely by physics integrating Acceleration the way a player-controlled
    // Pawn does. Left set, the bot's own latent movement code keeps walking
    // it toward that target every tick regardless of what Acceleration says
    // -- measured live: with a no-op policy continuously sending zero
    // actions, one bot settled to a stop but another kept moving across a
    // full patrol cycle. Clearing MoveTarget every tick we have a fresh
    // answer removes the native pathing target so Acceleration is what's
    // left driving movement, which we do control.
    B.MoveTarget = None;

    NewView = B.ViewRotation;
    NewView.Pitch += DegToUnits(ActPitch[Index]);
    NewView.Yaw   += DegToUnits(ActYaw[Index]);
    B.ViewRotation = NewView;
    B.DesiredRotation = NewView;

    YawOnly.Yaw = NewView.Yaw;
    YawOnly.Pitch = 0;
    YawOnly.Roll = 0;
    GetAxes(YawOnly, Fwd, Right, Up);
    WishDir = (Fwd * ActFwd[Index]) + (Right * ActSide[Index]);
    B.Acceleration = WishDir * B.AccelRate;

    B.bFire = HasButton(ActButtons[Index], class'GBSchema'.static.BTN_ATTACK());
    B.bAltFire = HasButton(ActButtons[Index], class'GBSchema'.static.BTN_ATTACK2());
    B.bDuck = HasButton(ActButtons[Index], class'GBSchema'.static.BTN_CROUCH());

    if (bDebug)
    {
        Log("gamebots(ut99): bot " $ Index $ " fwd=" $ ActFwd[Index]
            $ " side=" $ ActSide[Index] $ " buttons=" $ ActButtons[Index]);
    }
}

function int DegToUnits(float Deg)
{
    return int(Deg * 65536.0 / 360.0);
}

// Returns byte, not bool: Pawn.bFire/bAltFire/bDuck are all declared as BYTE
// on the engine side (`ucc packagedump Engine` confirms this -- the "b" naming
// convention is a historical lie for these three specifically), so this
// avoids depending on an implicit bool->byte conversion for the assignment.
function byte HasButton(int Buttons, int Flag)
{
    if ((Buttons & Flag) != 0)
        return 1;
    return 0;
}

defaultproperties
{
    bAlwaysTick=True
    bEnabled=False
    NumBots=4
    TickRate=10.0
    PolicyHost="127.0.0.1"
    PolicyPort=27300
    ResponseTimeout=0.5
    bDebug=False
}
