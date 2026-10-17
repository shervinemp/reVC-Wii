#include <assert.h>
#include <string.h>
#include <math.h>

#include <gccore.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwanim.h"
#include "../rwplugins.h"

#include "rwgx.h"

namespace rw {
namespace gx {

#ifdef RW_GX

// GX skins with an indexed matrix palette the fixed function GL layer cannot
// reach, so the blend runs on the CPU straight into the instanced vertices.
// The bind pose stays in the geometry's morph target, which is what every
// frame reads from, so this costs no extra memory.

#define MAX_SKIN_BONES 64

static Matrix skinMatrices[MAX_SKIN_BONES];

static uint32
alignArraySize(uint32 size)
{
	return (size + 31) & ~uint32(31);
}

// Returns how many matrices were actually written.  A hierarchy with fewer
// nodes than the skin has bones would otherwise leave the tail holding another
// atomic's matrices, and a vertex weighted to one of those tears across the
// screen.
static int32
buildSkinMatrices(Atomic *atomic, Skin *skin)
{
	HAnimHierarchy *hierarchy = Skin::getHierarchy(atomic);
	int32 numBones = skin->numBones;
	int32 i;

	if(numBones > MAX_SKIN_BONES)
		numBones = MAX_SKIN_BONES;
	if(hierarchy == nil){
		for(i = 0; i < numBones; i++)
			skinMatrices[i].setIdentity();
		return numBones;
	}

	if(numBones > hierarchy->numNodes)
		numBones = hierarchy->numNodes;

	Matrix *inverseMatrices = (Matrix*)skin->inverseMatrices;
	if(hierarchy->flags & HAnimHierarchy::LOCALSPACEMATRICES){
		for(i = 0; i < numBones; i++){
			inverseMatrices[i].flags = 0;
			Matrix::mult(&skinMatrices[i], &inverseMatrices[i],
			             &hierarchy->matrices[i]);
		}
	}else{
		Matrix inverseAtomic;
		Matrix relative;
		Matrix::invert(&inverseAtomic, atomic->getFrame()->getLTM());
		for(i = 0; i < numBones; i++){
			inverseMatrices[i].flags = 0;
			Matrix::mult(&relative, &hierarchy->matrices[i], &inverseAtomic);
			Matrix::mult(&skinMatrices[i], &inverseMatrices[i], &relative);
		}
	}
	return numBones;
}

// 1/sqrt(x) for a normal's squared length: frsqrte and two Newton-Raphson
// steps in single precision, about 20 bits -- far beyond what an 8-bit lit
// colour can show, and a fraction of the sqrt and divide it replaces, which
// was most of the cost of a skinned vertex.  x must be positive and finite.
static inline float32
normalRecipLength(float32 x)
{
	float32 estimate;
	__asm__("frsqrte %0,%1" : "=f" (estimate) : "f" (x));
	float32 half = 0.5f*x;
	estimate = estimate*(1.5f - half*estimate*estimate);
	estimate = estimate*(1.5f - half*estimate*estimate);
	return estimate;
}

static void
blendVertices(InstanceDataHeader *header, Geometry *geometry, Skin *skin,
	int32 numBones)
{
	const V3d *bindPose = geometry->morphTargets[0].vertices;
	const V3d *bindNormals = geometry->morphTargets[0].normals;
	const float32 *weights = skin->weights;
	const uint8 *indices = skin->indices;
	bool32 blendNormals = header->hasNormals && bindNormals != nil;
	V3d *positions = header->positions;
	V3d *normals = header->normals;
	uint32 numVertices = header->numVertices;

	for(uint32 i = 0; i < numVertices; i++){
		const V3d source = bindPose[i];
		V3d normal = { 0.0f, 0.0f, 0.0f };
		if(blendNormals)
			normal = bindNormals[i];
		V3d blended = { 0.0f, 0.0f, 0.0f };
		V3d blendedNormal = { 0.0f, 0.0f, 0.0f };
		float32 blendedWeight = 0.0f;
		const float32 *vertexWeights = &weights[i*4];
		const uint8 *vertexBones = &indices[i*4];
		for(int32 j = 0; j < 4; j++){
			float32 weight = vertexWeights[j];
			if(weight == 0.0f)
				continue;
			int32 bone = vertexBones[j];
			if(bone >= numBones)
				continue;
			const Matrix &matrix = skinMatrices[bone];
			blended.x += weight*(matrix.right.x*source.x +
			                     matrix.up.x*source.y +
			                     matrix.at.x*source.z + matrix.pos.x);
			blended.y += weight*(matrix.right.y*source.x +
			                     matrix.up.y*source.y +
			                     matrix.at.y*source.z + matrix.pos.y);
			blended.z += weight*(matrix.right.z*source.x +
			                     matrix.up.z*source.y +
			                     matrix.at.z*source.z + matrix.pos.z);

			if(blendNormals){
				blendedNormal.x += weight*(matrix.right.x*normal.x +
				                         matrix.up.x*normal.y +
				                         matrix.at.x*normal.z);
				blendedNormal.y += weight*(matrix.right.y*normal.x +
				                         matrix.up.y*normal.y +
				                         matrix.at.y*normal.z);
				blendedNormal.z += weight*(matrix.right.z*normal.x +
				                         matrix.up.z*normal.y +
				                         matrix.at.z*normal.z);
			}
			blendedWeight += weight;
		}
		// An unweighted vertex keeps its bind pose rather than collapsing onto
		// the atomic's origin.
		positions[i] = blendedWeight > 0.0f ? blended : source;

		if(blendNormals){
			if(blendedWeight > 0.0f){
				float32 lengthSquared = blendedNormal.x*blendedNormal.x +
				                        blendedNormal.y*blendedNormal.y +
				                        blendedNormal.z*blendedNormal.z;
				if(lengthSquared > 1.0e-12f && lengthSquared < 1.0e12f){
					float32 inverseLength = normalRecipLength(lengthSquared);
					normal.x = blendedNormal.x*inverseLength;
					normal.y = blendedNormal.y*inverseLength;
					normal.z = blendedNormal.z*inverseLength;
				}
			}
			normals[i] = normal;
		}
	}
	DCStoreRange(positions, alignArraySize(numVertices*sizeof(V3d)));
	if(blendNormals)
		DCStoreRange(normals, alignArraySize(numVertices*sizeof(V3d)));
	header->vertexCacheDirty = 1;
}

// Whether skinAtomic will write the vertices at all.
static bool32
hasUsableSkin(Geometry *geometry)
{
	Skin *skin = Skin::get(geometry);
	return skin != nil && skin->numBones > 0 &&
	       geometry->morphTargets[0].vertices != nil &&
	       skin->weights != nil && skin->indices != nil;
}

void
skinAtomic(Atomic *atomic, InstanceDataHeader *header)
{
	Geometry *geometry = atomic->geometry;
	if(!hasUsableSkin(geometry))
		return;
	Skin *skin = Skin::get(geometry);
	blendVertices(header, geometry, skin, buildSkinMatrices(atomic, skin));
}

// Vertices for a second clone of a geometry skinned in the same frame.
//
// Every clone of a geometry shares its instance data -- a clump clone shares
// the geometry (librw clump.cpp) -- and the blend writes into it, while GX
// reads a vertex array when it executes a draw, not when the draw is queued.
// Two peds of one model in a frame are common, and the second blend would land
// in vertices the first one's queued draw has yet to read: one mesh drawn half
// in each pose.  So a geometry's own arrays take the first clone each frame,
// and every further clone blends into this ring and draws from it.  The ring
// starts over whenever the GPU is known to be idle, which the start of every
// frame's camera update guarantees.
enum { SKIN_RING_SIZE = 512*1024 };

struct SkinRing
{
	uint8 *allocation;
	uint8 *base;
	uint32 used;
	uint32 epoch;
	bool32 failed;
};

static SkinRing skinRing;

static uint8*
allocateSkinVertices(uint32 size)
{
	size = alignArraySize(size);
	if(size > SKIN_RING_SIZE)
		return nil;
	if(skinRing.base == nil){
		if(skinRing.failed)
			return nil;
		skinRing.allocation = rwNewT(uint8, SKIN_RING_SIZE + 31,
		                             MEMDUR_GLOBAL | ID_GEOMETRY);
		if(skinRing.allocation == nil){
			// Then every repeat waits for the GPU instead, as it must.
			skinRing.failed = 1;
			return nil;
		}
		skinRing.base = (uint8*)(((uintptr)skinRing.allocation + 31) &
		                         ~uintptr(31));
		skinRing.used = 0;
		skinRing.epoch = nativeGpuIdleEpoch();
	}
	if(skinRing.epoch != nativeGpuIdleEpoch()){
		skinRing.epoch = nativeGpuIdleEpoch();
		skinRing.used = 0;
	}
	if(skinRing.used + size > SKIN_RING_SIZE){
		// More repeats in one frame than the ring holds: let the GPU catch up
		// with everything drawn from it, then reuse it from the start.
		waitForGpu();
		skinRing.epoch = nativeGpuIdleEpoch();
		skinRing.used = 0;
	}
	uint8 *vertices = skinRing.base + skinRing.used;
	skinRing.used += size;
	return vertices;
}

static void
render(rw::ObjPipeline *pipeline, Atomic *atomic)
{
	pipeline->instance(atomic);
	Geometry *geometry = atomic->geometry;
	assert(geometry->instData != nil);
	assert(geometry->instData->platform == PLATFORM_GX);
	InstanceDataHeader *header = (InstanceDataHeader*)geometry->instData;

	V3d *ownPositions = header->positions;
	V3d *ownNormals = header->normals;
	bool32 borrowed = 0;
	uint32 frame = nativeFrameSerial();
	if(!hasUsableSkin(geometry)){
		// Nothing is blended, so nothing is rewritten: draw what is there.
	}else if(header->lastSkinFrame == frame){
		bool32 blendsNormals = header->hasNormals &&
		                       geometry->morphTargets[0].normals != nil;
		uint32 arraySize = alignArraySize(header->numVertices*sizeof(V3d));
		uint8 *vertices = allocateSkinVertices(blendsNormals ?
		                                       2*arraySize : arraySize);
		if(vertices != nil){
			header->positions = (V3d*)vertices;
			if(blendsNormals)
				header->normals = (V3d*)(vertices + arraySize);
			borrowed = 1;
		}else
			waitForGpu();
	}else
		header->lastSkinFrame = frame;

	skinAtomic(atomic, header);
	renderAtomic(atomic, header);

	if(borrowed){
		header->positions = ownPositions;
		header->normals = ownNormals;
		// The vertex cache may hold the ring's copy under the ring's address,
		// and the next draw from the geometry's own arrays must not see it.
		header->vertexCacheDirty = 1;
	}
}

ObjPipeline*
makeSkinPipeline(void)
{
	// Instancing is the default one: skinning overwrites the positions it
	// produced, every frame, from the bind pose.
	ObjPipeline *pipeline = ObjPipeline::create();
	pipeline->impl.render = gx::render;
	pipeline->pluginID = ID_SKIN;
	pipeline->pluginData = 1;
	return pipeline;
}

static void*
skinOpen(void *o, int32, int32)
{
	skinGlobals.pipelines[PLATFORM_GX] = makeSkinPipeline();
	return o;
}

static void*
skinClose(void *o, int32, int32)
{
	((ObjPipeline*)skinGlobals.pipelines[PLATFORM_GX])->destroy();
	skinGlobals.pipelines[PLATFORM_GX] = nil;
	return o;
}

void
initSkin(void)
{
	Driver::registerPlugin(PLATFORM_GX, 0, ID_SKIN, skinOpen, skinClose);
}

#else

void initSkin(void) { }

#endif

}
}
