#include <iostream>
#include <string>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cfloat>
#include <cassert>
#include <map>
#include <algorithm>
#include <functional>
#include <cstring>
#include <unordered_map>
#include <filesystem>
#include <system_error>

#include <fbxsdk.h>

using namespace std;

static constexpr double EXPORT_SCALE_D = 0.01;
static constexpr float  EXPORT_SCALE_F = 0.01f;
static constexpr bool MIRROR_X_EXPORT = true;

#define DEBUGLOG 1

#if DEBUGLOG
#define DLOG(x)   do { std::cout << x; } while(0)
#define DLOGLN(x) do { std::cout << x << "\n"; } while(0)
#else
#define DLOG(x)   do {} while(0)
#define DLOGLN(x) do {} while(0)
#endif


// ==========================================================
// 전역 저장 데이터 (FBX 파싱 후 여기에 채움)
// ==========================================================

struct Bone {
    string name;
    int32_t parentIndex;
    float bindLocal[16];
    float offsetMatrix[16];
};

struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    uint32_t boneIndices[4];
    float boneWeights[4];
};

struct Material {
    string name;               // material 이름 (ex: "face")
    string diffuseTextureName; // texture 파일명 base (ex: "face")
};

struct SubMesh {
    string meshName;
    uint32_t materialIndex;   // g_Materials 인덱스
    vector<Vertex> vertices;
    vector<uint32_t> indices;
};

vector<Bone> g_Bones;
vector<SubMesh> g_SubMeshes;
unordered_map<string, int> g_BoneNameToIndex;
unordered_map<string, FbxNode*> g_BoneNameToNode;

vector<Material> g_Materials;
unordered_map<string, uint32_t> g_MaterialNameToIndex;

// ==========================================================
// [수정] u8path() 제거 (C++20 deprecation 대응)
// - u8path(fn) 대신: std::u8string(UTF-8 바이트) -> path 생성
// ==========================================================
static std::string SafeStemFromFbxFileName(const char* fn)
{
    if (!fn || !fn[0]) return "";

    try
    {
        std::string s(fn);
        std::u8string u8(reinterpret_cast<const char8_t*>(s.data()), s.size());
        std::filesystem::path p(u8);

        std::u8string stem_u8 = p.stem().u8string();
        return std::string(reinterpret_cast<const char*>(stem_u8.data()), stem_u8.size());
    }
    catch (...)
    {
        std::string s(fn);

        size_t pos = s.find_last_of("/\\");
        std::string base = (pos == std::string::npos) ? s : s.substr(pos + 1);

        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base = base.substr(0, dot);

        return base;
    }
}

// ==========================================================
// 파일 출력 스트림 (전역)
// ==========================================================
static std::ofstream g_out;

// ==========================================================
// Raw write helpers
// ==========================================================
static void WriteRaw(const void* data, size_t size)
{
    g_out.write(reinterpret_cast<const char*>(data), size);
}

static void WriteUInt16(uint16_t v) { WriteRaw(&v, sizeof(v)); }
static void WriteUInt32(uint32_t v) { WriteRaw(&v, sizeof(v)); }
static void WriteInt32(int32_t v) { WriteRaw(&v, sizeof(v)); }

static void WriteFloatArray(const float* f, size_t count)
{
    WriteRaw(f, sizeof(float) * count);
}

static void WriteStringUtf8(const std::string& s)
{
    uint16_t len = static_cast<uint16_t>(s.size());
    WriteUInt16(len);
    if (len > 0)
        WriteRaw(s.data(), len);
}

static void WriteMaterialSection()
{
    for (auto& m : g_Materials)
    {
        WriteStringUtf8(m.name);
        WriteStringUtf8(m.diffuseTextureName);
    }
}

// ==========================================================
// 1) 파일 헤더 저장
// ==========================================================
static void WriteModelHeader()
{
    char magic[4] = { 'M', 'B', 'I', 'N' };
    WriteRaw(magic, 4);

    uint32_t version = 1;
    uint32_t flags = 0;
    uint32_t boneCount = (uint32_t)g_Bones.size();
    uint32_t materialCount = (uint32_t)g_Materials.size();
    uint32_t subCount = (uint32_t)g_SubMeshes.size();

    WriteUInt32(version);
    WriteUInt32(flags);
    WriteUInt32(boneCount);
    WriteUInt32(materialCount);
    WriteUInt32(subCount);
}

// ==========================================================
// 2) Skeleton 섹션 저장
// ==========================================================
static void WriteSkeletonSection()
{
    for (auto& b : g_Bones)
    {
        WriteStringUtf8(b.name);
        WriteInt32(b.parentIndex);
        WriteFloatArray(b.bindLocal, 16);
        WriteFloatArray(b.offsetMatrix, 16);
    }
}

// ==========================================================
// 3) SubMesh 섹션 저장
// ==========================================================
static void WriteSubMeshSection()
{
    for (auto& sm : g_SubMeshes)
    {
        WriteStringUtf8(sm.meshName);
        WriteUInt32(sm.materialIndex);

        uint32_t vtxCount = (uint32_t)sm.vertices.size();
        uint32_t idxCount = (uint32_t)sm.indices.size();
        WriteUInt32(vtxCount);
        WriteUInt32(idxCount);

        for (const Vertex& v : sm.vertices)
        {
            WriteFloatArray(v.position, 3);
            WriteFloatArray(v.normal, 3);
            WriteFloatArray(v.uv, 2);
            WriteRaw(v.boneIndices, sizeof(uint32_t) * 4);
            WriteFloatArray(v.boneWeights, 4);
        }

        if (idxCount > 0)
            WriteRaw(sm.indices.data(), sizeof(uint32_t) * idxCount);
    }
}

// ==========================================================
// BIN 파일 저장 함수
// ==========================================================
static bool SaveModelBin(const std::string& filename)
{
    g_out.open(filename, ios::binary);
    if (!g_out.is_open()) return false;

    WriteModelHeader();
    WriteSkeletonSection();
    WriteMaterialSection();
    WriteSubMeshSection();

    g_out.close();
    return true;
}

// ==========================================================
// 스킨 가중치 채우기
// ==========================================================
static void FillSkinWeights(FbxMesh* mesh, SubMesh& sm, const std::vector<int>& vtxCpIndex)
{
    int cpCount = mesh->GetControlPointsCount();

    struct Influence { int bone; float weight; };
    vector<vector<Influence>> cpInfluences(cpCount);

    int skinCount = mesh->GetDeformerCount(FbxDeformer::eSkin);
    for (int s = 0; s < skinCount; ++s)
    {
        FbxSkin* skin = (FbxSkin*)mesh->GetDeformer(s, FbxDeformer::eSkin);
        if (!skin) continue;

        int clusterCount = skin->GetClusterCount();
        for (int c = 0; c < clusterCount; ++c)
        {
            FbxCluster* cluster = skin->GetCluster(c);
            if (!cluster) continue;

            string boneName = cluster->GetLink() ? cluster->GetLink()->GetName() : "";
            auto it = g_BoneNameToIndex.find(boneName);
            if (it == g_BoneNameToIndex.end()) continue;

            int boneIndex = it->second;

            int idxCount = cluster->GetControlPointIndicesCount();
            int* idxArr = cluster->GetControlPointIndices();
            double* wArr = cluster->GetControlPointWeights();

            for (int i = 0; i < idxCount; ++i)
            {
                int cpIndex = idxArr[i];
                double w = wArr[i];

                if (cpIndex < 0 || cpIndex >= cpCount) continue;
                if (w <= 0.0) continue;

                cpInfluences[cpIndex].push_back({ boneIndex, float(w) });
            }
        }
    }

    for (int v = 0; v < (int)sm.vertices.size(); ++v)
    {
        int cpIdx = (v < (int)vtxCpIndex.size()) ? vtxCpIndex[v] : -1;
        if (cpIdx < 0 || cpIdx >= cpCount)
        {
            for (int i = 0; i < 4; ++i) { sm.vertices[v].boneIndices[i] = 0; sm.vertices[v].boneWeights[i] = 0.0f; }
            continue;
        }

        auto& inf = cpInfluences[cpIdx];

        sort(inf.begin(), inf.end(),
            [](const Influence& a, const Influence& b) { return a.weight > b.weight; });

        if ((int)inf.size() > 4) inf.resize(4);

        float sumW = 0.0f;
        for (int i = 0; i < (int)inf.size(); ++i) sumW += inf[i].weight;
        float inv = (sumW > 0.0f) ? 1.0f / sumW : 0.0f;

        for (int i = 0; i < 4; ++i)
        {
            if (i < (int)inf.size())
            {
                sm.vertices[v].boneIndices[i] = inf[i].bone;
                sm.vertices[v].boneWeights[i] = inf[i].weight * inv;
            }
            else
            {
                sm.vertices[v].boneIndices[i] = 0;
                sm.vertices[v].boneWeights[i] = 0.0f;
            }
        }
    }
}
// ==========================================================
// DEBUG DUMP (one-shot)
// ==========================================================
static double Det3x3(const FbxAMatrix& m)
{
    double a = m.Get(0, 0), b = m.Get(0, 1), c = m.Get(0, 2);
    double d = m.Get(1, 0), e = m.Get(1, 1), f = m.Get(1, 2);
    double g = m.Get(2, 0), h = m.Get(2, 1), i = m.Get(2, 2);
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}

static void DumpTRS(const char* tag, const FbxAMatrix& m)
{
    FbxVector4    T = m.GetT();
    FbxQuaternion Q = m.GetQ();
    FbxVector4    S = m.GetS();
    Q.Normalize();

    DLOG(tag);
    DLOG(" det="); DLOG(Det3x3(m));
    DLOG(" T=("); DLOG(T[0]); DLOG(","); DLOG(T[1]); DLOG(","); DLOG(T[2]); DLOG(")");
    DLOG(" Q=("); DLOG(Q[0]); DLOG(","); DLOG(Q[1]); DLOG(","); DLOG(Q[2]); DLOG(","); DLOG(Q[3]); DLOG(")");
    DLOG(" S=("); DLOG(S[0]); DLOG(","); DLOG(S[1]); DLOG(","); DLOG(S[2]); DLOGLN(")");
}

static void DumpNodeChain(FbxNode* node)
{
    if (!node) { DLOGLN("[Chain] null"); return; }
    DLOG("[Chain] ");
    for (FbxNode* n = node; n; n = n->GetParent())
    {
        DLOG(n->GetName());
        FbxNodeAttribute* a = n->GetNodeAttribute();
        if (a && a->GetAttributeType() == FbxNodeAttribute::eSkeleton) DLOG("(Skel)");
        else if (a && a->GetAttributeType() == FbxNodeAttribute::eMesh) DLOG("(Mesh)");
        else DLOG("(none)");

        if (n->GetParent()) DLOG(" <- ");
    }
    DLOGLN("");
}

static const char* SafeName(FbxNode* n)
{
    return (n && n->GetName() && n->GetName()[0] != '\0') ? n->GetName() : "null";
}

static int Bool01(bool v) { return v ? 1 : 0; }


static void DumpModelDebug_All(
    FbxScene* scene,
    const vector<Bone>& bones,
    const vector<bool>& boneHasBind,
    const vector<FbxAMatrix>& boneGlobalBind,
    FbxNode* baseNode,
    const vector<pair<FbxNode*, FbxMesh*>>& meshRefsSimple,
    int baseMeshIndex,
    const FbxAMatrix& S,              // mirror matrix
    bool mirrorXExport
)
{
    // 출력 포맷(가독성)
    std::cout.setf(std::ios::fixed);
    std::cout.precision(6);

    DLOGLN("========== [DEBUG DUMP BEGIN] ==========");

    // 0) Scene/Root
    if (scene)
    {
        FbxNode* root = scene->GetRootNode();
        DLOG("[Scene] root="); DLOGLN(SafeName(root));
    }

    // 1) Base node
    DLOG("[Base] index="); DLOG(baseMeshIndex);
    DLOG(" node="); DLOGLN(SafeName(baseNode));

    if (baseNode)
    {
        DumpNodeChain(baseNode);
        FbxAMatrix baseG = baseNode->EvaluateGlobalTransform();
        DumpTRS("[BaseG] ", baseG);
        DumpTRS("[BaseInv] ", baseG.Inverse());
    }

    // 2) Mirror matrix 자체
    DLOG("[Mirror] enabled="); DLOG(mirrorXExport ? 1 : 0); DLOGLN("");
    if (mirrorXExport)
        DumpTRS("[MirrorS] ", S);

    // 3) Mesh nodes overview (각 메시 det, toBase det 등)
    DLOGLN("[Meshes]");
    for (int i = 0; i < (int)meshRefsSimple.size(); ++i)
    {
        FbxNode* node = meshRefsSimple[i].first;
        FbxMesh* mesh = meshRefsSimple[i].second;
        if (!node || !mesh) continue;

        FbxAMatrix meshG = node->EvaluateGlobalTransform();

        FbxAMatrix geo; geo.SetIdentity();
        geo.SetT(node->GetGeometricTranslation(FbxNode::eSourcePivot));
        geo.SetR(node->GetGeometricRotation(FbxNode::eSourcePivot));
        geo.SetS(node->GetGeometricScaling(FbxNode::eSourcePivot));

        double detMeshGeo = Det3x3(meshG * geo);

        DLOG("  ["); DLOG(i); DLOG("] ");
        DLOG(node->GetName());
        DLOG(" cp="); DLOG(mesh->GetControlPointsCount());
        DLOG(" det(meshG*geo)="); DLOGLN(detMeshGeo);

        if (i == baseMeshIndex) DLOGLN("      (BASE MESH)");

        DumpNodeChain(node);
        DumpTRS("    meshG ", meshG);
        DumpTRS("    geo   ", geo);

        if (baseNode)
        {
            FbxAMatrix baseG = baseNode->EvaluateGlobalTransform();
            FbxAMatrix toBase = baseG.Inverse() * meshG * geo;
            DumpTRS("    toBase", toBase);
        }
    }

    // 4) Bones overview (여기가 “애니메이션과 비교” 핵심)
    DLOGLN("[Bones]");
    DLOG("  count="); DLOGLN((int)bones.size());

    int missing = 0;
    for (int i = 0; i < (int)bones.size(); ++i)
        if (!boneHasBind[i]) missing++;
    DLOG("  bindMissing="); DLOG(missing); DLOG(" / "); DLOGLN((int)bones.size());

    for (int i = 0; i < (int)bones.size(); ++i)
    {
        const Bone& b = bones[i];
        DLOG("  ["); DLOG(i); DLOG("] ");
        DLOG(b.name); DLOG(" parent="); DLOG(b.parentIndex);
        bool hb = (bool)boneHasBind[i];
        DLOG(" hasBind="); DLOGLN(Bool01(hb));


        if (boneHasBind[i])
        {
            const FbxAMatrix& g = boneGlobalBind[i];
            DumpTRS("    GBind ", g);

            // local bind은 저장된 float[16] 기반으로 “대략” 확인(정확비교는 float 출력용)
            // (여기서는 matrix det 정도만 보고 싶으면 float->matrix로 복원 가능)
        }

        // bindLocal / offsetMatrix는 이미 float로 저장됨 -> 그대로 덤프(정확 비교용)
        DLOGLN("    bindLocal[16]=");
        DLOG("      ");
        for (int k = 0; k < 16; ++k) { DLOG(b.bindLocal[k]); DLOG((k % 4 == 3) ? "\n      " : ", "); }
        DLOGLN("");

        DLOGLN("    offsetMatrix[16]=");
        DLOG("      ");
        for (int k = 0; k < 16; ++k) { DLOG(b.offsetMatrix[k]); DLOG((k % 4 == 3) ? "\n      " : ", "); }
        DLOGLN("");
    }

    DLOGLN("========== [DEBUG DUMP END] ==========");
}


// ==========================================================
// 스킨 전용 FBX 파싱
// - 스킨 메시만 SubMesh로 추출
// - 비스킨 베이크/보정 로직 전부 제거
// ==========================================================
static void ExtractFromFBX(FbxScene* scene)
{
    g_Bones.clear();
    g_SubMeshes.clear();
    g_BoneNameToIndex.clear();
    g_BoneNameToNode.clear();

    // 1) DirectX 좌표계 + meter 단위
    FbxAxisSystem::DirectX.ConvertScene(scene);
    FbxSystemUnit::m.ConvertScene(scene);

    // 2) Triangulate
    {
        FbxGeometryConverter conv(scene->GetFbxManager());
        conv.Triangulate(scene, true);
    }

    // 3) 모든 "스킨 메시" 수집
    struct MeshRef { FbxNode* node; FbxMesh* mesh; };
    vector<MeshRef> meshRefs;
    vector<int> meshVertexCount;

    function<void(FbxNode*)> dfs = [&](FbxNode* n)
        {
            if (!n) return;

            if (auto* m = n->GetMesh())
            {
                bool hasSkin = (m->GetDeformerCount(FbxDeformer::eSkin) > 0);
                if (hasSkin)
                {
                    meshRefs.push_back({ n, m });
                    meshVertexCount.push_back(m->GetControlPointsCount());
                }
            }

            for (int i = 0; i < n->GetChildCount(); ++i)
                dfs(n->GetChild(i));
        };
    dfs(scene->GetRootNode());

    if (meshRefs.empty())
        return; // 스킨 메시가 없으면 스킨용 추출기에서는 아무것도 안 뽑음

    // 4) Bone skeleton 수집 (eSkeleton 노드만)
    function<void(FbxNode*, int)> ExtractBones = [&](FbxNode* node, int parentIdx)
        {
            if (!node) return;

            FbxNodeAttribute* attr = node->GetNodeAttribute();
            int myIdx = parentIdx;

            if (attr && attr->GetAttributeType() == FbxNodeAttribute::eSkeleton)
            {
                Bone b{};
                b.name = node->GetName();
                b.parentIndex = parentIdx;

                for (int i = 0; i < 16; ++i) {
                    b.bindLocal[i] = (i % 5 == 0) ? 1.0f : 0.0f;
                    b.offsetMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
                }

                myIdx = (int)g_Bones.size();
                g_BoneNameToIndex[b.name] = myIdx;
                g_BoneNameToNode[b.name] = node;
                g_Bones.push_back(b);
            }

            for (int i = 0; i < node->GetChildCount(); ++i)
                ExtractBones(node->GetChild(i), myIdx);
        };
    ExtractBones(scene->GetRootNode(), -1);

    const int boneCount = (int)g_Bones.size();

    // 5) base mesh 선택 (가장 많은 control points)
    int baseMeshIndex = 0;
    int maxVerts = -1;
    for (int i = 0; i < (int)meshRefs.size(); ++i)
    {
        if (meshVertexCount[i] > maxVerts)
        {
            maxVerts = meshVertexCount[i];
            baseMeshIndex = i;
        }
    }

    FbxNode* baseNode = meshRefs[baseMeshIndex].node;

    // 6) boneGlobalBind 계산 (base mesh 기준 좌표)
    vector<FbxAMatrix> boneGlobalBind(boneCount);
    vector<bool> boneHasBind(boneCount, false);

    FbxAMatrix baseMeshGlobal;
    if (baseNode) baseMeshGlobal = baseNode->EvaluateGlobalTransform();
    else          baseMeshGlobal.SetIdentity();

    FbxAMatrix baseMeshGlobalInv = baseMeshGlobal.Inverse();
    // X reflection matrix S (좌우반전)
    FbxAMatrix S; S.SetIdentity();
    S.SetRow(0, FbxVector4(-1, 0, 0, 0));
    S.SetRow(1, FbxVector4(0, 1, 0, 0));
    S.SetRow(2, FbxVector4(0, 0, 1, 0));
    S.SetRow(3, FbxVector4(0, 0, 0, 1));

    for (int i = 0; i < boneCount; ++i)
    {
        auto itN = g_BoneNameToNode.find(g_Bones[i].name);
        FbxNode* boneNode = (itN != g_BoneNameToNode.end()) ? itN->second : nullptr;

        if (!boneNode)
        {
            boneGlobalBind[i].SetIdentity();
            boneHasBind[i] = false;
            continue;
        }

        FbxAMatrix boneGlobal = boneNode->EvaluateGlobalTransform();
        FbxAMatrix boneInMesh = baseMeshGlobalInv * boneGlobal;

        // translation만 0.01
        FbxVector4 t = boneInMesh.GetT();
        t[0] *= EXPORT_SCALE_D;
        t[1] *= EXPORT_SCALE_D;
        t[2] *= EXPORT_SCALE_D;
        boneInMesh.SetT(t);

        if (MIRROR_X_EXPORT)
            boneInMesh = S * boneInMesh * S;

        boneGlobalBind[i] = boneInMesh;
        boneHasBind[i] = true;
    }

    // 7) bindLocal 계산
    for (int i = 0; i < boneCount; ++i)
    {
        if (!boneHasBind[i])
        {
            FbxAMatrix I; I.SetIdentity();
            boneGlobalBind[i] = I;
            boneHasBind[i] = true;
        }

        int p = g_Bones[i].parentIndex;

        FbxAMatrix parentM;
        if (p >= 0 && boneHasBind[p]) parentM = boneGlobalBind[p];
        else                          parentM.SetIdentity();

        FbxAMatrix local = parentM.Inverse() * boneGlobalBind[i];

        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                g_Bones[i].bindLocal[r * 4 + c] = (float)local.Get(r, c);
    }

    // 8) offsetMatrix 계산
    for (int i = 0; i < boneCount; ++i)
    {
        FbxAMatrix off = boneGlobalBind[i].Inverse();

        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                g_Bones[i].offsetMatrix[r * 4 + c] = (float)off.Get(r, c);
    }

#if DEBUGLOG
    {
        // Dump 함수 인자가 pair<FbxNode*,FbxMesh*> 벡터라서 meshRefs를 간단히 복사/변환
        vector<pair<FbxNode*, FbxMesh*>> meshRefsSimple;
        meshRefsSimple.reserve(meshRefs.size());
        for (auto& mr : meshRefs) meshRefsSimple.push_back({ mr.node, mr.mesh });

        DumpModelDebug_All(
            scene,
            g_Bones,
            boneHasBind,
            boneGlobalBind,
            baseNode,
            meshRefsSimple,
            baseMeshIndex,
            S,
            MIRROR_X_EXPORT
        );
    }
#endif


    // 9) Material + Diffuse Texture 수집 (전체 노드에서 수집해도 무방)
    g_Materials.clear();
    g_MaterialNameToIndex.clear();

    function<void(FbxNode*)> CollectMaterials = [&](FbxNode* node)
        {
            if (!node) return;

            int matCount = node->GetMaterialCount();
            for (int i = 0; i < matCount; ++i)
            {
                FbxSurfaceMaterial* mat = node->GetMaterial(i);
                if (!mat) continue;

                string matName = mat->GetName();
                if (g_MaterialNameToIndex.count(matName))
                    continue;

                Material m{};
                m.name = matName;
                m.diffuseTextureName = "";

                FbxProperty prop = mat->FindProperty(FbxSurfaceMaterial::sDiffuse);
                if (prop.IsValid())
                {
                    int texCount = prop.GetSrcObjectCount<FbxTexture>();
                    if (texCount > 0)
                    {
                        FbxFileTexture* tex = FbxCast<FbxFileTexture>(prop.GetSrcObject<FbxTexture>(0));
                        if (tex)
                        {
                            const char* fn = tex->GetFileName();
                            m.diffuseTextureName = SafeStemFromFbxFileName(fn);
                        }
                    }
                }

                uint32_t idx = (uint32_t)g_Materials.size();
                g_Materials.push_back(m);
                g_MaterialNameToIndex[matName] = idx;
            }

            for (int i = 0; i < node->GetChildCount(); ++i)
                CollectMaterials(node->GetChild(i));
        };
    CollectMaterials(scene->GetRootNode());
#if DEBUGLOG
    DLOGLN("\n[Material List]");
    for (size_t i = 0; i < g_Materials.size(); ++i)
    {
        const auto& m = g_Materials[i];
        DLOG("  ["); DLOG(i); DLOG("] ");
        DLOG("name=\""); DLOG(m.name); DLOG("\" ");
        DLOG("diffuse=\""); DLOG(m.diffuseTextureName); DLOGLN("\"");
    }
#endif


    // 10) SubMesh 생성 (스킨 메시만)
    for (int mi = 0; mi < (int)meshRefs.size(); ++mi)
    {
        FbxMesh* mesh = meshRefs[mi].mesh;
        FbxNode* node = meshRefs[mi].node;
        if (!mesh || !node) continue;

        SubMesh sm{};
        sm.meshName = node->GetName();
        sm.materialIndex = 0;

        // 재질(첫 번째 슬롯만)
        int matCount = node->GetMaterialCount();
        if (matCount > 0)
        {
            FbxSurfaceMaterial* mat = node->GetMaterial(0);
            if (mat)
            {
                auto it = g_MaterialNameToIndex.find(mat->GetName());
                if (it != g_MaterialNameToIndex.end())
                    sm.materialIndex = it->second;
            }
        }

        int polyCount = mesh->GetPolygonCount();
        int cpCount = mesh->GetControlPointsCount();
        FbxVector4* cp = mesh->GetControlPoints();

        // UV 세트
        FbxStringList uvSetNames;
        mesh->GetUVSetNames(uvSetNames);
        const char* uvSetName = (uvSetNames.GetCount() > 0) ? uvSetNames[0] : nullptr;
        bool hasUVSet = (uvSetName != nullptr);

        // === 변환행렬 준비 (base mesh 공간) ===
        FbxAMatrix baseG;
        if (baseNode) baseG = baseNode->EvaluateGlobalTransform();
        else          baseG.SetIdentity();
        FbxAMatrix baseInv = baseG.Inverse();

        FbxAMatrix meshG = node->EvaluateGlobalTransform();

        // Geometric transform (FBX에서 메시 노드에만 붙는 별도 오프셋)
        FbxAMatrix geo; geo.SetIdentity();
        geo.SetT(node->GetGeometricTranslation(FbxNode::eSourcePivot));
        geo.SetR(node->GetGeometricRotation(FbxNode::eSourcePivot));
        geo.SetS(node->GetGeometricScaling(FbxNode::eSourcePivot));

        // 정점은 "base mesh 기준 공간"으로 통일
        FbxAMatrix toBase = baseInv * meshG * geo;

        // 미러링(det<0)이면 winding을 뒤집어야 좌우/앞뒤가 정상
        auto Det3x3 = [](const FbxAMatrix& m) -> double
            {
                double a = m.Get(0, 0), b = m.Get(0, 1), c = m.Get(0, 2);
                double d = m.Get(1, 0), e = m.Get(1, 1), f = m.Get(1, 2);
                double g = m.Get(2, 0), h = m.Get(2, 1), i = m.Get(2, 2);
                return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
            };
        bool flipWinding = (Det3x3(meshG * geo) < 0.0) ^ MIRROR_X_EXPORT;

        // 정점/인덱스
        std::vector<int> vtxCpIndex;
        vtxCpIndex.reserve(polyCount * 3);

        for (int p = 0; p < polyCount; ++p)
        {
            // triangulated라 3개 고정
            Vertex triV[3]{};
            int    triCp[3]{ -1,-1,-1 };

            for (int k = 0; k < 3; ++k)
            {
                int cpIdx = mesh->GetPolygonVertex(p, k);
                triCp[k] = cpIdx;

                // position (base 공간으로 변환)
                FbxVector4 p4 = toBase.MultT(cp[cpIdx]);
                if (MIRROR_X_EXPORT) p4[0] = -p4[0];
                triV[k].position[0] = (float)p4[0] * EXPORT_SCALE_F;
                triV[k].position[1] = (float)p4[1] * EXPORT_SCALE_F;
                triV[k].position[2] = (float)p4[2] * EXPORT_SCALE_F;

                // normal (벡터 변환: w=0)
                FbxVector4 nL;
                mesh->GetPolygonVertexNormal(p, k, nL);
                FbxVector4 n4(nL[0], nL[1], nL[2], 0.0);
                FbxVector4 nW = toBase.MultT(n4);
                if (MIRROR_X_EXPORT) nW[0] = -nW[0];

                nW.Normalize();
                triV[k].normal[0] = (float)nW[0];
                triV[k].normal[1] = (float)nW[1];
                triV[k].normal[2] = (float)nW[2];

                // UV
                if (hasUVSet)
                {
                    FbxVector2 uv; bool unmapped = false;
                    if (mesh->GetPolygonVertexUV(p, k, uvSetName, uv, unmapped))
                    {
                        triV[k].uv[0] = (float)uv[0];
                        triV[k].uv[1] = 1.0f - (float)uv[1];
                    }
                    else
                    {
                        triV[k].uv[0] = triV[k].uv[1] = 0.0f;
                    }
                }
                else
                {
                    triV[k].uv[0] = triV[k].uv[1] = 0.0f;
                }
            }

            // push vertices
            uint32_t base = (uint32_t)sm.vertices.size();
            for (int k = 0; k < 3; ++k)
            {
                sm.vertices.push_back(triV[k]);
                vtxCpIndex.push_back(triCp[k]);
            }

            // push indices (미러링이면 winding swap)
            if (!flipWinding)
            {
                sm.indices.push_back(base + 0);
                sm.indices.push_back(base + 1);
                sm.indices.push_back(base + 2);
            }
            else
            {
                sm.indices.push_back(base + 0);
                sm.indices.push_back(base + 2);
                sm.indices.push_back(base + 1);
            }
        }

        // 스킨 웨이트 채우기 (cp 인덱스 매핑은 그대로 사용)
        FillSkinWeights(mesh, sm, vtxCpIndex);


#if DEBUGLOG
        DLOG("[SubMesh] mesh=\""); DLOG(sm.meshName); DLOG("\" ");
        DLOG("materialIndex="); DLOG(sm.materialIndex);

        if (sm.materialIndex < g_Materials.size())
        {
            const auto& mat = g_Materials[sm.materialIndex];
            DLOG(" ("); DLOG(mat.name); DLOG(")");
            DLOG(" diffuse=\""); DLOG(mat.diffuseTextureName); DLOG("\"");
        }
        DLOGLN("");
#endif

        g_SubMeshes.push_back(std::move(sm));
    }
}

// ==========================================================
// main
// ==========================================================
int main()
{
    std::string importDir = "import";
    std::string exportDir = "export";

    namespace fs = std::filesystem;

    // export 폴더 보장
    std::error_code ec;
    fs::create_directories(exportDir, ec);

    FbxManager* manager = FbxManager::Create();
    if (!manager)
    {
        cout << "FBX Manager 생성 실패.\n";
        return -1;
    }

    FbxIOSettings* ios = FbxIOSettings::Create(manager, IOSROOT);
    manager->SetIOSettings(ios);

    for (const auto& entry : fs::directory_iterator(importDir))
    {
        if (!entry.is_regular_file()) continue;

        fs::path path = entry.path();
        if (path.extension() != ".fbx") continue;

        std::string name = path.stem().string();
        std::string fbxFileName = path.string();
        std::string binFileName = exportDir + "/" + name + ".bin";

        cout << "\n==========================================\n";
        cout << "처리 중: " << fbxFileName << "\n";

        FbxImporter* importer = FbxImporter::Create(manager, "");
        bool ok = importer->Initialize(fbxFileName.c_str(), -1, manager->GetIOSettings());
        if (!ok)
        {
            cout << "FBX 파일 열기 실패: " << fbxFileName << "\n";
            importer->Destroy();
            continue;
        }

        FbxScene* scene = FbxScene::Create(manager, ("scene_" + name).c_str());
        importer->Import(scene);
        importer->Destroy();

        ExtractFromFBX(scene);

        if (SaveModelBin(binFileName))
            cout << "BIN 생성 완료: " << binFileName << "\n";
        else
            cout << "BIN 생성 실패: " << binFileName << "\n";

        scene->Destroy();
    }

    manager->Destroy();
    return 0;
}
