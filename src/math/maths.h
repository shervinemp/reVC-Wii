#ifndef __GTA_MATHS_H__
#define __GTA_MATHS_H__

// wrapper around float versions of functions
// in gta they are in CMaths but that makes the code rather noisy

#ifdef GEKKO
// Broadway has no fsqrt, so newlib's sqrtf is its integer fallback: a 25-step
// bit-at-a-time loop with a data-dependent branch in it, a few hundred cycles a
// call.  What the chip does have is frsqrte, a reciprocal square root estimate,
// and three Newton-Raphson steps in double take that from the architecture's
// worst case of 5 correct bits to more than float holds -- the scheme
// Nintendo's own runtime used.  About 40 cycles, no branch in the common case.
//
// Only for a positive, normal, finite x.  The callers below sort out the rest.
// Each step is written as square, then 1.5 - half*square, so it compiles to
// fmul, fnmsub, fmul.
inline double WiiRecipSqrtNormal(double x)
{
	double estimate;
	__asm__("frsqrte %0,%1" : "=f" (estimate) : "f" (x));
	double half = 0.5*x;
	double square = estimate*estimate;
	estimate = estimate*(1.5 - half*square);
	square = estimate*estimate;
	estimate = estimate*(1.5 - half*square);
	square = estimate*estimate;
	estimate = estimate*(1.5 - half*square);
	return estimate;
}

// Zero, a denormal, infinity, NaN or a negative; in WiiMath.cpp.  Out of line
// so that every inlined square root carries only the common case.
float WiiSqrtEdge(float x) __attribute__((noinline, cold));

inline float WiiSqrt(float x)
{
	if(__builtin_expect(x >= 1.17549435e-38f && x <= 3.40282347e+38f, 1))
		return (float)(x*WiiRecipSqrtNormal(x));
	return WiiSqrtEdge(x);
}
#endif

inline float Sin(float x) { return sinf(x); }
inline float Asin(float x) { return asinf(x); }
inline float Cos(float x) { return cosf(x); }
inline float Acos(float x) { return acosf(x); }
inline float Tan(float x) { return tanf(x); }
inline float Atan(float x) { return atanf(x); }
inline float Atan2(float y, float x) { return atan2f(y, x); }
inline float Abs(float x) { return fabsf(x); }
#ifdef GEKKO
inline float Sqrt(float x) { return WiiSqrt(x); }
#else
inline float Sqrt(float x) { return sqrtf(x); }
#endif
#ifdef GTA_PS2
inline float RecipSqrt(float x, float y)
{
	float ret;
	__asm__ __volatile__("\n\
		rsqrt.s	%0,%1,%2\n\
		" : "=f" (ret)
		  : "f" (x), "f" (y)
	);
	return ret;
}
#elif defined(GEKKO)
// x/sqrt(y) without the divide, which costs as much again as the square root.
// Anything but a positive normal y keeps the exact x/Sqrt(y), including the
// +inf that a zero length gives some callers (Normalise2D does not test for it).
inline float RecipSqrt(float x, float y)
{
	if(__builtin_expect(y >= 1.17549435e-38f && y <= 3.40282347e+38f, 1))
		return (float)(x*WiiRecipSqrtNormal(y));
	return x/Sqrt(y);
}
#else
inline float RecipSqrt(float x, float y) { return x/Sqrt(y); }
#endif
inline float RecipSqrt(float x) { return RecipSqrt(1.0f, x); }
inline float Pow(float x, float y) { return powf(x, y); }
inline float Floor(float x) { return floorf(x); }
inline float Ceil(float x) { return ceilf(x); }

#endif // __GTA_MATHS_H__
