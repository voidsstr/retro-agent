//=============================================================================
// GBMath -- software IEEE-754 single-precision float <-> bit-pattern
// conversion, and hex nibble<->char encoding, for the gamebots wire protocol.
//
// THE reason the float codec exists: UnrealScript (this OldUnreal 469e build)
// has no operator that reinterprets a float's bit pattern as an int -- no
// union, no pointer cast, no native "FloatAsInt" call. `gb_client.c` gets this
// for free from a single `memcpy`; here it has to be computed. Investigated
// and confirmed absent (not merely undiscovered): `ucc packagedump Core`
// lists every native function Core/Engine expose, and none of GetAxes/Normal/
// VSize/Rotator/Vector -- the ones this adapter DOES use -- is a bit-cast. So
// this is arithmetic, not a shortcut avoided out of laziness.
//
// The algorithm is the textbook software float encoder: normalise the
// magnitude into [1,2) by repeated multiply/divide by 2 (our values are all
// small -- observations are pre-clamped to roughly [-2000,2000] and actions
// to single-digit ranges, so this loop runs at most ~11 times), then read off
// the exponent and a 23-bit mantissa, and assemble sign/exponent/mantissa with
// integer bitwise ops (UnrealScript has <<, >>>, &, | on int -- confirmed the
// same way). NaN/Inf/subnormal are not encoded on the way out (nothing we send
// is ever non-finite) and are not specially decoded on the way in: a garbage
// exponent from a half-trained policy decodes to *some* finite float, and the
// caller clamps immediately after (GBMutator.ApplyAction), exactly the
// distrust gb_client.c's gb_clamp() applies to every other engine's policy
// answer.
//
// THE reason the hex helpers exist: see GBLink.uc's header comment for the
// full story, but in short -- `UdpLink.ReceivedBinary`'s `B` byte-array
// parameter never carries real payload content on this build (`Count` is
// correct, `B` is uninitialised memory, verified byte-for-byte against a
// known pattern), while `SendText`/`ReceivedText` deliver content correctly.
// So the wire payload is hex-encoded ASCII text, and these are the two
// halves of that codec: a nibble (0-15) to its ASCII hex digit, and back.
//=============================================================================
class GBMath extends Object;

const TWO_POW_23 = 8388608;   // 2^23 -- the mantissa scale
const BIAS       = 127;

// ---- float -> IEEE754 bit pattern (as a 32-bit int, two's complement) -----
static final function int FloatToBits(float F)
{
    local int sign;
    local int exp;
    local int mantissa;
    local float af;

    if (F == 0.0)
        return 0;

    sign = 0;
    af = F;
    if (af < 0.0)
    {
        sign = 1;
        af = -af;
    }

    exp = 0;
    while (af >= 2.0)
    {
        af = af * 0.5;
        exp++;
    }
    while (af < 1.0)
    {
        af = af * 2.0;
        exp--;
    }

    // af is now in [1,2). mantissa = (af - 1) * 2^23, rounded to nearest.
    mantissa = int((af - 1.0) * float(TWO_POW_23) + 0.5);
    if (mantissa >= TWO_POW_23)
    {
        // rounded up into the next power of two
        mantissa = 0;
        exp++;
    }

    return (sign << 31) | ((exp + BIAS) << 23) | mantissa;
}

// ---- IEEE754 bit pattern -> float ------------------------------------------
static final function float BitsToFloat(int Bits)
{
    local int sign;
    local int exp;
    local int mantissa;
    local float result;
    local int i;

    if (Bits == 0)
        return 0.0;

    sign     = (Bits >>> 31) & 1;
    exp      = ((Bits >>> 23) & 255) - BIAS;
    mantissa = Bits & 0x7FFFFF;

    result = 1.0 + (float(mantissa) / float(TWO_POW_23));

    if (exp > 0)
    {
        for (i = 0; i < exp; i++)
            result = result * 2.0;
    }
    else if (exp < 0)
    {
        for (i = 0; i < -exp; i++)
            result = result * 0.5;
    }

    if (sign == 1)
        result = -result;

    return result;
}

// ---- hex nibble <-> ASCII char, for the text-wrapped wire encoding --------
// Lower-case, matching schema.py's own `f"{x:02x}"` formatting on the
// policyd side, though case does not actually matter -- HexValue below
// accepts both.

static final function int HexChar(int Nibble)
{
    if (Nibble < 10)
        return 48 + Nibble;        // '0'..'9'
    return 87 + Nibble;            // 'a'..'f' (97 + (Nibble-10))
}

// Returns -1 for a non-hex-digit character, so a corrupt/foreign datagram is
// detected rather than silently decoded into garbage.
static final function int HexValue(int Ch)
{
    if (Ch >= 48 && Ch <= 57)
        return Ch - 48;             // '0'-'9'
    if (Ch >= 97 && Ch <= 102)
        return Ch - 87;             // 'a'-'f'
    if (Ch >= 65 && Ch <= 70)
        return Ch - 55;             // 'A'-'F'
    return -1;
}

defaultproperties
{
}
