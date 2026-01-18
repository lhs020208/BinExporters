#include <iostream>
#include <string>
#include <fstream>
#include <vector>
#include <cstdint>
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

// ==========================================================
// Material 정보 (BIN에 저장할 대상)
// ==========================================================

struct Material
{
    string name;               // material 이름 (ex: "face")
    string diffuseTextureName; // texture 파일명 base (ex: "face")
};

vector<Material> g_Materials;
unordered_map<string, uint32_t> g_MaterialNameToIndex;


struct SubMesh {
    string meshName;
    uint32_t materialIndex;   // g_Materials 인덱스
    vector<Vertex> vertices;
    vector<uint32_t> indices;
};


vector<Bone> g_Bones;
vector<SubMesh> g_SubMeshes;
unordered_map<string, int> g_BoneNameToIndex;


// ==========================================================
// [추가] 유틸: std::u8string(char8_t) -> std::string(char) 변환
// - 실제 바이트(UTF-8)를 그대로 std::string에 담는다.
// ==========================================================
static std::string U8ToString(const std::u8string& u8)
{
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// ==========================================================
// [수정] u8path() 제거 (C++20 deprecation 대응)
// - u8path(fn) 대신: std::u8string(UTF-8 바이트) -> path 생성
// ==========================================================
static std::string SafeStemFromFbxFileName(const char* fn)
{
    if (!fn || !fn[0]) return "";

    try
    {
        // fn(char*)의 바이트열을 "UTF-8"로 간주하고 char8_t로 승격
        std::string s(fn);
        std::u8string u8(reinterpret_cast<const char8_t*>(s.data()), s.size());

        // C++20 권장: u8string/char8_t 기반 생성자 사용
        std::filesystem::path p(u8);

        // stem 추출 (u8string -> string)
        std::u8string stem_u8 = p.stem().u8string();
        return std::string(reinterpret_cast<const char*>(stem_u8.data()), stem_u8.size());
    }
    catch (...)
    {
        // fallback: filesystem 변환 자체를 안 함
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

void WriteRaw(const void* data, size_t size)
{
    g_out.write(reinterpret_cast<const char*>(data), size);
}

void WriteUInt16(uint16_t v) { WriteRaw(&v, sizeof(v)); }
void WriteUInt32(uint32_t v) { WriteRaw(&v, sizeof(v)); }
void WriteInt32(int32_t v) { WriteRaw(&v, sizeof(v)); }

void WriteFloatArray(const float* f, size_t count)
{
    WriteRaw(f, sizeof(float) * count);
}

void WriteStringUtf8(const std::string& s)
{
    uint16_t len = static_cast<uint16_t>(s.size());
    WriteUInt16(len);
    if (len > 0)
        WriteRaw(s.data(), len);
}

void WriteMaterialSection()
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

void WriteModelHeader()
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

void WriteSkeletonSection()
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

void WriteSubMeshSection()
{
    for (auto& sm : g_SubMeshes)
    {
        // 이름들
        WriteStringUtf8(sm.meshName);
        WriteUInt32(sm.materialIndex);

        // 개수
        uint32_t vtxCount = (uint32_t)sm.vertices.size();
        uint32_t idxCount = (uint32_t)sm.indices.size();
        WriteUInt32(vtxCount);
        WriteUInt32(idxCount);

        // 정점
        for (const Vertex& v : sm.vertices)
        {
            WriteFloatArray(v.position, 3);
            WriteFloatArray(v.normal, 3);
            WriteFloatArray(v.uv, 2);
            WriteRaw(v.boneIndices, sizeof(uint32_t) * 4);
            WriteFloatArray(v.boneWeights, 4);
        }

        // 인덱스
        if (idxCount > 0)
            WriteRaw(sm.indices.data(), sizeof(uint32_t) * idxCount);
    }
}

// ==========================================================
// BIN 파일 저장 함수
// ==========================================================

bool SaveModelBin(const std::string& filename)
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

void FillSkinWeights(FbxMesh* mesh, SubMesh& sm)
{
    int cpCount = mesh->GetControlPointsCount();

    // control point별로 영향받는 bone 정보를 저장
    struct Influence { int bone; float weight; };
    vector<vector<Influence>> cpInfluences;
    cpInfluences.resize(cpCount);

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

    // 정점 수 = sm.vertices.size() (triangulated된 정점 기준)
    for (int v = 0; v < sm.vertices.size(); ++v)
    {
        // polygon vertex에 대응되는 cp index
        int cpIdx = mesh->GetPolygonVertex(v / 3, v % 3);

        // influence 가져오기
        auto& inf = cpInfluences[cpIdx];

        // 4개 초과면 weight 큰 순으로 정렬해서 4개만
        sort(inf.begin(), inf.end(),
            [](const Influence& a, const Influence& b) {
                return a.weight > b.weight;
            });

        if (inf.size() > 4) inf.resize(4);

        float sumW = 0.0f;
        for (int i = 0; i < inf.size(); ++i)
            sumW += inf[i].weight;

        float inv = (sumW > 0.0f) ? 1.0f / sumW : 0.0f;

        // 채우기
        for (int i = 0; i < 4; ++i)
        {
            if (i < inf.size())
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
// ★★★★★ FBX 파싱 함수 (추후 단계에서 구현) ★★★★★
// 지금은 틀만 만들고 내용은 비워둔다.
// ==========================================================
void ExtractFromFBX(FbxScene* scene)
{
    g_Bones.clear();
    g_SubMeshes.clear();
    g_BoneNameToIndex.clear();

    // -----------------------------------------------------
    // 1) DirectX 좌표계 적용
    // -----------------------------------------------------
    FbxAxisSystem::DirectX.ConvertScene(scene);
    FbxSystemUnit::m.ConvertScene(scene);

    // -----------------------------------------------------
    // 2) Triangulate
    // -----------------------------------------------------
    {
        FbxGeometryConverter conv(scene->GetFbxManager());
        conv.Triangulate(scene, true);
    }

    // -----------------------------------------------------
    // 3) 모든 mesh 수집 + 스킨 여부 파악
    // -----------------------------------------------------
    vector<FbxMesh*> meshes;
    vector<bool> meshHasSkin;
    vector<int> meshVertexCount;

    function<void(FbxNode*)> dfs = [&](FbxNode* n)
        {
            if (!n) return;

            if (auto* m = n->GetMesh())
            {
                meshes.push_back(m);
                meshHasSkin.push_back(m->GetDeformerCount(FbxDeformer::eSkin) > 0);
                meshVertexCount.push_back(m->GetControlPointsCount());
            }

            for (int i = 0; i < n->GetChildCount(); ++i)
                dfs(n->GetChild(i));
        };
    dfs(scene->GetRootNode());

    if (meshes.empty()) return;

    // -----------------------------------------------------
    // 4) Bone skeleton 수집
    // -----------------------------------------------------
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

                // 기본은 4x4 identity
                for (int i = 0; i < 16; ++i) {
                    b.bindLocal[i] = (i % 5 == 0) ? 1.0f : 0.0f;
                    b.offsetMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
                }

                myIdx = (int)g_Bones.size();
                g_BoneNameToIndex[b.name] = myIdx;
                g_Bones.push_back(b);
            }

            for (int i = 0; i < node->GetChildCount(); ++i)
                ExtractBones(node->GetChild(i), myIdx);
        };
    ExtractBones(scene->GetRootNode(), -1);

    const int boneCount = (int)g_Bones.size();

    // -----------------------------------------------------
    // 5) base mesh 자동 선택
    // -----------------------------------------------------
    int baseMeshIndex = -1;
    int maxVerts = -1;

    for (int i = 0; i < meshes.size(); ++i)
    {
        if (!meshHasSkin[i]) continue;
        if (meshVertexCount[i] > maxVerts)
        {
            maxVerts = meshVertexCount[i];
            baseMeshIndex = i;
        }
    }
    if (baseMeshIndex < 0)
        baseMeshIndex = 0;

    FbxMesh* baseMesh = meshes[baseMeshIndex];
    FbxNode* baseNode = baseMesh->GetNode();

    // -----------------------------------------------------
    // 6) boneGlobalBind 계산
    // -----------------------------------------------------
    vector<FbxAMatrix> boneGlobalBind(boneCount);
    vector<bool> boneHasBind(boneCount, false);

    FbxAMatrix baseMeshGlobal;
    if (baseNode)
        baseMeshGlobal = baseNode->EvaluateGlobalTransform();
    else
        baseMeshGlobal.SetIdentity();

    FbxAMatrix baseMeshGlobalInv = baseMeshGlobal.Inverse();

    for (int i = 0; i < boneCount; ++i)
    {
        FbxNode* boneNode = scene->FindNodeByName(g_Bones[i].name.c_str());
        if (!boneNode)
        {
            boneGlobalBind[i].SetIdentity();
            continue;
        }
        constexpr double LENGTH_SCALE_D = 0.01; // double 버전(취향)

        FbxAMatrix boneGlobal = boneNode->EvaluateGlobalTransform();
        FbxAMatrix boneInMesh = baseMeshGlobalInv * boneGlobal;

        // translation만 스케일 (회전/기타 성분 건드리지 않음)
        FbxVector4 t = boneInMesh.GetT();
        t[0] *= LENGTH_SCALE_D;
        t[1] *= LENGTH_SCALE_D;
        t[2] *= LENGTH_SCALE_D;
        boneInMesh.SetT(t);

        boneGlobalBind[i] = boneInMesh;
        boneHasBind[i] = true;
    }

    // -----------------------------------------------------
    // 7) bindLocal 계산
    // -----------------------------------------------------
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
        if (p >= 0 && boneHasBind[p])
            parentM = boneGlobalBind[p];
        else
            parentM.SetIdentity();

        FbxAMatrix local = parentM.Inverse() * boneGlobalBind[i];

        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                g_Bones[i].bindLocal[r * 4 + c] = (float)local.Get(r, c);
    }

    // -----------------------------------------------------
    // 8) offsetMatrix 계산
    // -----------------------------------------------------
    for (int i = 0; i < boneCount; ++i)
    {
        FbxAMatrix off = boneGlobalBind[i].Inverse();

        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                g_Bones[i].offsetMatrix[r * 4 + c] = (float)off.Get(r, c);
    }

    // -----------------------------------------------------
    // ★ Material + Diffuse Texture 수집
    // -----------------------------------------------------
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

                // Diffuse texture 추출
                // ==========================================================
                // [수정] CollectMaterials 내부 diffuseTextureName 추출 블록 교체
                // ==========================================================
                FbxProperty prop = mat->FindProperty(FbxSurfaceMaterial::sDiffuse);
                if (prop.IsValid())
                {
                    int texCount = prop.GetSrcObjectCount<FbxTexture>();
                    if (texCount > 0)
                    {
                        FbxFileTexture* tex =
                            FbxCast<FbxFileTexture>(prop.GetSrcObject<FbxTexture>(0));
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
    {
        cout << "\n[Material List]\n";
        for (size_t i = 0; i < g_Materials.size(); ++i)
        {
            const auto& m = g_Materials[i];
            cout << "  [" << i << "] "
                << "name=\"" << m.name << "\" "
                << "diffuse=\"" << m.diffuseTextureName << "\"\n";
        }
    }



    // -----------------------------------------------------
    // 9) SubMesh 생성
    // -----------------------------------------------------
    auto ToF3 = [&](const FbxVector4& v) {
        float out[3] = { (float)v[0], (float)v[1], (float)v[2] };
        return out;
        };
    auto ToF2 = [&](const FbxVector2& v) {
        float out[2] = { (float)v[0], (float)v[1] };
        return out;
        };

    for (int mi = 0; mi < meshes.size(); ++mi)
    {
        FbxMesh* mesh = meshes[mi];
        if (!mesh) continue;

        SubMesh sm;

        // 이름
        FbxNode* node = mesh->GetNode();
        sm.meshName = node ? node->GetName() : "Unnamed";
        sm.materialIndex = 0; // default

        if (node)
        {
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
        }

        // flip 검사
        FbxAMatrix global = node ? node->EvaluateGlobalTransform() : FbxAMatrix();
        FbxAMatrix geo;
        if (node)
        {
            geo.SetT(node->GetGeometricTranslation(FbxNode::eSourcePivot));
            geo.SetR(node->GetGeometricRotation(FbxNode::eSourcePivot));
            geo.SetS(node->GetGeometricScaling(FbxNode::eSourcePivot));
        }
        FbxAMatrix xform = global * geo;
        bool flip = (xform.Determinant() < 0);

        // 스킨 없는 mesh → bone 붙이기
        int attachedBoneIndex = -1;
        if (!meshHasSkin[mi] && !g_Bones.empty())
        {
            FbxNode* cur = node;
            while (cur)
            {
                auto it = g_BoneNameToIndex.find(cur->GetName());
                if (it != g_BoneNameToIndex.end()) {
                    attachedBoneIndex = it->second;
                    break;
                }
                cur = cur->GetParent();
            }
            if (attachedBoneIndex < 0)
                attachedBoneIndex = 0;
        }

        int polyCount = mesh->GetPolygonCount();
        int cpCount = mesh->GetControlPointsCount();
        FbxVector4* cp = mesh->GetControlPoints();

        // UV 세트
        FbxStringList uvSetNames;
        mesh->GetUVSetNames(uvSetNames);
        const char* uvSetName = nullptr;
        if (uvSetNames.GetCount() > 0)
            uvSetName = uvSetNames[0];
        bool hasUVSet = uvSetName != nullptr;

        // 정점 추출
        for (int p = 0; p < polyCount; ++p)
        {
            int idx[3] = { 0,1,2 };
            if (flip) std::swap(idx[1], idx[2]);

            for (int k = 0; k < 3; ++k)
            {
                int cpIdx = mesh->GetPolygonVertex(p, idx[k]);
                if (cpIdx < 0 || cpIdx >= cpCount) continue;

                Vertex v{};

                // pos
                constexpr float LENGTH_SCALE = 0.01f; // 100 -> 1

                FbxVector4 pos = cp[cpIdx];
                v.position[0] = (float)pos[0] * LENGTH_SCALE;
                v.position[1] = (float)pos[1] * LENGTH_SCALE;
                v.position[2] = (float)pos[2] * LENGTH_SCALE;

                // normal
                FbxVector4 n;
                mesh->GetPolygonVertexNormal(p, idx[k], n);
                v.normal[0] = (float)n[0];
                v.normal[1] = (float)n[1];
                v.normal[2] = (float)n[2];

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
                    else {
                        v.uv[0] = v.uv[1] = 0;
                    }
                }
                else {
                    v.uv[0] = v.uv[1] = 0;
                }

                // non-skinned mesh의 bone index 설정
                if (!meshHasSkin[mi])
                {
                    for (int bi = 0; bi < 4; ++bi) v.boneIndices[bi] = 0;
                    for (int bi = 0; bi < 4; ++bi) v.boneWeights[bi] = 0;

                    if (attachedBoneIndex >= 0) {
                        v.boneIndices[0] = attachedBoneIndex;
                        v.boneWeights[0] = 1.0f;
                    }
                }

                sm.vertices.push_back(v);
                sm.indices.push_back((uint32_t)sm.indices.size());
            }
        }

        // 스킨 처리
        if (meshHasSkin[mi])
        {
            FillSkinWeights(mesh, sm);
        }
        else
        {
            // 보정
            for (int i = 0; i < sm.vertices.size(); ++i)
            {
                if (attachedBoneIndex >= 0)
                {
                    sm.vertices[i].boneIndices[0] = attachedBoneIndex;
                    sm.vertices[i].boneWeights[0] = 1.0f;
                }
            }
        }
        {
            cout << "[SubMesh] "
                << "mesh=\"" << sm.meshName << "\" "
                << "materialIndex=" << sm.materialIndex;

            if (sm.materialIndex < g_Materials.size())
            {
                cout << " ("
                    << g_Materials[sm.materialIndex].name
                    << ")";
            }
            cout << "\n";
        }


        g_SubMeshes.push_back(sm);
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

    // FBX SDK 초기화
    FbxManager* manager = FbxManager::Create();
    if (!manager)
    {
        cout << "FBX Manager 생성 실패.\n";
        return -1;
    }

    FbxIOSettings* ios = FbxIOSettings::Create(manager, IOSROOT);
    manager->SetIOSettings(ios);

    // ========================================
    // import 폴더의 모든 *.fbx 파일 순회
    // ========================================
    for (const auto& entry : fs::directory_iterator(importDir))
    {
        if (!entry.is_regular_file()) continue;

        fs::path path = entry.path();
        if (path.extension() != ".fbx") continue; // 혹시 모를 안전장치

        std::string name = path.stem().string();

        std::string fbxFileName = path.string();
        std::string binFileName = exportDir + "/" + name + ".bin";

        cout << "\n==========================================\n";
        cout << "처리 중: " << fbxFileName << "\n";

        // Importer 생성
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

        // FBX → RAM 추출
        ExtractFromFBX(scene);

        // BIN 저장
        if (SaveModelBin(binFileName))
            cout << "BIN 생성 완료: " << binFileName << "\n";
        else
            cout << "BIN 생성 실패: " << binFileName << "\n";

        scene->Destroy();
    }

    manager->Destroy();
    return 0;
}
