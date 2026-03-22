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
#include <cmath>

#include <fbxsdk.h>

using namespace std;

static constexpr double EXPORT_SCALE_D = 0.01;
static constexpr float  EXPORT_SCALE_F = 0.01f; //왜인지 모르겠는데 기본 0.01배 해야 맞음.
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
    float tangent[4];
    uint32_t boneIndices[4];
    float boneWeights[4];
};

struct MaterialTexTransform
{
    float scale[2] = { 1.0f, 1.0f };
    float offset[2] = { 0.0f, 0.0f };
    uint32_t wrapMode[2] = { 0u, 0u }; // 0=Repeat, 1=Clamp
};

struct Material
{
    string name;
    string diffuseTextureName;
    string normalTextureName;
    string emissiveTextureName;
    string specularTextureName;

    float diffuseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float emissiveColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float specularColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // rgb=specular, a=shininess

    MaterialTexTransform diffuseTransform;
    MaterialTexTransform normalTransform;
    MaterialTexTransform emissiveTransform;
    MaterialTexTransform specularTransform;
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
static void WriteMaterialTexTransform(const MaterialTexTransform& t)
{
    WriteFloatArray(t.scale, 2);
    WriteFloatArray(t.offset, 2);
    WriteRaw(t.wrapMode, sizeof(uint32_t) * 2);
}

static void WriteMaterialSection()
{
    for (auto& m : g_Materials)
    {
        WriteStringUtf8(m.name);
        WriteStringUtf8(m.diffuseTextureName);
        WriteStringUtf8(m.normalTextureName);
        WriteStringUtf8(m.emissiveTextureName);
        WriteStringUtf8(m.specularTextureName);

        WriteFloatArray(m.diffuseColor, 4);
        WriteFloatArray(m.emissiveColor, 4);
        WriteFloatArray(m.specularColor, 4);

        WriteMaterialTexTransform(m.diffuseTransform);
        WriteMaterialTexTransform(m.normalTransform);
        WriteMaterialTexTransform(m.emissiveTransform);
        WriteMaterialTexTransform(m.specularTransform);
    }
}

// ==========================================================
// 1) 파일 헤더 저장
// ==========================================================
static void WriteModelHeader()
{
    char magic[4] = { 'M', 'B', 'I', 'N' };
    WriteRaw(magic, 4);

    uint32_t version = 3;
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
            WriteFloatArray(v.tangent, 4);
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
#if DEBUGLOG
    DLOGLN("\n==========================================================");
    DLOG("[SaveModelBin] filename = "); DLOGLN(filename);
    DLOG("  boneCount     = "); DLOGLN((uint32_t)g_Bones.size());
    DLOG("  materialCount = "); DLOGLN((uint32_t)g_Materials.size());
    DLOG("  subMeshCount  = "); DLOGLN((uint32_t)g_SubMeshes.size());
#endif

    g_out.open(filename, ios::binary);
    if (!g_out.is_open()) return false;

    WriteModelHeader();
    WriteSkeletonSection();
    WriteMaterialSection();
    WriteSubMeshSection();

    g_out.close();
    return true;
}
static std::string ExtractFirstTextureStem(FbxProperty prop)
{
    if (!prop.IsValid()) return "";

    // LayeredTexture (있으면 0번만)
    if (prop.GetSrcObjectCount<FbxLayeredTexture>() > 0)
    {
        auto* layered = prop.GetSrcObject<FbxLayeredTexture>(0);
        if (layered && layered->GetSrcObjectCount<FbxTexture>() > 0)
        {
            auto* tex = FbxCast<FbxFileTexture>(layered->GetSrcObject<FbxTexture>(0));
            if (tex) return SafeStemFromFbxFileName(tex->GetFileName());
        }
        return "";
    }

    // 일반 텍스처
    if (prop.GetSrcObjectCount<FbxTexture>() > 0)
    {
        auto* tex = FbxCast<FbxFileTexture>(prop.GetSrcObject<FbxTexture>(0));
        if (tex) return SafeStemFromFbxFileName(tex->GetFileName());
    }
    return "";
}

static FbxTexture* ExtractFirstTextureObject(FbxProperty prop)
{
    if (!prop.IsValid()) return nullptr;

    if (prop.GetSrcObjectCount<FbxLayeredTexture>() > 0)
    {
        auto* layered = prop.GetSrcObject<FbxLayeredTexture>(0);
        if (layered && layered->GetSrcObjectCount<FbxTexture>() > 0)
            return layered->GetSrcObject<FbxTexture>(0);
        return nullptr;
    }

    if (prop.GetSrcObjectCount<FbxTexture>() > 0)
        return prop.GetSrcObject<FbxTexture>(0);

    return nullptr;
}

static const char* WrapModeToString(FbxTexture::EWrapMode mode)
{
    switch (mode)
    {
    case FbxTexture::eRepeat: return "Repeat";
    case FbxTexture::eClamp:  return "Clamp";
    default:                  return "Unknown";
    }
}

static const char* MaterialClassToString(FbxSurfaceMaterial* mat)
{
    if (!mat) return "null";
    if (FbxCast<FbxSurfacePhong>(mat))   return "Phong";
    if (FbxCast<FbxSurfaceLambert>(mat)) return "Lambert";
    return "Other";
}

static void DumpDouble3Value(const char* label, const FbxDouble3& v)
{
    DLOG("  ");
    DLOG(label);
    DLOG(" = (");
    DLOG(v[0]); DLOG(", ");
    DLOG(v[1]); DLOG(", ");
    DLOG(v[2]); DLOGLN(")");
}

static void DumpDoubleValue(const char* label, double v)
{
    DLOG("  ");
    DLOG(label);
    DLOG(" = ");
    DLOGLN(v);
}

static void DumpTextureSlotDebug(const char* slotName, FbxProperty prop)
{
    DLOG("  [TextureSlot] ");
    DLOG(slotName);

    if (!prop.IsValid())
    {
        DLOGLN(" : property invalid");
        return;
    }

    const bool hasLayered = (prop.GetSrcObjectCount<FbxLayeredTexture>() > 0);
    FbxTexture* tex = ExtractFirstTextureObject(prop);

    if (!tex)
    {
        DLOG(" : connected=0");
        DLOG(" layered="); DLOG(hasLayered ? 1 : 0);
        DLOGLN("");
        return;
    }

    auto* fileTex = FbxCast<FbxFileTexture>(tex);

    DLOG(" : connected=1");
    DLOG(" layered="); DLOG(hasLayered ? 1 : 0);
    DLOG(" type=\""); DLOG(fileTex ? "FileTexture" : "Texture"); DLOG("\"");

    DLOG(" scale=(");
    DLOG(tex->GetScaleU()); DLOG(", ");
    DLOG(tex->GetScaleV()); DLOG(")");

    DLOG(" trans=(");
    DLOG(tex->GetTranslationU()); DLOG(", ");
    DLOG(tex->GetTranslationV()); DLOG(")");

    DLOG(" wrap=(");
    DLOG(WrapModeToString(tex->GetWrapModeU())); DLOG(", ");
    DLOG(WrapModeToString(tex->GetWrapModeV())); DLOG(")");

    if (fileTex)
    {
        const char* fileName = fileTex->GetFileName();
        DLOG(" file=\""); DLOG(fileName ? fileName : ""); DLOG("\"");
        DLOG(" stem=\""); DLOG(SafeStemFromFbxFileName(fileName)); DLOG("\"");
    }

    DLOGLN("");
}

static void DumpMaterialDebug(FbxSurfaceMaterial* mat)
{
    if (!mat)
    {
        DLOGLN("[MaterialDump] null");
        return;
    }

    DLOGLN("\n--------------------------------------------------");
    DLOG("[MaterialDump] name=\""); DLOG(mat->GetName()); DLOG("\" ");
    DLOG("class=\""); DLOG(MaterialClassToString(mat)); DLOGLN("\"");

    if (auto* lambert = FbxCast<FbxSurfaceLambert>(mat))
    {
        DumpDouble3Value("Lambert.Ambient", lambert->Ambient.Get());
        DumpDoubleValue("Lambert.AmbientFactor", lambert->AmbientFactor.Get());

        DumpDouble3Value("Lambert.Diffuse", lambert->Diffuse.Get());
        DumpDoubleValue("Lambert.DiffuseFactor", lambert->DiffuseFactor.Get());

        DumpDouble3Value("Lambert.Emissive", lambert->Emissive.Get());
        DumpDoubleValue("Lambert.EmissiveFactor", lambert->EmissiveFactor.Get());

        DumpDouble3Value("Lambert.TransparentColor", lambert->TransparentColor.Get());
        DumpDoubleValue("Lambert.TransparencyFactor", lambert->TransparencyFactor.Get());
    }

    if (auto* phong = FbxCast<FbxSurfacePhong>(mat))
    {
        DumpDouble3Value("Phong.Specular", phong->Specular.Get());
        DumpDoubleValue("Phong.SpecularFactor", phong->SpecularFactor.Get());
        DumpDoubleValue("Phong.Shininess", phong->Shininess.Get());
        DumpDouble3Value("Phong.Reflection", phong->Reflection.Get());
        DumpDoubleValue("Phong.ReflectionFactor", phong->ReflectionFactor.Get());
    }

    DumpTextureSlotDebug("Diffuse", mat->FindProperty(FbxSurfaceMaterial::sDiffuse));
    DumpTextureSlotDebug("NormalMap", mat->FindProperty(FbxSurfaceMaterial::sNormalMap));
    DumpTextureSlotDebug("Bump", mat->FindProperty(FbxSurfaceMaterial::sBump));
    DumpTextureSlotDebug("Emissive", mat->FindProperty(FbxSurfaceMaterial::sEmissive));
    DumpTextureSlotDebug("Specular", mat->FindProperty(FbxSurfaceMaterial::sSpecular));
}

static uint32_t EncodeWrapMode(FbxTexture::EWrapMode mode)
{
    return (mode == FbxTexture::eClamp) ? 1u : 0u;
}

static void FillColor4(float out[4], double x, double y, double z, double w)
{
    out[0] = (float)x;
    out[1] = (float)y;
    out[2] = (float)z;
    out[3] = (float)w;
}

static void FillTexTransformFromProperty(FbxProperty prop, MaterialTexTransform& outTransform)
{
    outTransform = MaterialTexTransform{};

    FbxTexture* tex = ExtractFirstTextureObject(prop);
    if (!tex) return;

    outTransform.scale[0] = (float)tex->GetScaleU();
    outTransform.scale[1] = (float)tex->GetScaleV();
    outTransform.offset[0] = (float)tex->GetTranslationU();
    outTransform.offset[1] = (float)tex->GetTranslationV();
    outTransform.wrapMode[0] = EncodeWrapMode(tex->GetWrapModeU());
    outTransform.wrapMode[1] = EncodeWrapMode(tex->GetWrapModeV());
}

static void ExtractMaterialAttributes(FbxSurfaceMaterial* mat, Material& outMat)
{
    if (!mat) return;

    FbxProperty diffuseProp = mat->FindProperty(FbxSurfaceMaterial::sDiffuse);
    FbxProperty normalProp = mat->FindProperty(FbxSurfaceMaterial::sNormalMap);
    FbxProperty bumpProp = mat->FindProperty(FbxSurfaceMaterial::sBump);
    FbxProperty emissiveProp = mat->FindProperty(FbxSurfaceMaterial::sEmissive);
    FbxProperty specularProp = mat->FindProperty(FbxSurfaceMaterial::sSpecular);

    outMat.diffuseTextureName = ExtractFirstTextureStem(diffuseProp);
    outMat.normalTextureName = ExtractFirstTextureStem(normalProp);
    if (outMat.normalTextureName.empty())
    {
        outMat.normalTextureName = ExtractFirstTextureStem(bumpProp);
        FillTexTransformFromProperty(bumpProp, outMat.normalTransform);
    }
    else
    {
        FillTexTransformFromProperty(normalProp, outMat.normalTransform);
    }

    outMat.emissiveTextureName = ExtractFirstTextureStem(emissiveProp);
    outMat.specularTextureName = ExtractFirstTextureStem(specularProp);

    FillTexTransformFromProperty(diffuseProp, outMat.diffuseTransform);
    FillTexTransformFromProperty(emissiveProp, outMat.emissiveTransform);
    FillTexTransformFromProperty(specularProp, outMat.specularTransform);

    if (auto* lambert = FbxCast<FbxSurfaceLambert>(mat))
    {
        const FbxDouble3 d = lambert->Diffuse.Get();
        const double df = lambert->DiffuseFactor.Get();
        FillColor4(outMat.diffuseColor, d[0] * df, d[1] * df, d[2] * df, 1.0);

        const FbxDouble3 e = lambert->Emissive.Get();
        const double ef = lambert->EmissiveFactor.Get();
        FillColor4(outMat.emissiveColor, e[0] * ef, e[1] * ef, e[2] * ef, 1.0);
    }

    if (auto* phong = FbxCast<FbxSurfacePhong>(mat))
    {
        const FbxDouble3 s = phong->Specular.Get();
        const double sf = phong->SpecularFactor.Get();
        const double shininess = phong->Shininess.Get();
        FillColor4(outMat.specularColor, s[0] * sf, s[1] * sf, s[2] * sf, shininess);
    }
}

static void ComputeTangentForTri(Vertex& a, Vertex& b, Vertex& c)
{
    // p, uv
    float x1 = b.position[0] - a.position[0];
    float y1 = b.position[1] - a.position[1];
    float z1 = b.position[2] - a.position[2];

    float x2 = c.position[0] - a.position[0];
    float y2 = c.position[1] - a.position[1];
    float z2 = c.position[2] - a.position[2];

    float s1 = b.uv[0] - a.uv[0];
    float t1 = b.uv[1] - a.uv[1];
    float s2 = c.uv[0] - a.uv[0];
    float t2 = c.uv[1] - a.uv[1];

    float denom = (s1 * t2 - t1 * s2);
    if (fabsf(denom) < 1e-8f)
    {
        // 퇴화 UV: 기본값
        a.tangent[0] = b.tangent[0] = c.tangent[0] = 1.0f;
        a.tangent[1] = b.tangent[1] = c.tangent[1] = 0.0f;
        a.tangent[2] = b.tangent[2] = c.tangent[2] = 0.0f;
        a.tangent[3] = b.tangent[3] = c.tangent[3] = 1.0f;
        return;
    }

    float r = 1.0f / denom;

    // tangent, bitangent (unnormalized)
    float tx = (x1 * t2 - x2 * t1) * r;
    float ty = (y1 * t2 - y2 * t1) * r;
    float tz = (z1 * t2 - z2 * t1) * r;

    float bx = (x2 * s1 - x1 * s2) * r;
    float by = (y2 * s1 - y1 * s2) * r;
    float bz = (z2 * s1 - z1 * s2) * r;

    auto Normalize3 = [](float& x, float& y, float& z)
        {
            float len = sqrtf(x * x + y * y + z * z);
            if (len > 1e-8f) { float inv = 1.0f / len; x *= inv; y *= inv; z *= inv; }
        };

    auto Dot3 = [](float ax, float ay, float az, float bx, float by, float bz)->float
        { return ax * bx + ay * by + az * bz; };

    auto Cross3 = [](float ax, float ay, float az, float bx, float by, float bz,
        float& rx, float& ry, float& rz)
        { rx = ay * bz - az * by; ry = az * bx - ax * bz; rz = ax * by - ay * bx; };

    auto FixOne = [&](Vertex& v)
        {
            float nx = v.normal[0], ny = v.normal[1], nz = v.normal[2];
            Normalize3(nx, ny, nz);

            // Gram-Schmidt: T = normalize(T - N*dot(N,T))
            float dotNT = Dot3(nx, ny, nz, tx, ty, tz);
            float tpx = tx - nx * dotNT;
            float tpy = ty - ny * dotNT;
            float tpz = tz - nz * dotNT;
            Normalize3(tpx, tpy, tpz);

            // handedness: sign = dot(cross(N,T), B) < 0 ? -1 : +1
            float cx, cy, cz;
            Cross3(nx, ny, nz, tpx, tpy, tpz, cx, cy, cz);
            float sign = (Dot3(cx, cy, cz, bx, by, bz) < 0.0f) ? -1.0f : 1.0f;

            v.tangent[0] = tpx; v.tangent[1] = tpy; v.tangent[2] = tpz; v.tangent[3] = sign;
        };

    FixOne(a); FixOne(b); FixOne(c);
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

// ==========================================================
// DEBUG DUMP HELPERS
// ==========================================================
static void DumpMatrix4x4RowMajor(const char* tag, const float m[16])
{
    DLOGLN(tag);
    for (int r = 0; r < 4; ++r)
    {
        DLOG("    [ ");
        for (int c = 0; c < 4; ++c)
        {
            DLOG(m[r * 4 + c]);
            if (c < 3) DLOG(", ");
        }
        DLOGLN(" ]");
    }
}

static void DumpBoneSummary()
{
    DLOGLN("\n==========================================================");
    DLOGLN("[Bone Summary]");
    DLOG("BoneCount = "); DLOGLN((int)g_Bones.size());

    for (int i = 0; i < (int)g_Bones.size(); ++i)
    {
        const Bone& b = g_Bones[i];
        DLOG("--------------------------------------------------\n");
        DLOG("[Bone "); DLOG(i); DLOG("] ");
        DLOG("name=\""); DLOG(b.name); DLOG("\" ");
        DLOG("parentIndex="); DLOGLN(b.parentIndex);

        DumpMatrix4x4RowMajor("  bindLocal:", b.bindLocal);
        DumpMatrix4x4RowMajor("  offsetMatrix:", b.offsetMatrix);
    }
}

static void DumpVertexSample(const Vertex& v, int idx)
{
    DLOG("    [Vertex "); DLOG(idx); DLOGLN("]");
    DLOG("      pos     = (");
    DLOG(v.position[0]); DLOG(", ");
    DLOG(v.position[1]); DLOG(", ");
    DLOG(v.position[2]); DLOGLN(")");

    DLOG("      normal  = (");
    DLOG(v.normal[0]); DLOG(", ");
    DLOG(v.normal[1]); DLOG(", ");
    DLOG(v.normal[2]); DLOGLN(")");

    DLOG("      uv      = (");
    DLOG(v.uv[0]); DLOG(", ");
    DLOG(v.uv[1]); DLOGLN(")");

    DLOG("      tangent = (");
    DLOG(v.tangent[0]); DLOG(", ");
    DLOG(v.tangent[1]); DLOG(", ");
    DLOG(v.tangent[2]); DLOG(", ");
    DLOG(v.tangent[3]); DLOGLN(")");

    DLOG("      bones   = [");
    DLOG(v.boneIndices[0]); DLOG(", ");
    DLOG(v.boneIndices[1]); DLOG(", ");
    DLOG(v.boneIndices[2]); DLOG(", ");
    DLOG(v.boneIndices[3]); DLOGLN("]");

    DLOG("      weights = [");
    DLOG(v.boneWeights[0]); DLOG(", ");
    DLOG(v.boneWeights[1]); DLOG(", ");
    DLOG(v.boneWeights[2]); DLOG(", ");
    DLOG(v.boneWeights[3]); DLOGLN("]");
}

static void DumpSubMeshSummary()
{
    DLOGLN("\n==========================================================");
    DLOGLN("[SubMesh Summary]");
    DLOG("SubMeshCount = "); DLOGLN((int)g_SubMeshes.size());

    for (int smi = 0; smi < (int)g_SubMeshes.size(); ++smi)
    {
        const SubMesh& sm = g_SubMeshes[smi];

        DLOG("--------------------------------------------------\n");
        DLOG("[SubMesh "); DLOG(smi); DLOG("] ");
        DLOG("name=\""); DLOG(sm.meshName); DLOG("\" ");
        DLOG("materialIndex="); DLOG(sm.materialIndex);

        if (sm.materialIndex < g_Materials.size())
        {
            DLOG(" materialName=\"");
            DLOG(g_Materials[sm.materialIndex].name);
            DLOG("\"");
        }
        DLOGLN("");

        DLOG("  vertexCount = "); DLOGLN((uint32_t)sm.vertices.size());
        DLOG("  indexCount  = "); DLOGLN((uint32_t)sm.indices.size());

        if (!sm.vertices.empty())
        {
            float minP[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
            float maxP[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

            int invalidBoneIndexCount = 0;
            int zeroWeightVertexCount = 0;
            int badWeightSumCount = 0;

            float minWeightSum = FLT_MAX;
            float maxWeightSum = -FLT_MAX;

            for (size_t i = 0; i < sm.vertices.size(); ++i)
            {
                const Vertex& v = sm.vertices[i];

                for (int k = 0; k < 3; ++k)
                {
                    minP[k] = std::min(minP[k], v.position[k]);
                    maxP[k] = std::max(maxP[k], v.position[k]);
                }

                float sumW =
                    v.boneWeights[0] +
                    v.boneWeights[1] +
                    v.boneWeights[2] +
                    v.boneWeights[3];

                minWeightSum = std::min(minWeightSum, sumW);
                maxWeightSum = std::max(maxWeightSum, sumW);

                if (sumW < 1e-6f)
                    zeroWeightVertexCount++;

                if (fabsf(sumW - 1.0f) > 0.02f && sumW > 1e-6f)
                    badWeightSumCount++;

                for (int j = 0; j < 4; ++j)
                {
                    if (v.boneWeights[j] > 0.0f)
                    {
                        if (v.boneIndices[j] >= g_Bones.size())
                            invalidBoneIndexCount++;
                    }
                }
            }

            DLOG("  AABB min    = (");
            DLOG(minP[0]); DLOG(", ");
            DLOG(minP[1]); DLOG(", ");
            DLOG(minP[2]); DLOGLN(")");

            DLOG("  AABB max    = (");
            DLOG(maxP[0]); DLOG(", ");
            DLOG(maxP[1]); DLOG(", ");
            DLOG(maxP[2]); DLOGLN(")");

            DLOG("  weightSumMin = "); DLOGLN(minWeightSum);
            DLOG("  weightSumMax = "); DLOGLN(maxWeightSum);
            DLOG("  zeroWeightVertexCount = "); DLOGLN(zeroWeightVertexCount);
            DLOG("  badWeightSumCount     = "); DLOGLN(badWeightSumCount);
            DLOG("  invalidBoneIndexRefs  = "); DLOGLN(invalidBoneIndexCount);

            int sampleCount = (int)std::min<size_t>(sm.vertices.size(), 5);
            DLOG("  vertexSamples = "); DLOGLN(sampleCount);
            for (int i = 0; i < sampleCount; ++i)
                DumpVertexSample(sm.vertices[i], i);
        }

        if (!sm.indices.empty())
        {
            uint32_t maxIdx = 0;
            int outOfRangeIndexCount = 0;

            for (uint32_t idx : sm.indices)
            {
                if (idx > maxIdx) maxIdx = idx;
                if (idx >= sm.vertices.size())
                    outOfRangeIndexCount++;
            }

            DLOG("  maxIndex = "); DLOGLN(maxIdx);
            DLOG("  outOfRangeIndexCount = "); DLOGLN(outOfRangeIndexCount);

            int triSampleCount = (int)std::min<size_t>(sm.indices.size() / 3, 5);
            DLOG("  triangleSamples = "); DLOGLN(triSampleCount);
            for (int t = 0; t < triSampleCount; ++t)
            {
                uint32_t i0 = sm.indices[t * 3 + 0];
                uint32_t i1 = sm.indices[t * 3 + 1];
                uint32_t i2 = sm.indices[t * 3 + 2];

                DLOG("    tri "); DLOG(t); DLOG(" = [");
                DLOG(i0); DLOG(", ");
                DLOG(i1); DLOG(", ");
                DLOG(i2); DLOGLN("]");
            }
        }
    }
}

static void DumpMaterialSummary()
{
    DLOGLN("\n==========================================================");
    DLOGLN("[Material Summary]");
    DLOG("MaterialCount = "); DLOGLN((int)g_Materials.size());

    for (int i = 0; i < (int)g_Materials.size(); ++i)
    {
        const Material& m = g_Materials[i];

        DLOG("[Material "); DLOG(i); DLOG("] ");
        DLOG("name=\""); DLOG(m.name); DLOGLN("\"");

        DLOG("  diffuseTex=\""); DLOG(m.diffuseTextureName); DLOGLN("\"");
        DLOG("  normalTex=\""); DLOG(m.normalTextureName); DLOGLN("\"");
        DLOG("  emissiveTex=\""); DLOG(m.emissiveTextureName); DLOGLN("\"");
        DLOG("  specularTex=\""); DLOG(m.specularTextureName); DLOGLN("\"");

        DLOG("  diffuseColor = (");
        DLOG(m.diffuseColor[0]); DLOG(", ");
        DLOG(m.diffuseColor[1]); DLOG(", ");
        DLOG(m.diffuseColor[2]); DLOG(", ");
        DLOG(m.diffuseColor[3]); DLOGLN(")");

        DLOG("  emissiveColor = (");
        DLOG(m.emissiveColor[0]); DLOG(", ");
        DLOG(m.emissiveColor[1]); DLOG(", ");
        DLOG(m.emissiveColor[2]); DLOG(", ");
        DLOG(m.emissiveColor[3]); DLOGLN(")");

        DLOG("  specularColor = (");
        DLOG(m.specularColor[0]); DLOG(", ");
        DLOG(m.specularColor[1]); DLOG(", ");
        DLOG(m.specularColor[2]); DLOG(", ");
        DLOG(m.specularColor[3]); DLOGLN(")");

        DLOG("  diffuseTransform scale=(");
        DLOG(m.diffuseTransform.scale[0]); DLOG(", ");
        DLOG(m.diffuseTransform.scale[1]); DLOG(") offset=(");
        DLOG(m.diffuseTransform.offset[0]); DLOG(", ");
        DLOG(m.diffuseTransform.offset[1]); DLOG(") wrap=(");
        DLOG(m.diffuseTransform.wrapMode[0]); DLOG(", ");
        DLOG(m.diffuseTransform.wrapMode[1]); DLOGLN(")");

        DLOG("  normalTransform scale=(");
        DLOG(m.normalTransform.scale[0]); DLOG(", ");
        DLOG(m.normalTransform.scale[1]); DLOG(") offset=(");
        DLOG(m.normalTransform.offset[0]); DLOG(", ");
        DLOG(m.normalTransform.offset[1]); DLOG(") wrap=(");
        DLOG(m.normalTransform.wrapMode[0]); DLOG(", ");
        DLOG(m.normalTransform.wrapMode[1]); DLOGLN(")");

        DLOG("  emissiveTransform scale=(");
        DLOG(m.emissiveTransform.scale[0]); DLOG(", ");
        DLOG(m.emissiveTransform.scale[1]); DLOG(") offset=(");
        DLOG(m.emissiveTransform.offset[0]); DLOG(", ");
        DLOG(m.emissiveTransform.offset[1]); DLOG(") wrap=(");
        DLOG(m.emissiveTransform.wrapMode[0]); DLOG(", ");
        DLOG(m.emissiveTransform.wrapMode[1]); DLOGLN(")");

        DLOG("  specularTransform scale=(");
        DLOG(m.specularTransform.scale[0]); DLOG(", ");
        DLOG(m.specularTransform.scale[1]); DLOG(") offset=(");
        DLOG(m.specularTransform.offset[0]); DLOG(", ");
        DLOG(m.specularTransform.offset[1]); DLOG(") wrap=(");
        DLOG(m.specularTransform.wrapMode[0]); DLOG(", ");
        DLOG(m.specularTransform.wrapMode[1]); DLOGLN(")");
    }
}

static void DumpExportSummary()
{
    DLOGLN("\n==========================================================");
    DLOGLN("[Export Summary]");
    DLOG("Bones     = "); DLOGLN((int)g_Bones.size());
    DLOG("Materials = "); DLOGLN((int)g_Materials.size());
    DLOG("SubMeshes = "); DLOGLN((int)g_SubMeshes.size());

    DumpMaterialSummary();
    DumpBoneSummary();
    DumpSubMeshSummary();
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
                ExtractMaterialAttributes(mat, m);

#if DEBUGLOG
                DumpMaterialDebug(mat);
#endif

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
        DLOG(" diffuse=\""); DLOG(m.diffuseTextureName); DLOG("\"");
        DLOG(" normal=\"");  DLOG(m.normalTextureName);  DLOGLN("\"");
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

            if (!flipWinding)
                ComputeTangentForTri(triV[0], triV[1], triV[2]);
            else
                ComputeTangentForTri(triV[0], triV[2], triV[1]); // 인덱스 swap과 동일한 삼각형 순서

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

#if DEBUGLOG
        DumpExportSummary();
#endif

        if (SaveModelBin(binFileName))
            cout << "BIN 생성 완료: " << binFileName << "\n";
        else
            cout << "BIN 생성 실패: " << binFileName << "\n";

        scene->Destroy();
    }

    manager->Destroy();
    return 0;
}
