#include <assert.h>

#include <gccore.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwanim.h"
#include "../rwplugins.h"

#include "rwgx.h"

namespace rw {
namespace gx {

// Every model the game streams in from a PC DFF used to be held twice once it
// was drawn: the platform-independent arrays streamRead filled, and the GX copy
// instanceGeometry made of them, plus display lists that encode every index
// once more.  After instancing only the GX copy is read, so for a geometry read
// from a stream the rest is freed here, the first time it is instanced:
//
//   - triangles, prelight colours and texture coordinates (one block, which
//     geometry->triangles owns): instanceGeometry copied colours and UVs, and
//     triangles are only for building meshes, which the stream already did.
//   - the mesh index lists (the tail of the meshHeader block): the display
//     lists carry them.  The header and its Mesh table stay -- the serial
//     number, material pointers and counts are small and still read.
//   - the bind pose and normals (the tail of the morphTargets block), unless the
//     geometry is skinned: gxskin.cpp blends from them every frame.  The
//     MorphTarget structs stay, because Atomic::setGeometry copies their
//     bounding sphere whenever an atomic is given the geometry (LOD swaps too).
//   - the instance's own index copy, once every mesh has a display list: it is
//     only the immediate-mode fallback for a mesh whose list failed to record,
//     and only ever read by the CPU.
//
// What reads geometry data after load was checked across librw and the game:
// the skin pipeline (kept, above), this file's re-instancing on a lock or a new
// mesh serial (stopped below: a released geometry keeps its instance until it is
// destroyed), buildMeshes (only when meshHeader is nil, which it never is here),
// and the water, which locks and rewrites its geometry every frame but builds
// it in code -- never from a stream -- so it is never released.  Everything else
// in the game touches flags, materials, counts and skin weights, which live
// elsewhere.  The freed pointers are nil, so a reader missed here faults on
// address zero, which the Wii leaves unmapped, rather than reading memory that
// has since been reused.
static void
releaseGeometryData(Geometry *geometry, InstanceDataHeader *header)
{
	rwFree(geometry->triangles);
	geometry->triangles = nil;
	geometry->colors = nil;
	for(int32 i = 0; i < 8; i++)
		geometry->texCoords[i] = nil;

	MeshHeader *meshHeader = geometry->meshHeader;
	if(meshHeader){
		// Shrinking keeps the block where it is; the index pointers into
		// the freed tail go either way.
		meshHeader = (MeshHeader*)rwResize(meshHeader,
			sizeof(MeshHeader) + meshHeader->numMeshes*sizeof(Mesh),
			MEMDUR_EVENT | ID_GEOMETRY);
		geometry->meshHeader = meshHeader;
		Mesh *meshes = meshHeader->getMeshes();
		for(uint32 i = 0; i < meshHeader->numMeshes; i++)
			meshes[i].indices = nil;
	}

	// The offset is zero only while the skin plugin is unregistered, and then
	// get() would read the geometry's own header.
	bool32 skinned = skinGlobals.geoOffset != 0 && Skin::get(geometry) != nil;
	if(!skinned){
		geometry->morphTargets = (MorphTarget*)rwResize(geometry->morphTargets,
			geometry->numMorphTargets*sizeof(MorphTarget),
			MEMDUR_EVENT | ID_GEOMETRY);
		for(int32 i = 0; i < geometry->numMorphTargets; i++){
			geometry->morphTargets[i].vertices = nil;
			geometry->morphTargets[i].normals = nil;
		}
	}

	bool32 allListed = 1;
	for(uint32 i = 0; i < header->numMeshes; i++)
		if(header->meshes[i].numIndices != 0 &&
		   header->meshes[i].displayList == nil)
			allListed = 0;
	if(allListed){
		rwFree(header->indices);
		header->indices = nil;
	}

	geometry->dataReleased = 1;
}

static void
instance(rw::ObjPipeline*, Atomic *atomic)
{
	Geometry *geometry = atomic->geometry;
	// GX-native DFFs were already instanced by the native-data plugin while
	// streaming. Their display lists are prepared once when that plugin reads.
	if(geometry->flags & Geometry::NATIVE)
		return;
	// The arrays an instance is built from are gone (releaseGeometryData), so
	// the instance it has is the only one it will ever have.
	if(geometry->dataReleased)
		return;
	if(geometry->meshHeader == nil)
		geometry->buildMeshes();

	InstanceDataHeader *header = (InstanceDataHeader*)geometry->instData;
	if(header){
		assert(header->platform == PLATFORM_GX);
		if(header->serialNumber != geometry->meshHeader->serialNum)
			freeInstanceData(geometry);
	}
	if(geometry->instData == nil || geometry->lockedSinceInst){
		freeInstanceData(geometry);
		instanceGeometry(geometry);
	}
	geometry->lockedSinceInst = 0;
	if(geometry->streamed && geometry->numMorphTargets == 1 &&
	   geometry->instData != nil)
		releaseGeometryData(geometry,
		                    (InstanceDataHeader*)geometry->instData);
}

static void
uninstance(rw::ObjPipeline*, Atomic *atomic)
{
	// Nothing is left to instance a released geometry from again.
	if(atomic->geometry->dataReleased)
		return;
	freeInstanceData(atomic->geometry);
}

static void
render(rw::ObjPipeline *pipeline, Atomic *atomic)
{
	pipeline->instance(atomic);
	Geometry *geometry = atomic->geometry;
	assert(geometry->instData != nil);
	assert(geometry->instData->platform == PLATFORM_GX);
	renderAtomic(atomic, (InstanceDataHeader*)geometry->instData);
}

void
ObjPipeline::init(void)
{
	this->rw::ObjPipeline::init(PLATFORM_GX);
	this->impl.instance = gx::instance;
	this->impl.uninstance = gx::uninstance;
	this->impl.render = gx::render;
}

ObjPipeline*
ObjPipeline::create(void)
{
	ObjPipeline *pipeline = rwNewT(ObjPipeline, 1, MEMDUR_GLOBAL);
	pipeline->init();
	return pipeline;
}

ObjPipeline*
makeDefaultPipeline(void)
{
	return ObjPipeline::create();
}

}
}
