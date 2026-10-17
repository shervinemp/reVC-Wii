// The one sqrtf the whole program links against.
//
// maths.h gives the game's own Sqrt() the frsqrte path inline, but plenty of
// square roots on this port never go through it: librw and parts of the game
// call sqrtf directly.  An executable's own definition of a C library function
// wins over the archive one at link time, so this replaces newlib's integer-loop
// sqrtf (a few hundred cycles a call on Broadway, which has no fsqrt) for every
// one of those callers and for librw's static library alike, without touching
// any of them.
//
// errno is the only behaviour given up: newlib sets EDOM for a negative
// argument, and nothing in this program reads errno after a square root.

#include <math.h>

#include "maths.h"

// IEEE behaviour at the edges: -0, +0, NaN and +inf come back as themselves, a
// negative gives NaN, and a denormal is scaled into range by an exact power of
// two first, because frsqrte sees one as zero when the FPU flushes denormals.
float
WiiSqrtEdge(float x)
{
	if(x > 3.40282347e+38f)
		return x;
	if(x > 0.0f){
		double scaled = (double)x*16777216.0;
		return (float)(scaled*WiiRecipSqrtNormal(scaled)*(1.0/4096.0));
	}
	if(x < 0.0f)
		return __builtin_nanf("");
	return x;
}

extern "C" float
sqrtf(float x)
{
	return WiiSqrt(x);
}
