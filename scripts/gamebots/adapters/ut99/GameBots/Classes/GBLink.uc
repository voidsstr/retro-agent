//=============================================================================
// GBLink -- the policy-server client. Speaks the identical wire protocol
// gb_client.c speaks over AF_UNIX, over TCP instead: UnrealScript's only
// outbound networking is TcpLink, and policyd.py's --tcp-listen endpoint
// (added for exactly this adapter) accepts the same bytes on a TCP socket.
//
// Everything here is asynchronous by construction, not by careful discipline:
// TcpLink's Open/SendBinary never block, and a response arrives later as a
// ReceivedBinary event whenever the engine's network tick delivers it -- there
// is no call in UnrealScript that "sends and waits". GBMutator relies on that:
// it starts an exchange and moves on; if a response has not arrived by the
// next policy tick, that is read as "no answer yet" and the bots keep running
// on whatever the built-in AI already set (see GBBot.uc). A dead or slow
// policy server therefore degrades the bots, it cannot stall the server --
// there is no code path here that even could block the server's Tick loop.
//
// Two gotchas that shaped this file:
//
//   1. SendBinary/ReadBinary/ReceivedBinary all move byte[255] AT MOST per
//      call (`function int SendBinary(int Count, byte B[255])` -- confirmed
//      via `ucc packagedump IpDrv`, not assumed). One bot's observation is
//      580 bytes on its own; a multi-bot batch is much larger. So every send
//      is chunked through SendChunk[255], and every receive is reassembled in
//      RecvBuf[] across as many ReceivedBinary events as it takes.
//   2. There is no bit-cast from float to int in UnrealScript, so every float
//      in and out goes through GBMath's software IEEE-754 codec. See
//      GBMath.uc's header comment for why, and the adapter README for the
//      honest verdict on what that costs.
//=============================================================================
class GBLink extends TcpLink;

const RECV_BUF_SIZE = 2048;   // header + up to ~84 bots' worth of actions --
                              // comfortably above MAX_BOTS in GBMutator

var GBMutator Brain;

// --- outgoing: a streaming byte writer over one 255-byte TcpLink chunk -----
var byte SendChunk[255];
var int  SendFill;
var int  SendTotal;          // bytes written this request, for logging only

// --- incoming: reassembly buffer across possibly many ReceivedBinary calls -
var byte RecvBuf[RECV_BUF_SIZE];
var int  RecvLen;
var int  ExpectedLen;        // -1 until the header's bot count is known

// --- round-trip bookkeeping --------------------------------------------
var bool  bAwaitingResponse;
var float RequestSentTime;
var float LastConnectAttempt;
var bool  bResolving;

const RECONNECT_COOLDOWN = 2.0;   // seconds; matches gb_client.c's 2s cooldown

// ------------------------------------------------------------------ setup

function Init(GBMutator InOwner)
{
    Brain = InOwner;
    LinkMode = MODE_Binary;
    ReceiveMode = RMODE_Event;
    // The standard TcpLink client pattern binds a local port BEFORE the
    // first Open() -- the underlying native socket has nothing to connect
    // *from* otherwise. Omitting this was the actual cause of an early
    // silent-failure mode during development: Open() returned true, nothing
    // ever reached policyd, and neither Opened() nor Closed() ever fired.
    BindPort();
}

// -------------------------------------------------------------- connect

function bool EnsureConnected()
{
    if (IsConnected())
        return True;
    if (bResolving)
        return False;          // a Resolved()/ResolveFailed() is already due

    if (Level.TimeSeconds - LastConnectAttempt < RECONNECT_COOLDOWN)
        return False;         // a dead policy server costs one attempt every
                               // couple of seconds, not one per bot per tick
    LastConnectAttempt = Level.TimeSeconds;

    RecvLen = 0;
    ExpectedLen = -1;
    bAwaitingResponse = False;

    // Resolve() + the Resolved() event, NOT StringToIpAddr()+Open() directly.
    // The direct form compiled, ran, and Open() returned true every time, but
    // no SYN packet ever left the box (confirmed with `ss` against the policy
    // server's listening socket) and neither Opened() nor Closed() ever
    // fired -- a silent dead end on this build even for a numeric loopback
    // address. Resolve() is the documented TcpLink client pattern and is
    // what actually produces a connection; see the README's honest verdict.
    bResolving = True;
    Resolve(Brain.PolicyHost);
    return False;
}

event Resolved(IpAddr Addr)
{
    bResolving = False;
    Addr.Port = Brain.PolicyPort;
    if (!Open(Addr))
        Brain.Log("GBLink: Open() failed, err=" $ GetLastError());
    // NOTE: on the OldUnreal 469e Linux dedicated server this was developed
    // and tested against, Open() reports success here but no connection is
    // ever actually established -- confirmed with `ss` that not even a SYN
    // packet leaves the process, on loopback OR the LAN interface, and the
    // destination process never sees an attempt. See the adapter README's
    // honest-verdict section for the full account and what was ruled out.
}

event ResolveFailed()
{
    bResolving = False;
    Brain.Log("GBLink: cannot resolve PolicyHost '" $ Brain.PolicyHost $ "'");
}

event Opened()
{
    Brain.OnLinkOpened();
}

event Closed()
{
    RecvLen = 0;
    ExpectedLen = -1;
    bAwaitingResponse = False;
    Brain.OnLinkClosed();
}

// ------------------------------------------------------------ send path

function BeginRequest(int NBots, int Tick)
{
    SendFill = 0;
    SendTotal = 0;
    WriteByte(class'GBSchema'.static.REQ_MAGIC_0());
    WriteByte(class'GBSchema'.static.REQ_MAGIC_1());
    WriteByte(class'GBSchema'.static.REQ_MAGIC_2());
    WriteByte(class'GBSchema'.static.REQ_MAGIC_3());
    WriteU32(class'GBSchema'.static.SCHEMA_HASH());
    WriteU16(NBots);
    WriteU16(class'GBSchema'.static.FLAG_NONE());
    WriteU32(Tick);
}

function WriteBotObs(int BotId, float Obs[144])
{
    local int i;
    WriteU16(BotId);
    WriteU16(0);                       // pad
    for (i = 0; i < class'GBSchema'.static.OBS_DIM(); i++)
        WriteFloat(Obs[i]);
}

function EndRequest()
{
    FlushChunk();
    bAwaitingResponse = True;
    RequestSentTime = Level.TimeSeconds;
    RecvLen = 0;
    ExpectedLen = -1;
}

function WriteByte(byte B)
{
    SendChunk[SendFill] = B;
    SendFill++;
    SendTotal++;
    if (SendFill >= 255)
        FlushChunk();
}

function WriteU16(int Value)
{
    WriteByte(Value & 0xFF);
    WriteByte((Value >>> 8) & 0xFF);
}

function WriteU32(int Value)
{
    WriteByte(Value & 0xFF);
    WriteByte((Value >>> 8) & 0xFF);
    WriteByte((Value >>> 16) & 0xFF);
    WriteByte((Value >>> 24) & 0xFF);
}

function WriteFloat(float F)
{
    WriteU32(class'GBMath'.static.FloatToBits(F));
}

function FlushChunk()
{
    if (SendFill > 0)
    {
        SendBinary(SendFill, SendChunk);
        SendFill = 0;
    }
}

// ----------------------------------------------------------- receive path

event ReceivedBinary(int Count, byte B[255])
{
    local int i;

    for (i = 0; i < Count; i++)
    {
        if (RecvLen >= RECV_BUF_SIZE)
        {
            // A response bigger than we sized for is not a bigger bot count
            // than we asked about -- it is a desynchronised stream (or a
            // schema mismatch that changed ACTION_SIZE). Drop it the same way
            // gb_client.c does rather than guess where the framing resumes.
            Brain.Log("GBLink: response overflowed RecvBuf, dropping connection");
            Close();
            return;
        }
        RecvBuf[RecvLen] = B[i];
        RecvLen++;
    }

    if (ExpectedLen < 0 && RecvLen >= class'GBSchema'.static.HEADER_SIZE())
    {
        if (!CheckHeader())
        {
            Close();            // bad magic/hash -- see CheckHeader's log line
            return;
        }
    }

    if (ExpectedLen > 0 && RecvLen >= ExpectedLen)
    {
        ParseActions();
        bAwaitingResponse = False;
        RecvLen = 0;
        ExpectedLen = -1;
    }
}

function bool CheckHeader()
{
    local int nBots;
    local int hash;

    if (RecvBuf[0] != class'GBSchema'.static.RESP_MAGIC_0() || RecvBuf[1] != class'GBSchema'.static.RESP_MAGIC_1()
        || RecvBuf[2] != class'GBSchema'.static.RESP_MAGIC_2() || RecvBuf[3] != class'GBSchema'.static.RESP_MAGIC_3())
    {
        Brain.Log("GBLink: bad response magic");
        return False;
    }

    hash = RecvBuf[4] | (RecvBuf[5] << 8) | (RecvBuf[6] << 16) | (RecvBuf[7] << 24);
    if (hash != class'GBSchema'.static.SCHEMA_HASH())
    {
        // The single most valuable error message in the system -- see
        // schema.py's unpack_request(). An adapter built against a different
        // field table is otherwise undetectable.
        Brain.Log("GBLink: schema hash mismatch (policyd sent " $ hash
            $ ", adapter built with " $ class'GBSchema'.static.SCHEMA_HASH()
            $ ") -- rebuild the adapter against the current schema.py");
        return False;
    }

    nBots = RecvBuf[8] | (RecvBuf[9] << 8);
    ExpectedLen = class'GBSchema'.static.HEADER_SIZE() + nBots * class'GBSchema'.static.ACTION_SIZE();
    return True;
}

function ParseActions()
{
    local int nBots;
    local int i, off;
    local int botId, buttons;
    local float pitch, yaw, fwd, side;
    local byte weapon;

    nBots = RecvBuf[8] | (RecvBuf[9] << 8);
    off = class'GBSchema'.static.HEADER_SIZE();
    for (i = 0; i < nBots; i++)
    {
        botId   = RecvBuf[off]      | (RecvBuf[off + 1] << 8);
        buttons = RecvBuf[off + 2]  | (RecvBuf[off + 3] << 8);
        pitch = class'GBMath'.static.BitsToFloat(RecvBuf[off + 4]  | (RecvBuf[off + 5] << 8)
                    | (RecvBuf[off + 6] << 16)  | (RecvBuf[off + 7] << 24));
        yaw   = class'GBMath'.static.BitsToFloat(RecvBuf[off + 8]  | (RecvBuf[off + 9] << 8)
                    | (RecvBuf[off + 10] << 16) | (RecvBuf[off + 11] << 24));
        fwd   = class'GBMath'.static.BitsToFloat(RecvBuf[off + 12] | (RecvBuf[off + 13] << 8)
                    | (RecvBuf[off + 14] << 16) | (RecvBuf[off + 15] << 24));
        side  = class'GBMath'.static.BitsToFloat(RecvBuf[off + 16] | (RecvBuf[off + 17] << 8)
                    | (RecvBuf[off + 18] << 16) | (RecvBuf[off + 19] << 24));
        weapon = RecvBuf[off + 20];
        // off+21 pad0, off+22..23 reserved -- unused

        Brain.OnAction(botId, buttons, pitch, yaw, fwd, side, weapon);
        off += class'GBSchema'.static.ACTION_SIZE();
    }
}

defaultproperties
{
    RecvLen=0
    ExpectedLen=-1
    // TcpLink's native socket polling runs from ITS OWN Tick(), same as any
    // other Actor -- GBMutator being always-ticked does not imply GBLink is.
    // Without this, Open() completed the local setup but nothing ever
    // serviced the connect()/read/write afterward: no SYN even appeared on
    // the wire. Found the hard way; see the README.
    bAlwaysTick=True
}
