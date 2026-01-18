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

// ==========================================================
// 스태틱(비스킨) 전용 추출기
// - "엔진 BIN 포맷을 안 바꾼다" 가정:
//   Vertex에 boneIndices/weights는 남겨두되 항상 0으로 채움.
// - 스케일 정책(중요):
//   씬을 FbxSystemUnit::m.ConvertScene(scene)로 미터로 변환했으므로,
//   정점에 EXPORT_SCALE_F(0.01)를 추가로 곱하지 않는다. (중복 스케일 방지)
// ==========================================================

static constexpr float FINAL_SCALE_F = 1.0f; // ConvertScene(m) 사용 시 1.0 권장

#define DEBUGLOG 1

#if DEBUGLOG
#define DLOG(x) do { std::cout << x; } while(0)
#define DLOGLN(x) do { std::cout << x << "\n"; } while(0)
#else
#define DLOG(x) do {} while(0)
#define DLOGLN(x) do {} while(0)
#endif

// ==========================================================
// 전역 저장 데이터
// ==========================================================

struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    uint32_t boneIndices[4];
    float boneWeights[4];
};

struct Material
{
    string name;
    string diffuseTextureName;
};

struct SubMesh {
    string meshName;
    uint32_t materialIndex;
    vector<Vertex> vertices;
    vector<uint32_t> indices;
};

vector<Material> g_Materials;
unordered_map<string, uint32_t> g_MaterialNameToIndex;
vector<SubMesh> g_SubMeshes;

// ==========================================================
// 파일 출력 스트림
// ==========================================================

static std::ofstream g_out;

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
    if (len > 0) WriteRaw(s.data(), len);
}

// ==========================================================
// [C++20] u8path 대체: 파일명 stem만 뽑기
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
// FBX Node Geometric Transform (별도 오프셋)
// ==========================================================

static FbxAMatrix GetGeometry(FbxNode* node)
{
    FbxAMatrix geo;
    geo.SetIdentity();
    if (!node) return geo;

    geo.SetT(node->GetGeometricTranslation(FbxNode::eSourcePivot));
    geo.SetR(node->GetGeometricRotation(FbxNode::eSourcePivot));
    geo.SetS(node->GetGeometricScaling(FbxNode::eSourcePivot));
    return geo;
}

// ==========================================================
// Material 섹션
// ==========================================================

static void WriteMaterialSection()
{
    for (auto& m : g_Materials)
    {
        WriteStringUtf8(m.name);
        WriteStringUtf8(m.diffuseTextureName);
    }
}

// ==========================================================
// 헤더 (boneCount=0)
// ==========================================================

static void WriteModelHeader()
{
    char magic[4] = { 'M', 'B', 'I', 'N' };
    WriteRaw(magic, 4);

    uint32_t version = 1;
    uint32_t flags = 0;
    uint32_t boneCount = 0; // 비스킨 전용
    uint32_t materialCount = (uint32_t)g_Materials.size();
    uint32_t subCount = (uint32_t)g_SubMeshes.size();

    WriteUInt32(version);
    WriteUInt32(flags);
    WriteUInt32(boneCount);
    WriteUInt32(materialCount);
    WriteUInt32(subCount);
}

// ==========================================================
// Skeleton 섹션: boneCount=0 이므로 아무것도 안 씀
// ==========================================================

static void WriteSkeletonSection_Empty()
{
    // intentionally empty
}

// ==========================================================
// SubMesh 섹션
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

            // 엔진 포맷 유지용(항상 0)
            WriteRaw(v.boneIndices, sizeof(uint32_t) * 4);
            WriteFloatArray(v.boneWeights, 4);
        }

        if (idxCount > 0)
            WriteRaw(sm.indices.data(), sizeof(uint32_t) * idxCount);
    }
}

static bool SaveModelBin(const std::string& filename)
{
    g_out.open(filename, ios::binary);
    if (!g_out.is_open()) return false;

    WriteModelHeader();
    WriteSkeletonSection_Empty();
    WriteMaterialSection();
    WriteSubMeshSection();

    g_out.close();
    return true;
}

// ==========================================================
// FBX -> RAM 추출 (비스킨 전용)
// ==========================================================

static void ExtractFromFBX_StaticOnly(FbxScene* scene)
{
    g_SubMeshes.clear();
    g_Materials.clear();
    g_MaterialNameToIndex.clear();

    // 1) 좌표계/단위 변환
    FbxAxisSystem::DirectX.ConvertScene(scene);
    FbxSystemUnit::m.ConvertScene(scene);

    // 2) Triangulate
    {
        FbxGeometryConverter conv(scene->GetFbxManager());
        conv.Triangulate(scene, true);
    }

    // 3) Material 수집(전체 노드 순회)
    function<void(FbxNode*)> CollectMaterials = [&](FbxNode* node)
        {
            if (!node) return;

            int matCount = node->GetMaterialCount();
            for (int i = 0; i < matCount; ++i)
            {
                FbxSurfaceMaterial* mat = node->GetMaterial(i);
                if (!mat) continue;

                string matName = mat->GetName();
                if (g_MaterialNameToIndex.count(matName)) continue;

                Material m{};
                m.name = matName;
                m.diffuseTextureName.clear();

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

    // 4) 모든 mesh 수집 후 "비스킨만" 처리
    struct MeshRef { FbxNode* node; FbxMesh* mesh; bool hasSkin; };

    vector<MeshRef> meshRefs;

    function<void(FbxNode*)> dfs = [&](FbxNode* n)
        {
            if (!n) return;

            if (auto* m = n->GetMesh())
            {
                bool hasSkin = (m->GetDeformerCount(FbxDeformer::eSkin) > 0);
                meshRefs.push_back({ n, m, hasSkin });
            }

            for (int i = 0; i < n->GetChildCount(); ++i)
                dfs(n->GetChild(i));
        };
    dfs(scene->GetRootNode());

    for (auto& mr : meshRefs)
    {
        if (!mr.node || !mr.mesh) continue;
        if (mr.hasSkin) continue; // ★ 스킨 메시 제거: 비스킨 전용

        FbxNode* node = mr.node;
        FbxMesh* mesh = mr.mesh;

        SubMesh sm;
        sm.meshName = node->GetName();
        sm.materialIndex = 0;

        // 재질(첫 번째만)
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

        // 5) 비스킨: 노드 글로벌 + 지오를 정점에 베이크
        FbxAMatrix global = node->EvaluateGlobalTransform();
        FbxAMatrix geo = GetGeometry(node);
        FbxAMatrix xform = global * geo;

        // ------------------------------------------------------
        // [HACK] baked 결과가 항상 완전 반전(좌/우, 상/하, 전/후)이라고 가정하고 상쇄
        // (x,y,z 모두 -1 스케일)
        // ------------------------------------------------------
        FbxAMatrix invFix;
        invFix.SetIdentity();
        invFix.SetS(FbxVector4(-1.0, -1.0, -1.0, 0.0));

        // 좌표계를 통째로 뒤집는 보정은 '왼쪽 곱'으로 적용
        xform = invFix * xform;


        bool flip = (xform.Determinant() < 0.0);

        // normal matrix
        FbxAMatrix nMat = xform;
        nMat.SetT(FbxVector4(0, 0, 0, 0));
        nMat = nMat.Inverse().Transpose();

        int polyCount = mesh->GetPolygonCount();
        int cpCount = mesh->GetControlPointsCount();
        FbxVector4* cp = mesh->GetControlPoints();

        // UV set
        FbxStringList uvSetNames;
        mesh->GetUVSetNames(uvSetNames);
        const char* uvSetName = (uvSetNames.GetCount() > 0) ? uvSetNames[0] : nullptr;
        bool hasUVSet = (uvSetName != nullptr);

        sm.vertices.reserve(polyCount * 3);
        sm.indices.reserve(polyCount * 3);

        for (int p = 0; p < polyCount; ++p)
        {
            int idx[3] = { 0,1,2 };
            if (flip) std::swap(idx[1], idx[2]);

            for (int k = 0; k < 3; ++k)
            {
                int cpIdx = mesh->GetPolygonVertex(p, idx[k]);
                if (cpIdx < 0 || cpIdx >= cpCount) continue;

                Vertex v{};
                // bone data는 항상 0
                for (int i = 0; i < 4; ++i) { v.boneIndices[i] = 0; v.boneWeights[i] = 0.0f; }

                // position bake
                FbxVector4 posL = cp[cpIdx];
                FbxVector4 posW = xform.MultT(posL);

                v.position[0] = (float)posW[0] * FINAL_SCALE_F;
                v.position[1] = (float)posW[1] * FINAL_SCALE_F;
                v.position[2] = (float)posW[2] * FINAL_SCALE_F;

                // normal bake
                FbxVector4 nL;
                mesh->GetPolygonVertexNormal(p, idx[k], nL);
                FbxVector4 nW = nMat.MultT(nL);
                nW.Normalize();
                v.normal[0] = (float)nW[0];
                v.normal[1] = (float)nW[1];
                v.normal[2] = (float)nW[2];

                // UV
                if (hasUVSet)
                {
                    FbxVector2 uv;
                    bool unmapped = false;
                    if (mesh->GetPolygonVertexUV(p, idx[k], uvSetName, uv, unmapped))
                    {
                        v.uv[0] = (float)uv[0];
                        v.uv[1] = 1.0f - (float)uv[1];
                    }
                    else
                    {
                        v.uv[0] = 0.0f;
                        v.uv[1] = 0.0f;
                    }
                }
                else
                {
                    v.uv[0] = 0.0f;
                    v.uv[1] = 0.0f;
                }

                sm.vertices.push_back(v);
                sm.indices.push_back((uint32_t)sm.indices.size());
            }
        }

        // 빈 메시 방지
        if (!sm.vertices.empty())
        {
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
}

// ==========================================================
// main
// ==========================================================

int main()
{
    std::string importDir = "import";
    std::string exportDir = "export";

    namespace fs = std::filesystem;
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

        ExtractFromFBX_StaticOnly(scene);

        if (SaveModelBin(binFileName))
            cout << "BIN 생성 완료: " << binFileName << "\n";
        else
            cout << "BIN 생성 실패: " << binFileName << "\n";

        scene->Destroy();
    }

    manager->Destroy();
    return 0;
}
