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
#include <functional>
#include <cfloat>   // FLT_MAX

using namespace std;
static constexpr float EXPORT_SCALE_F = 0.01f;
static constexpr bool MIRROR_X_EXPORT = true; // 모델 추출기와 동일 옵션

static constexpr double EXPORT_ROT_X_DEG = -90.0;
static constexpr double EXPORT_ROT_Y_DEG = 0.0;
static constexpr double EXPORT_ROT_Z_DEG = 0.0;

static FbxAMatrix MakeMirrorX()
{
    FbxAMatrix S; S.SetIdentity();
    S.SetRow(0, FbxVector4(-1, 0, 0, 0));
    S.SetRow(1, FbxVector4(0, 1, 0, 0));
    S.SetRow(2, FbxVector4(0, 0, 1, 0));
    S.SetRow(3, FbxVector4(0, 0, 0, 1));
    return S;
}

static FbxAMatrix MakeRotateX(double deg)
{
    FbxAMatrix R;
    R.SetIdentity();
    R.SetR(FbxVector4(deg, 0.0, 0.0, 0.0));
    return R;
}

static FbxAMatrix MakeRotateY(double deg)
{
    FbxAMatrix R;
    R.SetIdentity();
    R.SetR(FbxVector4(0.0, deg, 0.0, 0.0));
    return R;
}

static FbxAMatrix MakeRotateZ(double deg)
{
    FbxAMatrix R;
    R.SetIdentity();
    R.SetR(FbxVector4(0.0, 0.0, deg, 0.0));
    return R;
}

static FbxAMatrix BuildExportRotation()
{
    const FbxAMatrix Rx = MakeRotateX(EXPORT_ROT_X_DEG);
    const FbxAMatrix Ry = MakeRotateY(EXPORT_ROT_Y_DEG);
    const FbxAMatrix Rz = MakeRotateZ(EXPORT_ROT_Z_DEG);

    // 적용 순서: X -> Y -> Z
    return Rz * Ry * Rx;
}

// =========================================================
// 옵션
// 1: Skeleton(eSkeleton) 노드만 트랙 생성 (권장: 불필요 노드 트랙 방지)
// 0: 키가 있는 모든 노드 트랙 생성 (루트모션/더미노드 포함 가능)
// =========================================================
#define EXPORT_SKELETON_ONLY 0

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

static const char* SafeName(FbxNode* n)
{
    return (n && n->GetName() && n->GetName()[0] != '\0') ? n->GetName() : "null";
}

static const char* SafeNameStack(FbxAnimStack* s)
{
    return (s && s->GetName() && s->GetName()[0] != '\0') ? s->GetName() : "null";
}

static void PrintVec3(const char* tag, const FbxVector4& v)
{
    cout << tag << "=("
        << (double)v[0] << ","
        << (double)v[1] << ","
        << (double)v[2] << ")\n";
}

static void PrintQuat(const char* tag, const FbxQuaternion& q)
{
    cout << tag << "=("
        << (double)q[0] << ","
        << (double)q[1] << ","
        << (double)q[2] << ","
        << (double)q[3] << ")\n";
}

static double Det3x3(const FbxAMatrix& m)
{
    double a = m.Get(0, 0), b = m.Get(0, 1), c = m.Get(0, 2);
    double d = m.Get(1, 0), e = m.Get(1, 1), f = m.Get(1, 2);
    double g = m.Get(2, 0), h = m.Get(2, 1), i = m.Get(2, 2);
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
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

            static FbxAMatrix BasisRot = BuildExportRotation();
            static FbxAMatrix BasisRotInv = BasisRot.Inverse();
            fbxLocal = BasisRot * fbxLocal * BasisRotInv;

            if (MIRROR_X_EXPORT)
            {
                static FbxAMatrix MirrorX = MakeMirrorX();
                fbxLocal = MirrorX * fbxLocal * MirrorX;   // ★ 모델 추출기와 동일한 공액변환
            }

            FbxVector4 T = fbxLocal.GetT();
            FbxQuaternion Q = fbxLocal.GetQ();
            FbxVector4 Scale = fbxLocal.GetS();

            // quaternion 정규화(수치 안정성)
            Q.Normalize();

            KeyframeBin k{};
            k.timeSec = (float)((t.GetSecondDouble() - startSec) * timeScale);

            // [중요] 신버전 모델 추출기 기준: 추가 0.01 스케일 제거
            k.tx = (float)T[0] * EXPORT_SCALE_F;
            k.ty = (float)T[1] * EXPORT_SCALE_F;
            k.tz = (float)T[2] * EXPORT_SCALE_F;

            k.rx = (float)Q[0];
            k.ry = (float)Q[1];
            k.rz = (float)Q[2];
            k.rw = (float)Q[3];

            k.sx = (float)Scale[0];
            k.sy = (float)Scale[1];
            k.sz = (float)Scale[2];

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

static void DumpAnimExtractorDebug(
    const char* phaseTag,
    FbxScene* scene,
    FbxAnimStack* stack,
    FbxAnimLayer* layer,
    const FbxTimeSpan& span,
    const vector<TrackBin>* tracks,                       // 없으면 nullptr
    const unordered_map<string, int>* nameToTrack,         // 없으면 nullptr
    const vector<string>& probeBones,                     // 관심 본들(예: Hips/Hands)
    float timeScale)
{
    cout << "\n==================== [AnimDump] " << phaseTag << " ====================\n";

    // ---- Scene / Stack / Span
    cout << "[Scene] root=" << SafeName(scene ? scene->GetRootNode() : nullptr) << "\n";
    cout << "[Stack] name=" << SafeNameStack(stack) << " layer=" << (layer ? "ok" : "null") << "\n";

    const double s0 = span.GetStart().GetSecondDouble();
    const double s1 = span.GetStop().GetSecondDouble();
    cout << "[Span] start=" << s0 << " end=" << s1 << " dur=" << (s1 - s0) << " timeScale=" << timeScale << "\n";

    // ---- Skeleton 노드 개수/이름 샘플
    int skelCount = 0;
    vector<string> skelNames;
    function<void(FbxNode*)> dfs = [&](FbxNode* n)
        {
            if (!n) return;
            if (IsSkeletonNode(n))
            {
                skelCount++;
                if ((int)skelNames.size() < 20) skelNames.push_back(n->GetName());
            }
            for (int i = 0; i < n->GetChildCount(); ++i) dfs(n->GetChild(i));
        };
    dfs(scene ? scene->GetRootNode() : nullptr);

    cout << "[Skeleton] count=" << skelCount << " sample(<=20)=";
    for (size_t i = 0; i < skelNames.size(); ++i)
    {
        if (i) cout << ", ";
        cout << skelNames[i];
    }
    cout << "\n";

    // ---- 관심 본의 “로컬/글로벌”을 특정 시간에 찍기 (start/mid/end)
    auto DumpNodeAt = [&](FbxNode* n, const char* label, const FbxTime& t)
        {
            if (!n) { cout << "  [" << label << "] node=null\n"; return; }

            FbxAMatrix L = n->EvaluateLocalTransform(t);

            static FbxAMatrix BasisRot = BuildExportRotation();
            static FbxAMatrix BasisRotInv = BasisRot.Inverse();
            L = BasisRot * L * BasisRotInv;

            if (MIRROR_X_EXPORT) { static FbxAMatrix MirrorX = MakeMirrorX(); L = MirrorX * L * MirrorX; }

            FbxVector4 T = L.GetT();
            FbxQuaternion Q = L.GetQ(); Q.Normalize();
            FbxVector4 S = L.GetS();

            cout << "  [" << label << "] " << n->GetName()
                << " det3=" << Det3x3(L)
                << " T=(" << (double)T[0] << "," << (double)T[1] << "," << (double)T[2] << ")"
                << " S=(" << (double)S[0] << "," << (double)S[1] << "," << (double)S[2] << ")"
                << " Q=(" << (double)Q[0] << "," << (double)Q[1] << "," << (double)Q[2] << "," << (double)Q[3] << ")"
                << "\n";
        };

    if (scene && scene->GetRootNode())
    {
        FbxTime tStart = span.GetStart();
        FbxTime tEnd = span.GetStop();
        FbxTime tMid;  tMid.SetSecondDouble((s0 + s1) * 0.5);

        cout << "[ProbeBones] (local after mirror-conjugation)\n";
        for (const string& bn : probeBones)
        {
            // 이름으로 노드 찾기(간단 DFS)
            FbxNode* found = nullptr;
            function<void(FbxNode*)> findDfs = [&](FbxNode* n)
                {
                    if (!n || found) return;
                    if (bn == n->GetName()) { found = n; return; }
                    for (int i = 0; i < n->GetChildCount(); ++i) findDfs(n->GetChild(i));
                };
            findDfs(scene->GetRootNode());

            DumpNodeAt(found, "Start", tStart);
            DumpNodeAt(found, "Mid", tMid);
            DumpNodeAt(found, "End", tEnd);
        }
    }

    // ---- Track 결과(있으면) 요약 + 관심 본 key 샘플
    if (tracks && nameToTrack)
    {
        cout << "[Tracks] count=" << tracks->size() << "\n";

        // 길이 분포(최소/최대)와 상위 몇개 출력
        size_t minK = (size_t)-1, maxK = 0;
        string minN, maxN;
        for (auto& tr : *tracks)
        {
            size_t k = tr.keys.size();
            if (k < minK) { minK = k; minN = tr.boneName; }
            if (k > maxK) { maxK = k; maxN = tr.boneName; }
        }
        cout << "  keysMin=" << minK << " (" << minN << "), keysMax=" << maxK << " (" << maxN << ")\n";

        auto DumpTrackSample = [&](const string& bn)
            {
                auto it = nameToTrack->find(bn);
                if (it == nameToTrack->end()) { cout << "  [TrackSample] " << bn << " : NOT FOUND\n"; return; }
                const TrackBin& tr = (*tracks)[it->second];
                cout << "  [TrackSample] " << bn << " keys=" << tr.keys.size() << "\n";

                // 앞/중/뒤 1개씩 (있을 때)
                auto printK = [&](const KeyframeBin& k, const char* tag)
                    {
                        cout << "    " << tag << " t=" << k.timeSec
                            << " T=(" << k.tx << "," << k.ty << "," << k.tz << ")"
                            << " S=(" << k.sx << "," << k.sy << "," << k.sz << ")"
                            << " Q=(" << k.rx << "," << k.ry << "," << k.rz << "," << k.rw << ")"
                            << "\n";
                    };

                if (!tr.keys.empty())
                {
                    printK(tr.keys.front(), "first");
                    printK(tr.keys[tr.keys.size() / 2], "mid");
                    printK(tr.keys.back(), "last");
                }
            };

        cout << "[TrackProbe] (after export packing)\n";
        for (const string& bn : probeBones) DumpTrackSample(bn);
    }

    cout << "==================== [AnimDump End] ====================\n";
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

        vector<string> probe = { "Bind_Hips", "Bind_Spine", "Bind_LeftHand", "Bind_RightHand" };

        DumpAnimExtractorDebug(
            "PRE-EXTRACT",
            scene, stack, layer, timeSpan,
            nullptr, nullptr,      // tracks/nameToTrack 아직 없음
            probe,
            timeScale);


        TraverseAndExtractTracks(
            scene->GetRootNode(),
            layer,
            timeSpan,
            timeScale,
            tracks,
            nameToTrack);

        DumpAnimExtractorDebug(
            "POST-EXTRACT",
            scene, stack, layer, timeSpan,
            &tracks, &nameToTrack,
            probe,
            timeScale);

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
