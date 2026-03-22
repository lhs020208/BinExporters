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

static void WriteMaterialTexTransform(const MaterialTexTransform& t)
{
    WriteFloatArray(t.scale, 2);
    WriteFloatArray(t.offset, 2);
    WriteRaw(t.wrapMode, sizeof(uint32_t) * 2);
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

static bool IsNearlyBlack3(const float c[4], float eps = 1e-6f)
{
    return
        (fabsf(c[0]) <= eps) &&
        (fabsf(c[1]) <= eps) &&
        (fabsf(c[2]) <= eps);
}

static bool IsIdentityTexTransform(const MaterialTexTransform& t, float eps = 1e-6f)
{
    return
        (fabsf(t.scale[0] - 1.0f) <= eps) &&
        (fabsf(t.scale[1] - 1.0f) <= eps) &&
        (fabsf(t.offset[0]) <= eps) &&
        (fabsf(t.offset[1]) <= eps) &&
        (t.wrapMode[0] == 0u) &&
        (t.wrapMode[1] == 0u);
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
    outMat.emissiveTextureName = ExtractFirstTextureStem(emissiveProp);
    outMat.specularTextureName = ExtractFirstTextureStem(specularProp);

    FillTexTransformFromProperty(diffuseProp, outMat.diffuseTransform);

    if (outMat.normalTextureName.empty())
    {
        outMat.normalTextureName = ExtractFirstTextureStem(bumpProp);
        FillTexTransformFromProperty(bumpProp, outMat.normalTransform);
    }
    else
    {
        FillTexTransformFromProperty(normalProp, outMat.normalTransform);
    }

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

    if (!outMat.normalTextureName.empty() &&
        IsIdentityTexTransform(outMat.normalTransform) &&
        !IsIdentityTexTransform(outMat.diffuseTransform))
    {
        outMat.normalTransform = outMat.diffuseTransform;
    }

    if (!outMat.emissiveTextureName.empty() &&
        IsIdentityTexTransform(outMat.emissiveTransform) &&
        !IsIdentityTexTransform(outMat.diffuseTransform))
    {
        outMat.emissiveTransform = outMat.diffuseTransform;
    }

    if (!outMat.specularTextureName.empty() &&
        IsIdentityTexTransform(outMat.specularTransform) &&
        !IsIdentityTexTransform(outMat.diffuseTransform))
    {
        outMat.specularTransform = outMat.diffuseTransform;
    }

    if (!outMat.emissiveTextureName.empty() && IsNearlyBlack3(outMat.emissiveColor))
    {
        FillColor4(outMat.emissiveColor, 1.0, 1.0, 1.0, 1.0);
    }

    if (!outMat.specularTextureName.empty())
    {
        if (IsNearlyBlack3(outMat.specularColor))
        {
            outMat.specularColor[0] = 1.0f;
            outMat.specularColor[1] = 1.0f;
            outMat.specularColor[2] = 1.0f;
        }

        if (outMat.specularColor[3] <= 0.0f)
        {
            outMat.specularColor[3] = 32.0f;
        }
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
static int GetPolygonMaterialSlot(FbxNode* node, FbxMesh* mesh, int polygonIndex)
{
    if (!node || !mesh) return 0;

    const int nodeMaterialCount = node->GetMaterialCount();
    if (nodeMaterialCount <= 1) return 0;

    FbxGeometryElementMaterial* matElem = mesh->GetElementMaterial();
    if (!matElem) return 0;

    int localMaterialSlot = 0;

    if (matElem->GetMappingMode() == FbxGeometryElement::eByPolygon)
    {
        if (polygonIndex >= 0 && polygonIndex < matElem->GetIndexArray().GetCount())
            localMaterialSlot = matElem->GetIndexArray().GetAt(polygonIndex);
    }
    else if (matElem->GetMappingMode() == FbxGeometryElement::eAllSame)
    {
        if (matElem->GetIndexArray().GetCount() > 0)
            localMaterialSlot = matElem->GetIndexArray().GetAt(0);
    }
    else
    {
        localMaterialSlot = 0;
    }

    if (localMaterialSlot < 0 || localMaterialSlot >= nodeMaterialCount)
        localMaterialSlot = 0;

    return localMaterialSlot;
}
static void ComputeTangentForTri(Vertex& a, Vertex& b, Vertex& c)
{
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
        a.tangent[0] = b.tangent[0] = c.tangent[0] = 1.f;
        a.tangent[1] = b.tangent[1] = c.tangent[1] = 0.f;
        a.tangent[2] = b.tangent[2] = c.tangent[2] = 0.f;
        a.tangent[3] = b.tangent[3] = c.tangent[3] = 1.f;
        return;
    }

    float r = 1.0f / denom;

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
    auto Cross3 = [](float ax, float ay, float az, float bx, float by, float bz, float& rx, float& ry, float& rz)
        { rx = ay * bz - az * by; ry = az * bx - ax * bz; rz = ax * by - ay * bx; };

    auto FixOne = [&](Vertex& v)
        {
            float nx = v.normal[0], ny = v.normal[1], nz = v.normal[2];
            Normalize3(nx, ny, nz);

            float dotNT = Dot3(nx, ny, nz, tx, ty, tz);
            float tpx = tx - nx * dotNT;
            float tpy = ty - ny * dotNT;
            float tpz = tz - nz * dotNT;
            Normalize3(tpx, tpy, tpz);

            float cx, cy, cz;
            Cross3(nx, ny, nz, tpx, tpy, tpz, cx, cy, cz);
            float sign = (Dot3(cx, cy, cz, bx, by, bz) < 0.f) ? -1.f : 1.f;

            v.tangent[0] = tpx; v.tangent[1] = tpy; v.tangent[2] = tpz; v.tangent[3] = sign;
        };

    FixOne(a); FixOne(b); FixOne(c);
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
// 헤더 (boneCount=0)
// ==========================================================

static void WriteModelHeader()
{
    char magic[4] = { 'M', 'B', 'I', 'N' };
    WriteRaw(magic, 4);

    uint32_t version = 3;
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
            WriteFloatArray(v.tangent, 4);

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
        DLOG("diffuse=\""); DLOG(m.diffuseTextureName); DLOG("\" ");
        DLOG("normal=\"");  DLOG(m.normalTextureName);  DLOGLN("\"");

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

        const int nodeMaterialCount = std::max(1, node->GetMaterialCount());

        std::vector<SubMesh> splitSubMeshes(nodeMaterialCount);
        std::vector<bool> splitSubMeshUsed(nodeMaterialCount, false);

        for (int mi = 0; mi < nodeMaterialCount; ++mi)
        {
            uint32_t globalMaterialIndex = 0;

            if (mi < node->GetMaterialCount())
            {
                FbxSurfaceMaterial* mat = node->GetMaterial(mi);
                if (mat)
                {
                    auto it = g_MaterialNameToIndex.find(mat->GetName());
                    if (it != g_MaterialNameToIndex.end())
                        globalMaterialIndex = it->second;
                }
            }

            splitSubMeshes[mi].meshName = node->GetName();
            splitSubMeshes[mi].materialIndex = globalMaterialIndex;
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

        for (int mi = 0; mi < nodeMaterialCount; ++mi)
        {
            splitSubMeshes[mi].vertices.reserve(polyCount * 3);
            splitSubMeshes[mi].indices.reserve(polyCount * 3);
        }

        for (int p = 0; p < polyCount; ++p)
        {
            int order[3] = { 0,1,2 };
            int localMaterialSlot = GetPolygonMaterialSlot(node, mesh, p);
            if (localMaterialSlot < 0 || localMaterialSlot >= nodeMaterialCount)
                localMaterialSlot = 0;

            SubMesh& sm = splitSubMeshes[localMaterialSlot];
            splitSubMeshUsed[localMaterialSlot] = true;
            if (flip) std::swap(order[1], order[2]);

            Vertex triV[3]{};

            for (int k = 0; k < 3; ++k)
            {
                int vi = order[k];
                int cpIdx = mesh->GetPolygonVertex(p, vi);
                if (cpIdx < 0 || cpIdx >= cpCount) { triV[k] = Vertex{}; continue; }

                Vertex v{};
                for (int i = 0; i < 4; ++i) { v.boneIndices[i] = 0; v.boneWeights[i] = 0.0f; }

                // position bake
                FbxVector4 posL = cp[cpIdx];
                FbxVector4 posW = xform.MultT(posL);
                v.position[0] = (float)posW[0] * FINAL_SCALE_F;
                v.position[1] = (float)posW[1] * FINAL_SCALE_F;
                v.position[2] = (float)posW[2] * FINAL_SCALE_F;

                // normal bake
                FbxVector4 nL;
                mesh->GetPolygonVertexNormal(p, vi, nL);
                FbxVector4 nW = nMat.MultT(nL);
                nW.Normalize();
                v.normal[0] = (float)nW[0];
                v.normal[1] = (float)nW[1];
                v.normal[2] = (float)nW[2];

                // UV
                if (hasUVSet)
                {
                    FbxVector2 uv; bool unmapped = false;
                    if (mesh->GetPolygonVertexUV(p, vi, uvSetName, uv, unmapped))
                    {
                        v.uv[0] = (float)uv[0];
                        v.uv[1] = 1.0f - (float)uv[1];
                    }
                    else
                    {
                        v.uv[0] = v.uv[1] = 0.0f;
                    }
                }
                else
                {
                    v.uv[0] = v.uv[1] = 0.0f;
                }

                triV[k] = v;
            }

            // tangent 계산(현재 order가 이미 flip 반영됨)
            ComputeTangentForTri(triV[0], triV[1], triV[2]);

            // push
            uint32_t base = (uint32_t)sm.vertices.size();
            sm.vertices.push_back(triV[0]);
            sm.vertices.push_back(triV[1]);
            sm.vertices.push_back(triV[2]);

            sm.indices.push_back(base + 0);
            sm.indices.push_back(base + 1);
            sm.indices.push_back(base + 2);
        }


        for (int mi = 0; mi < nodeMaterialCount; ++mi)
        {
            SubMesh& sm = splitSubMeshes[mi];
            if (!splitSubMeshUsed[mi]) continue;
            if (sm.vertices.empty()) continue;

#if DEBUGLOG
            DLOG("[SubMesh] mesh=\""); DLOG(sm.meshName); DLOG("\" ");
            DLOG("materialIndex="); DLOG(sm.materialIndex);

            if (sm.materialIndex < g_Materials.size())
            {
                const auto& mat = g_Materials[sm.materialIndex];
                DLOG(" ("); DLOG(mat.name); DLOG(")");
                DLOG(" diffuse=\""); DLOG(mat.diffuseTextureName); DLOG("\"");
                DLOG(" normal=\""); DLOG(mat.normalTextureName); DLOG("\"");
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
