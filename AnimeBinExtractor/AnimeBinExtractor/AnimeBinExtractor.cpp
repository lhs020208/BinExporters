#include <iostream>
#include <fbxsdk.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <fstream>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <cfloat>   // FLT_MAX

using namespace std;

// =========================================================
// 옵션
// 1: Skeleton(eSkeleton) 노드만 트랙 생성 (권장: 불필요 노드 트랙 방지)
// 0: 키가 있는 모든 노드 트랙 생성 (루트모션/더미노드 포함 가능)
// =========================================================
#define EXPORT_SKELETON_ONLY 1

// ======================================================================
// BIN 쓰기 헬퍼
// ======================================================================

static void WriteRaw(ofstream& out, const void* data, size_t size)
{
    out.write(reinterpret_cast<const char*>(data), size);
}

static void WriteUInt16(ofstream& out, uint16_t v) { WriteRaw(out, &v, sizeof(v)); }
static void WriteUInt32(ofstream& out, uint32_t v) { WriteRaw(out, &v, sizeof(v)); }
static void WriteInt32(ofstream& out, int32_t  v) { WriteRaw(out, &v, sizeof(v)); }
static void WriteFloat(ofstream& out, float    f) { WriteRaw(out, &f, sizeof(f)); }

static void WriteStringUtf8(ofstream& out, const std::string& s)
{
    uint16_t len = static_cast<uint16_t>(s.size());
    WriteUInt16(out, len);
    if (len > 0) WriteRaw(out, s.data(), len);
}

// ======================================================================
// 애니메이션용 임시 구조체
// ======================================================================

struct KeyframeBin
{
    float timeSec;

    float tx, ty, tz;
    float rx, ry, rz, rw;
    float sx, sy, sz;
};

struct TrackBin
{
    std::string boneName;
    std::vector<KeyframeBin> keys;
};

// ======================================================================
// 유틸
// ======================================================================

static bool IsSkeletonNode(FbxNode* node)
{
    if (!node) return false;
    FbxNodeAttribute* attr = node->GetNodeAttribute();
    return (attr && attr->GetAttributeType() == FbxNodeAttribute::eSkeleton);
}

// ======================================================================
// FBX 키 타임 수집: node의 T/R/S 커브에서 모든 키 시간을 outTimes에 넣는다
// ======================================================================

static void CollectKeyTimes(FbxNode* node, FbxAnimLayer* layer, std::set<FbxTime>& outTimes)
{
    if (!node || !layer) return;

    auto addCurve = [&](FbxAnimCurve* curve)
        {
            if (!curve) return;
            int keyCount = curve->KeyGetCount();
            for (int i = 0; i < keyCount; ++i)
                outTimes.insert(curve->KeyGetTime(i));
        };

    addCurve(node->LclTranslation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_X));
    addCurve(node->LclTranslation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Y));
    addCurve(node->LclTranslation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Z));

    addCurve(node->LclRotation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_X));
    addCurve(node->LclRotation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Y));
    addCurve(node->LclRotation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Z));

    addCurve(node->LclScaling.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_X));
    addCurve(node->LclScaling.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Y));
    addCurve(node->LclScaling.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Z));
}

// ======================================================================
// 재귀 순회하면서 각 노드(=본 이름)에 대해 TrackBin을 채운다
// ======================================================================

static void TraverseAndExtractTracks(
    FbxNode* node,
    FbxAnimLayer* layer,
    const FbxTimeSpan& timeSpan,
    float timeScale,
    std::vector<TrackBin>& tracks,
    std::unordered_map<std::string, int>& nameToTrack)
{
    if (!node) return;

    const bool isSkeleton = IsSkeletonNode(node);

    // 자식은 항상 재귀
    auto TraverseChildren = [&]()
        {
            const int childCount = node->GetChildCount();
            for (int i = 0; i < childCount; ++i)
            {
                TraverseAndExtractTracks(
                    node->GetChild(i),
                    layer,
                    timeSpan,
                    timeScale,
                    tracks,
                    nameToTrack);
            }
        };

#if EXPORT_SKELETON_ONLY
    if (!isSkeleton)
    {
        TraverseChildren();
        return;
    }
#endif

    // 이 노드에서 키가 있는지 확인
    std::set<FbxTime> keyTimes;
    CollectKeyTimes(node, layer, keyTimes);

    if (!keyTimes.empty())
    {
        const char* nodeNameC = node->GetName();
        std::string nodeName = (nodeNameC ? nodeNameC : "");

        int trackIndex = -1;
        auto it = nameToTrack.find(nodeName);
        if (it == nameToTrack.end())
        {
            trackIndex = (int)tracks.size();
            TrackBin tb;
            tb.boneName = nodeName;
            tracks.push_back(tb);
            nameToTrack[nodeName] = trackIndex;
        }
        else
        {
            trackIndex = it->second;
        }

        TrackBin& track = tracks[trackIndex];

        const double startSec = timeSpan.GetStart().GetSecondDouble();

        for (const FbxTime& t : keyTimes)
        {
            if (t < timeSpan.GetStart() || t > timeSpan.GetStop())
                continue;

            // 로컬 TRS (DirectX + meter 변환 이후 값)
            FbxAMatrix fbxLocal = node->EvaluateLocalTransform(t);

            FbxVector4     T = fbxLocal.GetT();
            FbxQuaternion  R = fbxLocal.GetQ();
            FbxVector4     S = fbxLocal.GetS();

            // quaternion 정규화(수치 안정성)
            R.Normalize();

            KeyframeBin k{};
            k.timeSec = (float)((t.GetSecondDouble() - startSec) * timeScale);

            // [중요] 신버전 모델 추출기 기준: 추가 0.01 스케일 제거
            float LENGTH_SCALE = 0.01f;
            k.tx = (float)T[0] * LENGTH_SCALE;
            k.ty = (float)T[1] * LENGTH_SCALE;
            k.tz = (float)T[2] * LENGTH_SCALE;

            k.rx = (float)R[0];
            k.ry = (float)R[1];
            k.rz = (float)R[2];
            k.rw = (float)R[3];

            k.sx = (float)S[0];
            k.sy = (float)S[1];
            k.sz = (float)S[2];

            track.keys.push_back(k);
        }

        // 시간순 정렬
        std::sort(track.keys.begin(), track.keys.end(),
            [](const KeyframeBin& a, const KeyframeBin& b)
            {
                return a.timeSec < b.timeSec;
            });
    }

    TraverseChildren();
}

// ======================================================================
// main: 애니메이션 FBX → 애니메이션 BIN(ABIN) 추출기
// ======================================================================

int main()
{
    string importDir = "import";
    string exportDir = "export";

    namespace fs = std::filesystem;

    // 출력 폴더 보장
    std::error_code ec;
    fs::create_directories(exportDir, ec);

    // FBX SDK 초기화
    FbxManager* manager = FbxManager::Create();
    if (!manager)
    {
        cout << "FBX Manager 생성 실패.\n";
        return -1;
    }

    FbxIOSettings* ioSettings = FbxIOSettings::Create(manager, IOSROOT);
    manager->SetIOSettings(ioSettings);

    // ================================================
    // import 폴더 순회: *.fbx 전부 애니메이션 BIN 추출
    // ================================================
    for (const auto& entry : fs::directory_iterator(importDir))
    {
        if (!entry.is_regular_file()) continue;

        fs::path path = entry.path();
        if (path.extension() != ".fbx") continue;

        string name = path.stem().string();
        string fbxFileName = path.string();
        string binFileName = exportDir + "/" + name + ".bin";

        cout << "\n==========================================\n";
        cout << "처리 중: " << fbxFileName << "\n";

        // FBX Import
        FbxImporter* importer = FbxImporter::Create(manager, "");
        if (!importer->Initialize(fbxFileName.c_str(), -1, manager->GetIOSettings()))
        {
            cout << "FBX 파일을 열 수 없습니다: " << fbxFileName << "\n";
            importer->Destroy();
            continue;
        }

        FbxScene* scene = FbxScene::Create(manager, ("AnimScene_" + name).c_str());
        importer->Import(scene);
        importer->Destroy();

        // DirectX 좌표계 + meter 단위로 변환
        FbxAxisSystem::DirectX.ConvertScene(scene);
        FbxSystemUnit::m.ConvertScene(scene);

        // -----------------------------
        // AnimStack / AnimLayer / TimeSpan
        // -----------------------------
        FbxAnimStack* stack = scene->GetCurrentAnimationStack();
        if (!stack && scene->GetSrcObjectCount<FbxAnimStack>() > 0)
            stack = scene->GetSrcObject<FbxAnimStack>(0);

        if (!stack)
        {
            cout << "애니메이션 스택이 없습니다.\n";
            scene->Destroy();
            continue;
        }

        scene->SetCurrentAnimationStack(stack);

        FbxTimeSpan timeSpan = stack->GetLocalTimeSpan();
        FbxAnimLayer* layer = stack->GetMember<FbxAnimLayer>(0);
        if (!layer)
        {
            cout << "AnimLayer가 없습니다.\n";
            scene->Destroy();
            continue;
        }

        const float timeScale = 1.0f;
        const double startSec = timeSpan.GetStart().GetSecondDouble();
        const double endSec = timeSpan.GetStop().GetSecondDouble();
        float duration = (float)((endSec - startSec) * timeScale);

        // 클립 이름
        string clipName;
        const char* stackNameC = stack->GetName();
        if (stackNameC && stackNameC[0] != '\0') clipName = stackNameC;
        else                                     clipName = name;

        // -----------------------------
        // Track 추출
        // -----------------------------
        vector<TrackBin> tracks;
        unordered_map<string, int> nameToTrack;

        TraverseAndExtractTracks(
            scene->GetRootNode(),
            layer,
            timeSpan,
            timeScale,
            tracks,
            nameToTrack);

        if (tracks.empty())
        {
            cout << "키프레임이 존재하지 않습니다.\n";
            scene->Destroy();
            continue;
        }

        // -----------------------------
        // 0초(T포즈) 제거: minTime 만큼 전체 shift
        // -----------------------------
        float minTime = FLT_MAX;
        for (auto& tr : tracks)
            for (auto& k : tr.keys)
                if (k.timeSec > 0.0f && k.timeSec < minTime)
                    minTime = k.timeSec;

        if (minTime != FLT_MAX)
        {
            for (auto& tr : tracks)
            {
                for (auto& k : tr.keys)
                    k.timeSec -= minTime;

                tr.keys.erase(
                    remove_if(tr.keys.begin(), tr.keys.end(),
                        [](const KeyframeBin& k) { return k.timeSec < 0.0f; }),
                    tr.keys.end());
            }

            duration -= minTime;
            if (duration < 0.0f) duration = 0.0f;
        }

        // -----------------------------
        // BIN 저장
        // -----------------------------
        ofstream out(binFileName, std::ios::binary);
        if (!out.is_open())
        {
            cout << "BIN 파일 생성 실패: " << binFileName << "\n";
            scene->Destroy();
            continue;
        }

        // Header
        char magic[4] = { 'A','B','I','N' };
        WriteRaw(out, magic, 4);
        WriteUInt32(out, 1); // version

        // Clip
        WriteStringUtf8(out, clipName);
        WriteFloat(out, duration);

        WriteUInt32(out, (uint32_t)tracks.size());

        for (auto& tr : tracks)
        {
            WriteStringUtf8(out, tr.boneName);
            WriteInt32(out, -1); // boneIndex placeholder (런타임에서 이름 매핑 권장)

            WriteUInt32(out, (uint32_t)tr.keys.size());

            for (auto& k : tr.keys)
            {
                WriteFloat(out, k.timeSec);

                WriteFloat(out, k.tx);
                WriteFloat(out, k.ty);
                WriteFloat(out, k.tz);

                WriteFloat(out, k.rx);
                WriteFloat(out, k.ry);
                WriteFloat(out, k.rz);
                WriteFloat(out, k.rw);

                WriteFloat(out, k.sx);
                WriteFloat(out, k.sy);
                WriteFloat(out, k.sz);
            }
        }

        out.close();
        cout << "애니메이션 BIN 생성 완료: " << binFileName << "\n";

        scene->Destroy();
    }

    manager->Destroy();
    return 0;
}
