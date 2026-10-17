#include <math.h>
#include <string.h>

#include <ogc/gx.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwrender.h"

#include "rwgx.h"
#include "gxbatch.h"

namespace rw {
namespace gx {

uint32 renderedAtomicCount;
uint32 renderedGsAlphaPassCount;

// What the GX pipeline was last programmed with, so a mesh that needs the same
// setup as the one before it -- most of the world, which is prelit, textured
// and white -- programs nothing but its texture.  Every GX call here costs a
// function call and a register write into the FIFO, and the full setup is a
// dozen and more of them per mesh, thousands of meshes a frame.
//
// Everything in this file that touches the vertex format, the colour channels,
// the TEV, texture map 0 or the lights goes through this cache, and the GX
// state no other file sets.  gxraster.cpp drops the texture entry when it
// changes a texture object in place.
struct NativePipelineCache
{
	// Vertex descriptor: VERTEX_LAYOUT_* below.
	uint32 vertexLayout;

	// Colour channels: 1 for unlit, 2 for lit, 0 when unknown.
	uint32 numChannels;
	bool32 lightMaskValid;
	uint8 lightMask;
	bool32 ambientValid;
	GXColor ambient;

	// TEV programme: TEV_* bits, ~0 when unknown.
	uint32 tevKey;
	bool32 konstValid;
	GXColor konst;
	const GXTexObj *texObj;

	// What each hardware light slot holds.
	bool32 lightValid[8];
	GXLightObj lights[8];
};

static NativePipelineCache pipelineCache;

enum
{
	VERTEX_LAYOUT_UNKNOWN = 0,
	VERTEX_LAYOUT_INDEXED = 1 << 0,
	VERTEX_LAYOUT_NORMALS = 1 << 1,
	VERTEX_LAYOUT_TEXCOORDS = 1 << 2,
	// Distinguishes the direct (immediate mode) layout without texture
	// coordinates from "unknown".
	VERTEX_LAYOUT_SET = 1 << 3
};

enum
{
	TEV_TEXTURED = 1 << 0,
	TEV_MATERIAL = 1 << 1,
	TEV_LIGHTING = 1 << 2,
	TEV_UNKNOWN = ~0u
};

static bool32
isOpaqueWhite(const RGBA &color)
{
	return color.red == 255 && color.green == 255 && color.blue == 255 &&
	       color.alpha == 255;
}

static uint8
colorToByte(float32 value)
{
	if(value <= 0.0f)
		return 0;
	if(value >= 1.0f)
		return 255;
	return (uint8)(value*255.0f + 0.5f);
}

static bool32
sameColor(const GXColor &a, const GXColor &b)
{
	return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

void
invalidateNativeTextureStage(void)
{
	pipelineCache.texObj = nil;
}

static void
resetPipelineCache(void)
{
	memset(&pipelineCache, 0, sizeof(pipelineCache));
	pipelineCache.vertexLayout = VERTEX_LAYOUT_UNKNOWN;
	pipelineCache.tevKey = TEV_UNKNOWN;
}

// One attribute format per attribute, for every path: indexed and direct
// streams differ only in the descriptor, so the format table never changes
// after this and the descriptor is all a layout switch has to send.
static void
setupVertexFormats(void)
{
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
}

void
setNativeVertexLayout(bool32 indexed, bool32 normals, bool32 texCoords)
{
	uint32 layout = VERTEX_LAYOUT_SET;
	if(indexed)
		layout |= VERTEX_LAYOUT_INDEXED;
	if(normals)
		layout |= VERTEX_LAYOUT_NORMALS;
	if(texCoords)
		layout |= VERTEX_LAYOUT_TEXCOORDS;
	if(pipelineCache.vertexLayout == layout)
		return;
	pipelineCache.vertexLayout = layout;

	uint8 type = indexed ? GX_INDEX16 : GX_DIRECT;
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, type);
	if(normals)
		GX_SetVtxDesc(GX_VA_NRM, type);
	GX_SetVtxDesc(GX_VA_CLR0, type);
	if(texCoords)
		GX_SetVtxDesc(GX_VA_TEX0, type);
}

static void
setupNativeVertexArrays(InstanceDataHeader *header)
{
	setNativeVertexLayout(1, header->hasNormals, header->hasTexCoords);
	GX_SetArray(GX_VA_POS, header->positions, sizeof(V3d));
	GX_SetArray(GX_VA_CLR0, header->colors, sizeof(RGBA));
	if(header->hasNormals)
		GX_SetArray(GX_VA_NRM, header->normals, sizeof(V3d));
	if(header->hasTexCoords)
		GX_SetArray(GX_VA_TEX0, header->texCoords, sizeof(TexCoords));
	if(header->vertexCacheDirty){
		GX_InvVtxCache();
		header->vertexCacheDirty = 0;
	}
}

void
initNativeTevState(void)
{
	// These are backend-wide invariants. No GX path in librw enables indirect
	// TEV, Z-texturing or a non-default swap table, so programming them once
	// avoids repeating the same BP writes for every mesh.
	GX_SetNumIndStages(0);
	GX_SetZTexture(GX_ZT_DISABLE, GX_TF_Z24X8, 0);
	GX_SetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN,
	                       GX_CH_BLUE, GX_CH_ALPHA);

	// The native atomic/immediate pipelines currently use at most four TEV
	// stages. Keep all of them direct with the default swap mapping up front.
	for(uint8 stage = GX_TEVSTAGE0; stage <= GX_TEVSTAGE3; stage++){
		GX_SetTevDirect(stage);
		GX_SetTevSwapMode(stage, GX_TEV_SWAP0, GX_TEV_SWAP0);
	}

	// Channel 0 is the same in every path: vertex colour straight through.
	// Channel 1's material colour is the white the lighting multiplies.
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
	               GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GXColor white = { 255, 255, 255, 255 };
	GX_SetChanMatColor(GX_COLOR1A1, white);
	// Lighting only changes RGB in librw. Keep the second channel's alpha out
	// of the equation entirely; TEV preserves channel 0 alpha in the add stage.
	GX_SetChanCtrl(GX_ALPHA1, GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
	               GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetCurrentMtx(GX_PNMTX0);
	setupVertexFormats();
	resetPipelineCache();
}

static void
setupTevCopyRaster(uint8 stage, uint8 channel)
{
	GX_SetTevOrder(stage, GX_TEXCOORDNULL, GX_TEXMAP_DISABLE, channel);
	GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
	GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO,
	                 GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO,
	                 GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

static void
setupTevRasterTextureModulate(uint8 stage)
{
	// TEX * RASC in one stage. This replaces the old stage0 RASC copy followed
	// by a texture stage in the dominant unlit/prelit world path.
	GX_SetTevOrder(stage, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_TEXC,
	                 GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_TEXA,
	                 GX_CA_RASA, GX_CA_ZERO);
	GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO,
	                 GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO,
	                 GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

static void
setupTevPrevTextureModulate(uint8 stage)
{
	GX_SetTevOrder(stage, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_TEXC,
	                 GX_CC_CPREV, GX_CC_ZERO);
	GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_TEXA,
	                 GX_CA_APREV, GX_CA_ZERO);
	GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO,
	                 GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO,
	                 GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

// The material colour itself goes into KCOLOR0 separately, so that a change of
// colour alone does not reprogram the stage.
static void
setupTevMaterialModulate(uint8 stage, bool32 hasPrevious)
{
	GX_SetTevKColorSel(stage, GX_TEV_KCSEL_K0);
	GX_SetTevKAlphaSel(stage, GX_TEV_KASEL_K0_A);
	GX_SetTevOrder(stage, GX_TEXCOORDNULL, GX_TEXMAP_DISABLE,
	               hasPrevious ? GX_COLORNULL : GX_COLOR0A0);
	GX_SetTevColorIn(stage, GX_CC_ZERO,
	                 hasPrevious ? GX_CC_CPREV : GX_CC_RASC,
	                 GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevAlphaIn(stage, GX_CA_ZERO,
	                 hasPrevious ? GX_CA_APREV : GX_CA_RASA,
	                 GX_CA_KONST, GX_CA_ZERO);
	GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO,
	                 GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO,
	                 GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

// Programs the TEV stages for one combination of texture, material colour and
// lighting.  Only called when that combination changes.
static void
programTev(uint32 key)
{
	bool32 textured = (key & TEV_TEXTURED) != 0;
	bool32 materialModulate = (key & TEV_MATERIAL) != 0;
	uint8 stage = GX_TEVSTAGE0;

	if(textured){
		GX_SetNumTexGens(1);
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4,
		                  GX_TG_TEX0, GX_IDENTITY);
	}else
		GX_SetNumTexGens(0);

	if((key & TEV_LIGHTING) == 0){
		// Prelight is already clamped vertex data. Therefore, for the unlit
		// path, texture*RASC is algebraically identical to copying RASC into
		// CPREV first and multiplying it on the following stage.
		if(textured){
			setupTevRasterTextureModulate(stage++);
			if(materialModulate)
				setupTevMaterialModulate(stage++, 1);
		}else if(materialModulate)
			setupTevMaterialModulate(stage++, 0);
		else
			setupTevCopyRaster(stage++, GX_COLOR0A0);

		GX_SetNumTevStages(stage);
		return;
	}

	// Lit geometry must preserve the PS2/RW clamp point exactly:
	// clamp(prelight + lighting) happens before material/texture modulation.
	// Since a TEV stage can select only one raster colour channel at a time,
	// keep the prelight copy and lighting add as two stages here.
	setupTevCopyRaster(stage++, GX_COLOR0A0);
	GX_SetTevOrder(stage, GX_TEXCOORDNULL, GX_TEXMAP_DISABLE, GX_COLOR1A1);
	GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_RASC,
	                 GX_CC_ONE, GX_CC_CPREV);
	GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO,
	                 GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO,
	                 GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO,
	                 GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	stage++;

	if(textured)
		setupTevPrevTextureModulate(stage++);
	if(materialModulate)
		setupTevMaterialModulate(stage++, 1);

	GX_SetNumTevStages(stage);
}

// Build the RenderWare colour equation in TEV stages:
//   clamp(prelight + ambient + diffuse) * material * texture
// Alpha deliberately does not receive lighting; it is prelight alpha multiplied
// by material/texture alpha, just like the programmable librw backends.
static void
setupNativeTevPipeline(const RGBA &materialColor, GxRaster *nativeRaster,
	bool32 addLighting)
{
	bool32 textured = nativeRaster != nil && nativeRaster->nativeReady;
	bool32 materialModulate = !isOpaqueWhite(materialColor);
	uint32 key = 0;
	if(textured)
		key |= TEV_TEXTURED;
	if(materialModulate)
		key |= TEV_MATERIAL;
	if(addLighting)
		key |= TEV_LIGHTING;

	if(pipelineCache.tevKey != key){
		programTev(key);
		pipelineCache.tevKey = key;
	}
	if(materialModulate){
		GXColor konst = {
			materialColor.red, materialColor.green,
			materialColor.blue, materialColor.alpha
		};
		if(!pipelineCache.konstValid ||
		   !sameColor(pipelineCache.konst, konst)){
			GX_SetTevKColor(GX_KCOLOR0, konst);
			pipelineCache.konst = konst;
			pipelineCache.konstValid = 1;
		}
	}
	if(textured && pipelineCache.texObj != &nativeRaster->nativeTexture){
		GX_LoadTexObj(&nativeRaster->nativeTexture, GX_TEXMAP0);
		pipelineCache.texObj = &nativeRaster->nativeTexture;
	}
}

static void
setupUnlitChannels(void)
{
	if(pipelineCache.numChannels == 1)
		return;
	GX_SetNumChans(1);
	pipelineCache.numChannels = 1;
}

// Immediate mode is unlit unless its caller explicitly configures a lit path.
void
setupNativeTev(const RGBA &materialColor, GxRaster *nativeRaster)
{
	setupUnlitChannels();
	setupNativeTevPipeline(materialColor, nativeRaster, 0);
}

// An atomic's directional lights in view space, worked out once per atomic
// rather than once per mesh: they depend on the camera and the lights, not on
// anything a mesh carries.
struct NativeLights
{
	int32 count;
	V3d direction[8];
	RGBAf color[8];
	RGBAf ambient;
	// The diffuse factor the light slots were last loaded with for this atomic.
	// Its meshes nearly always share one, and then the second mesh on need not
	// even build the light objects to compare them.
	bool32 loaded;
	float32 loadedDiffuse;
	uint8 loadedMask;
};

static void
getAtomicLights(Atomic *atomic, uint32 geometryFlags, bool32 hasNormals,
	NativeLights *lights)
{
	lights->count = 0;
	lights->loaded = 0;
	lights->ambient.red = 0.0f;
	lights->ambient.green = 0.0f;
	lights->ambient.blue = 0.0f;
	lights->ambient.alpha = 0.0f;
	if((geometryFlags & Geometry::LIGHT) == 0 || engine->currentWorld == nil)
		return;

	Light *directionals[8];
	Light *locals[8];
	WorldLights lightData;
	memset(&lightData, 0, sizeof(lightData));
	lightData.directionals = directionals;
	lightData.locals = locals;
	// enumerateLights treats these fields as capacities on input.
	lightData.numDirectionals = 8;
	lightData.numLocals = 8;
	((World*)engine->currentWorld)->enumerateLights(atomic, &lightData);
	lights->ambient = lightData.ambient;
	if(!hasNormals)
		return;

	int32 count = lightData.numDirectionals;
	if(count > 8)
		count = 8;
	for(int32 i = 0; i < count; i++){
		Light *light = lightData.directionals[i];
		if(light == nil || light->getFrame() == nil)
			continue;

		V3d viewDirection;
		transformNativeWorldVectorToView(light->getFrame()->getLTM()->at,
		                                 &viewDirection);
		float32 len2 = viewDirection.x*viewDirection.x +
		               viewDirection.y*viewDirection.y +
		               viewDirection.z*viewDirection.z;
		if(len2 <= 1.0e-12f)
			continue;
		float32 invLen = 1.0f/sqrtf(len2);
		V3d *direction = &lights->direction[lights->count];
		direction->x = viewDirection.x*invLen;
		direction->y = viewDirection.y*invLen;
		direction->z = viewDirection.z*invLen;
		lights->color[lights->count] = light->color;
		lights->count++;
	}
}

static const uint8 nativeLightIds[8] = {
	GX_LIGHT0, GX_LIGHT1, GX_LIGHT2, GX_LIGHT3,
	GX_LIGHT4, GX_LIGHT5, GX_LIGHT6, GX_LIGHT7
};

static uint8
loadDirectionalLights(NativeLights *lights,
	const SurfaceProperties &surfaceProps)
{
	if(surfaceProps.diffuse <= 0.0f)
		return GX_LIGHTNULL;
	if(lights->loaded && lights->loadedDiffuse == surfaceProps.diffuse)
		return lights->loadedMask;

	uint8 lightMask = GX_LIGHTNULL;
	for(int32 i = 0; i < lights->count; i++){
		GXLightObj lightObject;
		memset(&lightObject, 0, sizeof(lightObject));
		GX_InitLightAttn(&lightObject, 1.0f, 0.0f, 0.0f,
		                 1.0f, 0.0f, 0.0f);
		GXColor color = {
			colorToByte(lights->color[i].red*surfaceProps.diffuse),
			colorToByte(lights->color[i].green*surfaceProps.diffuse),
			colorToByte(lights->color[i].blue*surfaceProps.diffuse),
			255
		};
		GX_InitLightColor(&lightObject, color);

		// GX diffuse lighting is local/positional.  A very remote point in the
		// opposite direction reproduces RenderWare's parallel directional light.
		const float32 directionalDistance = 10000000.0f;
		const V3d &direction = lights->direction[i];
		GX_InitLightPos(&lightObject,
		                -direction.x*directionalDistance,
		                -direction.y*directionalDistance,
		                -direction.z*directionalDistance);
		// A light object is sixteen words through the FIFO.  Peds and cars
		// light every mesh with the same two or three, so load only a change.
		if(!pipelineCache.lightValid[i] ||
		   memcmp(&pipelineCache.lights[i], &lightObject,
		          sizeof(lightObject)) != 0){
			GX_LoadLightObj(&lightObject, nativeLightIds[i]);
			pipelineCache.lights[i] = lightObject;
			pipelineCache.lightValid[i] = 1;
		}
		lightMask |= nativeLightIds[i];
	}
	lights->loaded = 1;
	lights->loadedDiffuse = surfaceProps.diffuse;
	lights->loadedMask = lightMask;
	return lightMask;
}

static void
setupNativeLighting(NativeLights *lights,
	const SurfaceProperties &surfaceProps)
{
	// Channel 0 carries PRELIT unchanged.  Channel 1 is reserved for the
	// RenderWare ambient + diffuse contribution and is added by TEV.
	GXColor ambient = {
		colorToByte(lights->ambient.red*surfaceProps.ambient),
		colorToByte(lights->ambient.green*surfaceProps.ambient),
		colorToByte(lights->ambient.blue*surfaceProps.ambient),
		255
	};
	uint8 lightMask = loadDirectionalLights(lights, surfaceProps);

	if(pipelineCache.numChannels != 2){
		GX_SetNumChans(2);
		pipelineCache.numChannels = 2;
	}
	if(!pipelineCache.ambientValid ||
	   !sameColor(pipelineCache.ambient, ambient)){
		GX_SetChanAmbColor(GX_COLOR1A1, ambient);
		pipelineCache.ambient = ambient;
		pipelineCache.ambientValid = 1;
	}
	if(!pipelineCache.lightMaskValid || pipelineCache.lightMask != lightMask){
		GX_SetChanCtrl(GX_COLOR1, GX_ENABLE, GX_SRC_REG, GX_SRC_REG,
		               lightMask,
		               lightMask ? GX_DF_CLAMP : GX_DF_NONE,
		               GX_AF_NONE);
		pipelineCache.lightMask = lightMask;
		pipelineCache.lightMaskValid = 1;
	}
}

static void
drawNativeMeshSimple(InstanceDataHeader *header, const InstanceData *mesh)
{
	prepareNativeDraw();
	if(mesh->displayList != nil && mesh->displayListSize != 0){
		GX_CallDispList(mesh->displayList, mesh->displayListSize);
		nativeDisplayListCallCount++;
		return;
	}

	// Allocation/recording failure is non-fatal: preserve the old path so a
	// bad or unusually large mesh can never make the whole model disappear.
	nativeDisplayListFallbackCount++;
	PrimitiveType primitive = header->triangleStrip ?
	                              PRIMTYPETRISTRIP : PRIMTYPETRILIST;
	drawNativeBatches(primitive, header->indices + mesh->indexOffset,
	                  mesh->numIndices, [header](uint32 index) {
		GX_Position1x16(index);
		if(header->hasNormals)
			GX_Normal1x16(index);
		GX_Color1x16(index);
		if(header->hasTexCoords)
			GX_TexCoord1x16(index);
	});
}

// Emulate the PS2 GS alpha-test FB_ONLY case. Pixels below the GS reference
// still blend into the frame buffer, but must not update Z.
static void
drawNativeMeshGsAlpha(InstanceDataHeader *header, const InstanceData *mesh,
	bool32 hasAlpha)
{
	if(!hasAlpha){
		drawNativeMeshSimple(header, mesh);
		return;
	}

	uint32 zTest = GetRenderState(ZTESTENABLE);
	uint32 zWrite = GetRenderState(ZWRITEENABLE);
	uint32 alphaFunction = GetRenderState(ALPHATESTFUNC);
	uint32 alphaReference = GetRenderState(ALPHATESTREF);
	if(zWrite){
		uint32 gsAlphaReference = GetRenderState(GSALPHATESTREF);

		setNativeAlphaTest(ALPHAGREATEREQUAL, gsAlphaReference);
		drawNativeMeshSimple(header, mesh);

		setNativeAlphaTest(ALPHALESS, gsAlphaReference);
		setNativeDepthState(zTest, 0);
		drawNativeMeshSimple(header, mesh);
		renderedGsAlphaPassCount++;

		setNativeDepthState(zTest, zWrite);
		setNativeAlphaTest(alphaFunction, alphaReference);
	}else{
		setNativeAlphaTest(ALPHAALWAYS, alphaReference);
		drawNativeMeshSimple(header, mesh);
		setNativeAlphaTest(alphaFunction, alphaReference);
	}
}

static void
drawNativeMesh(InstanceDataHeader *header, const InstanceData *mesh,
	bool32 hasAlpha)
{
	if(GetRenderState(GSALPHATEST))
		drawNativeMeshGsAlpha(header, mesh, hasAlpha);
	else
		drawNativeMeshSimple(header, mesh);
}

static void
renderAtomicNative(Atomic *atomic, InstanceDataHeader *header,
	uint32 geometryFlags, NativeLights *lights)
{
	static const RGBA white = { 255, 255, 255, 255 };
	bool32 lightingEnabled = (geometryFlags & Geometry::LIGHT) != 0;

	setupNativeVertexArrays(header);
	for(uint32 i = 0; i < header->numMeshes; i++){
		InstanceData *mesh = &header->meshes[i];
		if(mesh->numIndices == 0)
			continue;
		RGBA sourceMaterialColor = mesh->material->color;
		RGBA materialColor = geometryFlags & Geometry::MODULATE ?
		                         sourceMaterialColor : white;
		setTexture(header->hasTexCoords ? mesh->material->texture : nil);
		setVertexAlpha(mesh->vertexAlpha ||
		               sourceMaterialColor.alpha != 255);
		Raster *raster = getRasterStage();
		GxRaster *nativeRaster = raster ? GETGXRASTEREXT(raster) : nil;

		if(lightingEnabled)
			setupNativeLighting(lights, mesh->material->surfaceProps);
		else
			setupUnlitChannels();
		setupNativeTevPipeline(materialColor, nativeRaster, lightingEnabled);

		bool32 hasAlpha = mesh->vertexAlpha ||
		                  sourceMaterialColor.alpha != 255 ||
		                  (nativeRaster && nativeRaster->hasAlpha);
		drawNativeMesh(header, mesh, hasAlpha);
	}
	(void)atomic;
}

void
renderAtomic(Atomic *atomic, InstanceDataHeader *header)
{
	if(header->numVertices == 0 || header->numIndices == 0)
		return;
	renderedAtomicCount++;

	uint32 geometryFlags = atomic->geometry->flags;
	NativeLights lights;
	getAtomicLights(atomic, geometryFlags, header->hasNormals, &lights);
	// The normal matrix is an inverse-transpose, and only directional lights
	// read it: ambient light and prelit colour take no normal.  The world is
	// lit by ambient alone, so most atomics skip it.
	loadNativeWorldMatrix(atomic->getFrame()->getLTM(), lights.count > 0);
	renderAtomicNative(atomic, header, geometryFlags, &lights);
}

}
}
