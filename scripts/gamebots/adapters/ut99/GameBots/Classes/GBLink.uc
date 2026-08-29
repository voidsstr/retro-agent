//=============================================================================
// GBLink -- the policy-server client. Speaks the SAME schema (header, magic,
// per-bot obs entries, per-bot actions) `gb_client.c` speaks over AF_UNIX,
// wrapped in ASCII hex text and carried over `UdpLink` to policyd's
// `--udp-listen` endpoint. Not TCP, and not raw binary either -- both were
// tried first and both failed on this build, for two entirely different
// reasons. Full account below and in the adapter README's honest-verdict
// section; this is the design that actually works, verified end to end.
//
// HISTORY, kept because someone will otherwise repeat the first half:
//
//   1. TcpLink never completes an outbound connect on this OldUnreal 469e
//      Linux dedicated server -- `ss` shows not even a SYN leaving the
//      process, on loopback and on the LAN interface, across every
//      documented client pattern (StringToIpAddr+Open, Resolve+Open,
//      BindPort, bAlwaysTick). But our OWN live ut99-server answers a
//      489-byte GameSpy query on UDP 7798 through IpDrv's OTHER link class,
//      which is proof of real send-and-receive UDP traffic inside this exact
//      process -- so the blocker was TcpLink specifically, not "UT99 can't
//      do networking", and UdpLink was the next thing to try.
//
//   2. UdpLink CAN send real datagrams -- confirmed byte-for-byte correct on
//      a receiving Python socket, both via `SendBinary` and `SendText`. But
//      `UdpLink.ReceivedBinary`'s `B` byte-array parameter never carries real
//      payload content on THIS build: `Count` correctly reports the true
//      datagram size every time, but `B` is uninitialised memory (verified by
//      searching for a known incrementing byte pattern across the full
//      255-byte buffer and never finding it -- the same stack-address-shaped
//      garbage came back regardless of the reply's actual size or content).
//      Calling `ReadBinary()` manually, immediately, from inside the event
//      -- in case the event was only a "data is ready" notification -- also
//      returned nothing (`n=0`). `SendText`/`ReceivedText`, tried next, work
//      correctly in both directions: content round-trips byte-for-byte.
//
// So the wire is: pack the request exactly as `schema.py`/`gb_client.c`
// define it, hex-encode every byte to two ASCII characters (GBMath.HexChar),
// and send the WHOLE thing in one `SendText` call -- measured intact up to at
// least 25,000 characters on this build, comfortably above the largest
// request this adapter ever sends (MAX_BOTS=16 -> 9296 raw bytes -> 18,592
// hex characters). The reply direction has its own ceiling: `ReceivedText`
// truncates to exactly 4095 characters no matter how much more was sent --
// also measured, not assumed -- but a 16-bot action reply is header(16) +
// 16*24 = 400 raw bytes = 800 hex characters, nowhere near that limit.
//
// Because a datagram is the frame (no chunking, no accumulation), and UDP is
// connectionless (no Open/Opened/Closed/IsConnected), this class is far
// simpler than the TcpLink version it replaced: resolve the destination once
// at Init(), and every exchange is "build one hex string, SendText it" /
// "one ReceivedText call, decode it". A lost or malformed datagram is just a
// datagram that never updates LastGoodReplyTime -- GBMutator reads that
// staleness and reports the same fallback state a timeout used to.
//=============================================================================
class GBLink extends UdpLink;

var GBMutator Brain;
var IpAddr    ServerAddr;
var bool      bAddrKnown;

var float     LastGoodReplyTime;    // Level.TimeSeconds of the last accepted reply

var string    HexOut;               // accumulates one outgoing request

// ------------------------------------------------------------------ setup

function Init(GBMutator InOwner)
{
    Brain = InOwner;
    LinkMode = MODE_Text;
    ReceiveMode = RMODE_Event;
    BindPort();

    // UDP is connectionless: resolving the destination is a one-time,
    // synchronous lookup, not a connect() that can hang or fail async the
    // way TcpLink's did. "127.0.0.1" and a LAN IP both resolved correctly
    // in testing; DNS names would too, StringToIpAddr handles both.
    bAddrKnown = StringToIpAddr(Brain.PolicyHost, ServerAddr);
    if (bAddrKnown)
        ServerAddr.Port = Brain.PolicyPort;
    else
        Brain.Log("GBLink: cannot resolve PolicyHost '" $ Brain.PolicyHost $ "'");
}

function bool HasServerAddr()
{
    return bAddrKnown;
}

// -------------------------------------------------------------- outgoing

function BeginRequest(int NBots, int Tick)
{
    HexOut = "";
    AppendByte(class'GBSchema'.static.REQ_MAGIC_0());
    AppendByte(class'GBSchema'.static.REQ_MAGIC_1());
    AppendByte(class'GBSchema'.static.REQ_MAGIC_2());
    AppendByte(class'GBSchema'.static.REQ_MAGIC_3());
    AppendU32(class'GBSchema'.static.SCHEMA_HASH());
    AppendU16(NBots);
    AppendU16(class'GBSchema'.static.FLAG_NONE());
    AppendU32(Tick);
}

function WriteBotObs(int BotId, float Obs[144])
{
    local int i;
    AppendU16(BotId);
    AppendU16(0);                       // pad
    for (i = 0; i < class'GBSchema'.static.OBS_DIM(); i++)
        AppendFloat(Obs[i]);
}

function EndRequest()
{
    if (!bAddrKnown)
        return;                          // nothing to send to; caller
                                          // (GBMutator) is on the fallback
                                          // path already via HasServerAddr()
    SendText(ServerAddr, HexOut);
}

function AppendByte(int V)
{
    HexOut = HexOut $ Chr(class'GBMath'.static.HexChar((V >>> 4) & 0xF))
                    $ Chr(class'GBMath'.static.HexChar(V & 0xF));
}

function AppendU16(int V)
{
    AppendByte(V & 0xFF);
    AppendByte((V >>> 8) & 0xFF);
}

function AppendU32(int V)
{
    AppendByte(V & 0xFF);
    AppendByte((V >>> 8) & 0xFF);
    AppendByte((V >>> 16) & 0xFF);
    AppendByte((V >>> 24) & 0xFF);
}

function AppendFloat(float F)
{
    AppendU32(class'GBMath'.static.FloatToBits(F));
}

// -------------------------------------------------------------- incoming

event ReceivedText(IpAddr Addr, string Text)
{
    local int nBots;
    local int hash;
    local int i, off;
    local int botId, buttons;
    local float pitch, yaw, fwd, side;
    local byte weapon;
    local int len;

    len = Len(Text);
    if (len < class'GBSchema'.static.HEADER_SIZE() * 2)
    {
        Brain.Log("GBLink: reply too short (" $ len $ " hex chars)");
        return;
    }

    if (ByteAt(Text, 0) != class'GBSchema'.static.RESP_MAGIC_0()
        || ByteAt(Text, 1) != class'GBSchema'.static.RESP_MAGIC_1()
        || ByteAt(Text, 2) != class'GBSchema'.static.RESP_MAGIC_2()
        || ByteAt(Text, 3) != class'GBSchema'.static.RESP_MAGIC_3())
    {
        Brain.Log("GBLink: bad response magic");
        return;
    }

    hash = U32At(Text, 4);
    if (hash != class'GBSchema'.static.SCHEMA_HASH())
    {
        // The single most valuable error message in the system -- see
        // schema.py's unpack_request(). An adapter built against a different
        // field table is otherwise undetectable.
        Brain.Log("GBLink: schema hash mismatch (policyd sent " $ hash
            $ ", adapter built with " $ class'GBSchema'.static.SCHEMA_HASH()
            $ ") -- rebuild the adapter against the current schema.py");
        return;
    }

    nBots = U16At(Text, 8);
    if (len < (class'GBSchema'.static.HEADER_SIZE()
               + nBots * class'GBSchema'.static.ACTION_SIZE()) * 2)
    {
        Brain.Log("GBLink: reply truncated for " $ nBots $ " bots ("
            $ len $ " hex chars)");
        return;
    }

    off = class'GBSchema'.static.HEADER_SIZE();
    for (i = 0; i < nBots; i++)
    {
        botId   = U16At(Text, off);
        buttons = U16At(Text, off + 2);
        pitch = class'GBMath'.static.BitsToFloat(U32At(Text, off + 4));
        yaw   = class'GBMath'.static.BitsToFloat(U32At(Text, off + 8));
        fwd   = class'GBMath'.static.BitsToFloat(U32At(Text, off + 12));
        side  = class'GBMath'.static.BitsToFloat(U32At(Text, off + 16));
        weapon = ByteAt(Text, off + 20);
        // off+21 pad0, off+22..23 reserved -- unused

        Brain.OnAction(botId, buttons, pitch, yaw, fwd, side, weapon);
        off += class'GBSchema'.static.ACTION_SIZE();
    }

    LastGoodReplyTime = Level.TimeSeconds;
}

// Byte offset -> the pair of hex characters at ByteOffset*2. Returns -1 (an
// impossible byte value cast from a real 0-255 range) if either character
// is not a hex digit, so a corrupt or foreign datagram is caught rather than
// decoded into silent garbage.
function int ByteAt(string S, int ByteOffset)
{
    local int hi, lo;
    hi = class'GBMath'.static.HexValue(Asc(Mid(S, ByteOffset * 2, 1)));
    lo = class'GBMath'.static.HexValue(Asc(Mid(S, ByteOffset * 2 + 1, 1)));
    if (hi < 0 || lo < 0)
        return -1;
    return (hi << 4) | lo;
}

function int U16At(string S, int ByteOffset)
{
    return ByteAt(S, ByteOffset) | (ByteAt(S, ByteOffset + 1) << 8);
}

function int U32At(string S, int ByteOffset)
{
    return ByteAt(S, ByteOffset) | (ByteAt(S, ByteOffset + 1) << 8)
         | (ByteAt(S, ByteOffset + 2) << 16) | (ByteAt(S, ByteOffset + 3) << 24);
}

defaultproperties
{
}
