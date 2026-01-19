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
static constexpr bool FLIP_X_TO_MATCH_UNITY = true;

#define DEBUGLOG 1

#if DEBUGLOG
#define DLOG(x)   do { std::cout << x; } while(0)
#define DLOGLN(x) do { std::cout << x << "\n"; } while(0)
#else
#define DLOG(x)   do {} while(0)
#define DLOGLN(x) do {} while(0)
#endif

static double Det3x3(const FbxAMatrix & m)
{
    double a = m.Get(0, 0), b = m.Get(0, 1), c = m.Get(0, 2);
    double d = m.Get(1, 0), e = m.Get(1, 1), f = m.Get(1, 2);
    double g = m.Get(2, 0), h = m.Get(2, 1), i = m.Get(2, 2);
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}


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
    //FbxAxisSystem::DirectX.ConvertScene(scene);
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

    // 6) boneGlobalBind 계산 (base mesh 기준, EvaluateGlobalTransform 기반)
    vector<FbxAMatrix> boneGlobalBind(boneCount);
    vector<bool> boneHasBind(boneCount, false);

    FbxAMatrix baseG; baseG.SetIdentity();
    if (baseNode) baseG = baseNode->EvaluateGlobalTransform();
    FbxAMatrix baseInv = baseG.Inverse();

    // X 반사 행렬 S
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

        // FBX global (bind pose 시점)
        FbxAMatrix boneGlobal = boneNode->EvaluateGlobalTransform();

        // base 공간으로 내림
        FbxAMatrix boneInBase = baseInv * boneGlobal;

        // 스케일: translation만 0.01
        FbxVector4 t = boneInBase.GetT();
        t[0] *= EXPORT_SCALE_D;
        t[1] *= EXPORT_SCALE_D;
        t[2] *= EXPORT_SCALE_D;
        boneInBase.SetT(t);

        // 정점을 X flip 했으면, bind도 "같은 공간"으로 맞춰야 함
        // (정점은 p4.x = -p4.x == S 적용이므로, 본 변환은 공액변환 S*M*S)
        if (FLIP_X_TO_MATCH_UNITY)
            boneInBase = S * boneInBase * S;

        boneGlobalBind[i] = boneInBase;
        boneHasBind[i] = true;
    }

#if DEBUGLOG
    {
        int missing = 0;
        for (int i = 0; i < boneCount; ++i)
            if (!boneHasBind[i]) missing++;

        DLOG("[BindMissing] "); DLOG(missing);
        DLOG(" / "); DLOGLN(boneCount);
    }
#endif
#if DEBUGLOG
    {
        int bi = 0; // 0번 본이 root가 아닐 수도 있지만, 일단 0으로 시작
        double det = Det3x3(boneGlobalBind[bi]);
        FbxVector4 t = boneGlobalBind[bi].GetT();

        DLOG("[Bind0] det="); DLOG(det);
        DLOG(" T=("); DLOG(t[0]); DLOG(","); DLOG(t[1]); DLOG(","); DLOG(t[2]); DLOGLN(")");
    }
#endif

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
#if DEBUGLOG
        {
            double ldet = Det3x3(local);
            if (ldet < 0.0)
            {
                DLOG("[LocalDetNeg] i="); DLOG(i);
                DLOG(" name="); DLOG(g_Bones[i].name);
                DLOG(" det="); DLOGLN(ldet);
            }
        }
#endif
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
        int bi = 0;
        double det = Det3x3(boneGlobalBind[bi].Inverse());
        DLOG("[Offset0] det="); DLOGLN(det);
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
        FbxAMatrix baseG; baseG.SetIdentity();
        if (baseNode) baseG = baseNode->EvaluateGlobalTransform();
        FbxAMatrix baseInv = baseG.Inverse();

        FbxAMatrix meshG = node->EvaluateGlobalTransform();

        FbxAMatrix geo; geo.SetIdentity();
        geo.SetT(node->GetGeometricTranslation(FbxNode::eSourcePivot));
        geo.SetR(node->GetGeometricRotation(FbxNode::eSourcePivot));
        geo.SetS(node->GetGeometricScaling(FbxNode::eSourcePivot));

        FbxAMatrix toBase = baseInv * meshG * geo;

        double det = Det3x3(meshG * geo);
        bool mirrored = (det < 0.0);
        bool finalFlip = (mirrored ^ FLIP_X_TO_MATCH_UNITY);

#if DEBUGLOG
        DLOG("[NodeDet] "); DLOG(node->GetName());
        DLOG(" det="); DLOGLN(det);
#endif

        std::vector<int> vtxCpIndex;
        vtxCpIndex.reserve(polyCount * 3);

        for (int p = 0; p < polyCount; ++p)
        {
            for (int k = 0; k < 3; ++k)
            {
                int cpIdx = mesh->GetPolygonVertex(p, k);
                if (cpIdx < 0 || cpIdx >= cpCount) continue;

                Vertex v{};

                // position
                FbxVector4 p4 = toBase.MultT(cp[cpIdx]);

                if (FLIP_X_TO_MATCH_UNITY) p4[0] = -p4[0];

                v.position[0] = (float)p4[0] * EXPORT_SCALE_F;
                v.position[1] = (float)p4[1] * EXPORT_SCALE_F;
                v.position[2] = (float)p4[2] * EXPORT_SCALE_F;

                // normal (주의: 비균일 스케일 있으면 inverse-transpose가 정석)
                FbxVector4 nL;
                mesh->GetPolygonVertexNormal(p, k, nL);
                FbxVector4 n4(nL[0], nL[1], nL[2], 0.0);
                FbxVector4 nW = toBase.MultT(n4);

                if (FLIP_X_TO_MATCH_UNITY) nW[0] = -nW[0];

                nW.Normalize();
                v.normal[0] = (float)nW[0];
                v.normal[1] = (float)nW[1];
                v.normal[2] = (float)nW[2];

                // UV
                if (hasUVSet)
                {
                    FbxVector2 uv; bool unmapped = false;
                    if (mesh->GetPolygonVertexUV(p, k, uvSetName, uv, unmapped))
                    {
                        v.uv[0] = (float)uv[0];
                        v.uv[1] = 1.0f - (float)uv[1];
                    }
                }

                sm.vertices.push_back(v);
                vtxCpIndex.push_back(cpIdx);

                if ((sm.vertices.size() % 3) == 0)
                {
                    uint32_t base = (uint32_t)sm.vertices.size() - 3;
                    if (!finalFlip)
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
            }
        }

        FillSkinWeights(mesh, sm, vtxCpIndex);
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
