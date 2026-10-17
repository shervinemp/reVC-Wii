#include <assert.h>
#include <string.h>
#include <math.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>

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
#include "gxskinps.h"

namespace rw {
namespace gx {

#ifdef RW_GX

// GX skins with an indexed matrix palette the fixed function GL layer cannot
// reach, so the blend runs on the CPU straight into the instanced vertices.
// The bind pose stays in the geometry's morph target, which is what every
// frame reads from, so this costs no extra memory.

#define MAX_SKIN_BONES 64

static Matrix skinMatrices[MAX_SKIN_BONES];

uint64 nativeSkinTicks;
uint32 nativeSkinnedVertices;
uint32 nativeSkinRepeats;
uint32 nativeSkinStalls;

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

static PairedBone pairedBones[MAX_SKIN_BONES] __attribute__((aligned(32)));

// The kernels in gxskinps.h do the arithmetic, a vertex at a time in paired
// singles; this picks each vertex's bones.
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

	for(int32 i = 0; i < numBones; i++)
		makePairedBone(&pairedBones[i], &skinMatrices[i]);

	for(uint32 i = 0; i < numVertices; i++){
		PairedSkinVertex vertex;
		uint32 count = 0;
		const float32 *vertexWeights = &weights[i*4];
		const uint8 *vertexBones = &indices[i*4];
		for(int32 j = 0; j < 4; j++){
			float32 weight = vertexWeights[j];
			if(weight == 0.0f)
				continue;
			int32 bone = vertexBones[j];
			if(bone >= numBones)
				continue;
			vertex.bone[count] = &pairedBones[bone];
			vertex.weight[count] = weight;
			count++;
		}
		// An unweighted vertex keeps its bind pose rather than collapsing onto
		// the atomic's origin.
		if(count == 0){
			positions[i] = bindPose[i];
			if(blendNormals)
				normals[i] = bindNormals[i];
		}else if(blendNormals)
			skinVertexPaired(&vertex, count, &bindPose[i], &positions[i],
			                 &bindNormals[i], &normals[i]);
		else
			skinPositionPaired(&vertex, count, &bindPose[i], &positions[i]);
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
		nativeSkinStalls++;
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
		}else{
			waitForGpu();
			nativeSkinStalls++;
		}
		nativeSkinRepeats++;
	}else
		header->lastSkinFrame = frame;

	uint64 start = gettime();
	skinAtomic(atomic, header);
	nativeSkinTicks += gettime() - start;
	nativeSkinnedVertices += header->numVertices;
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
