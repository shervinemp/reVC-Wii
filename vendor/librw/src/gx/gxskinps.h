#pragma once

// Paired-single skinning kernels for Broadway, used by gxskin.cpp.
//
// Broadway's FPU works on pairs of floats, and a skinned vertex is mostly
// pairs: the x and y of a position or a normal take one instruction where the
// scalar code takes two.  Each vertex first blends its bones' matrices into one
// (six paired multiply-adds a bone) and then transforms its position and its
// normal once, instead of transforming them once per bone.
//
// GCC has had no paired-single builtins since GCC 9, and it never knows a
// register holds a pair: it would spill one with stfd and lose the second half.
// So each vertex is a single asm block from its loads to its stores, and no pair
// ever crosses the block's edge.  A thread switch inside one is safe: libogc's
// FPU context handler saves and restores both halves of every register
// (psq_st/psq_l), and GQR0 is the plain float format libogc's own paired matrix
// code (ps_guMtx*) already depends on.

namespace rw {
namespace gx {

// A bone matrix laid out in pairs: the x and y rows side by side, z apart.
//   (rx ry) (ux uy) (ax ay) (px py) (rz uz) (az pz)
// Offsets 0, 8, 16, 24, 32 and 40, which the asm below hardcodes.
struct PairedBone
{
	float32 rx, ry, ux, uy, ax, ay, px, py, rz, uz, az, pz;
};

// One vertex's bones and weights, only the ones that count, in the first
// `count` entries.  The asm hardcodes this layout too: bone pointers at 0,
// weights at 16.
struct PairedSkinVertex
{
	const PairedBone *bone[4];
	float32 weight[4];
};

// 0.5 and 1.5 for Newton-Raphson, and the squared length below which a blended
// normal is too short to normalise and the bind normal is kept instead.
static const float32 pairedSkinConstants[4] __attribute__((aligned(8))) = {
	0.5f, 1.5f, 1.0e-12f, 0.0f
};

static inline void
makePairedBone(PairedBone *bone, const Matrix *matrix)
{
	bone->rx = matrix->right.x; bone->ry = matrix->right.y;
	bone->ux = matrix->up.x;    bone->uy = matrix->up.y;
	bone->ax = matrix->at.x;    bone->ay = matrix->at.y;
	bone->px = matrix->pos.x;   bone->py = matrix->pos.y;
	bone->rz = matrix->right.z; bone->uz = matrix->up.z;
	bone->az = matrix->at.z;    bone->pz = matrix->pos.z;
}

#ifdef GEKKO

// The blend shared by both kernels: f0-f5 = sum of weight * bone, in the
// PairedBone layout.  Expects %[v] and %[n] (count, at least 1, consumed) and
// scratch %[b] and %[j]; uses f6 and f7 and ctr.
#define GX_PAIRED_BLEND_BONES                                              \
	"lwz      %[b], 0(%[v])\n"                                         \
	"lfs      7, 16(%[v])\n"                                           \
	"psq_l    0, 0(%[b]), 0, 0\n"                                      \
	"psq_l    1, 8(%[b]), 0, 0\n"                                      \
	"psq_l    2, 16(%[b]), 0, 0\n"                                     \
	"psq_l    3, 24(%[b]), 0, 0\n"                                     \
	"psq_l    4, 32(%[b]), 0, 0\n"                                     \
	"psq_l    5, 40(%[b]), 0, 0\n"                                     \
	"ps_muls0 0, 0, 7\n"                                               \
	"ps_muls0 1, 1, 7\n"                                               \
	"ps_muls0 2, 2, 7\n"                                               \
	"ps_muls0 3, 3, 7\n"                                               \
	"ps_muls0 4, 4, 7\n"                                               \
	"ps_muls0 5, 5, 7\n"                                               \
	"addic.   %[n], %[n], -1\n"                                        \
	"beq      2f\n"                                                    \
	"mtctr    %[n]\n"                                                  \
	"mr       %[j], %[v]\n"                                            \
	"1:\n"                                                             \
	"lwzu     %[b], 4(%[j])\n"                                         \
	"lfs      7, 16(%[j])\n"                                           \
	"psq_l    6, 0(%[b]), 0, 0\n"                                      \
	"ps_madds0 0, 6, 7, 0\n"                                           \
	"psq_l    6, 8(%[b]), 0, 0\n"                                      \
	"ps_madds0 1, 6, 7, 1\n"                                           \
	"psq_l    6, 16(%[b]), 0, 0\n"                                     \
	"ps_madds0 2, 6, 7, 2\n"                                           \
	"psq_l    6, 24(%[b]), 0, 0\n"                                     \
	"ps_madds0 3, 6, 7, 3\n"                                           \
	"psq_l    6, 32(%[b]), 0, 0\n"                                     \
	"ps_madds0 4, 6, 7, 4\n"                                           \
	"psq_l    6, 40(%[b]), 0, 0\n"                                     \
	"ps_madds0 5, 6, 7, 5\n"                                           \
	"bdnz     1b\n"                                                    \
	"2:\n"

// The position through the blended matrix:
//   xy = (rx ry)*x + (ux uy)*y + (ax ay)*z + (px py)
//   z  = (rz*x + uz*y) + (az*z + pz*1)
#define GX_PAIRED_TRANSFORM_POSITION                                       \
	"psq_l    8, 0(%[sp]), 0, 0\n"                                     \
	"psq_l    9, 8(%[sp]), 1, 0\n"                                     \
	"ps_madds0 10, 0, 8, 3\n"                                          \
	"ps_madds1 10, 1, 8, 10\n"                                         \
	"ps_madds0 10, 2, 9, 10\n"                                         \
	"ps_mul   11, 4, 8\n"                                              \
	"ps_madd  11, 5, 9, 11\n"                                          \
	"ps_sum0  11, 11, 11, 11\n"                                        \
	"psq_st   10, 0(%[dp]), 0, 0\n"                                    \
	"stfs     11, 8(%[dp])\n"

// Position only, for a skin without normals.
static inline void
skinPositionPaired(const PairedSkinVertex *vertex, uint32 count,
	const V3d *position, V3d *outPosition)
{
	const PairedBone *bone;
	const PairedSkinVertex *cursor;
	__asm__ volatile(
		GX_PAIRED_BLEND_BONES
		GX_PAIRED_TRANSFORM_POSITION
		: [n] "+b" (count), [b] "=&b" (bone), [j] "=&b" (cursor)
		: [v] "b" (vertex), [sp] "b" (position), [dp] "b" (outPosition)
		: "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7",
		  "fr8", "fr9", "fr10", "fr11", "ctr", "cr0", "memory");
}

// Position and normal.  The blended normal is normalised with frsqrte and two
// Newton-Raphson steps (about 20 bits); one whose squared length is under
// 1e-12 keeps the bind normal, as the scalar blend did.
static inline void
skinVertexPaired(const PairedSkinVertex *vertex, uint32 count,
	const V3d *position, V3d *outPosition,
	const V3d *normal, V3d *outNormal)
{
	const PairedBone *bone;
	const PairedSkinVertex *cursor;
	__asm__ volatile(
		GX_PAIRED_BLEND_BONES
		GX_PAIRED_TRANSFORM_POSITION
		// The normal through the blended matrix, without the translation:
		//   xy = (rx ry)*x + (ux uy)*y + (ax ay)*z
		//   z  = (rz*x + uz*y) + az*z
		"psq_l    8, 0(%[sn]), 0, 0\n"
		"psq_l    9, 8(%[sn]), 1, 0\n"
		"ps_muls0 10, 0, 8\n"
		"ps_madds1 10, 1, 8, 10\n"
		"ps_madds0 10, 2, 9, 10\n"
		"ps_mul   11, 4, 8\n"
		"ps_sum0  11, 11, 11, 11\n"
		"fmadds   11, 5, 9, 11\n"
		// Squared length, then its reciprocal square root.
		"ps_mul   12, 10, 10\n"
		"ps_sum0  12, 12, 12, 12\n"
		"fmadds   12, 11, 11, 12\n"
		"psq_l    0, 0(%[k]), 0, 0\n"
		"lfs      1, 8(%[k])\n"
		"frsqrte  13, 12\n"
		"fmuls    2, 12, 0\n"
		"ps_merge11 3, 0, 0\n"
		"fmuls    4, 13, 13\n"
		"fnmsubs  4, 2, 4, 3\n"
		"fmuls    13, 13, 4\n"
		"fmuls    4, 13, 13\n"
		"fnmsubs  4, 2, 4, 3\n"
		"fmuls    13, 13, 4\n"
		"ps_muls0 10, 10, 13\n"
		"fmuls    11, 11, 13\n"
		// Too short to normalise: keep the bind normal.  fsel and ps_sel
		// take the first source when the condition is >= 0, and a NaN from a
		// zero length fails that test too.
		"fsubs    1, 12, 1\n"
		"ps_merge00 1, 1, 1\n"
		"ps_sel   10, 1, 10, 8\n"
		"fsel     11, 1, 11, 9\n"
		"psq_st   10, 0(%[dn]), 0, 0\n"
		"stfs     11, 8(%[dn])\n"
		: [n] "+b" (count), [b] "=&b" (bone), [j] "=&b" (cursor)
		: [v] "b" (vertex), [sp] "b" (position), [dp] "b" (outPosition),
		  [sn] "b" (normal), [dn] "b" (outNormal),
		  [k] "b" (pairedSkinConstants)
		: "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7",
		  "fr8", "fr9", "fr10", "fr11", "fr12", "fr13",
		  "ctr", "cr0", "memory");
}

#undef GX_PAIRED_BLEND_BONES
#undef GX_PAIRED_TRANSFORM_POSITION

#endif

}
}
