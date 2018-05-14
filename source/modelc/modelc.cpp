#include <cstdio>
#include <cstdlib>

#include "bx/allocator.h"
#include "bx/commandline.h"
#include "bx/math.h"
#include "bx/string.h"
#include "bx/file.h"
#include "bxx/array.h"
#include "bxx/path.h"
#include "bx/file.h"

#define BX_IMPLEMENT_JSON
#include "bxx/json.h"

#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "assimp/postprocess.h"

#include "../include_common/t3d_format.h"
#include "../include_common/coord_convert.h"
#include "../tools_common/log_format_proxy.h"

#define MODELC_VERSION "0.1"

using namespace tee;

static bx::DefaultAllocator gAlloc;
static LogFormatProxy* g_logger = nullptr;

struct Args
{
    bx::Path inFilepath;
    bx::Path outFilepath;
    bool verbose;
    bool buildTangents;
    float scale;
    ZAxis zaxis;
    bx::Path outputMtl;
    char modelName[32];

    Args()
    {
        verbose = false;
        buildTangents = false;
        scale = 1.0f;
        zaxis = ZAxis::Unknown;
        modelName[0] = 0;
    }
};

struct ModelData
{
    struct Geometry
    {
        t3dGeometry g;
        t3dJoint* joints;
        float* initPose;
        t3dVertexAttrib::Enum* attribs;
        int* attribOffsets;
        void* verts;
        uint16_t* indices;
    };

    struct Material
    {
        t3dMaterial m;
        t3dTexture* textures;
    };

    struct Mesh
    {
        t3dMesh m;
        t3dSubmesh* submeshes;
    };

    struct Node
    {
        t3dNode n;
        int* childs;
    };

    bx::Array<Geometry> geos;
    bx::Array<Material> mtls;
    bx::Array<Mesh> meshes;
    bx::Array<Node> nodes;

    ModelData()
    {
        geos.create(10, 10, &gAlloc);
        mtls.create(10, 20, &gAlloc);
        meshes.create(10, 10, &gAlloc);
        nodes.create(10, 10, &gAlloc);
    }

    ~ModelData()
    {
        for (int i = 0; i < geos.getCount(); i++) {
            const Geometry& geo = geos[i];
            if (geo.verts)
                BX_FREE(&gAlloc, geo.verts);
            if (geo.attribs)
                BX_FREE(&gAlloc, geo.attribs);
            if (geo.attribOffsets)
                BX_FREE(&gAlloc, geo.attribOffsets);
            if (geo.indices)
                BX_FREE(&gAlloc, geo.indices);
            if (geo.joints)
                BX_FREE(&gAlloc, geo.joints);
            if (geo.initPose)
                BX_FREE(&gAlloc, geo.initPose);
        }

        for (int i = 0; i < meshes.getCount(); i++) {
            const Mesh& m = meshes[i];
            if (m.submeshes)
                BX_FREE(&gAlloc, m.submeshes);
        }

        for (int i = 0; i < mtls.getCount(); i++) {
            const Material& mtl = mtls[i];
            if (mtl.textures)
                BX_FREE(&gAlloc, mtl.textures);
        }

        for (int i = 0; i < nodes.getCount(); i++) {
            const Node& node = nodes[i];
            if (node.childs)
                BX_FREE(&gAlloc, node.childs);
        }

        geos.destroy();
        mtls.destroy();
        meshes.destroy();
        nodes.destroy();
    }
};

static aiNode* findNodeRecursive(aiNode* anode, const char* name)
{
    if (bx::strCmpI(anode->mName.C_Str(), name) == 0)
        return anode;
    for (uint32_t i = 0; i < anode->mNumChildren; i++) {
        aiNode* achild = findNodeRecursive(anode->mChildren[i], name);
        if (achild)
            return achild;
    }
    return nullptr;
}

static void addBone(aiNode* abone, bx::Array<aiNode*>* bones)
{
    for (int i = 0; i < bones->getCount(); i++) {
        aiNode* refBone = (*bones)[i];
        if (refBone->mName == abone->mName)
            return;
    }

    *(bones->push()) = abone;
}

static aiBone* getGeoSkinBone(const bx::Array<aiBone*>& bones, const char* name)
{
    aiBone* bone = nullptr;
    for (int i = 0; i < bones.getCount(); i++) {
        if (strcmp(bones[i]->mName.C_Str(), name) == 0) {
            bone = bones[i];
            break;
        }
    }
    return bone;
}

static void gatherGeoSkinBones(aiBone** abones, uint32_t numBones, bx::Array<aiBone*>* skinBones)
{
    for (uint32_t i = 0; i < numBones; i++) {
        // Check for existing bone in the array
        if (!getGeoSkinBone(*skinBones, abones[i]->mName.C_Str()))
            *(skinBones->push()) = abones[i];
    }
}

static void gatherGeoBonesRecursive(aiNode* rootNode, const char *name, bx::Array<aiNode*>* bones)
{
    aiNode* anode = findNodeRecursive(rootNode, name);
    assert(anode);
    addBone(anode, bones);

    // Move-up to root_node and add all nodes (as bones)
    while (anode->mParent && anode->mParent != rootNode) {
        anode = anode->mParent;
        gatherGeoBonesRecursive(rootNode, anode->mName.C_Str(), bones);
    }
}

static void gatherGeoChildBonesRecursive(aiNode* rootNode, const char *name, bx::Array<aiNode*>* bones)
{
    aiNode *anode = findNodeRecursive(rootNode, name);
    assert(anode);
    addBone(anode, bones);

    // Add child bones recursively
    for (uint32_t i = 0; i < anode->mNumChildren; i++) {
        gatherGeoChildBonesRecursive(rootNode, anode->mChildren[i]->mName.C_Str(), bones);
    }
}

static int findGeoBoneIndex(const bx::Array<aiNode*>& bones, const char *name)
{
    for (int i = 0; i < bones.getCount(); i++) {
        aiNode *refbone = bones[i];
        if (strcmp(refbone->mName.C_Str(), name) == 0)
            return i;
    }
    return -1;
}

static void setupGeoJoints(const aiScene* scene, const bx::Array<aiNode*>& bones,
                           const bx::Array<aiBone*>& skinBones, const Args& conf,
                           const mat4_t& rootMtx, t3dJoint* joints, float* initPose)
{
    mat4_t offsetMtx;
    mat4_t scaleMtx;
    bx::mtxScale(scaleMtx.f, conf.scale, conf.scale, conf.scale);

    for (int i = 0; i < bones.getCount(); i++) {
        aiNode* bone = bones[i];
        bx::strCopy(joints[i].name, sizeof(joints[i].name), bone->mName.C_Str());

        // If joint name is found within SkinBones (aiBone), retreive offset matrix
        // Else use identity matrix
        aiBone* skinBone = getGeoSkinBone(skinBones, bone->mName.C_Str());
        if (skinBone)
            offsetMtx = convertMtx(skinBone->mOffsetMatrix, conf.zaxis);
        else
            offsetMtx = mat4I();

        saveMtx(offsetMtx, joints[i].offsetMtx);
        joints[i].parent = -1;

        // Resolve parent and Transformation of joint
        aiNode* ajointNode = findNodeRecursive(scene->mRootNode, bone->mName.C_Str());
        if (ajointNode) {
            joints[i].parent = ajointNode->mParent ?
                findGeoBoneIndex(bones, ajointNode->mParent->mName.C_Str()) : -1;
            mat4_t jointMtx = convertMtx(ajointNode->mTransformation, conf.zaxis);
            if (ajointNode->mParent == scene->mRootNode) {
                jointMtx = (jointMtx * rootMtx) * scaleMtx;
            }
            saveMtx(jointMtx, &initPose[i * 12]);
        }
    }
}

static int findAttrib(const t3dVertexAttrib::Enum* attribs, int numAttribs, t3dVertexAttrib::Enum elem)
{
    for (int i = 0; i < numAttribs; i++) {
        if (attribs[i] == elem)
            return i;
    }
    return -1;
}

static int importGeo(const aiScene* scene, ModelData* model, unsigned int* ameshIds,
                     uint32_t numMeshes, bool mainNode, t3dSubmesh* submeshes,
                     const Args& conf, const mat4_t& rootMtx)
{
    ModelData::Geometry* geo = model->geos.push();
    bx::memSet(geo, 0x00, sizeof(ModelData::Geometry));

    bx::Array<aiNode*> bones;
    bx::Array<aiBone*> skinBones;
    int numVerts = 0;
    int numTris = 0;
    bool skin = false;

    bones.create(100, 100, &gAlloc);
    skinBones.create(100, 100, &gAlloc);

    for (uint32_t i = 0; i < numMeshes; i++) {
        aiMesh* submesh = scene->mMeshes[ameshIds[i]];
        numVerts += submesh->mNumVertices;
        numTris += submesh->mNumFaces;

        gatherGeoSkinBones(submesh->mBones, submesh->mNumBones, &skinBones);
        for (uint32_t k = 0; k < submesh->mNumBones; k++)
            gatherGeoBonesRecursive(scene->mRootNode, submesh->mBones[k]->mName.C_Str(), &bones);
        for (uint32_t k = 0; k < submesh->mNumBones; k++)
            gatherGeoChildBonesRecursive(scene->mRootNode, submesh->mBones[k]->mName.C_Str(), &bones);

        if (submesh->mNumBones > 0)
            skin = true;
    }

    assert(numVerts != 0);
    assert(numTris != 0);

    geo->g.numTris = numTris;
    geo->g.numVerts = numVerts;

    if (numTris * 3 > UINT16_MAX) {
        bones.destroy();
        skinBones.destroy();
        g_logger->warn("Triangle count (%d) exceeds maximum %d", numTris, UINT16_MAX / 3);
        return -1;
    }

    t3dVertexAttrib::Enum attribs[t3dVertexAttrib::Count];
    int attribOffsets[t3dVertexAttrib::Count];
    int numAttribs = 0;
    int vertStride = 0;

    // Vertex elements
    for (uint32_t i = 0; i < numMeshes; i++) {
        aiMesh* asubmesh = scene->mMeshes[ameshIds[i]];

        if (findAttrib(attribs, numAttribs, t3dVertexAttrib::Position) == -1) {
            attribOffsets[numAttribs] = vertStride;
            vertStride += sizeof(float) * 3;
            attribs[numAttribs++] = t3dVertexAttrib::Position;
        }
        if (asubmesh->mNormals && findAttrib(attribs, numAttribs, t3dVertexAttrib::Normal) == -1) {
            attribOffsets[numAttribs] = vertStride;
            vertStride += sizeof(float) * 3;
            attribs[numAttribs++] = t3dVertexAttrib::Normal;
        }
        if (asubmesh->mColors[0] && findAttrib(attribs, numAttribs, t3dVertexAttrib::Color0) == -1) {
            attribOffsets[numAttribs] = vertStride;
            vertStride += sizeof(uint32_t);
            attribs[numAttribs++] = t3dVertexAttrib::Color0;
        }
        if (asubmesh->mTextureCoords[0] &&
            findAttrib(attribs, numAttribs, t3dVertexAttrib::TexCoord0) == -1) {
            attribOffsets[numAttribs] = vertStride;
            vertStride += sizeof(float) * 2;
            attribs[numAttribs++] = t3dVertexAttrib::TexCoord0;
        }
        if (asubmesh->mTextureCoords[1] &&
            findAttrib(attribs, numAttribs, t3dVertexAttrib::TexCoord1) == -1) {
            attribOffsets[numAttribs] = vertStride;
            vertStride += sizeof(float) * 2;
            attribs[numAttribs++] = t3dVertexAttrib::TexCoord1;
        }
        if (asubmesh->mTextureCoords[2] &&
            findAttrib(attribs, numAttribs, t3dVertexAttrib::TexCoord2) == -1) {
            attribOffsets[numAttribs] = vertStride;
            vertStride += sizeof(float) * 2;
            attribs[numAttribs++] = t3dVertexAttrib::TexCoord2;
        }
        if (asubmesh->mTextureCoords[3] &&
            findAttrib(attribs, numAttribs, t3dVertexAttrib::TexCoord3) == -1) {
            attribOffsets[numAttribs] = vertStride;
            vertStride += sizeof(float) * 2;
            attribs[numAttribs++] = t3dVertexAttrib::TexCoord3;
        }
        if (asubmesh->mTangents && findAttrib(attribs, numAttribs, t3dVertexAttrib::Tangent) == -1) {
            attribOffsets[numAttribs] = vertStride;
            vertStride += sizeof(float) * 3;
            attribs[numAttribs++] = t3dVertexAttrib::Tangent;
        }
        if (asubmesh->mBitangents && findAttrib(attribs, numAttribs, t3dVertexAttrib::Bitangent) == -1) {
            attribOffsets[numAttribs] = vertStride;
            vertStride += sizeof(float) * 3;
            attribs[numAttribs++] = t3dVertexAttrib::Bitangent;
        }
        if (asubmesh->mNumBones && findAttrib(attribs, numAttribs, t3dVertexAttrib::Indices) == -1) {
            attribOffsets[numAttribs] = vertStride;
            vertStride += sizeof(uint8_t) * 4;
            attribs[numAttribs++] = t3dVertexAttrib::Indices;
        }
        if (asubmesh->mNumBones && findAttrib(attribs, numAttribs, t3dVertexAttrib::Weight) == -1) {
            attribOffsets[numAttribs] = vertStride;
            vertStride += sizeof(float) * 4;
            attribs[numAttribs++] = t3dVertexAttrib::Weight;
        }
    }
    geo->g.numAttribs = numAttribs;
    geo->g.vertStride = vertStride;
    geo->attribs = (t3dVertexAttrib::Enum*)BX_ALLOC(&gAlloc, sizeof(t3dVertexAttrib::Enum)*numAttribs);
    geo->attribOffsets = (int*)BX_ALLOC(&gAlloc, sizeof(int)*numAttribs);
    assert(geo->attribs);
    memcpy(geo->attribs, attribs, sizeof(t3dVertexAttrib::Enum)*numAttribs);
    memcpy(geo->attribOffsets, attribOffsets, sizeof(int)*numAttribs);

    // Skeleton, and joints
    uint8_t* vertIwIndices = nullptr;   // Counters for skin indices (per vertex)
    if (findAttrib(attribs, numAttribs, t3dVertexAttrib::Indices) != -1) {
        vertIwIndices = (uint8_t*)alloca(sizeof(uint8_t)*numVerts);
        bx::memSet(vertIwIndices, 0x00, sizeof(uint8_t)*numVerts);

        // Setup joints
        geo->g.skel.numJoints = bones.getCount();
        geo->joints = (t3dJoint*)BX_ALLOC(&gAlloc, sizeof(t3dJoint)*bones.getCount());
        geo->initPose = (float*)BX_ALLOC(&gAlloc, sizeof(float)*12*bones.getCount());
        assert(geo->joints);
        assert(geo->initPose);

        mat4_t jointRoot = convertMtx(scene->mRootNode->mTransformation, conf.zaxis);
        setupGeoJoints(scene, bones, skinBones, conf, jointRoot, geo->joints, geo->initPose);

        saveMtx(jointRoot, geo->g.skel.rootMtx);
    }

    // Vertices and Indices
    geo->indices = (uint16_t*)BX_ALLOC(&gAlloc, numTris*sizeof(uint16_t) * 3);
    assert(geo->indices);

    int indexOffset = 0;
    int vertOffset = 0;

    geo->verts = BX_ALLOC(&gAlloc, numVerts*vertStride);
    assert(geo->verts);
    uint8_t* verts = (uint8_t*)geo->verts;

    // Some lambda helpers
    auto saveVec2 = [](const aiVector3D& v, float* vf) { vf[0] = v.x;  vf[1] = v.y; };

    for (uint32_t i = 0; i < numMeshes; i++) {
        aiMesh* submesh = scene->mMeshes[ameshIds[i]];

        // Indices
        uint16_t* indices = geo->indices;
        for (uint32_t k = 0; k < submesh->mNumFaces; k++) {
            if (submesh->mFaces[k].mNumIndices != 3)
                continue;

            uint32_t idx = 3 * k + indexOffset;

            indices[idx] = (uint16_t)(submesh->mFaces[k].mIndices[0] + vertOffset);
            indices[idx + 1] = (uint16_t)(submesh->mFaces[k].mIndices[1] + vertOffset);
            indices[idx + 2] = (uint16_t)(submesh->mFaces[k].mIndices[2] + vertOffset);
        }

        // Fill subset data
        submeshes[i].startIndex = indexOffset;
        submeshes[i].numIndices = submesh->mNumFaces * 3;

        // Vertices
        // Transform vertices by rootMtx if it's not skinned and it's the main node
        // Skinned meshes are transformed by their joints
        // child nodes are transformed by parent (main), so they don't need to be xformed by root
        mat4_t vertMtx;
        if (!skin && mainNode)
            vertMtx = rootMtx;
        else
            vertMtx = mat4I();

        for (uint32_t k = 0; k < submesh->mNumVertices; k++) {
            uint32_t idx = k + vertOffset;
            if (submesh->mVertices) {
                int attribOffset = attribOffsets[findAttrib(attribs, numAttribs, t3dVertexAttrib::Position)];
                float* p = (float*)(verts + vertStride*idx + attribOffset);
                bx::vec3MulMtx(p, convertVec3(submesh->mVertices[k], conf.zaxis).f, vertMtx.f);
            }
            if (submesh->mNormals) {
                int attribOffset = attribOffsets[findAttrib(attribs, numAttribs, t3dVertexAttrib::Normal)];
                float* n = (float*)(verts + vertStride*idx + attribOffset);
                bx::vec3MulMtxRot(n, convertVec3(submesh->mNormals[k], conf.zaxis).f, vertMtx.f);
            }
            if (submesh->mTextureCoords[0]) {
                int attribOffset = attribOffsets[findAttrib(attribs, numAttribs, t3dVertexAttrib::TexCoord0)];
                float* t = (float*)(verts + vertStride*idx + attribOffset);
                saveVec2(submesh->mTextureCoords[0][k], t);
            }
            if (submesh->mTextureCoords[1]) {
                int attribOffset = attribOffsets[findAttrib(attribs, numAttribs, t3dVertexAttrib::TexCoord1)];
                float* t = (float*)(verts + vertStride*idx + attribOffset);
                saveVec2(submesh->mTextureCoords[1][k], t);
            }
            if (submesh->mTextureCoords[2]) {
                int attribOffset = attribOffsets[findAttrib(attribs, numAttribs, t3dVertexAttrib::TexCoord2)];
                float* t = (float*)(verts + vertStride*idx + attribOffset);
                saveVec2(submesh->mTextureCoords[2][k], t);
            }
            if (submesh->mTextureCoords[3]) {
                int attribOffset = attribOffsets[findAttrib(attribs, numAttribs, t3dVertexAttrib::TexCoord3)];
                float* t = (float*)(verts + vertStride*idx + attribOffset);
                saveVec2(submesh->mTextureCoords[3][k], t);
            }
            if (submesh->mTangents) {
                int attribOffset = attribOffsets[findAttrib(attribs, numAttribs, t3dVertexAttrib::Tangent)];
                float* t = (float*)(verts + vertStride*idx + attribOffset);
                bx::vec3MulMtxRot(t, convertVec3(submesh->mTangents[k], conf.zaxis).f, vertMtx.f);
            }
            if (submesh->mBitangents) {
                int attribOffset = attribOffsets[findAttrib(attribs, numAttribs, t3dVertexAttrib::Bitangent)];
                float* t = (float*)(verts + vertStride*idx + attribOffset);
                bx::vec3MulMtxRot(t, convertVec3(submesh->mBitangents[k], conf.zaxis).f, vertMtx.f);
            }

            if (submesh->mColors[0]) {
                int attribOffset = attribOffsets[findAttrib(attribs, numAttribs, t3dVertexAttrib::Color0)];
                uint32_t* c = (uint32_t*)(verts + vertStride*idx + attribOffset);
                aiColor4D mc = submesh->mColors[0][k];
                *c = (uint32_t(mc.r*255.0f) << 24) |
                     (uint32_t(mc.g*255.0f) << 16) |
                     (uint32_t(mc.b*255.0f) << 8) |
                     (uint32_t(mc.a*255.0f));
            }
        }

        // Skin Data (BLEND_INDEXES, BLEND_WEIGHTS)
        if (submesh->mNumBones) {
            for (uint32_t k = 0; k < submesh->mNumBones; k++) {
                aiBone* bone = submesh->mBones[k];
                int boneIndex = findGeoBoneIndex(bones, bone->mName.C_Str());
                assert(boneIndex != -1);
                assert(boneIndex < UINT8_MAX);

                for (uint32_t c = 0; c < bone->mNumWeights; c++) {
                    int idx = (int)bone->mWeights[c].mVertexId + vertOffset;
                    int attribIndices = attribOffsets[findAttrib(attribs, numAttribs, t3dVertexAttrib::Indices)];
                    int attribWeight = attribOffsets[findAttrib(attribs, numAttribs, t3dVertexAttrib::Weight)];
                    uint8_t* indices = (uint8_t*)(verts + vertStride*idx + attribIndices);
                    float* weights = (float*)(verts + vertStride*idx + attribWeight);
                    indices[vertIwIndices[idx]] = uint8_t(boneIndex);
                    weights[vertIwIndices[idx]] = bone->mWeights[c].mWeight;
                    vertIwIndices[idx]++;
                }
            }
        }

        // progress offsets
        indexOffset += submesh->mNumFaces * 3;
        vertOffset += submesh->mNumVertices;
    }

    bones.destroy();
    skinBones.destroy();

    return model->geos.getCount() - 1;
}

static int importMaterial(const aiScene* scene, ModelData* model, aiMaterial* amtl)
{
    ModelData::Material* mtl = model->mtls.push();
    bx::memSet(mtl, 0x00, sizeof(ModelData::Material));

    auto setColor = [](const aiColor4D &c, float *fv) {fv[0] = c.r; fv[1] = c.g; fv[2] = c.b; };
    auto setColor1 = [](float c, float *fv) {fv[0] = c; fv[1] = c; fv[2] = c; };
    // Colors
    aiColor4D clr;
    if (amtl->Get(AI_MATKEY_COLOR_AMBIENT, clr) == AI_SUCCESS)
        setColor(clr, mtl->m.ambient);
    else
        setColor1(1.0f, mtl->m.ambient);

    if (amtl->Get(AI_MATKEY_COLOR_DIFFUSE, clr) == AI_SUCCESS)
        setColor(clr, mtl->m.diffuse);
    else
        setColor1(1.0f, mtl->m.diffuse);

    if (amtl->Get(AI_MATKEY_COLOR_SPECULAR, clr) == AI_SUCCESS)
        setColor(clr, mtl->m.specular);
    else
        setColor1(1.0f, mtl->m.specular);

    if (amtl->Get(AI_MATKEY_COLOR_EMISSIVE, clr) == AI_SUCCESS)
        setColor(clr, mtl->m.emissive);
    else
        setColor1(0.0f, mtl->m.emissive);

    // Normalize specular-exponent to 0-1 (do not saturate)
    float specExp;
    if (amtl->Get(AI_MATKEY_SHININESS, specExp) == AI_SUCCESS)
        mtl->m.specExp = specExp / 100.0f;
    else
        mtl->m.specExp = 0.5f;

    if (amtl->Get(AI_MATKEY_SHININESS_STRENGTH, mtl->m.specIntensity) != AI_SUCCESS)
        mtl->m.specIntensity = 1.0f;

    // Transparency
    float opacity;
    if (amtl->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS)
        mtl->m.opacity = opacity;
    else
        mtl->m.opacity = 1.0f;

    // Textures
    t3dTexture textures[10];
    int tcount = 0;
    aiString filepath;

    if (amtl->Get(AI_MATKEY_TEXTURE_DIFFUSE(0), filepath) == AI_SUCCESS) {
        bx::strCopy(textures[tcount].filepath, sizeof(textures[tcount].filepath), filepath.C_Str());
        textures[tcount++].usage = t3dTextureUsage::Diffuse;
    }
    if (amtl->Get(AI_MATKEY_TEXTURE_SHININESS(0), filepath) == AI_SUCCESS) {
        bx::strCopy(textures[tcount].filepath, sizeof(textures[tcount].filepath), filepath.C_Str());
        textures[tcount++].usage = t3dTextureUsage::Gloss;
    }
    if (amtl->Get(AI_MATKEY_TEXTURE_NORMALS(0), filepath) == AI_SUCCESS) {
        bx::strCopy(textures[tcount].filepath, sizeof(textures[tcount].filepath), filepath.C_Str());
        textures[tcount++].usage = t3dTextureUsage::Normal;
    }
    if (amtl->Get(AI_MATKEY_TEXTURE_OPACITY(0), filepath) == AI_SUCCESS) {
        bx::strCopy(textures[tcount].filepath, sizeof(textures[tcount].filepath), filepath.C_Str());
        textures[tcount++].usage = t3dTextureUsage::Alpha;
    }
    if (amtl->Get(AI_MATKEY_TEXTURE_LIGHTMAP(0), filepath) == AI_SUCCESS) {
        bx::strCopy(textures[tcount].filepath, sizeof(textures[tcount].filepath), filepath.C_Str());
        textures[tcount++].usage = t3dTextureUsage::Light;
    }
    if (amtl->Get(AI_MATKEY_TEXTURE_REFLECTION(0), filepath) == AI_SUCCESS) {
        bx::strCopy(textures[tcount].filepath, sizeof(textures[tcount].filepath), filepath.C_Str());
        textures[tcount++].usage = t3dTextureUsage::Reflection;
    }
    if (amtl->Get(AI_MATKEY_TEXTURE_EMISSIVE(0), filepath) == AI_SUCCESS) {
        bx::strCopy(textures[tcount].filepath, sizeof(textures[tcount].filepath), filepath.C_Str());
        textures[tcount++].usage = t3dTextureUsage::Emissive;
    }
    if (amtl->Get(AI_MATKEY_TEXTURE_AMBIENT(0), filepath) == AI_SUCCESS) {
        bx::strCopy(textures[tcount].filepath, sizeof(textures[tcount].filepath), filepath.C_Str());
        textures[tcount++].usage = t3dTextureUsage::AO;
    }
    if (amtl->Get(AI_MATKEY_TEXTURE_SPECULAR(0), filepath) == AI_SUCCESS) {
        bx::strCopy(textures[tcount].filepath, sizeof(textures[tcount].filepath), filepath.C_Str());
        textures[tcount++].usage = t3dTextureUsage::Specular;
    }

    if (tcount) {
        mtl->m.numTextures = tcount;
        mtl->textures = (t3dTexture*)BX_ALLOC(&gAlloc, sizeof(t3dTexture)*tcount);
        memcpy(mtl->textures, textures, sizeof(t3dTexture)*tcount);
    }

    return model->mtls.getCount() - 1;
}

static int importMesh(const aiScene* scene, ModelData* model, unsigned int* ameshIds,
                      unsigned int numMeshes, bool mainNode, const Args& conf,
                      const mat4_t& rootMtx)
{
    ModelData::Mesh* mesh = model->meshes.push();
    bx::memSet(mesh, 0x00, sizeof(ModelData::Mesh));

    mesh->m.numSubmeshes = (int)numMeshes;
    mesh->submeshes = (t3dSubmesh*)BX_ALLOC(&gAlloc, sizeof(t3dSubmesh)*numMeshes);
    assert(mesh->submeshes);
    bx::memSet(mesh->submeshes, 0x00, sizeof(t3dSubmesh)*numMeshes);

    // Geomerty (with some data of submeshes)
    int geo = importGeo(scene, model, ameshIds, numMeshes, mainNode, mesh->submeshes, conf, rootMtx);
    if (geo == -1)
        return -1;
    mesh->m.geo = geo;

    // Materials
    for (uint32_t i = 0; i < numMeshes; i++) {
        aiMesh *asubmesh = scene->mMeshes[ameshIds[i]];
        mesh->submeshes[i].mtl = importMaterial(scene, model, scene->mMaterials[asubmesh->mMaterialIndex]);
    }

    return model->meshes.getCount() - 1;
}

static aabb_t calcGeoBoundsNoSkin(const ModelData::Geometry& geo)
{
    aabb_t bb = aabbZero();
    for (int i = 0; i < geo.g.numVerts; i++) {
        const float* poss = (const float*)((uint8_t*)geo.verts + i*geo.g.vertStride);
        tmath::aabbPushPoint(&bb, vec3(poss[0], poss[1], poss[2]));
    }
    return bb;
}

static aabb_t calcGeoBoundsSkin(const ModelData::Geometry& geo)
{
    aabb_t bb = aabbZero();
    int numJoints = geo.g.skel.numJoints;

    mat4_t* skinMtxs = (mat4_t*)alloca(sizeof(mat4_t)*numJoints);

    mat4_t* initPose = (mat4_t*)alloca(sizeof(mat4_t)*numJoints);
    memcpy(initPose, geo.initPose, sizeof(mat4_t)*numJoints);

    for (int i = 0; i < numJoints; i++) {
        const t3dJoint* joint = &geo.joints[i];
        skinMtxs[i] = initPose[i];

        while (joint->parent != -1) {
            skinMtxs[i] = skinMtxs[i] * initPose[joint->parent];
            joint = &geo.joints[joint->parent];
        }

        // Offset matrix
        joint = &geo.joints[i];
        const float* off = joint->offsetMtx;
        mat4_t offsetMtx = mat4(off[0], off[1], off[2],
                                      off[3], off[4], off[5],
                                      off[6], off[7], off[8],
                                      off[9], off[10], off[11]);

        // Final skin mtx
        skinMtxs[i] = offsetMtx * skinMtxs[i];
    }

    // Apply skinning to each vertex and add it to aabb
    for (int i = 0; i < geo.g.numVerts; i++) {
        const float* poss = (const float*)((uint8_t*)geo.verts + i*geo.g.vertStride);
        int indicesOffset = geo.attribOffsets[findAttrib(geo.attribs, geo.g.numAttribs, t3dVertexAttrib::Indices)];
        int weightOffset = geo.attribOffsets[findAttrib(geo.attribs, geo.g.numAttribs, t3dVertexAttrib::Weight)];
        const int* indices = (const int*)((uint8_t*)geo.verts + i*geo.g.vertStride + indicesOffset);
        const float* weights = (const float*)((uint8_t*)geo.verts + i*geo.g.vertStride + weightOffset);

        vec3_t pos = vec3(poss[0], poss[1], poss[2]);
        vec3_t skinned = vec3(0, 0, 0);

        for (int c = 0; c < 4; c++) {
            const mat4_t& mtx = skinMtxs[indices[c]];
            float w = weights[c];

            vec3_t sp;
            bx::vec3MulMtx(sp.f, pos.f, mtx.f);
            bx::vec3Mul(sp.f, sp.f, w);
            bx::vec3Add(skinned.f, skinned.f, sp.f);

            tmath::aabbPushPoint(&bb, skinned);
        }
    }

    return bb;
}

static int importNodeRecursive(const aiScene* scene, aiNode* anode, ModelData* model, const Args& conf, int parent, 
                               mat4_t& rootMtx)
{
    ModelData::Node* node = model->nodes.push();
    bx::memSet(node, 0x00, sizeof(ModelData::Node));

    bx::strCopy(node->n.name, sizeof(node->n.name), anode->mName.C_Str());
    node->n.parent = parent;

    // Calculate local transform mat of the node
    mat4_t localMtx;
    if (parent == -1) {
        // Calculate rootMtx
        mat4_t resizeMtx;     
        bx::mtxScale(resizeMtx.f, conf.scale, conf.scale, conf.scale);

        mat4_t sceneRoot = convertMtx(scene->mRootNode->mTransformation, conf.zaxis);
        rootMtx = convertMtx(anode->mTransformation, conf.zaxis);
        rootMtx = (rootMtx * sceneRoot) * resizeMtx;

        // Assign rootMtx to localMtx, because we are on the first node
        localMtx = rootMtx;
    } else {
        // For childs nodes, only the first level child nodes are transformed by rootMtx
        localMtx = convertMtx(anode->mTransformation, conf.zaxis);
        if (model->nodes.itemPtr(parent)->n.parent == -1) {
            localMtx = localMtx * rootMtx;
        }
    }

    saveMtx(localMtx, node->n.xformMtx);

    // Import meshes (Geo/Material) and calculate bounding box
    aabb_t bb;
    if (anode->mNumMeshes > 0) {
        node->n.mesh = importMesh(scene, model, anode->mMeshes, anode->mNumMeshes, parent == -1, conf, rootMtx);
        if (node->n.mesh == -1) {
            g_logger->fatal("Import node '%s' failed", anode->mName.C_Str());
            return -1;
        }

        // Calculate bounds (skinned or non-skinned) for the mesh
        const ModelData::Geometry& geo = model->geos[model->meshes[node->n.mesh].m.geo];
        if (geo.g.skel.numJoints)
            bb = calcGeoBoundsSkin(geo);
        else
            bb = calcGeoBoundsNoSkin(geo);
    } else {
        node->n.mesh = -1;
    }

    node->n.aabbMin[0] = bb.vmin.x;  node->n.aabbMin[1] = bb.vmin.y;  node->n.aabbMin[2] = bb.vmin.z;
    node->n.aabbMax[0] = bb.vmax.x;  node->n.aabbMax[1] = bb.vmax.y;  node->n.aabbMax[2] = bb.vmax.z;

    int myidx = model->nodes.getCount() - 1;

    // Recurse for child nodes
    if (anode->mNumChildren) {
        node->n.numChilds = anode->mNumChildren;
        node->childs = (int*)BX_ALLOC(&gAlloc, sizeof(int)*anode->mNumChildren);
        for (int i = 0; i < node->n.numChilds; i++) {
            node->childs[i] = importNodeRecursive(scene, anode->mChildren[i], model, conf, myidx, rootMtx);
            if (node->childs[i] == -1)
                return -1;
        }
    }

    return myidx;
}

static bool exportT3d(const char* t3dFilepath, const ModelData& model)
{
    t3dHeader hdr;
    hdr.sign = T3D_SIGN;
    hdr.version = T3D_VERSION_10;

    hdr.numNodes = model.nodes.getCount();
    hdr.numGeos = model.geos.getCount();
    hdr.numMeshes = model.meshes.getCount();

    bx::FileWriter file;
    bx::Error err;
    if (!file.open(t3dFilepath, false, &err)) {
        g_logger->fatal("Could not open file '%s' for writing", t3dFilepath);
        return false;
    }

    // skip header, we will fill it later
    file.write(&hdr, sizeof(hdr), &err);

    // Nodes
    for (int i = 0; i < model.nodes.getCount(); i++) {
        const ModelData::Node& node = model.nodes[i];
        file.write(&node.n, sizeof(node.n), &err);
        if (node.n.numChilds)
            file.write(node.childs, sizeof(int)*node.n.numChilds, &err);
    }

    // Meshes
    for (int i = 0; i < model.meshes.getCount(); i++) {
        const ModelData::Mesh& mesh = model.meshes[i];
        file.write(&mesh.m, sizeof(mesh.m), &err);
        file.write(mesh.submeshes, sizeof(t3dSubmesh)*mesh.m.numSubmeshes, &err);
    }

    // Geos
    size_t s = sizeof(t3dVertexAttrib::Enum);
    for (int i = 0; i < model.geos.getCount(); i++) {
        const ModelData::Geometry& geo = model.geos[i];
        file.write(&geo.g, sizeof(geo.g), &err);

        if (geo.joints)
            file.write(geo.joints, sizeof(t3dJoint)*geo.g.skel.numJoints, &err);
        if (geo.initPose)
            file.write(geo.initPose, sizeof(float)*12*geo.g.skel.numJoints, &err);
        if (geo.attribs)
            file.write(geo.attribs, sizeof(t3dVertexAttrib::Enum)*geo.g.numAttribs, &err);
        if (geo.indices)
            file.write(geo.indices, sizeof(uint16_t)*geo.g.numTris*3, &err);
        if (geo.verts)
            file.write(geo.verts, geo.g.vertStride*geo.g.numVerts, &err);
    }

    // Materials block (Meta-data)
    hdr.metaOffset = (int)file.seek();
    t3dMetablock metaMtl;
    strcpy(metaMtl.name, "Materials");
    metaMtl.stride = -1;
    file.write(&metaMtl, sizeof(metaMtl), &err);

    // Materials
    int numMtls = model.mtls.getCount();
    file.write(&numMtls, sizeof(numMtls), &err);
    for (int i = 0; i < model.mtls.getCount(); i++) {
        const ModelData::Material& mtl = model.mtls[i];
        file.write(&mtl.m, sizeof(mtl.m), &err);
        file.write(&mtl.textures, sizeof(t3dTexture)*mtl.m.numTextures, &err);
    }

    // Rewrite header (updated meta offset)
    file.seek(0, bx::Whence::Begin);
    file.write(&hdr, sizeof(hdr), &err);

    file.close();
    return true;
}

static bx::JsonNode* jsonCreateVec3(bx::AllocatorI* alloc, const char* name, const float v[3])
{
    bx::JsonNode* jv = bx::createJsonNode(alloc, name, bx::JsonType::Array);
    jv->addChild(bx::createJsonNode(alloc)->setFloat(v[0])).
        addChild(bx::createJsonNode(alloc)->setFloat(v[1])).
        addChild(bx::createJsonNode(alloc)->setFloat(v[2]));
    return jv;
}

static bool exportMeta(const char* metaJsonFilepath, const ModelData& model)
{
    bx::JsonNodeAllocator alloc(&gAlloc);
    bx::JsonNode* jroot = bx::createJsonNode(&alloc, nullptr, bx::JsonType::Object);
    bx::JsonNode* jmtls = bx::createJsonNode(&alloc, "materials", bx::JsonType::Array);
    jroot->addChild(jmtls);

    for (int i = 0; i < model.mtls.getCount(); i++) {
        const ModelData::Material& mtl = model.mtls[i];
        bx::JsonNode* jmtl = bx::createJsonNode(&alloc, nullptr, bx::JsonType::Object);
        jmtls->addChild(jmtl);

        jmtl->addChild(jsonCreateVec3(&alloc, "ambient", mtl.m.ambient));
        jmtl->addChild(jsonCreateVec3(&alloc, "diffuse", mtl.m.diffuse));
        jmtl->addChild(jsonCreateVec3(&alloc, "specular", mtl.m.specular));
        jmtl->addChild(jsonCreateVec3(&alloc, "emissive", mtl.m.emissive));
        jmtl->addChild(bx::createJsonNode(&alloc, "specular_exp")->setFloat(mtl.m.specExp));
        jmtl->addChild(bx::createJsonNode(&alloc, "specular_intensity")->setFloat(mtl.m.specIntensity));
        jmtl->addChild(bx::createJsonNode(&alloc, "opacity")->setFloat(mtl.m.opacity));

        // Textures
        for (int c = 0; c < mtl.m.numTextures; c++) {
            const t3dTexture& texture = mtl.textures[c];
            const char* name;
            switch (texture.usage) {
            case t3dTextureUsage::Diffuse:      name = "diffuse_map";    break;
            case t3dTextureUsage::AO:           name = "ao_map";         break;
            case t3dTextureUsage::Light:        name = "light_map";      break;
            case t3dTextureUsage::Normal:       name = "normal_map";     break;
            case t3dTextureUsage::Specular:     name = "specular_map";   break;
            case t3dTextureUsage::Emissive:     name = "emissive_map";   break;
            case t3dTextureUsage::Gloss:        name = "gloss_map";      break;
            case t3dTextureUsage::Reflection:   name = "reflection_map"; break;
            case t3dTextureUsage::Alpha:        name = "alpha_map";      break;
            }

            jmtl->addChild(bx::createJsonNode(&alloc, name)->setString(texture.filepath));
        }
    }

    // Save/Show
    if (metaJsonFilepath[0]) {
        char* jmeta = bx::makeJson(jroot, &gAlloc, false);
        if (!jmeta)
            return false;
        bx::FileWriter file;
        bx::Error err;
        if (!file.open(metaJsonFilepath, false, &err))
            return false;
        file.write(jmeta, (int)strlen(jmeta) + 1, &err);
        file.close();
        BX_FREE(&gAlloc, jmeta);
    } else {
        char* jmeta = bx::makeJson(jroot, &gAlloc, false);
        if (!jmeta)
            return false;
        puts(jmeta);
        BX_FREE(&gAlloc, jmeta);
    }

    jroot->destroy();
    return true;
}

static bool importModel(const Args& conf)
{
    Assimp::Importer importer;

    uint32_t flags =
        aiProcess_JoinIdenticalVertices |
        aiProcess_Triangulate |
        aiProcess_ImproveCacheLocality |
        aiProcess_LimitBoneWeights |
        aiProcess_OptimizeMeshes |
        aiProcess_RemoveRedundantMaterials |
        aiProcess_ValidateDataStructure |
        aiProcess_GenUVCoords |
        aiProcess_TransformUVCoords |
        aiProcess_FlipUVs |
        aiProcess_SortByPType |
        aiProcess_FindDegenerates;

    if (conf.buildTangents) {
        flags |= aiProcess_CalcTangentSpace | aiProcess_RemoveComponent;
        importer.SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS, aiComponent_TANGENTS_AND_BITANGENTS);
    }

    if (conf.zaxis == ZAxis::Unknown)
        flags |= aiProcess_MakeLeftHanded;

    // Load scene
    const aiScene* scene = importer.ReadFile(conf.inFilepath.cstr(), flags);
    if (scene == nullptr) {
        g_logger->fatal("Loading '%s' failed: %s", conf.inFilepath.cstr(), importer.GetErrorString());
        return false;
    }

    // Find node in scene
    aiNode* anode = findNodeRecursive(scene->mRootNode, conf.modelName);
    if (anode == nullptr) {
        g_logger->fatal("Model '%s' does not exist in the file", conf.modelName);
        return false;
    }

    // Import model (Geometry, meshes, materials)
    ModelData model;
    mat4_t rootMtx = mat4I();
    if (importNodeRecursive(scene, anode, &model, conf, -1, rootMtx) == -1) {
        g_logger->fatal("Model import '%s' failed", conf.modelName);
        return false;
    }

    // Write to file
    if (!exportT3d(conf.outFilepath.cstr(), model)) {
        g_logger->fatal("Writing to file '%s' failed", conf.outFilepath.cstr());
        return false;
    }

    // Output material props (stdout or JSON file)
    if (!conf.outputMtl.isEmpty()) {
        if (!exportMeta(conf.outputMtl.cstr(), model)) {
            g_logger->fatal("Exporting JSON meta data failed");
            return false;
        }
    }

    return true;
}

static void showHelp()
{
    const char* help =
        "modelc v" MODELC_VERSION " - Model compiler for T3D file format\n"
        "Arguments:\n"
        "  -i --input <filepath> Input model file (*.dae, *.fbx, *.obj, etc.)\n"
        "  -o --output <filepath> Output T3D file\n"
        "  -v --verbose Verbose mode\n"
        "  -T --maketangents Calculate tangents\n"
        "  -n --name <name> Model name inside the source file\n"
        "  -s --scale <scale> Set scale multiplier (default=1)\n"
        "  -z --zaxis <zaxis> Set Z-Axis, choises are ['UP', 'GL']\n"
        "  -M --metafile <filepath> Output meta data to a file instead of stdout\n"
        "  -j --jsonlog Enable json logging instead of normal text\n";
    puts(help);
}

int main(int argc, char** argv)
{
    // Read arguments (config)
    Args conf;
    bx::CommandLine cmd(argc, argv);
    conf.verbose = cmd.hasArg('v', "verbose");
    conf.buildTangents = cmd.hasArg('T', "maketangents");
    
    const char* scaleStr = cmd.findOption('s', "scale", "1.0");
    sscanf(scaleStr, "%f", &conf.scale);

    const char* zaxis = cmd.findOption('z', "zaxis", "");
    if (bx::strCmpI(zaxis, "UP") == 0)
        conf.zaxis = ZAxis::Up;
    else if (bx::strCmpI(zaxis, "GL") == 0)
        conf.zaxis = ZAxis::GL;
    else
        conf.zaxis = ZAxis::Unknown;

    conf.outputMtl = cmd.findOption('M', "metafile", "");
    conf.inFilepath = cmd.findOption('i', "input", "");
    conf.outFilepath = cmd.findOption('o', "output", "");
    bx::strCopy(conf.modelName, sizeof(conf.modelName), cmd.findOption('n', "name", ""));
    bool jsonLog = cmd.hasArg('j', "jsonlog");

    bool help = cmd.hasArg('h', "help");
    if (help) {
        showHelp();
        return 0;
    }        

    LogFormatProxy logger(jsonLog ? LogProxyOptions::Json : LogProxyOptions::Text);
    g_logger = &logger;

    // Argument check
    if (conf.inFilepath.isEmpty() || conf.outFilepath.isEmpty()) {
        g_logger->fatal("Invalid arguments");
        return -1;
    }   

    bx::FileInfo finfo;
    if (!bx::stat(conf.inFilepath.cstr(), finfo) || finfo.m_type != bx::FileInfo::Regular) {
        g_logger->fatal("File '%s' is invalid", conf.inFilepath.cstr());
        return -1;
    }        

    return importModel(conf) ? 0 : -1;
}
