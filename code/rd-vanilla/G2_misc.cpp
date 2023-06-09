/*
===========================================================================
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

#include "../server/exe_headers.h"

#ifndef __Q_SHARED_H
#include "../qcommon/q_shared.h"
#endif

#include "tr_common.h"

#if !defined(TR_LOCAL_H)
#include "tr_local.h"
#endif

#include "qcommon/matcomp.h"

#if !defined(G2_H_INC)
#include "../ghoul2/G2.h"
#endif

#if !defined (MINIHEAP_H_INC)
#include "../qcommon/MiniHeap.h"
#endif

#define G2_MODEL_OK(g) ((g)&&(g)->mValid&&(g)->aHeader&&(g)->currentModel&&(g)->animModel)

#include "../server/server.h"

#include <cfloat>

#include "qcommon/ojk_saved_game_helper.h"

#ifdef _G2_GORE
#include "../ghoul2/ghoul2_gore.h"

#define GORE_TAG_UPPER (256)
#define GORE_TAG_MASK (~255)

static int CurrentTag = GORE_TAG_UPPER + 1;
static int CurrentTagUpper = GORE_TAG_UPPER;

static std::map<int, GoreTextureCoordinates> GoreRecords;
static std::map<std::pair<int, int>, int> GoreTagsTemp; // this is a surface index to gore tag map used only
// temporarily during the generation phase so we reuse gore tags per LOD
int goreModelIndex;

static cvar_t* cg_g2MarksAllModels = nullptr;

GoreTextureCoordinates* FindGoreRecord(int tag);
static void DestroyGoreTexCoordinates(const int tag)
{
	GoreTextureCoordinates* gTC = FindGoreRecord(tag);
	if (!gTC)
	{
		return;
	}
	(*gTC).~GoreTextureCoordinates();
	//I don't know what's going on here, it should call the destructor for
	//this when it erases the record but sometimes it doesn't. -rww
}

//TODO: This needs to be set via a scalability cvar with some reasonable minimum value if pgore is used at all
#define MAX_GORE_RECORDS (500)

int AllocGoreRecord()
{
	while (GoreRecords.size() > MAX_GORE_RECORDS)
	{
		const int tag_high = (*GoreRecords.begin()).first & GORE_TAG_MASK;
		std::map<int, GoreTextureCoordinates>::iterator it;

		it = GoreRecords.begin();
		GoreTextureCoordinates* gTC = &(*it).second;

		if (gTC)
		{
			gTC->~GoreTextureCoordinates();
		}
		GoreRecords.erase(GoreRecords.begin());
		while (GoreRecords.size())
		{
			if (((*GoreRecords.begin()).first & GORE_TAG_MASK) != tag_high)
			{
				break;
			}
			it = GoreRecords.begin();
			gTC = &(*it).second;

			if (gTC)
			{
				gTC->~GoreTextureCoordinates();
			}
			GoreRecords.erase(GoreRecords.begin());
		}
	}
	const int ret = CurrentTag;
	GoreRecords[CurrentTag] = GoreTextureCoordinates();
	CurrentTag++;
	return ret;
}

void ResetGoreTag()
{
	GoreTagsTemp.clear();
	CurrentTag = CurrentTagUpper;
	CurrentTagUpper += GORE_TAG_UPPER;
}

GoreTextureCoordinates* FindGoreRecord(const int tag)
{
	const auto i = GoreRecords.find(tag);
	if (i != GoreRecords.end())
	{
		return &(*i).second;
	}
	return nullptr;
}

void* G2_GetGoreRecord(const int tag)
{
	return FindGoreRecord(tag);
}

void DeleteGoreRecord(const int tag)
{
	DestroyGoreTexCoordinates(tag);
	GoreRecords.erase(tag);
}

static int CurrentGoreSet = 1; // this is a UUID for gore sets
static std::map<int, CGoreSet*> GoreSets; // map from uuid to goreset

CGoreSet* FindGoreSet(const int goreSetTag)
{
	const auto f = GoreSets.find(goreSetTag);
	if (f != GoreSets.end())
	{
		return (*f).second;
	}
	return nullptr;
}

CGoreSet* NewGoreSet()
{
	const auto ret = new CGoreSet(CurrentGoreSet++);
	GoreSets[ret->mMyGoreSetTag] = ret;
	ret->mRefCount = 1;
	return ret;
}

void DeleteGoreSet(const int goreSetTag)
{
	const auto f = GoreSets.find(goreSetTag);
	if (f != GoreSets.end())
	{
		if ((*f).second->mRefCount == 0 || (*f).second->mRefCount - 1 == 0)
		{
			delete (*f).second;
			GoreSets.erase(f);
		}
		else
		{
			(*f).second->mRefCount--;
		}
	}
}

CGoreSet::~CGoreSet()
{
	for (const auto& m_gore_record : mGoreRecords)
	{
		DeleteGoreRecord(m_gore_record.second.mGoreTag);
	}
};
#endif

extern mdxaBone_t		worldMatrix;
extern mdxaBone_t		worldMatrixInv;

const mdxaBone_t& EvalBoneCache(int index, CBoneCache* bone_cache);
class CTraceSurface
{
public:
	int					surface_num;
	surfaceInfo_v& rootSList;
	const model_t* currentModel;
	const int			lod;
	vec3_t		rayStart;
	vec3_t		rayEnd;
	CCollisionRecord* collRecMap;
	const int			ent_num;
	const int			model_index;
	const skin_t* skin;
	const shader_t* cust_shader;
	intptr_t* TransformedVertsArray;
	const EG2_Collision	e_g2_trace_type;
	bool				hitOne;
	float				m_fRadius;

#ifdef _G2_GORE
	//gore application thing
	float				ssize;
	float				tsize;
	float				theta;
	int					goreShader;
	CGhoul2Info* ghoul2info;

	//	Procedural-gore application things
	SSkinGoreData* gore;
#endif

	CTraceSurface(
		const int					initsurfaceNum,
		surfaceInfo_v& initrootSList,
		const model_t* initcurrentModel,
		const int					initlod,
		vec3_t				initrayStart,
		vec3_t				initrayEnd,
		CCollisionRecord* initcollRecMap,
		const int					initentNum,
		const int					initmodelIndex,
		const skin_t* initskin,
		const shader_t* initcust_shader,
		intptr_t* initTransformedVertsArray,
		const EG2_Collision	einitG2TraceType,
#ifdef _G2_GORE
		float				fRadius,
		const float				initssize,
		const float				inittsize,
		const float				inittheta,
		const int					initgoreShader,
		CGhoul2Info* initghoul2info,
		SSkinGoreData* initgore
#else
		float				fRadius
#endif
	):

	surface_num(initsurfaceNum),
		rootSList(initrootSList),
		currentModel(initcurrentModel),
		lod(initlod),
		collRecMap(initcollRecMap),
		ent_num(initentNum),
		model_index(initmodelIndex),
		skin(initskin),
		cust_shader(initcust_shader),
		TransformedVertsArray(initTransformedVertsArray),
		e_g2_trace_type(einitG2TraceType),
		hitOne(false),
#ifdef _G2_GORE
		m_fRadius(fRadius),
		ssize(initssize),
		tsize(inittsize),
		theta(inittheta),
		goreShader(initgoreShader),
		ghoul2info(initghoul2info),
		gore(initgore)
#else
		m_fRadius(fRadius)
#endif
	{
		VectorCopy(initrayStart, rayStart);
		VectorCopy(initrayEnd, rayEnd);
	}
};

// assorted Ghoul 2 functions.
// list all surfaces associated with a model
void G2_List_Model_Surfaces(const char* fileName)
{
	const model_t* mod_m = R_GetModelByHandle(RE_RegisterModel(fileName));

	auto surf = reinterpret_cast<mdxmSurfHierarchy_t*>(reinterpret_cast<byte*>(mod_m->mdxm) + mod_m->mdxm->ofsSurfHierarchy);
	auto surface = reinterpret_cast<mdxmSurface_t*>(reinterpret_cast<byte*>(mod_m->mdxm) + mod_m->mdxm->ofsLODs + sizeof(mdxmLOD_t));

	for (int x = 0; x < mod_m->mdxm->numSurfaces; x++)
	{
		Com_Printf("Surface %i Name %s\n", x, surf->name);
		if (r_verbose->value)
		{
			Com_Printf("Num Descendants %i\n", surf->numChildren);
			for (int i = 0; i < surf->numChildren; i++)
			{
				Com_Printf("Descendant %i\n", surf->childIndexes[i]);
			}
		}
		// find the next surface
		surf = reinterpret_cast<mdxmSurfHierarchy_t*>(reinterpret_cast<byte*>(surf) + reinterpret_cast<intptr_t>(&static_cast<mdxmSurfHierarchy_t*>(nullptr)->childIndexes
			[surf->numChildren]));
		surface = reinterpret_cast<mdxmSurface_t*>(reinterpret_cast<byte*>(surface) + surface->ofsEnd);
	}
}

// list all bones associated with a model
void G2_List_Model_Bones(const char* file_name)
{
	const model_t* mod_m = R_GetModelByHandle(RE_RegisterModel(file_name));
	const model_t* mod_a = R_GetModelByHandle(mod_m->mdxm->animIndex);
	// 	mdxaFrame_t		*aframe=0;
	//	int				frameSize;
	mdxaHeader_t* header = mod_a->mdxa;

	// figure out where the offset list is
	const mdxaSkelOffsets_t* offsets = reinterpret_cast<mdxaSkelOffsets_t*>(reinterpret_cast<byte*>(header) + sizeof(mdxaHeader_t));

	//    frameSize = (int)( &((mdxaFrame_t *)0)->boneIndexes[ header->numBones ] );

	//	aframe = (mdxaFrame_t *)((byte *)header + header->ofsFrames + (frame * frameSize));
		// walk each bone and list it's name
	for (int x = 0; x < mod_a->mdxa->numBones; x++)
	{
		const auto skel = reinterpret_cast<mdxaSkel_t*>(reinterpret_cast<byte*>(header) + sizeof(mdxaHeader_t) + offsets->offsets[x]);
		Com_Printf("Bone %i Name %s\n", x, skel->name);

		Com_Printf("X pos %f, Y pos %f, Z pos %f\n", skel->BasePoseMat.matrix[0][3], skel->BasePoseMat.matrix[1][3], skel->BasePoseMat.matrix[2][3]);

		// if we are in verbose mode give us more details
		if (r_verbose->value)
		{
			Com_Printf("Num Descendants %i\n", skel->numChildren);
			for (int i = 0; i < skel->numChildren; i++)
			{
				Com_Printf("Num Descendants %i\n", skel->numChildren);
			}
		}
	}
}

/************************************************************************************************
 * G2_GetAnimFileName
 *    obtain the .gla filename for a model
 *
 * Input
 *    filename of model
 *
 * Output
 *    true if we successfully obtained a filename, false otherwise
 *
 ************************************************************************************************/
qboolean G2_GetAnimFileName(const char* file_name, char** filename)
{
	// find the model we want
	const model_t* mod = R_GetModelByHandle(RE_RegisterModel(file_name));

	if (mod && mod->mdxm && mod->mdxm->animName[0] != 0)
	{
		*filename = mod->mdxm->animName;
		return qtrue;
	}
	return qfalse;
}

/////////////////////////////////////////////////////////////////////
//
//	Code for collision detection for models gameside
//
/////////////////////////////////////////////////////////////////////

int G2_DecideTraceLod(const CGhoul2Info& ghoul2, const int use_lod)
{
	int return_lod = use_lod;

	// if we are overriding the LOD at top level, then we can afford to only check this level of model
	if (ghoul2.mLodBias > return_lod)
	{
		return_lod = ghoul2.mLodBias;
	}
	assert(G2_MODEL_OK(&ghoul2));

	assert(ghoul2.currentModel);
	assert(ghoul2.currentModel->mdxm);
	//what about r_lodBias?

	// now ensure that we haven't selected a lod that doesn't exist for this model
	if (return_lod >= ghoul2.currentModel->mdxm->numLODs)
	{
		return_lod = ghoul2.currentModel->mdxm->numLODs - 1;
	}

	return return_lod;
}

void R_TransformEachSurface(const mdxmSurface_t* surface, vec3_t scale, CMiniHeap* g2_vert_space, intptr_t* transformed_verts_array, CBoneCache* bone_cache)
{
	int				 j, k;

	//
	// deform the vertexes by the lerped bones
	//
	const int* pi_bone_references = reinterpret_cast<int*>((byte*)surface + surface->ofsBoneReferences);

	// alloc some space for the transformed verts to get put in
	auto transformed_verts = reinterpret_cast<float*>(g2_vert_space->MiniHeapAlloc(surface->num_verts * 5 * 4));
	transformed_verts_array[surface->thisSurfaceIndex] = reinterpret_cast<intptr_t>(transformed_verts);
	if (!transformed_verts)
	{
		assert(transformed_verts);
		Com_Error(ERR_DROP, "Ran out of transform space for Ghoul2 Models. Adjust G2_MINIHEAP_SIZE in sv_init.cpp.\n");
	}

	// whip through and actually transform each vertex
	const int num_verts = surface->num_verts;
	auto v = reinterpret_cast<mdxmVertex_t*>((byte*)surface + surface->ofsVerts);
	const mdxmVertexTexCoord_t* p_tex_coords = reinterpret_cast<mdxmVertexTexCoord_t*>(&v[num_verts]);

	// optimisation issue
	if (scale[0] != 1.0 || scale[1] != 1.0 || scale[2] != 1.0)
	{
		for (j = 0; j < num_verts; j++)
		{
			vec3_t			temp_vert, temp_normal;
			//			mdxmWeight_t	*w;

			VectorClear(temp_vert);
			VectorClear(temp_normal);
			//			w = v->weights;

			const int i_num_weights = G2_GetVertWeights(v);

			float f_total_weight = 0.0f;
			for (k = 0; k < i_num_weights; k++)
			{
				const int		i_bone_index = G2_GetVertBoneIndex(v, k);
				const float	f_bone_weight = G2_GetVertBoneWeight(v, k, f_total_weight, i_num_weights);

				const mdxaBone_t& bone = EvalBoneCache(pi_bone_references[i_bone_index], bone_cache);

				temp_vert[0] += f_bone_weight * (DotProduct(bone.matrix[0], v->vertCoords) + bone.matrix[0][3]);
				temp_vert[1] += f_bone_weight * (DotProduct(bone.matrix[1], v->vertCoords) + bone.matrix[1][3]);
				temp_vert[2] += f_bone_weight * (DotProduct(bone.matrix[2], v->vertCoords) + bone.matrix[2][3]);

				temp_normal[0] += f_bone_weight * DotProduct(bone.matrix[0], v->normal);
				temp_normal[1] += f_bone_weight * DotProduct(bone.matrix[1], v->normal);
				temp_normal[2] += f_bone_weight * DotProduct(bone.matrix[2], v->normal);
			}
			int pos = j * 5;

			// copy tranformed verts into temp space
			transformed_verts[pos++] = temp_vert[0] * scale[0];
			transformed_verts[pos++] = temp_vert[1] * scale[1];
			transformed_verts[pos++] = temp_vert[2] * scale[2];
			// we will need the S & T coors too for hitlocation and hitmaterial stuff
			transformed_verts[pos++] = p_tex_coords[j].texCoords[0];
			transformed_verts[pos] = p_tex_coords[j].texCoords[1];

			v++;// = (mdxmVertex_t *)&v->weights[/*v->numWeights*/surface->maxVertBoneWeights];
		}
	}
	else
	{
		int pos = 0;
		for (j = 0; j < num_verts; j++)
		{
			vec3_t			temp_vert, temp_normal;
			//			const mdxmWeight_t	*w;

			VectorClear(temp_vert);
			VectorClear(temp_normal);
			//			w = v->weights;

			const int i_num_weights = G2_GetVertWeights(v);

			float f_total_weight = 0.0f;
			for (k = 0; k < i_num_weights; k++)
			{
				const int		i_bone_index = G2_GetVertBoneIndex(v, k);
				const float	f_bone_weight = G2_GetVertBoneWeight(v, k, f_total_weight, i_num_weights);

				const mdxaBone_t& bone = EvalBoneCache(pi_bone_references[i_bone_index], bone_cache);

				temp_vert[0] += f_bone_weight * (DotProduct(bone.matrix[0], v->vertCoords) + bone.matrix[0][3]);
				temp_vert[1] += f_bone_weight * (DotProduct(bone.matrix[1], v->vertCoords) + bone.matrix[1][3]);
				temp_vert[2] += f_bone_weight * (DotProduct(bone.matrix[2], v->vertCoords) + bone.matrix[2][3]);

				temp_normal[0] += f_bone_weight * DotProduct(bone.matrix[0], v->normal);
				temp_normal[1] += f_bone_weight * DotProduct(bone.matrix[1], v->normal);
				temp_normal[2] += f_bone_weight * DotProduct(bone.matrix[2], v->normal);
			}

			// copy tranformed verts into temp space
			transformed_verts[pos++] = temp_vert[0];
			transformed_verts[pos++] = temp_vert[1];
			transformed_verts[pos++] = temp_vert[2];
			// we will need the S & T coors too for hitlocation and hitmaterial stuff
			transformed_verts[pos++] = p_tex_coords[j].texCoords[0];
			transformed_verts[pos++] = p_tex_coords[j].texCoords[1];

			v++;// = (mdxmVertex_t *)&v->weights[/*v->numWeights*/surface->maxVertBoneWeights];
		}
	}
}

void G2_TransformSurfaces(const int surface_num, surfaceInfo_v& root_s_list,
	CBoneCache* bone_cache, const model_t* current_model, const int lod, vec3_t scale, CMiniHeap* g2_vert_space, intptr_t* transformed_vert_array, const bool second_time_around)
{
	assert(current_model);
	assert(current_model->mdxm);
	// back track and get the surfinfo struct for this surface
	const mdxmSurface_t* surface = static_cast<mdxmSurface_t*>(G2_FindSurface(current_model, surface_num, lod));
	const mdxmHierarchyOffsets_t* surf_indexes = reinterpret_cast<mdxmHierarchyOffsets_t*>(reinterpret_cast<byte*>(current_model->mdxm) + sizeof(mdxmHeader_t));
	const mdxmSurfHierarchy_t* surf_info = reinterpret_cast<mdxmSurfHierarchy_t*>((byte*)surf_indexes + surf_indexes->offsets[surface->thisSurfaceIndex]);

	// see if we have an override surface in the surface list
	const surfaceInfo_t* surf_override = G2_FindOverrideSurface(surface_num, root_s_list);

	// really, we should use the default flags for this surface unless it's been overriden
	int off_flags = surf_info->flags;

	if (surf_override)
	{
		off_flags = surf_override->off_flags;
	}
	// if this surface is not off, add it to the shader render list
	if (!off_flags)
	{
		R_TransformEachSurface(surface, scale, g2_vert_space, transformed_vert_array, bone_cache);
	}

	// if we are turning off all descendants, then stop this recursion now
	if (off_flags & G2SURFACEFLAG_NODESCENDANTS)
	{
		return;
	}

	// now recursively call for the children
	for (int i = 0; i < surf_info->numChildren; i++)
	{
		G2_TransformSurfaces(surf_info->childIndexes[i], root_s_list, bone_cache, current_model, lod, scale, g2_vert_space, transformed_vert_array, second_time_around);
	}
}

// main calling point for the model transform for collision detection. At this point all of the skeleton has been transformed.
#ifdef _G2_GORE
void G2_TransformModel(CGhoul2Info_v& ghoul2, const int frame_num, vec3_t scale, CMiniHeap* g2_vert_space, int use_lod, const bool apply_gore, const SSkinGoreData* gore)
#else
void G2_TransformModel(CGhoul2Info_v& ghoul2, const int frame_num, vec3_t scale, CMiniHeap* g2_vert_space, int use_lod)
#endif
{
	int lod;
	vec3_t			correct_scale;

#if !defined(JK2_MODE) || defined(_G2_GORE)
	qboolean		first_model_only = qfalse;
#endif // !JK2_MODE || _G2_GORE

#ifndef JK2_MODE
	if (cg_g2MarksAllModels == nullptr)
	{
		cg_g2MarksAllModels = ri.Cvar_Get("cg_g2MarksAllModels", "0", 0);
	}

	if (cg_g2MarksAllModels == nullptr
		|| !cg_g2MarksAllModels->integer)
	{
		first_model_only = qtrue;
	}
#endif // !JK2_MODE

#ifdef _G2_GORE
	if (gore
		&& gore->firstModel > 0)
	{
		first_model_only = qfalse;
	}
#endif

	VectorCopy(scale, correct_scale);
	// check for scales of 0 - that's the default I believe
	if (!scale[0])
	{
		correct_scale[0] = 1.0;
	}
	if (!scale[1])
	{
		correct_scale[1] = 1.0;
	}
	if (!scale[2])
	{
		correct_scale[2] = 1.0;
	}

	// walk each possible model for this entity and try rendering it out
	for (int i = 0; i < ghoul2.size(); i++)
	{
		CGhoul2Info& g = ghoul2[i];
		// don't bother with models that we don't care about.
		if (!g.mValid)
		{
			continue;
		}
		assert(g.mBoneCache);
		assert(G2_MODEL_OK(&g));
		// stop us building this model more than once per frame
		g.mMeshFrameNum = frame_num;

		// decide the LOD
#ifdef _G2_GORE
		if (apply_gore)
		{
			lod = use_lod;
			assert(g.currentModel);
			if (lod >= g.currentModel->numLods)
			{
				g.mTransformedVertsArray = nullptr;
				if (first_model_only)
				{
					// we don't really need to do multiple models for gore.
					return;
				}
				//do the rest
				continue;
			}
		}
		else
#endif
		{
			lod = G2_DecideTraceLod(g, use_lod);
		}

		// give us space for the transformed vertex array to be put in
		g.mTransformedVertsArray = reinterpret_cast<intptr_t*>(g2_vert_space->MiniHeapAlloc(g.currentModel->mdxm->numSurfaces * sizeof(intptr_t)));
		if (!g.mTransformedVertsArray)
		{
			Com_Error(ERR_DROP, "Ran out of transform space for Ghoul2 Models. Adjust G2_MINIHEAP_SIZE in sv_init.cpp.\n");
		}

		memset(g.mTransformedVertsArray, 0, g.currentModel->mdxm->numSurfaces * sizeof(intptr_t));

		G2_FindOverrideSurface(-1, g.mSlist); //reset the quick surface override lookup;
		// recursively call the model surface transform
		G2_TransformSurfaces(g.mSurfaceRoot, g.mSlist, g.mBoneCache, g.currentModel, lod, correct_scale, g2_vert_space, g.mTransformedVertsArray, false);

#ifdef _G2_GORE

		if (apply_gore && first_model_only)
		{
			// we don't really need to do multiple models for gore.
			break;
		}
#endif
	}
}

// work out how much space a triangle takes
static float	G2_AreaOfTri(const vec3_t A, const vec3_t B, const vec3_t C)
{
	vec3_t	cross, ab, cb;
	VectorSubtract(A, B, ab);
	VectorSubtract(C, B, cb);

	CrossProduct(ab, cb, cross);

	return VectorLength(cross);
}

// actually determine the S and T of the coordinate we hit in a given poly
static void G2_BuildHitPointST(const vec3_t A, const float SA, const float TA,
	const vec3_t B, const float SB, const float TB,
	const vec3_t C, const float SC, const float TC,
	const vec3_t P, float* s, float* t, float& bary_i, float& bary_j)
{
	const float	area_abc = G2_AreaOfTri(A, B, C);

	const float i = G2_AreaOfTri(P, B, C) / area_abc;
	bary_i = i;
	const float j = G2_AreaOfTri(A, P, C) / area_abc;
	bary_j = j;
	const float k = G2_AreaOfTri(A, B, P) / area_abc;

	*s = SA * i + SB * j + SC * k;
	*t = TA * i + TB * j + TC * k;

	*s = fmod(*s, 1);
	if (*s < 0)
	{
		*s += 1.0;
	}

	*t = fmod(*t, 1);
	if (*t < 0)
	{
		*t += 1.0;
	}
}

// routine that works out given a ray whether or not it hits a poly
static qboolean G2_SegmentTriangleTest(const vec3_t start, const vec3_t end,
	const vec3_t A, const vec3_t B, const vec3_t C,
	const qboolean back_faces, const qboolean front_faces, vec3_t returned_point, vec3_t returned_normal, float* denom)
{
	static constexpr float tiny = 1E-10f;
	vec3_t returned_normal_t;
	vec3_t edge_ac;

	VectorSubtract(C, A, edge_ac);
	VectorSubtract(B, A, returned_normal_t);

	CrossProduct(returned_normal_t, edge_ac, returned_normal);

	vec3_t ray;
	VectorSubtract(end, start, ray);

	*denom = DotProduct(ray, returned_normal);

	if (Q_fabs(*denom) < tiny ||        // triangle parallel to ray
		!back_faces && *denom > 0 ||		// not accepting back faces
		!front_faces && *denom < 0)		//not accepting front faces
	{
		return qfalse;
	}

	vec3_t to_plane;
	VectorSubtract(A, start, to_plane);

	const float t = DotProduct(to_plane, returned_normal) / *denom;

	if (t < 0.0f || t>1.0f)
	{
		return qfalse; // off segment
	}

	VectorScale(ray, t, ray);

	VectorAdd(ray, start, returned_point);

	vec3_t edge_pa;
	VectorSubtract(A, returned_point, edge_pa);

	vec3_t edge_pb;
	VectorSubtract(B, returned_point, edge_pb);

	vec3_t edge_pc;
	VectorSubtract(C, returned_point, edge_pc);

	vec3_t temp;

	CrossProduct(edge_pa, edge_pb, temp);
	if (DotProduct(temp, returned_normal) < 0.0f)
	{
		return qfalse; // off triangle
	}

	CrossProduct(edge_pc, edge_pa, temp);
	if (DotProduct(temp, returned_normal) < 0.0f)
	{
		return qfalse; // off triangle
	}

	CrossProduct(edge_pb, edge_pc, temp);
	if (DotProduct(temp, returned_normal) < 0.0f)
	{
		return qfalse; // off triangle
	}
	return qtrue;
}

#ifdef _G2_GORE
struct SVertexTemp
{
	int flags;
	int touch;
	int newindex;
	float tex[2];
	SVertexTemp() : flags(0), newindex(0), tex{}
	{
		touch = 0;
	}
};

#define MAX_GORE_VERTS (3000)
static SVertexTemp GoreVerts[MAX_GORE_VERTS];
static int GoreIndexCopy[MAX_GORE_VERTS];
static int GoreTouch = 1;

#define MAX_GORE_INDECIES (6000)
static int GoreIndecies[MAX_GORE_INDECIES];

#define GORE_MARGIN (0.0f)
int	G2API_GetTime(int arg_time);

// now we at poly level, check each model space transformed poly against the model world transfomed ray
static void G2_GorePolys(const mdxmSurface_t* surface, CTraceSurface& ts)
{
	int			j;
	vec3_t basis1;
	vec3_t basis2;
	vec3_t taxis;
	vec3_t saxis;

	if (!ts.gore)
	{
		return;
	}

	if (!ts.gore->useTheta)
	{
		VectorCopy(ts.gore->uaxis, basis2);
		CrossProduct(ts.rayEnd, basis2, basis1);
		if (DotProduct(basis1, basis1) < 0.005f)
		{	//shot dir and slash dir are too close
			return;
		}
	}

	if (ts.gore->useTheta)
	{
		basis2[0] = 0.0f;
		basis2[1] = 0.0f;
		basis2[2] = 1.0f;

		CrossProduct(ts.rayEnd, basis2, basis1);

		if (DotProduct(basis1, basis1) < .1f)
		{
			basis2[0] = 0.0f;
			basis2[1] = 1.0f;
			basis2[2] = 0.0f;
			CrossProduct(ts.rayEnd, basis2, basis1);
		}
		CrossProduct(ts.rayEnd, basis1, basis2);
	}

	// Give me a shot direction not a bunch of zeros :) -Gil
	assert(DotProduct(basis1, basis1) > .0001f);
	assert(DotProduct(basis2, basis2) > .0001f);

	VectorNormalize(basis1);
	VectorNormalize(basis2);

	const float c = cos(ts.theta);
	const float s = sin(ts.theta);

	VectorScale(basis1, .5f * c / ts.tsize, taxis);
	VectorMA(taxis, .5f * s / ts.tsize, basis2, taxis);

	VectorScale(basis1, -.5f * s / ts.ssize, saxis);
	VectorMA(saxis, .5f * c / ts.ssize, basis2, saxis);

	//fixme, everything above here should be pre-calculated in G2API_AddSkinGore
	const float* verts = reinterpret_cast<float*>(ts.TransformedVertsArray[surface->thisSurfaceIndex]);
	const int num_verts = surface->num_verts;
	int flags = 63;
	assert(num_verts < MAX_GORE_VERTS);
	for (j = 0; j < num_verts; j++)
	{
		const int pos = j * 5;
		vec3_t delta;
		delta[0] = verts[pos + 0] - ts.rayStart[0];
		delta[1] = verts[pos + 1] - ts.rayStart[1];
		delta[2] = verts[pos + 2] - ts.rayStart[2];
		const float x = DotProduct(delta, saxis) + 0.5f;
		const float t = DotProduct(delta, taxis) + 0.5f;
		const float depth = DotProduct(delta, ts.rayEnd);
		int vflags = 0;
		if (x > GORE_MARGIN)
		{
			vflags |= 1;
		}
		if (x < 1.0f - GORE_MARGIN)
		{
			vflags |= 2;
		}
		if (t > GORE_MARGIN)
		{
			vflags |= 4;
		}
		if (t < 1.0f - GORE_MARGIN)
		{
			vflags |= 8;
		}
		if (depth > ts.gore->depthStart)
		{
			vflags |= 16;
		}
		if (depth < ts.gore->depthEnd)
		{
			vflags |= 32;
		}
		vflags = ~vflags;
		flags &= vflags;
		GoreVerts[j].flags = vflags;
		GoreVerts[j].tex[0] = x;
		GoreVerts[j].tex[1] = t;
	}
	if (flags)
	{
		return; // completely off the gore splotch.
	}
	const int num_tris = surface->numTriangles;
	const mdxmTriangle_t* tris = reinterpret_cast<mdxmTriangle_t*>((byte*)surface + surface->ofsTriangles);
	verts = reinterpret_cast<float*>(ts.TransformedVertsArray[surface->thisSurfaceIndex]);
	int new_num_tris = 0;
	int new_num_verts = 0;
	GoreTouch++;
	for (j = 0; j < num_tris; j++)
	{
		assert(tris[j].indexes[0] >= 0 && tris[j].indexes[0] < num_verts);
		assert(tris[j].indexes[1] >= 0 && tris[j].indexes[1] < num_verts);
		assert(tris[j].indexes[2] >= 0 && tris[j].indexes[2] < num_verts);
		flags = 63 &
			GoreVerts[tris[j].indexes[0]].flags &
			GoreVerts[tris[j].indexes[1]].flags &
			GoreVerts[tris[j].indexes[2]].flags;
		if (flags)
		{
			continue;
		}
		if (!ts.gore->frontFaces || !ts.gore->backFaces)
		{
			// we need to back/front face cull
			vec3_t e1, e2, n;

			VectorSubtract(&verts[tris[j].indexes[1] * 5], &verts[tris[j].indexes[0] * 5], e1);
			VectorSubtract(&verts[tris[j].indexes[2] * 5], &verts[tris[j].indexes[0] * 5], e2);
			CrossProduct(e1, e2, n);
			if (DotProduct(ts.rayEnd, n) > 0.0f)
			{
				if (!ts.gore->frontFaces)
				{
					continue;
				}
			}
			else
			{
				if (!ts.gore->backFaces)
				{
					continue;
				}
			}
		}

		assert(new_num_tris * 3 + 3 < MAX_GORE_INDECIES);
		for (int k = 0; k < 3; k++)
		{
			if (GoreVerts[tris[j].indexes[k]].touch == GoreTouch)
			{
				GoreIndecies[new_num_tris * 3 + k] = GoreVerts[tris[j].indexes[k]].newindex;
			}
			else
			{
				GoreVerts[tris[j].indexes[k]].touch = GoreTouch;
				GoreVerts[tris[j].indexes[k]].newindex = new_num_verts;
				GoreIndecies[new_num_tris * 3 + k] = new_num_verts;
				GoreIndexCopy[new_num_verts] = tris[j].indexes[k];
				new_num_verts++;
			}
		}
		new_num_tris++;
	}
	if (!new_num_verts)
	{
		return;
	}

	int new_tag;
	const auto f = GoreTagsTemp.find(std::make_pair(goreModelIndex, ts.surface_num));
	if (f == GoreTagsTemp.end()) // need to generate a record
	{
		new_tag = AllocGoreRecord();
		CGoreSet* gore_set = nullptr;
		if (ts.ghoul2info->mGoreSetTag)
		{
			gore_set = FindGoreSet(ts.ghoul2info->mGoreSetTag);
		}
		if (!gore_set)
		{
			gore_set = NewGoreSet();
			ts.ghoul2info->mGoreSetTag = gore_set->mMyGoreSetTag;
		}
		assert(gore_set);
		SGoreSurface add;
		add.shader = ts.goreShader;
		add.mDeleteTime = 0;
		if (ts.gore->lifeTime)
		{
			add.mDeleteTime = G2API_GetTime(0) + ts.gore->lifeTime;
		}
		add.mFadeTime = ts.gore->fadeOutTime;
		add.mFadeRGB = ts.gore->fadeRGB;
		add.mGoreTag = new_tag;

		add.mGoreGrowStartTime = G2API_GetTime(0);
		if (ts.gore->growDuration == -1)
		{
			add.mGoreGrowEndTime = -1;    // set this to -1 to disable growing
		}
		else
		{
			add.mGoreGrowEndTime = G2API_GetTime(0) + ts.gore->growDuration;
		}

		assert(ts.gore->growDuration != 0);
		add.mGoreGrowFactor = (1.0f - ts.gore->goreScaleStartFraction) / static_cast<float>(ts.gore->growDuration);	//curscale = (curtime-mGoreGrowStartTime)*mGoreGrowFactor;
		add.mGoreGrowOffset = ts.gore->goreScaleStartFraction;

		gore_set->mGoreRecords.insert(std::make_pair(ts.surface_num, add));
		GoreTagsTemp[std::make_pair(goreModelIndex, ts.surface_num)] = new_tag;
	}
	else
	{
		new_tag = (*f).second;
	}
	GoreTextureCoordinates* gore = FindGoreRecord(new_tag);
	if (gore)
	{
		assert(sizeof(float) == sizeof(int));
		// data block format:
		const unsigned int size =
			sizeof(int) + // num verts
			sizeof(int) + // num tris
			sizeof(int) * new_num_verts + // which verts to copy from original surface
			sizeof(float) * 4 * new_num_verts + // storgage for deformed verts
			sizeof(float) * 4 * new_num_verts + // storgage for deformed normal
			sizeof(float) * 2 * new_num_verts + // texture coordinates
			sizeof(int) * new_num_tris * 3;  // new indecies

		auto data = static_cast<int*>(R_Malloc(sizeof(int) * size, TAG_GHOUL2, qtrue));

		if (gore->tex[ts.lod])
			R_Free(gore->tex[ts.lod]);

		gore->tex[ts.lod] = reinterpret_cast<float*>(data);
		*data++ = new_num_verts;
		*data++ = new_num_tris;

		memcpy(data, GoreIndexCopy, sizeof(int) * new_num_verts);
		data += new_num_verts * 9; // skip verts and normals
		auto fdata = reinterpret_cast<float*>(data);

		for (j = 0; j < new_num_verts; j++)
		{
			*fdata++ = GoreVerts[GoreIndexCopy[j]].tex[0];
			*fdata++ = GoreVerts[GoreIndexCopy[j]].tex[1];
		}
		data = reinterpret_cast<int*>(fdata);
		memcpy(data, GoreIndecies, sizeof(int) * new_num_tris * 3);
		data += new_num_tris * 3;
		assert((data - reinterpret_cast<int*>(gore->tex[ts.lod])) * sizeof(int) == size);
		fdata = reinterpret_cast<float*>(data);
		// build the entity to gore matrix
		VectorCopy(saxis, fdata + 0);
		VectorCopy(taxis, fdata + 4);
		VectorCopy(ts.rayEnd, fdata + 8);
		VectorNormalize(fdata + 0);
		VectorNormalize(fdata + 4);
		VectorNormalize(fdata + 8);
		fdata[3] = -0.5f; // subtract texture center
		fdata[7] = -0.5f;
		fdata[11] = 0.0f;
		vec3_t shot_origin_in_current_space; // unknown space
		TransformPoint(ts.rayStart, shot_origin_in_current_space, reinterpret_cast<mdxaBone_t*>(fdata)); // dest middle arg
		// this will insure the shot origin in our unknown space is now the shot origin, making it a known space
		fdata[3] -= shot_origin_in_current_space[0];
		fdata[7] -= shot_origin_in_current_space[1];
		fdata[11] -= shot_origin_in_current_space[2];
		Inverse_Matrix(reinterpret_cast<mdxaBone_t*>(fdata), reinterpret_cast<mdxaBone_t*>(fdata + 12));  // dest 2nd arg
		data += 24;

		//		assert((data - (int *)gore->tex[TS.lod]) * sizeof(int) == size);
	}
}
#else
struct SVertexTemp
{
	int flags;
	//	int touch;
	//	int newindex;
	//	float tex[2];
	SVertexTemp()
	{
		//		touch=0;
	}
};

#define MAX_GORE_VERTS (3000)
static SVertexTemp GoreVerts[MAX_GORE_VERTS];
#endif

// now we're at poly level, check each model space transformed poly against the model world transfomed ray
static bool G2_TracePolys(const mdxmSurface_t* surface, CTraceSurface& ts)
{
	// whip through and actually transform each vertex
	const mdxmTriangle_t* tris = reinterpret_cast<mdxmTriangle_t*>((byte*)surface + surface->ofsTriangles);
	const float* verts = reinterpret_cast<float*>(ts.TransformedVertsArray[surface->thisSurfaceIndex]);
	const int num_tris = surface->numTriangles;
	for (int j = 0; j < num_tris; j++)
	{
		float			face;
		vec3_t	hit_point, normal;
		// determine actual coords for this triangle
		const float* point1 = &verts[(tris[j].indexes[0] * 5)];
		const float* point2 = &verts[(tris[j].indexes[1] * 5)];
		const float* point3 = &verts[(tris[j].indexes[2] * 5)];
		// did we hit it?
		if (G2_SegmentTriangleTest(ts.rayStart, ts.rayEnd, point1, point2, point3, qtrue, qtrue, hit_point, normal, &face))
		{	// find space in the collision records for this record
			int i = 0;
			for (; i < MAX_G2_COLLISIONS; i++)
			{
				if (ts.collRecMap[i].mEntityNum == -1)
				{
					CCollisionRecord& new_col = ts.collRecMap[i];
					vec3_t			  	dist_vect;
					float				x_pos = 0, y_pos = 0;

					new_col.mPolyIndex = j;
					new_col.mEntityNum = ts.ent_num;
					new_col.mSurfaceIndex = surface->thisSurfaceIndex;
					new_col.mModelIndex = ts.model_index;
					if (face > 0)
					{
						new_col.mFlags = G2_FRONTFACE;
					}
					else
					{
						new_col.mFlags = G2_BACKFACE;
					}

					VectorSubtract(hit_point, ts.rayStart, dist_vect);
					new_col.mDistance = VectorLength(dist_vect);
					assert(!Q_isnan(new_col.mDistance));

					// put the hit point back into world space
					TransformAndTranslatePoint(hit_point, new_col.mCollisionPosition, &worldMatrix);

					// transform normal (but don't translate) into world angles
					TransformPoint(normal, new_col.mCollisionNormal, &worldMatrix);
					VectorNormalize(new_col.mCollisionNormal);

					new_col.mMaterial = new_col.mLocation = 0;

					// Determine our location within the texture, and barycentric coordinates
					G2_BuildHitPointST(point1, point1[3], point1[4],
						point2, point2[3], point2[4],
						point3, point3[3], point3[4],
						hit_point, &x_pos, &y_pos, new_col.mBarycentricI, new_col.mBarycentricJ);

					/*
										const shader_t		*shader = 0;
										// now, we know what surface this hit belongs to, we need to go get the shader handle so we can get the correct hit location and hit material info
										if ( cust_shader )
										{
											shader = cust_shader;
										}
										else if ( skin )
										{
											int		j;

											// match the surface name to something in the skin file
											shader = tr.defaultShader;
											for ( j = 0 ; j < skin->numSurfaces ; j++ )
											{
												// the names have both been lowercased
												if ( !strcmp( skin->surfaces[j]->name, surfInfo->name ) )
												{
													shader = skin->surfaces[j]->shader;
													break;
												}
											}
										}
										else
										{
											shader = R_GetShaderByHandle( surfInfo->shaderIndex );
										}

										// do we even care to decide what the hit or location area's are? If we don't have them in the shader there is little point
										if ((shader->hitLocation) || (shader->hitMaterial))
										{
											// ok, we have a floating point position. - determine location in data we need to look at
											if (shader->hitLocation)
											{
												newCol.mLocation = *(hitMatReg[shader->hitLocation].loc +
																	((int)(y_pos * hitMatReg[shader->hitLocation].height) * hitMatReg[shader->hitLocation].width) +
																	((int)(x_pos * hitMatReg[shader->hitLocation].width)));
												Com_Printf("G2_TracePolys hit location: %d\n", newCol.mLocation);
											}

											if (shader->hitMaterial)
											{
												newCol.mMaterial = *(hitMatReg[shader->hitMaterial].loc +
																	((int)(y_pos * hitMatReg[shader->hitMaterial].height) * hitMatReg[shader->hitMaterial].width) +
																	((int)(x_pos * hitMatReg[shader->hitMaterial].width)));
											}
										}
					*/
					// exit now if we should
					if (ts.e_g2_trace_type == G2_RETURNONHIT)
					{
						ts.hitOne = true;
						return true;
					}

					break;
				}
			}
			if (i == MAX_G2_COLLISIONS)
			{
				//assert(i != MAX_G2_COLLISIONS);		// run out of collision record space - will probalbly never happen
				ts.hitOne = true;	//force stop recursion
				return true;	// return true to avoid wasting further time, but no hit will result without a record
			}
		}
	}
	return false;
}

// now we're at poly level, check each model space transformed poly against the model world transfomed ray
static bool G2_RadiusTracePolys(
	const mdxmSurface_t* surface,
	CTraceSurface& TS
)
{
	int		j;
	vec3_t basis1;
	vec3_t basis2;
	vec3_t taxis;
	vec3_t saxis;

	basis2[0] = 0.0f;
	basis2[1] = 0.0f;
	basis2[2] = 1.0f;

	vec3_t v3_ray_dir;
	VectorSubtract(TS.rayEnd, TS.rayStart, v3_ray_dir);

	CrossProduct(v3_ray_dir, basis2, basis1);

	if (DotProduct(basis1, basis1) < .1f)
	{
		basis2[0] = 0.0f;
		basis2[1] = 1.0f;
		basis2[2] = 0.0f;
		CrossProduct(v3_ray_dir, basis2, basis1);
	}

	CrossProduct(v3_ray_dir, basis1, basis2);
	// Give me a shot direction not a bunch of zeros :) -Gil
//	assert(DotProduct(basis1,basis1)>.0001f);
//	assert(DotProduct(basis2,basis2)>.0001f);

	VectorNormalize(basis1);
	VectorNormalize(basis2);

	const float c = cos(0.0f);//theta
	const float s = sin(0.0f);//theta

	VectorScale(basis1, 0.5f * c / TS.m_fRadius, taxis);
	VectorMA(taxis, 0.5f * s / TS.m_fRadius, basis2, taxis);

	VectorScale(basis1, -0.5f * s / TS.m_fRadius, saxis);
	VectorMA(saxis, 0.5f * c / TS.m_fRadius, basis2, saxis);

	const float* const verts = reinterpret_cast<float*>(TS.TransformedVertsArray[surface->thisSurfaceIndex]);
	const int num_verts = surface->num_verts;

	int flags = 63;
	//rayDir/=lengthSquared(raydir);
	const float f = VectorLengthSquared(v3_ray_dir);
	v3_ray_dir[0] /= f;
	v3_ray_dir[1] /= f;
	v3_ray_dir[2] /= f;

	for (j = 0; j < num_verts; j++)
	{
		const int pos = j * 5;
		vec3_t delta;
		delta[0] = verts[pos + 0] - TS.rayStart[0];
		delta[1] = verts[pos + 1] - TS.rayStart[1];
		delta[2] = verts[pos + 2] - TS.rayStart[2];
		const float x = DotProduct(delta, saxis) + 0.5f;
		const float t = DotProduct(delta, taxis) + 0.5f;
		const float u = DotProduct(delta, v3_ray_dir);
		int vflags = 0;

		if (x > 0)
		{
			vflags |= 1;
		}
		if (x < 1)
		{
			vflags |= 2;
		}
		if (t > 0)
		{
			vflags |= 4;
		}
		if (t < 1)
		{
			vflags |= 8;
		}
		if (u > 0)
		{
			vflags |= 16;
		}
		if (u < 1)
		{
			vflags |= 32;
		}

		vflags = ~vflags;
		flags &= vflags;
		GoreVerts[j].flags = vflags;
	}

	if (flags)
	{
		return false; // completely off the gore splotch  (so presumably hit nothing? -Ste)
	}
	const int num_tris = surface->numTriangles;
	const mdxmTriangle_t* const tris = reinterpret_cast<mdxmTriangle_t*>((byte*)surface + surface->ofsTriangles);

	for (j = 0; j < num_tris; j++)
	{
		assert(tris[j].indexes[0] >= 0 && tris[j].indexes[0] < num_verts);
		assert(tris[j].indexes[1] >= 0 && tris[j].indexes[1] < num_verts);
		assert(tris[j].indexes[2] >= 0 && tris[j].indexes[2] < num_verts);
		flags = 63 &
			GoreVerts[tris[j].indexes[0]].flags &
			GoreVerts[tris[j].indexes[1]].flags &
			GoreVerts[tris[j].indexes[2]].flags;
		if (flags)
		{
			continue;
		}
		// we hit a triangle, so init a collision record...
		//
		int i = 0;
		for (; i < MAX_G2_COLLISIONS; i++)
		{
			if (TS.collRecMap[i].mEntityNum == -1)
			{
				CCollisionRecord& newCol = TS.collRecMap[i];

				newCol.mPolyIndex = j;
				newCol.mEntityNum = TS.ent_num;
				newCol.mSurfaceIndex = surface->thisSurfaceIndex;
				newCol.mModelIndex = TS.model_index;
				//					if (face>0)
				//					{
				newCol.mFlags = G2_FRONTFACE;
				//					}
				//					else
				//					{
				//						newCol.mFlags = G2_BACKFACE;
				//					}

				//get normal from triangle
				const float* A = &verts[(tris[j].indexes[0] * 5)];
				const float* B = &verts[(tris[j].indexes[1] * 5)];
				const float* C = &verts[(tris[j].indexes[2] * 5)];
				vec3_t normal;
				vec3_t edge_ac, edge_ba;

				VectorSubtract(C, A, edge_ac);
				VectorSubtract(B, A, edge_ba);
				CrossProduct(edge_ba, edge_ac, normal);

				// transform normal (but don't translate) into world angles
				TransformPoint(normal, newCol.mCollisionNormal, &worldMatrix);
				VectorNormalize(newCol.mCollisionNormal);

				newCol.mMaterial = newCol.mLocation = 0;
				// exit now if we should
				if (TS.e_g2_trace_type == G2_RETURNONHIT)
				{
					TS.hitOne = true;
					return true;
				}

				vec3_t			  dist_vect;
#if 0
				//i don't know the hitPoint, but let's just assume it's the first vert for now...
				float* hitPoint = (float*)A;
#else
				//yeah, I want the collision point. Let's work out the impact point on the triangle. -rww
				vec3_t hit_point;
				float dist;
				const float third = -(A[0] * (B[1] * C[2] - C[1] * B[2]) + B[0] * (C[1] * A[2] - A[1] * C[2]) + C[0] * (A[1] * B[2] - B[1] * A[2]));

				VectorSubtract(TS.rayEnd, TS.rayStart, dist_vect);
				const float side = normal[0] * TS.rayStart[0] + normal[1] * TS.rayStart[1] + normal[2] * TS.rayStart[2] + third;
				const float side2 = normal[0] * dist_vect[0] + normal[1] * dist_vect[1] + normal[2] * dist_vect[2];
				if (fabsf(side2) < 1E-8f)
				{
					//i don't know the hitPoint, but let's just assume it's the first vert for now...
					VectorSubtract(A, TS.rayStart, dist_vect);
					dist = VectorLength(dist_vect);
					VectorSubtract(TS.rayEnd, TS.rayStart, dist_vect);
					VectorMA(TS.rayStart, dist / VectorLength(dist_vect), dist_vect, hit_point);
				}
				else
				{
					dist = side / side2;
					VectorMA(TS.rayStart, -dist, dist_vect, hit_point);
				}
#endif

				VectorSubtract(hit_point, TS.rayStart, dist_vect);
				newCol.mDistance = VectorLength(dist_vect);
				assert(!Q_isnan(newCol.mDistance));

				// put the hit point back into world space
				TransformAndTranslatePoint(hit_point, newCol.mCollisionPosition, &worldMatrix);
				newCol.mBarycentricI = newCol.mBarycentricJ = 0.0f;

				break;
			}
		}
		if (i == MAX_G2_COLLISIONS)
		{
			//assert(i!=MAX_G2_COLLISIONS);		// run out of collision record space - happens OFTEN
			TS.hitOne = true;	//force stop recursion
			return true;	// return true to avoid wasting further time, but no hit will result without a record
		}
	}

	return false;
}

// look at a surface and then do the trace on each poly
static void G2_TraceSurfaces(CTraceSurface& TS)
{
	// back track and get the surfinfo struct for this surface
	assert(TS.currentModel);
	assert(TS.currentModel->mdxm);
	const mdxmSurface_t* surface = static_cast<mdxmSurface_t*>(G2_FindSurface(TS.currentModel, TS.surface_num, TS.lod));
	const mdxmHierarchyOffsets_t* surf_indexes = reinterpret_cast<mdxmHierarchyOffsets_t*>(reinterpret_cast<byte*>(TS.currentModel->mdxm) + sizeof(mdxmHeader_t));
	const mdxmSurfHierarchy_t* surf_info = reinterpret_cast<mdxmSurfHierarchy_t*>((byte*)surf_indexes + surf_indexes->offsets[surface->thisSurfaceIndex]);

	// see if we have an override surface in the surface list
	const surfaceInfo_t* surf_override = G2_FindOverrideSurface(TS.surface_num, TS.rootSList);

	// don't allow recursion if we've already hit a polygon
	if (TS.hitOne)
	{
		return;
	}

	// really, we should use the default flags for this surface unless it's been overriden
	int off_flags = surf_info->flags;

	// set the off flags if we have some
	if (surf_override)
	{
		off_flags = surf_override->off_flags;
	}

	// if this surface is not off, try to hit it
	if (!off_flags)
	{
#ifdef _G2_GORE
		if (TS.collRecMap)
		{
#endif
			if (Q_fabs(TS.m_fRadius) >= 0.1)	// if not a point-trace
			{
				// .. then use radius check
				//
				if (G2_RadiusTracePolys(surface,		// const mdxmSurface_t *surface,
					TS
				)
					&& TS.e_g2_trace_type == G2_RETURNONHIT
					)
				{
					TS.hitOne = true;
					return;
				}
			}
			else
			{
				// go away and trace the polys in this surface
				if (G2_TracePolys(surface, TS)
					&& TS.e_g2_trace_type == G2_RETURNONHIT
					)
				{
					// ok, we hit one, *and* we want to return instantly because the returnOnHit is set
					// so indicate we've hit one, so other surfaces don't get hit and return
					TS.hitOne = true;
					return;
				}
			}
#ifdef _G2_GORE
		}
		else
		{
			G2_GorePolys(surface, TS);
		}
#endif
	}

	// if we are turning off all descendants, then stop this recursion now
	if (off_flags & G2SURFACEFLAG_NODESCENDANTS)
	{
		return;
	}

	// now recursively call for the children
	for (int i = 0; i < surf_info->numChildren && !TS.hitOne; i++)
	{
		TS.surface_num = surf_info->childIndexes[i];
		G2_TraceSurfaces(TS);
	}
}

#ifdef _G2_GORE
void G2_TraceModels(CGhoul2Info_v& ghoul2, vec3_t rayStart, vec3_t rayEnd, CCollisionRecord* collRecMap, int ent_num, EG2_Collision e_g2_trace_type, int use_lod, float fRadius, const float ssize, const float tsize, const float theta, const int shader, SSkinGoreData* gore, const qboolean skipIfLODNotMatch)
#else
void G2_TraceModels(CGhoul2Info_v& ghoul2, vec3_t rayStart, vec3_t rayEnd, CCollisionRecord* collRecMap, int ent_num, EG2_Collision e_g2_trace_type, int use_lod, float fRadius)
#endif
{
	int lod;
	skin_t* skin;
	shader_t* cust_shader;
#if !defined(JK2_MODE) || defined(_G2_GORE)
	qboolean		firstModelOnly = qfalse;
#endif // !JK2_MODE || _G2_GORE
	int				firstModel = 0;

#ifndef JK2_MODE
	if (cg_g2MarksAllModels == nullptr)
	{
		cg_g2MarksAllModels = ri.Cvar_Get("cg_g2MarksAllModels", "0", 0);
	}

	if (cg_g2MarksAllModels == nullptr
		|| !cg_g2MarksAllModels->integer)
	{
		firstModelOnly = qtrue;
	}
#endif // !JK2_MODE

#ifdef _G2_GORE
	if (gore
		&& gore->firstModel > 0)
	{
		firstModel = gore->firstModel;
		firstModelOnly = qfalse;
	}
#endif

	// walk each possible model for this entity and try tracing against it
	for (int i = firstModel; i < ghoul2.size(); i++)
	{
		CGhoul2Info& g = ghoul2[i];
#ifdef _G2_GORE
		goreModelIndex = i;
		// don't bother with models that we don't care about.
		if (g.mModelindex == -1)
		{
			continue;
		}
#endif
		// don't bother with models that we don't care about.
		if (!g.mValid)
		{
			continue;
		}
		assert(G2_MODEL_OK(&ghoul2[i]));
		// do we really want to collide with this object?
		if (g.mFlags & GHOUL2_NOCOLLIDE)
		{
			continue;
		}

		if (g.mCustomShader)
		{
			cust_shader = R_GetShaderByHandle(g.mCustomShader);
		}
		else
		{
			cust_shader = nullptr;
		}

		// figure out the custom skin thing
		if (g.mSkin > 0 && g.mSkin < tr.numSkins)
		{
			skin = R_GetSkinByHandle(g.mSkin);
		}
		else
		{
			skin = nullptr;
		}

		lod = G2_DecideTraceLod(g, use_lod);

#ifndef JK2_MODE
		if (skipIfLODNotMatch)
		{//we only want to hit this SPECIFIC LOD...
			if (lod != use_lod)
			{//doesn't match, skip this model
				continue;
			}
		}
#endif // !JK2_MODE

		//reset the quick surface override lookup
		G2_FindOverrideSurface(-1, g.mSlist);

#ifdef _G2_GORE
		CTraceSurface TS(g.mSurfaceRoot, g.mSlist, g.currentModel, lod, rayStart, rayEnd, collRecMap, ent_num, i, skin, cust_shader, g.mTransformedVertsArray, e_g2_trace_type, fRadius, ssize, tsize, theta, shader, &g, gore);
#else
		CTraceSurface TS(g.mSurfaceRoot, g.mSlist, g.currentModel, lod, rayStart, rayEnd, collRecMap, ent_num, i, skin, cust_shader, g.mTransformedVertsArray, e_g2_trace_type, fRadius);
#endif
		// start the surface recursion loop
		G2_TraceSurfaces(TS);

		// if we've hit one surface on one model, don't bother doing the rest
		if (TS.hitOne)
		{
			break;
		}
#ifdef _G2_GORE
		if (!collRecMap && firstModelOnly)
		{
			// we don't really need to do multiple models for gore.
			break;
		}
#endif
	}
}

void TransformPoint(const vec3_t in, vec3_t out, const mdxaBone_t* mat) {
	for (int i = 0; i < 3; i++)
	{
		out[i] = in[0] * mat->matrix[i][0] + in[1] * mat->matrix[i][1] + in[2] * mat->matrix[i][2];
	}
}

void TransformAndTranslatePoint(const vec3_t in, vec3_t out, const mdxaBone_t* mat) {
	for (int i = 0; i < 3; i++)
	{
		out[i] = in[0] * mat->matrix[i][0] + in[1] * mat->matrix[i][1] + in[2] * mat->matrix[i][2] + mat->matrix[i][3];
	}
}

// create a matrix using a set of angles
void Create_Matrix(const float* angle, mdxaBone_t* matrix)
{
	vec3_t		axis[3];

	// convert angles to axis
	AnglesToAxis(angle, axis);
	matrix->matrix[0][0] = axis[0][0];
	matrix->matrix[1][0] = axis[0][1];
	matrix->matrix[2][0] = axis[0][2];

	matrix->matrix[0][1] = axis[1][0];
	matrix->matrix[1][1] = axis[1][1];
	matrix->matrix[2][1] = axis[1][2];

	matrix->matrix[0][2] = axis[2][0];
	matrix->matrix[1][2] = axis[2][1];
	matrix->matrix[2][2] = axis[2][2];

	matrix->matrix[0][3] = 0;
	matrix->matrix[1][3] = 0;
	matrix->matrix[2][3] = 0;
}

// given a matrix, generate the inverse of that matrix
void Inverse_Matrix(const mdxaBone_t* src, mdxaBone_t* dest)
{
	int i, j;

	for (i = 0; i < 3; i++)
	{
		for (j = 0; j < 3; j++)
		{
			dest->matrix[i][j] = src->matrix[j][i];
		}
	}
	for (i = 0; i < 3; i++)
	{
		dest->matrix[i][3] = 0;
		for (j = 0; j < 3; j++)
		{
			dest->matrix[i][3] -= dest->matrix[i][j] * src->matrix[j][3];
		}
	}
}

// generate the world matrix for a given set of angles and origin - called from lots of places
void G2_GenerateWorldMatrix(const vec3_t angles, const vec3_t origin)
{
	Create_Matrix(angles, &worldMatrix);
	worldMatrix.matrix[0][3] = origin[0];
	worldMatrix.matrix[1][3] = origin[1];
	worldMatrix.matrix[2][3] = origin[2];

	Inverse_Matrix(&worldMatrix, &worldMatrixInv);
}

// go away and determine what the pointer for a specific surface definition within the model definition is
void* G2_FindSurface(const model_s* mod, const int index, const int lod)
{
	assert(mod);
	assert(mod->mdxm);

	// point at first lod list
	auto current = reinterpret_cast<byte*>(reinterpret_cast<intptr_t>(mod->mdxm) + mod->mdxm->ofsLODs);

	//walk the lods
	assert(lod >= 0 && lod < mod->mdxm->numLODs);
	for (int i = 0; i < lod; i++)
	{
		const mdxmLOD_t* lod_data = reinterpret_cast<mdxmLOD_t*>(current);
		current += lod_data->ofsEnd;
	}

	// avoid the lod pointer data structure
	current += sizeof(mdxmLOD_t);

	const mdxmLODSurfOffset_t* indexes = reinterpret_cast<mdxmLODSurfOffset_t*>(current);
	// we are now looking at the offset array
	assert(index >= 0 && index < mod->mdxm->numSurfaces);
	current += indexes->offsets[index];

	return current;
}

#define SURFACE_SAVE_BLOCK_SIZE	sizeof(surfaceInfo_t)
#define BOLT_SAVE_BLOCK_SIZE sizeof(boltInfo_t)
#define BONE_SAVE_BLOCK_SIZE sizeof(boneInfo_t)

void G2_SaveGhoul2Models(
	CGhoul2Info_v& ghoul2)
{
	ojk::SavedGameHelper saved_game(
		::ri.saved_game);

	saved_game.reset_buffer();

	// is there anything to save?
	if (!ghoul2.IsValid() || ghoul2.size() == 0)
	{
		constexpr int zero_size = 0;

#ifdef JK2_MODE
		saved_game.write<int32_t>(
			zero_size);

		saved_game.write_chunk_and_size<int32_t>(
			INT_ID('G', 'L', '2', 'S'),
			INT_ID('G', 'H', 'L', '2'));
#else
		saved_game.write_chunk<int32_t>(
			INT_ID('G', 'H', 'L', '2'),
			zero_size); //write out a zero buffer
#endif // JK2_MODE

		return;
	}

	// save out how many ghoul2 models we have
	const int model_count = ghoul2.size();

	saved_game.write<int32_t>(
		model_count);

	for (int i = 0; i < model_count; ++i)
	{
		// first save out the ghoul2 details themselves
		ghoul2[i].sg_export(
			saved_game);

		// save out how many surfaces we have
		const int surface_count = static_cast<int>(ghoul2[i].mSlist.size());

		saved_game.write<int32_t>(
			surface_count);

		// now save the all the surface list info
		for (int x = 0; x < surface_count; ++x)
		{
			ghoul2[i].mSlist[x].sg_export(
				saved_game);
		}

		// save out how many bones we have
		const int bone_count = static_cast<int>(ghoul2[i].mBlist.size());

		saved_game.write<int32_t>(
			bone_count);

		// now save the all the bone list info
		for (int x = 0; x < bone_count; ++x)
		{
			ghoul2[i].mBlist[x].sg_export(
				saved_game);
		}

		// save out how many bolts we have
		const int bolt_count = static_cast<int>(ghoul2[i].mBltlist.size());

		saved_game.write<int32_t>(
			bolt_count);

		// lastly save the all the bolt list info
		for (int x = 0; x < bolt_count; ++x)
		{
			ghoul2[i].mBltlist[x].sg_export(
				saved_game);
		}
	}

#ifdef JK2_MODE
	saved_game.write_chunk_and_size<int32_t>(
		INT_ID('G', 'L', '2', 'S'),
		INT_ID('G', 'H', 'L', '2'));
#else
	saved_game.write_chunk(
		INT_ID('G', 'H', 'L', '2'));
#endif // JK2_MODE
}

// FIXME Remove 'buffer' parameter
void G2_LoadGhoul2Model(
	CGhoul2Info_v& ghoul2,
	const char* buffer)
{
	static_cast<void>(buffer);

	ojk::SavedGameHelper saved_game(
		::ri.saved_game);

	// first thing, lets see how many ghoul2 models we have, and resize our buffers accordingly
	int model_count = 0;

#ifdef JK2_MODE
	if (saved_game.get_buffer_size() > 0)
	{
#endif // JK2_MODE

		saved_game.read<int32_t>(
			model_count);

#ifdef JK2_MODE
	}
#endif // JK2_MODE

	ghoul2.resize(
		model_count);

	// did we actually resize to a value?
	if (model_count == 0)
	{
		// no, ok, well, done then.
		return;
	}

	// now we have enough instances, lets go through each one and load up the relevant details
	for (decltype(model_count) i = 0; i < model_count; ++i)
	{
		ghoul2[i].mSkelFrameNum = 0;
		ghoul2[i].mModelindex = -1;
		ghoul2[i].mFileName[0] = 0;
		ghoul2[i].mValid = false;

		// load the ghoul2 info from the buffer
		ghoul2[i].sg_import(
			saved_game);

		if (ghoul2[i].mModelindex != -1 && ghoul2[i].mFileName[0])
		{
			ghoul2[i].mModelindex = i;

			::G2_SetupModelPointers(
				&ghoul2[i]);
		}

		// give us enough surfaces to load up the data
		int surface_count = 0;

		saved_game.read<int32_t>(
			surface_count);

		ghoul2[i].mSlist.resize(surface_count);

		// now load all the surfaces
		for (decltype(surface_count) x = 0; x < surface_count; ++x)
		{
			ghoul2[i].mSlist[x].sg_import(
				saved_game);
		}

		// give us enough bones to load up the data
		int bone_count = 0;

		saved_game.read<int32_t>(
			bone_count);

		ghoul2[i].mBlist.resize(
			bone_count);

		// now load all the bones
		for (decltype(bone_count) x = 0; x < bone_count; ++x)
		{
			ghoul2[i].mBlist[x].sg_import(
				saved_game);
		}

		// give us enough bolts to load up the data
		int bolt_count = 0;

		saved_game.read<int32_t>(
			bolt_count);

		ghoul2[i].mBltlist.resize(
			bolt_count);

		// now load all the bolts
		for (decltype(bolt_count) x = 0; x < bolt_count; ++x)
		{
			ghoul2[i].mBltlist[x].sg_import(
				saved_game);
		}
	}

	saved_game.ensure_all_data_read();
}