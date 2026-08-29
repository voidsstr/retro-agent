//=============================================================================
// GBBot -- a Bot that lets GBMutator drive it.
//
// This IS the fallback discipline, and it is the whole reason GBBot exists
// rather than poking a stock Bot's fields from outside: Bot.Tick() (inherited
// through TournamentPlayer/PlayerPawn/Pawn) is where UT99's own bot AI reads
// the world and writes Acceleration/DesiredRotation/bFire for this tick. We
// cannot see or influence what happens *inside* that call, but we CAN call it
// first and overwrite its output afterward -- the same ordering gb_adapter.c
// uses in Quake III ("botlib still runs and still produces a complete
// usercmd, so our policy declining to answer degrades to the stock bot rather
// than to a bot standing still"). If GBMutator has no fresh action for this
// bot this tick -- policy server down, still connecting, or the mutator is
// disabled -- Super.Tick()'s own output is simply left standing. There is no
// separate "fallback path" to maintain; it is the AI that was already there.
//
// This also means: **the mutator must spawn its bots as this class** for any
// of it to apply. It does not intercept the server's own `addbot`/autostart
// bots (those stay ordinary Botpack.Bot, driven by the AI alone) -- see the
// README for why GBMutator spawns its own roster directly instead of trying
// to reclassify stock bots.
//=============================================================================
class GBBot extends Bot;

var GBMutator Brain;
var int       MyIndex;      // this bot's slot in Brain's per-bot arrays

// Previous-frame state, for the observation's "what just happened" group.
// The engine hands us no deltas, so -- same as gb_adapter.c's gb_prev_health
// et al -- we keep last frame ourselves.
var int  PrevHealth;
var int  PrevScore;
var bool PrevAlive;
var bool bGBInitialized;

event Tick(float DeltaTime)
{
    Super.Tick(DeltaTime);

    if (!bGBInitialized)
    {
        PrevHealth = Health;
        PrevScore  = 0;
        if (PlayerReplicationInfo != None)
            PrevScore = PlayerReplicationInfo.Score;
        PrevAlive  = (Health > 0);
        bGBInitialized = True;
    }

    if (Brain != None)
        Brain.ApplyAction(Self, MyIndex, DeltaTime);
}

defaultproperties
{
    MyIndex=-1
}
