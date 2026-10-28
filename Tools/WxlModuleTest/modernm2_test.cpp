// Host-side test of ModernM2Load.cpp / ModernM2Skin.cpp against the real M2Data / CM2Shared headers.
// Build and run it with run_modernm2_tests.sh. It builds synthetic MD21 files and skins, so it needs no game data.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Engine/Source/Model2/ModernM2/ModernM2.h"
#include "Engine/Source/WXL/Services.h"
#include "Engine/Source/WXL/Log.h"
#define WXL_ALLOW_32BIT_HEADER 1
#include "Engine/Source/WXL/Sdk/wxl_listfile_api.h"

// ---- stubs for engine services the module calls
namespace SIMD
{
    void* AllocA16(uint32_t size, const char*, int32_t) { return std::aligned_alloc(16, (size + 15) & ~15u); }
    void FreeA16(void* p) { std::free(p); }
}
void* OsAllocNear(const void*, size_t size) { return std::aligned_alloc(16, (size + 15) & ~size_t(15)); }
void OsFreeNear(void* p) { std::free(p); }
namespace wxl
{
    void LogCat(const char* cat, const char* fmt, ...)
    {
        std::printf("    [log %s] ", cat); va_list a; va_start(a, fmt); std::vprintf(fmt, a); va_end(a); std::printf("\n");
    }
    bool LogThrottle(const char*, unsigned, unsigned) { return true; }
}

static int g_fail = 0, g_pass = 0;
#define CHECK(c, msg) do { if (c) { ++g_pass; std::printf("  [ok] %s\n", msg); } else { ++g_fail; std::printf("  [FAIL] %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

// ---- fake listfile service
static int FakeResolve(uint32_t id, uint32_t kind, char* out, size_t cap)
{
    if (id == 100) { std::snprintf(out, cap, "world\\generic\\tex100.blp"); return WXL_LISTFILE_FOUND; }
    std::snprintf(out, cap, "WXL\\NotFound%s", kind == WXL_LISTFILE_TEXTURE ? ".blp" : "");
    return WXL_LISTFILE_NOTFOUND;
}
static WxlListfileServices g_listfile = { sizeof(WxlListfileServices), nullptr, &FakeResolve, nullptr, nullptr, nullptr, nullptr };

// ---- synthetic file builders
static void Put32(std::vector<uint8_t>& v, size_t at, uint32_t x) { std::memcpy(&v[at], &x, 4); }
static void Put16(std::vector<uint8_t>& v, size_t at, uint16_t x) { std::memcpy(&v[at], &x, 2); }
static void PutF(std::vector<uint8_t>& v, size_t at, float x) { std::memcpy(&v[at], &x, 4); }
static uint32_t Get32(const uint8_t* p) { uint32_t x; std::memcpy(&x, p, 4); return x; }
static uint16_t Get16(const uint8_t* p) { uint16_t x; std::memcpy(&x, p, 2); return x; }
static float GetF(const uint8_t* p) { float x; std::memcpy(&x, p, 4); return x; }

struct Fixture
{
    std::vector<uint8_t> file;
    uint32_t partOff, camOff, seqOff, matOff, texOff;
};

static Fixture BuildModernFile(uint32_t version)
{
    const uint32_t hdr = sizeof(M2Data);
    const uint32_t seqOff = hdr, seqBytes = 2 * sizeof(M2Sequence);
    const uint32_t matOff = seqOff + seqBytes, matBytes = 2 * sizeof(M2Material);
    const uint32_t texOff = matOff + matBytes, texBytes = 3 * sizeof(M2Texture);
    const uint32_t partOff = (texOff + texBytes + 15) & ~15u, partBytes = 2 * 0x1FC;
    const uint32_t camOff = partOff + partBytes, camBytes = 2 * 0x74;
    const uint32_t bodySize = camOff + camBytes;

    Fixture f;
    f.partOff = partOff; f.camOff = camOff; f.seqOff = seqOff; f.matOff = matOff; f.texOff = texOff;
    std::vector<uint8_t> body(bodySize, 0);
    Put32(body, 0, 0x3032444D);                 // 'MD20'
    Put32(body, offsetof(M2Data, m_version), version);
    auto setArr = [&](size_t at, uint32_t count, uint32_t off) { Put32(body, at, count); Put32(body, at + 4, off); };
    setArr(offsetof(M2Data, m_sequences), 2, seqOff);
    setArr(offsetof(M2Data, m_materials), 2, matOff);
    setArr(offsetof(M2Data, m_textures), 3, texOff);
    setArr(offsetof(M2Data, m_particles), 2, partOff);
    setArr(offsetof(M2Data, m_cameras), 2, camOff);
    Put32(body, offsetof(M2Data, m_numSkinProfiles), 7); // LDV1-era count, must be clamped

    // sequences: #0 inline keys, #1 external (flags 0), source id 564 -> client 37, packed blend in|out
    auto* seqs = reinterpret_cast<M2Sequence*>(&body[seqOff]);
    seqs[0].m_id = 0;   seqs[0].m_flags = 0x20; seqs[0].m_blendTime = 0x00030002;
    seqs[1].m_id = 564; seqs[1].m_flags = 0x00; seqs[1].m_blendTime = 0x00050004;
    auto* mats = reinterpret_cast<M2Material*>(&body[matOff]);
    mats[0].m_flags = 0xFFFF; mats[0].m_blendMode = 9;
    mats[1].m_flags = 0x0003; mats[1].m_blendMode = 2;
    auto* texs = reinterpret_cast<M2Texture*>(&body[texOff]);
    texs[0].m_type = 0; texs[1].m_type = 0; texs[2].m_type = 11;

    // particles (modern stride): flags at +4, texture at +0x16, marker at +0x10
    for (uint32_t i = 0; i < 2; ++i)
    {
        const size_t r = partOff + i * 0x1FC;
        Put32(body, r + 0x10, 0xA0A00000u + i);
        Put32(body, r + 0x1D8, 0xBEEF0000u + i);                  // last client-shape dword
        Put32(body, r + 0x1DC, 0xDEAD0000u + i);                  // trailing multi-texture block
    }
    Put32(body, partOff + 4, 0x10000000);                          // multi-texture flag
    Put16(body, partOff + 0x16, 2 | (1 << 5) | (3 << 10));         // packed layers: first = 2
    Put16(body, partOff + 0x1FC + 0x16, 1);

    // cameras (modern stride): type, far, near, body marker, trailing fov track (empty)
    for (uint32_t i = 0; i < 2; ++i)
    {
        const size_t r = camOff + i * 0x74;
        Put32(body, r + 0x00, 1 + i);                 // type
        PutF(body, r + 0x04, 100.0f + i);             // far
        PutF(body, r + 0x08, 0.5f + i);               // near
        Put32(body, r + 0x0C, 0xC0DE0000u + i);       // first body dword
        Put32(body, r + 0x0C + 0x50, 0xFACE0000u + i); // last body dword
    }

    // container: MD21 chunk then TXID
    auto& out = f.file;
    auto chunk = [&](const char* tag, const std::vector<uint8_t>& payload)
    {
        const size_t at = out.size(); out.resize(at + 8 + payload.size());
        std::memcpy(&out[at], tag, 4); Put32(out, at + 4, static_cast<uint32_t>(payload.size()));
        std::memcpy(&out[at + 8], payload.data(), payload.size());
    };
    chunk("MD21", body);
    std::vector<uint8_t> txid(12); Put32(txid, 0, 100); Put32(txid, 4, 200); Put32(txid, 8, 300);
    chunk("TXID", txid);
    return f;
}

static CM2Shared* MakeShared(const std::vector<uint8_t>& file, const char* name)
{
    void* mem = std::calloc(1, sizeof(CM2Shared));
    auto* sh = static_cast<CM2Shared*>(mem);
    sh->m_data = static_cast<M2Data*>(SIMD::AllocA16(static_cast<uint32_t>(file.size()), __FILE__, __LINE__));
    std::memcpy(sh->m_data, file.data(), file.size());
    sh->m_fileSize = static_cast<uint32_t>(file.size());
    std::snprintf(sh->m_fileName, sizeof sh->m_fileName, "%s", name);
    return sh;
}

static void TestLoad()
{
    std::printf("modern container load\n");
    wxl::services::Register(WXL_SERVICE_LISTFILE, &g_listfile, sizeof g_listfile);

    Fixture f = BuildModernFile(272);
    CM2Shared* sh = MakeShared(f.file, "Creature\\Test\\Test.m2");
    auto result = wxl::modernm2::PrepareLoad(sh);
    CHECK(result == wxl::modernm2::LoadResult::Modern, "container recognized");
    const uint8_t* b = reinterpret_cast<const uint8_t*>(sh->m_data);
    CHECK(Get32(b) == 0x3032444D && Get32(b + offsetof(M2Data, m_version)) == 272, "flat MD20 body at base, version kept");
    CHECK(sh->m_fileSize == f.camOff + 2 * 0x74, "file size is the body size (container header and trailing chunks dropped)");
    CHECK(sh->m_data->m_numSkinProfiles == 1, "oversized skin-profile count clamped");

    const auto* seqs = reinterpret_cast<const M2Sequence*>(b + f.seqOff);
    CHECK(seqs[0].m_blendTime == 0x0002 && seqs[1].m_blendTime == 0x0004, "split blend times masked to the single value");
    CHECK(seqs[1].m_id == 37, "source anim id 564 remapped to client 37");
    const auto* mats = reinterpret_cast<const M2Material*>(b + f.matOff);
    CHECK(mats[0].m_blendMode == 4 && (mats[0].m_flags & 0x5) == 0x5 && (mats[0].m_flags & ~0x1Fu) == 0, "out-of-table blend mode clamped to Add, flags masked");
    CHECK(mats[1].m_blendMode == 2 && mats[1].m_flags == 3, "in-range material untouched");

    // particles: shrunk to the client stride in place
    const uint8_t* p0 = b + f.partOff;
    const uint8_t* p1 = b + f.partOff + 0x1DC;
    CHECK(Get32(p0 + 0x10) == 0xA0A00000u && Get32(p1 + 0x10) == 0xA0A00001u, "particle records re-strided to 0x1DC");
    CHECK(Get32(p1 + 0x1D8) == 0xBEEF0001u, "second particle's last client dword carried");
    CHECK(Get16(p0 + 0x16) == 2, "multi-texture packed id reduced to its first layer");
    CHECK(Get16(p1 + 0x16) == 1, "single-texture id untouched");

    // cameras: fov float inserted, body shifted one float
    const uint8_t* c0 = b + f.camOff;
    const uint8_t* c1 = b + f.camOff + 0x64;
    CHECK(Get32(c0) == 1 && Get32(c1) == 2, "camera types carried");
    CHECK(GetF(c0 + 0x08) == 100.0f && GetF(c0 + 0x0C) == 0.5f, "camera far/near clip moved to the client slots");
    CHECK(GetF(c0 + 0x04) > 0.78f && GetF(c0 + 0x04) < 0.79f, "camera with no addressable fov key gets the 45-degree default");
    CHECK(Get32(c0 + 0x10) == 0xC0DE0000u && Get32(c1 + 0x10) == 0xC0DE0001u, "camera track bodies re-strided to 0x64");
    CHECK(Get32(c1 + 0x10 + 0x50) == 0xFACE0001u, "second camera body complete");

    // TXID -> listfile
    char path[260];
    M2Texture hard{}; hard.m_type = 0; M2Texture dyn{}; dyn.m_type = 11;
    CHECK(wxl::modernm2::TextureName(sh, 0, hard, path, sizeof path) && std::strcmp(path, "world\\generic\\tex100.blp") == 0, "TXID[0] resolved through the listfile service");
    CHECK(wxl::modernm2::TextureName(sh, 1, hard, path, sizeof path) && std::strcmp(path, "WXL\\NotFound.blp") == 0, "unknown id yields the listfile's not-found texture name");
    CHECK(!wxl::modernm2::TextureName(sh, 2, dyn, path, sizeof path), "dynamic texture slot is not given a file");
    CHECK(!wxl::modernm2::TextureName(sh, 5, hard, path, sizeof path), "index past the TXID list: no answer");

    wxl::modernm2::Destroy(sh);
    SIMD::FreeA16(sh->m_data);
    std::free(sh);

    // refusals
    std::printf("refusals\n");
    Fixture bad = BuildModernFile(264 + 3);                         // 267: outside the window
    CM2Shared* s2 = MakeShared(bad.file, "x.m2");
    CHECK(wxl::modernm2::PrepareLoad(s2) == wxl::modernm2::LoadResult::Failed, "version outside the window refused");
    std::free(s2->m_data); std::free(s2);

    std::vector<uint8_t> stock(64, 0); Put32(stock, 0, 0x3032444D);
    CM2Shared* s3 = MakeShared(stock, "stock.m2");
    CHECK(wxl::modernm2::PrepareLoad(s3) == wxl::modernm2::LoadResult::NotModern && !s3->m_wxl, "plain MD20 left alone, no state");
    std::free(s3->m_data); std::free(s3);

    Fixture trunc = BuildModernFile(272); trunc.file.resize(trunc.file.size() / 2);
    CM2Shared* s4 = MakeShared(trunc.file, "t.m2");
    CHECK(wxl::modernm2::PrepareLoad(s4) == wxl::modernm2::LoadResult::Failed, "truncated container refused");
    std::free(s4->m_data); std::free(s4);
}

// ---- skin
struct SkinFixture { std::vector<uint8_t> block; };

static SkinFixture BuildSkin(uint32_t verts, uint32_t indices, uint32_t sections, uint32_t batches)
{
    const uint32_t hdr = sizeof(M2SkinProfile);
    size_t at = (hdr + 15) & ~15u;
    const size_t offV = at;  at += verts * 2;       at = (at + 15) & ~15u;
    const size_t offI = at;  at += indices * 2;     at = (at + 15) & ~15u;
    const size_t offB = at;  at += verts * 4;       at = (at + 15) & ~15u;
    const size_t offS = at;  at += sections * sizeof(M2SkinSection); at = (at + 15) & ~15u;
    const size_t offT = at;  at += batches * sizeof(M2Batch);        at = (at + 15) & ~15u;
    SkinFixture f; f.block.assign(at, 0);
    auto* sk = reinterpret_cast<M2SkinProfile*>(f.block.data());
    sk->m_magic = 0x4E494B53;
    sk->m_vertices.m_count = verts;  sk->m_vertices.Set(reinterpret_cast<uint16_t*>(&f.block[offV]));
    sk->m_indices.m_count = indices; sk->m_indices.Set(reinterpret_cast<uint16_t*>(&f.block[offI]));
    sk->m_bones.m_count = verts;     sk->m_bones.Set(reinterpret_cast<ubyte4*>(&f.block[offB]));
    sk->m_skinSections.m_count = sections; sk->m_skinSections.Set(reinterpret_cast<M2SkinSection*>(&f.block[offS]));
    sk->m_batches.m_count = batches; sk->m_batches.Set(reinterpret_cast<M2Batch*>(&f.block[offT]));
    sk->m_boneCountMax = 256;
    return f;
}

static void TestSkinSplit()
{
    std::printf("skin finalize: bone split + modern contract\n");
    Fixture f = BuildModernFile(272);
    CM2Shared* sh = MakeShared(f.file, "Creature\\Big\\Big.m2");
    CHECK(wxl::modernm2::PrepareLoad(sh) == wxl::modernm2::LoadResult::Modern, "loaded");
    M2Data* md = sh->m_data;

    // relocate the arrays the finalize reads (what the stock walk does), and give the model 100 bone combos
    uint8_t* base = reinterpret_cast<uint8_t*>(md);
    const uint32_t reserve = (sh->m_fileSize + 15) & ~15u;
    (void)reserve;
    md->m_materials.Relocate(base);
    md->m_textures.Relocate(base);
    // bone combos live in the body's own tail would need a larger fixture: park them in a malloc'd region by
    // using a far block the way the reader does, here simply through the reserve at the end of the model block
    // (the reader's reserve starts after the body; take the last 512 bytes so it cannot collide).
    uint16_t* combos = reinterpret_cast<uint16_t*>(base + reserve + 4096 - 256);
    for (int i = 0; i < 100; ++i) combos[i] = static_cast<uint16_t>(i + 10);
    md->m_boneCombos.m_count = 100; md->m_boneCombos.Set(combos);

    const uint32_t V = 300, T = 100; // 100 triangles over 300 vertices
    SkinFixture sf = BuildSkin(V, T * 3, 1, 2);
    auto* sk = reinterpret_cast<M2SkinProfile*>(sf.block.data());
    for (uint32_t i = 0; i < V; ++i)
    {
        sk->m_vertices.Data()[i] = static_cast<uint16_t>(i);
        ubyte4& b = sk->m_bones.Data()[i];
        for (int j = 0; j < 4; ++j) b.b[j] = static_cast<uint8_t>((i + j * 7) % 100);
    }
    for (uint32_t i = 0; i < T * 3; ++i) sk->m_indices.Data()[i] = static_cast<uint16_t>(i);
    M2SkinSection& sec = sk->m_skinSections.Data()[0];
    sec.m_vertexCount = V; sec.m_indexCount = T * 3; sec.m_boneCount = 100; sec.m_boneComboIndex = 0; sec.m_boneInfluences = 4;
    M2Batch* batches = sk->m_batches.Data();
    batches[0].m_shader = 0x8000;            // source effect 0 (env) 
    batches[0].m_textureCount = 2; batches[0].m_skinSectionIndex = 0;
    batches[1].m_shader = 0x4011;            // packed two-texture blend
    batches[1].m_textureCount = 2; batches[1].m_skinSectionIndex = 0;

    sh->m_skinData = static_cast<M2SkinProfile*>(SIMD::AllocA16(static_cast<uint32_t>(sf.block.size()), __FILE__, __LINE__));
    std::memcpy(sh->m_skinData, sf.block.data(), sf.block.size());
    const M2SkinProfile* oldSkin = sh->m_skinData;
    (void)oldSkin;

    wxl::modernm2::FinalizeSkin(sh, static_cast<uint32_t>(sf.block.size()));

    sk = sh->m_skinData;
    const uint32_t n = sk->m_skinSections.m_count;
    CHECK(n > 1, "the 100-bone submesh was split into several draws");
    bool allUnder = true; uint32_t idxTotal = 0, vertTotal = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        const M2SkinSection& s = sk->m_skinSections.Data()[i];
        if (s.m_boneCount > wxl::modernm2::kMaxBonesPerDraw) allUnder = false;
        idxTotal += s.m_indexCount; vertTotal += s.m_vertexCount;
    }
    CHECK(allUnder, "every sub-section holds at most 75 bones");
    CHECK(idxTotal == T * 3, "all triangles survive the split");
    CHECK(sk->m_batches.m_count == n * 2, "each original batch repeated once per sub-section");

    // bone remap correctness: every vertex keeps the same global bones
    bool bonesOk = true, idxOk = true;
    const uint16_t* newCombos = md->m_boneCombos.Data();
    for (uint32_t i = 0; i < n && bonesOk; ++i)
    {
        const M2SkinSection& s = sk->m_skinSections.Data()[i];
        for (uint32_t v = s.m_vertexStart; v < s.m_vertexStart + s.m_vertexCount; ++v)
        {
            const uint32_t orig = sk->m_vertices.Data()[v];
            for (int j = 0; j < 4; ++j)
            {
                const uint32_t want = (orig + j * 7) % 100 + 10;
                const uint32_t got = newCombos[s.m_boneComboIndex + sk->m_bones.Data()[v].b[j]];
                if (got != want) { bonesOk = false; break; }
            }
        }
        for (uint32_t k = 0; k < s.m_indexCount; ++k)
        {
            const uint32_t v = sk->m_indices.Data()[s.m_indexStart + k];
            if (v < s.m_vertexStart || v >= s.m_vertexStart + s.m_vertexCount) idxOk = false;
        }
    }
    CHECK(bonesOk, "every vertex still resolves to its original global bones through the new combos");
    CHECK(idxOk, "every index stays inside its sub-section's vertex block");

    // contract rebuild
    CHECK((md->m_flags & 0x8) != 0 && md->m_textureCoordCombos.m_count > 0 && md->m_textureCombinerCombos.m_count > 0,
          "texture-unit lookups and combiner combos synthesized, combiner flag set");
    const M2Batch& b0 = sk->m_batches.Data()[0];
    const M2Batch& b1 = sk->m_batches.Data()[1];
    CHECK(b0.m_shader == (0x8000 | 1), "source env effect 0 re-based to engine effect 1");
    CHECK(b1.m_textureCount == 2 && b1.m_shader != 0x4011, "packed blend shader id decoded into a combiner entry");
    CHECK(sh->m_billboardBoneCount >= 1, "shadow animate gate forced");
    for (uint32_t i = 0; i < n; ++i)
        if (sk->m_skinSections.Data()[i].m_boneInfluences == 0) { CHECK(false, "boneInfluences lifted"); break; }

    wxl::modernm2::Destroy(sh);
    SIMD::FreeA16(sh->m_skinData); SIMD::FreeA16(sh->m_data); std::free(sh);
}

static void TestExtendedStart()
{
    std::printf("skin finalize: extended index start\n");
    std::vector<uint8_t> stock(64, 0); Put32(stock, 0, 0x3032444D);
    CM2Shared* sh = MakeShared(stock, "Character\\Human\\Male\\HumanMale.m2");
    // minimal model data block big enough for the header
    std::free(sh->m_data);
    sh->m_data = static_cast<M2Data*>(SIMD::AllocA16(sizeof(M2Data) + 64, __FILE__, __LINE__));
    std::memset(sh->m_data, 0, sizeof(M2Data) + 64);

    const uint32_t I = 66000;
    SkinFixture sf = BuildSkin(300, I, 2, 0);
    auto* sk = reinterpret_cast<M2SkinProfile*>(sf.block.data());
    M2SkinSection* s = sk->m_skinSections.Data();
    s[0].m_indexCount = 300; s[0].m_indexStart = 0x0010; s[0].m_skinSectionId = (1u << 16) | 5; // start 0x10010, id 5
    s[0].m_boneCount = 10; s[0].m_boneInfluences = 4;
    s[1].m_indexCount = 300; s[1].m_indexStart = 0x0200; s[1].m_skinSectionId = 7;               // start 0x200, id 7
    s[1].m_boneCount = 10; s[1].m_boneInfluences = 0;

    sh->m_skinData = static_cast<M2SkinProfile*>(SIMD::AllocA16(static_cast<uint32_t>(sf.block.size()), __FILE__, __LINE__));
    std::memcpy(sh->m_skinData, sf.block.data(), sf.block.size());
    wxl::modernm2::FinalizeSkin(sh, static_cast<uint32_t>(sf.block.size()));

    sk = sh->m_skinData;
    CHECK(sh->m_wxl && sh->m_wxl->sourceStart.size() == 2, "full starts kept for the model");
    CHECK(sh->m_wxl->sourceStart[0] == 0x10010u && sh->m_wxl->sourceStart[1] == 0x200u, "high half taken from the section level");
    CHECK((sk->m_skinSections.Data()[0].m_skinSectionId >> 16) == 0 && (sk->m_skinSections.Data()[0].m_skinSectionId & 0xFFFF) == 5,
          "section id no longer carries the level (geoset ids stay plain)");
    CHECK(sk->m_skinSections.Data()[1].m_boneInfluences == 1, "boneInfluences 0 lifted to 1");
    sh->m_skinSections = sk->m_skinSections.Data(); // the shared's copy, as InitializeSkinProfile makes it
    sh->m_wxl->sectionStart = sh->m_wxl->sourceStart;
    CHECK(wxl::modernm2::SectionIndexStart(sh, &sh->m_skinSections[0]) == 0x10010u, "draw start of section 0 is the full 32-bit value");
    CHECK(wxl::modernm2::SkinIndexStart(sh, 1, &sk->m_skinSections.Data()[1]) == 0x200u, "file-order lookup agrees");

    wxl::modernm2::Destroy(sh);
    SIMD::FreeA16(sh->m_skinData); SIMD::FreeA16(sh->m_data); std::free(sh);
}

static void TestAnimAndCaps()
{
    std::printf("anim unwrap, caps\n");
    std::vector<uint8_t> a(8 + 16, 0);
    std::memcpy(&a[0], "AFM2", 4); Put32(a, 4, 16);
    for (int i = 0; i < 16; ++i) a[8 + i] = static_cast<uint8_t>(0x40 + i);
    uint32_t size = static_cast<uint32_t>(a.size());
    CHECK(wxl::modernm2::UnwrapAnim(a.data(), size) && size == 16 && a[0] == 0x40 && a[15] == 0x4F, "AFM2 payload slid onto the buffer base");
    std::vector<uint8_t> flat(16, 7); uint32_t fs = 16;
    CHECK(!wxl::modernm2::UnwrapAnim(flat.data(), fs) && fs == 16 && flat[0] == 7, "a flat anim file is left alone");
    std::vector<uint8_t> lie(8 + 4, 0); std::memcpy(&lie[0], "AFM2", 4); Put32(lie, 4, 9999); uint32_t ls = 12;
    CHECK(!wxl::modernm2::UnwrapAnim(lie.data(), ls), "chunk size past the file refused");
    CHECK(wxl::modernm2::DoodadInstanceCap(10) == 7 && wxl::modernm2::DoodadInstanceCap(100) == 1 && wxl::modernm2::DoodadInstanceCap(75) == 1,
          "doodad instance cap: 75 / bones, at least 1");
}

int main()
{
    TestLoad();
    TestSkinSplit();
    TestExtendedStart();
    TestAnimAndCaps();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
