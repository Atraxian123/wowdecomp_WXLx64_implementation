/*
 * WarcraftXL module API (ABI 1), for the 64-bit client.
 * Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
 *
 * A module is a DLL in <client folder>\WXL\Modules\ that talks to the core through the table below and
 * through nothing else: no engine header, no import library of the client, no C++ type crosses the
 * boundary, so a module never has to be rebuilt because the engine's structs changed, and it can be
 * built with any compiler that makes 64-bit Windows DLLs.
 *
 * What a module exports (see WXL_DEFINE_MODULE_ABI below for the first one):
 *
 *     uint32_t WxlModuleAbiVersion(void);                        the ABI it was built against
 *     int      WxlModuleInit(const WxlApi*, WxlModuleInfo*);     0 = loaded, anything else = refuse
 *     void     WxlModuleShutdown(void);                          optional, called at exit
 * all declared WXL_MODULE_API (unmangled C names).
 *
 * Rules of the boundary
 *   - Only C types cross it. Memory is freed by the side that allocated it: a buffer the core hands out
 *     (readFile) goes back through freeBuffer, never free() / delete in the module.
 *   - Strings are UTF-8 / ASCII, NUL-terminated, valid only for the duration of the call.
 *   - Exceptions must not leave a module function or a handler.
 *   - Handlers run on whatever thread the engine fires the event on (the animate threads, for
 *     OnBuildBonePalette): a module guards its own state.
 *   - A virtual file provider copies its bytes into a buffer the core allocates (WxlServeSink).
 *   - The table only ever grows at its end. A module checks WXL_API_HAS(api, member) before it calls a
 *     member that is newer than ABI 1.0.
 *   - A model "instance" and a loaded "model" are opaque handles: a module compares them, keeps them as
 *     map keys, hands them back to the API, and never dereferences them.
 */
#ifndef WXL_API_H
#define WXL_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The module API is for the 64-bit client only. (A 32-bit build of the client defines
 * WXL_ALLOW_32BIT_HEADER to compile the declarations; it neither builds the API table nor loads a module.) */
#ifndef WXL_ALLOW_32BIT_HEADER
#  ifdef __cplusplus
static_assert(sizeof(void*) == 8, "WXL modules are 64-bit only");
#  else
_Static_assert(sizeof(void*) == 8, "WXL modules are 64-bit only");
#  endif
#endif

#define WXL_ABI_VERSION 1u

#if defined(_WIN32)
#  define WXL_EXPORT __declspec(dllexport)
#else
#  define WXL_EXPORT __attribute__((visibility("default")))
#endif
/* a module's exported functions: unmangled C names, whatever language the module is written in */
#ifdef __cplusplus
#  define WXL_MODULE_API extern "C" WXL_EXPORT
#else
#  define WXL_MODULE_API WXL_EXPORT
#endif

/* ---- events ----------------------------------------------------------------------------------- */

/* Events are subscribed by NAME, so a module keeps working when the core adds events. A handler gets
 * the opaque pointer given to subscribe() and a const pointer to that event's args struct. */
typedef void (*WxlEventHandler)(void* user, const void* args);

#define WXL_EVENT_MODEL_LOAD_PRE     "OnModelLoadPre"
#define WXL_EVENT_MODEL_LOAD         "OnModelLoad"
#define WXL_EVENT_BUILD_BONE_PALETTE "OnBuildBonePalette"
#define WXL_EVENT_UPDATE             "OnUpdate"
#define WXL_EVENT_WORLD_ENTER        "OnWorldEnter"
#define WXL_EVENT_WORLD_LEAVE        "OnWorldLeave"
#define WXL_EVENT_ITEM_SLOT_CHANGE   "OnItemSlotChange"
#define WXL_EVENT_ITEM_SLOT_CLEAR    "OnItemSlotClear"
#define WXL_EVENT_HAND_ITEM_ATTACHED "OnHandItemAttached"
#define WXL_EVENT_CREATURE_MODEL_BUILT "OnCreatureModelBuilt"
#define WXL_EVENT_M2_SKIN_FINALIZE   "OnM2SkinFinalize"
#define WXL_EVENT_M2_PER_FRAME       "OnM2PerFrameUpdate"
#define WXL_EVENT_END_SCENE          "OnEndScene"
#define WXL_EVENT_M2_BATCH_DRAW      "OnM2BatchDraw"
#define WXL_EVENT_DEVICE_LOST        "OnDeviceLost"
#define WXL_EVENT_DEVICE_RESET       "OnDeviceReset"

/* OnModelLoadPre (a model's raw bytes are read, before parse) and OnModelLoad (parsed): `model` is the
 * engine's model object, an opaque handle. */
typedef struct WxlModelLoadArgs { void* model; } WxlModelLoadArgs;
/* OnBuildBonePalette: the engine has just filled the bone palette of one model instance. A subscriber may
 * overwrite it through writePalette. Handlers run in subscription order, and modules load after the
 * built-in extensions, so a module sees the palette after they have had their say. */
typedef struct WxlBuildBonePaletteArgs { void* instance; } WxlBuildBonePaletteArgs;
/* OnUpdate: once per frame. */
typedef struct WxlUpdateArgs { float dt; uint32_t timeMs; } WxlUpdateArgs;
/* OnWorldEnter / OnWorldLeave */
typedef struct WxlWorldArgs { uint32_t mapId; } WxlWorldArgs;

/* The character-model events (used by the equip module). `charModelObj` / `cmo` is the client's character
 * component, an opaque handle: ask cmoInfo for what a module may know about it. */
/* OnItemSlotChange: a character model slot received an item. itemDataPtr is opaque: itemDataDisplayId reads it. */
typedef struct WxlItemSlotChangeArgs { void* charModelObj; uint32_t modelSlot; void* itemDataPtr; } WxlItemSlotChangeArgs;
/* OnItemSlotClear: a character model equipment slot was cleared; equipSlotWow is the WoW slot index 0-18. */
typedef struct WxlItemSlotClearArgs { void* charModelObj; uint32_t equipSlotWow; } WxlItemSlotClearArgs;
/* OnHandItemAttached: a held item was attached or removed. model is the character's own instance; invSlot 15 main
 * hand, 16 off hand, 17 ranged; attachLink 0 shield, 1 right hand, 2 left hand, 26+ sheath points, -1 none. */
typedef struct WxlHandItemAttachedArgs { void* model; uint32_t invSlot; int32_t attachLink; uint32_t displayId; uint8_t sheathed; uint8_t removed; } WxlHandItemAttachedArgs;
/* OnCreatureModelBuilt: a unit's creature model (mount = 0) or its mount's model (mount = 1) was created for a
 * CreatureDisplayInfo row. model is the new instance (the handle OnBuildBonePalette reports for it); displayId is the
 * CreatureDisplayInfo id that was drawn; fileName is the model file it was created from (a virtual path for a baked
 * variant), valid only during the call. */
typedef struct WxlCreatureModelBuiltArgs { void* model; uint32_t displayId; uint8_t mount; const char* fileName; } WxlCreatureModelBuiltArgs;
/* OnM2SkinFinalize: a loaded model's skin profile is about to be finalized (the window to filter geosets). */
typedef struct WxlM2SkinFinalizeArgs { void* model; } WxlM2SkinFinalizeArgs;
/* OnM2PerFrameUpdate: the per-frame scene-graph tick of one model instance. */
typedef struct WxlM2PerFrameUpdateArgs { void* instance; } WxlM2PerFrameUpdateArgs;

/* The render events (used by the outline module). `device` is the live IDirect3DDevice9*. */
/* OnEndScene: the frame's last EndScene, before Present; the place to composite into the frame. */
typedef struct WxlEndSceneArgs { void* device; } WxlEndSceneArgs;
/* OnM2BatchDraw: fired from the device's indexed draw for each model batch, just after the engine's own draw; model
 * is the instance being drawn (compare it, or its parents, with handles from the engine access calls) and the draw
 * parameters are the ones that draw used, so a module can issue the same draw again into its own target. */
typedef struct WxlM2BatchDrawArgs
{
    void*    device;
    void*    model;
    int32_t  primType;
    int32_t  baseVertex;
    uint32_t minIndex;
    uint32_t numVerts;
    uint32_t startIndex;
    uint32_t primCount;
} WxlM2BatchDrawArgs;
/* OnDeviceLost: fired before the client resets the device, while every D3DPOOL_DEFAULT resource must be released.
 * OnDeviceReset: after a successful reset, when a module recreates them. params is the D3DPRESENT_PARAMETERS*. */
typedef struct WxlDeviceResetArgs { void* device; void* params; } WxlDeviceResetArgs;

/* ---- files ------------------------------------------------------------------------------------ */

typedef struct WxlBuffer
{
    void*  data;
    size_t size;
} WxlBuffer;

/* A module can answer a file open with bytes it holds (virtual files). The core calls the provider for every
 * open, before it looks in the archives, so a provider must reject a name it does not own cheaply.
 *
 * The bytes go into a buffer the CORE allocates: call sink->reserve(sink->ctx, size) for a writable block of
 * exactly `size` bytes, copy the file into it, return 1. Return 0 (without calling reserve) for a name that is
 * not yours. Nothing is allocated in one DLL and freed in another. A provider may be entered again on the
 * same thread while it is serving (its own populate reads real files through the client, which asks the
 * providers again): guard its state accordingly. Handlers run on loader threads. */
typedef struct WxlServeSink
{
    void* ctx;
    void* (*reserve)(void* ctx, size_t size);   /* NULL when the core could not allocate */
} WxlServeSink;
typedef int (*WxlFileProviderFn)(void* user, const char* name, const WxlServeSink* sink);

/* "Is this name a virtual path one of your providers minted?" Used by isVirtualKey for the other modules. */
typedef int (*WxlVirtualKeyFn)(void* user, const char* name);

/* ---- model instances -------------------------------------------------------------------------- */

/* readInstance / readBones / writePalette return 0 on success, otherwise the step that stopped them.
 * 12 and 13 ("not loaded yet" / "animation not ready") and 6 (fewer than 2 bones) are ordinary. */
enum
{
    WXL_READ_OK             = 0,
    WXL_READ_NO_MODEL       = 1,   /* the instance has no loaded model */
    WXL_READ_NO_SCENE       = 2,
    WXL_READ_NO_PALETTE     = 3,
    WXL_READ_NO_HEADER      = 4,
    WXL_READ_NO_BONE_COUNT  = 5,
    WXL_READ_BONE_COUNT     = 6,   /* fewer than 2 bones, or more than WXL_MAX_BONES */
    WXL_READ_NO_BONES       = 7,
    WXL_READ_NO_CLOCK       = 8,
    WXL_READ_NO_SCENE_MAT   = 9,
    WXL_READ_NO_OWN_MAT     = 10,
    WXL_READ_NO_PATH        = 11,
    WXL_READ_NOT_PARSED     = 12,  /* the model file is not fully loaded and parsed yet */
    WXL_READ_NOT_ANIMATED   = 13   /* the instance's animation data is not ready yet */
};

#define WXL_MAX_BONES 256
#define WXL_PATH_MAX  260

typedef struct WxlInstanceView
{
    uint32_t    structSize;        /* sizeof(WxlInstanceView), set by the caller */
    uint32_t    boneCount;
    uint32_t    clockMs;           /* the scene clock the engine animates with, milliseconds */
    uint32_t    reserved;
    const void* model;             /* opaque id of the loaded model file behind the instance */
    float       sceneMatrix[16];   /* camera space -> world (the engine's inverse view matrix). It is a mirror:
                                      never decompose it. Row-vector layout, translation in [12..14]. */
    float       ownMatrix[16];     /* the instance's own matrix in camera space, same layout */
    char        path[WXL_PATH_MAX];/* the model's path as the engine keyed it (a virtual path for minted models) */
} WxlInstanceView;

/* ---- virtual paths ---------------------------------------------------------------------------- */

/* The core owns the virtual-file table that makes the client open a model that does not exist on disk: a
 * model's bytes patched in memory (geosets kept, textures and materials replaced, mirrored...) and served
 * under a minted path. These are its public entry points. */
typedef int (*WxlVPathLazyResolver)(const char* normalizedVirtualPathName);   /* 1 = baked it, the open can go on */

typedef struct WxlVPathGlobal
{
    uint32_t        structSize;          /* sizeof(WxlVPathGlobal), set by the caller */
    const char*     realMdxPath;         /* the real model the bytes come from */
    uint32_t        itemDisplayId;
    const char*     texPath;
    const char*     materialPatchSpec;   /* "TextureType=Path|TextureType=Path|..." or NULL */
    const uint16_t* geoIds;              /* geosets to keep, or NULL for all */
    uint32_t        geoCount;
    int             evictable;
    char*           outVirtualPath;      /* receives the minted path, or NULL */
    size_t          outVirtualPathSz;
    const char*     evictionPool;        /* "Creature" when NULL */
    int             forceBake;
    int             mirrorAcrossY;
    const char*     keySuffix;           /* NULL for none */
} WxlVPathGlobal;

/* ---- engine access (for the equip module) ----------------------------------------------------- */

/* Everything here is a plain number or an opaque handle. The core reads the client's structures under SEH and
 * answers with a status; a handle that has died (a freed instance) gives an error or an empty answer, never a fault. */

typedef struct WxlModelInfo
{
    uint32_t    structSize;        /* sizeof(WxlModelInfo), set by the caller */
    uint32_t    flags;             /* the instance's init flags: bit 0 = animation data ready, bit 18 = attached with force */
    uint32_t    boneCount;         /* bones in the loaded model; 0 when it is not loaded yet or the count is implausible */
    uint32_t    parsed;            /* 1 when the model file is completely loaded and parsed */
    void*       shared;            /* opaque id of the loaded model file; NULL once the instance is dead */
    void*       scene;             /* opaque: the scene that owns the instance (createModel is called on it) */
    void*       parent;            /* opaque: the instance this one is attached to; NULL for a root */
    const void* palette;           /* opaque: the live bone palette, NULL before it exists. Never dereference it */
} WxlModelInfo;

typedef struct WxlBoneInfo
{
    uint32_t nameCrc;
    int32_t  boneId;               /* the key-bone id, -1 for none */
    int32_t  parent;               /* parent bone index, -1 for none */
} WxlBoneInfo;

/* readBoneInfo fills `bones` with min(boneCount, maxBones) records and `lookup` (the key-bone -> bone table) with
 * min(lookupCount, maxLookup) entries; boneCount / lookupCount are the real sizes. */
typedef struct WxlBoneRead
{
    uint32_t     structSize;       /* sizeof(WxlBoneRead), set by the caller */
    uint32_t     maxBones;
    WxlBoneInfo* bones;
    uint32_t     maxLookup;
    uint16_t*    lookup;
    uint32_t     boneCount;        /* out */
    uint32_t     lookupCount;      /* out */
    uint32_t     bonesOk;          /* out: 1 when the model has a readable bone table */
    uint32_t     lookupOk;         /* out: 1 when it has a readable key-bone lookup */
} WxlBoneRead;

typedef struct WxlCmoInfo
{
    uint32_t structSize;           /* sizeof(WxlCmoInfo), set by the caller */
    uint32_t race;                 /* ChrRaces id */
    uint32_t gender;               /* 0 male, 1 female */
    uint32_t reserved;
    void*    model;                /* the character's own model instance (opaque), NULL when it has none */
} WxlCmoInfo;

#define WXL_ITEM_TEXT 260
typedef struct WxlItemDisplayInfo
{
    uint32_t structSize;           /* sizeof(WxlItemDisplayInfo), set by the caller */
    char     modelName[2][WXL_ITEM_TEXT];     /* empty string for none */
    char     modelTexture[2][WXL_ITEM_TEXT];
    char     icon2[WXL_ITEM_TEXT];            /* the record's second inventory-icon field */
} WxlItemDisplayInfo;

typedef struct WxlOutlineUnit
{
    const void* model;
    uint32_t    reaction;          /* 0 hostile, 1 neutral, 2 friendly */
    uint32_t    isPlayer;
} WxlOutlineUnit;

/* The held-item model resolver: may replace the file name the client loads for a held weapon (a bare file name; the
 * client prepends its folder). 1 = out holds a replacement. */
typedef int (*WxlHandItemResolverFn)(uint32_t displayId, uint32_t invSlot, char* out, size_t outSize);
/* 1 when the instance is drawn with a mirrored (handedness-flipping) palette, so the renderer swaps the cull direction. */
typedef int (*WxlMirroredModelQueryFn)(const void* model);

/* ---- the table -------------------------------------------------------------------------------- */

typedef struct WxlApi
{
    uint32_t abiVersion;           /* WXL_ABI_VERSION of the core */
    uint32_t structSize;           /* sizeof(WxlApi) in the core: members past it do not exist */

    /* log: one finished line in a category; the core adds the time and the thread. logThrottle is the same
     * rate limiter the core uses (first N occurrences, then one per `every`). */
    void (*log)(const char* category, const char* message);
    int  (*logThrottle)(const char* key, uint32_t first, uint32_t every);

    /* events: 0 on success, nonzero for an unknown event name */
    int  (*subscribe)(const char* eventName, WxlEventHandler handler, void* user);

    /* WXLExtendedEquipment.ini, next to the client. iniGetString returns 1 when the key exists. */
    int      (*iniGetString)(const char* section, const char* key, char* out, size_t cap);
    int      (*iniGetInt)(const char* section, const char* key, int defaultValue);
    int      (*iniGetBool)(const char* section, const char* key, int defaultValue);
    uint64_t (*iniStamp)(void);    /* last-write time of the ini, 0 when unknown: changes when it is saved */

    /* files: reads a file the way the client does (archives, then loose files under Data\), quietly: a
     * miss is the normal answer for most probes. Returns 1 and fills *out, which goes back through
     * freeBuffer; returns 0 when the file is not there. */
    int  (*readFile)(const char* name, WxlBuffer* out);
    void (*freeBuffer)(WxlBuffer* buffer);

    /* virtual paths minted by the equip/creature extensions: 1 when the name is one of them */
    int  (*isVirtualKey)(const char* name);

    /* model instances */
    int  (*readInstance)(void* instance, WxlInstanceView* out);
    /* palette: boneCount x 16 floats (the bone matrices); parents: boneCount x int16 (-1 = none);
     * pivots: boneCount x 3 floats. Any of the three may be NULL. count must equal the instance's bone count. */
    int  (*readBones)(void* instance, uint32_t count, float* palette, int16_t* parents, float* pivots);
    int  (*writePalette)(void* instance, const float* palette, uint32_t count);

    /* ---- added after the first model-instance members; check WXL_API_HAS before using ---- */

    /* the client's working directory (where WXL.log and loose sidecars live) */
    const char* (*workingDirectory)(void);

    /* virtual paths (see above). vpathPopulate is the per-character variant, keyed by the character-model
     * object `cmo`; the global ones are keyed by display id. */
    int  (*vpathPopulateGlobal)(const WxlVPathGlobal* params);
    int  (*vpathPopulateGlobalSharedKey)(const char* keySourcePath, const char* byteSourcePath, uint32_t itemDisplayId,
                                         const char* texPath, const char* materialPatchSpec, const uint16_t* geoIds,
                                         uint32_t geoCount, char* outVirtualPath, size_t outVirtualPathSz);
    int  (*vpathPopulate)(void* cmo, const char* realMdxPath, const uint16_t* geoIds, uint32_t geoCount,
                          const char* texPath, uint32_t variantKey, const char* materialPatchSpec);
    void (*vpathEvictCmo)(void* cmo);
    void (*vpathRegisterLazyResolver)(WxlVPathLazyResolver resolver);
    int  (*vpathDecodeGlobalVirtualKey)(const char* virtualPathName, char* outNormPath, size_t outNormPathSz,
                                        uint32_t* outDisplayId);

    /* services: one module offers a table of C function pointers, another looks it up by name when it needs
     * it (so load order does not matter and a missing provider is a NULL answer). The table must outlive
     * the process's use of it; the core never looks inside. */
    int         (*registerService)(const char* name, const void* table, size_t size);
    const void* (*getService)(const char* name, size_t* size);

    /* ---- added after the services: check WXL_API_HAS(api, member) before using one ---- */

    /* virtual files: the provider is asked for every file open, before the archives. Providers run in
     * registration order, the first to answer wins. Register during WxlModuleInit. 0 on success. */
    int  (*registerFileProvider)(WxlFileProviderFn fn, void* user);
    /* virtual paths: isVirtualKey answers 1 when any registered query does. Register during init. */
    int  (*registerVirtualKeyQuery)(WxlVirtualKeyFn fn, void* user);

    /* 1 when the client can open the name (archives, then loose files under Data\, then any module's virtual
     * files), 0 when it cannot. Opens and closes the file, reads nothing: use it to ask "is it there?" without
     * paying for the bytes. Quiet on a miss. */
    int  (*fileExists)(const char* name);

    /* ---- engine access, added for the equip module: check WXL_API_HAS(api, vpathBuildKey) once ---- */

    /* instances (see WxlModelInfo). 0 on success, a WXL_READ_* code otherwise (the struct is zeroed first). */
    int  (*modelInfo)(void* instance, WxlModelInfo* out);
    /* the bone table of the instance's loaded model; WXL_READ_NOT_PARSED (12) until the model is fully parsed */
    int  (*readBoneInfo)(void* instance, WxlBoneRead* io);
    /* the model file's path as the cache keys it; returns its length, 0 when unknown */
    size_t (*sharedPath)(void* shared, char* out, size_t cap);
    /* a loaded model's skin profile (opaque), NULL before it is attached; filterSkin keeps only the listed geosets'
     * triangles visible (the others are collapsed). */
    void*  (*sharedSkin)(void* shared);
    void   (*filterSkin)(void* skin, const uint16_t* geosetIds, uint32_t count);
    /* a character component's own instance, race and gender; 0 on success */
    int  (*cmoInfo)(void* cmo, WxlCmoInfo* out);

    /* model instances: get-or-create by path in a scene (the returned instance is one reference the caller must
     * release), attach to a parent at an attachment id (force bypasses the "point missing" exit), detach every child
     * of a parent at an attachment id, release one reference, bind a texture to the replaceable texture type 2, load a
     * texture by path (NULL on failure). A fault inside any of them is caught and logged; they then do nothing. */
    void*  (*createModel)(void* scene, const char* path);
    void   (*attachModel)(void* child, void* parent, uint32_t attachId, int force);
    void   (*detachSlot)(void* parent, uint32_t attachId);
    void   (*releaseModel)(void* instance);
    void   (*bindTexture)(void* instance, void* texture);
    void*  (*loadTexture)(const char* path);
    /* the child of `parent` attached at attachId whose model file has the same stem as `name`; NULL for none */
    void*  (*findAttachedChild)(void* parent, uint32_t attachId, const char* name);

    /* animation sequences. boneId 0xFFFFFFFF = the root/primary bone. */
    int      (*hasSequence)(void* instance, uint32_t sequenceId);
    uint32_t (*boneSequenceId)(void* instance, uint32_t boneId);
    void     (*setBoneSequence)(void* instance, uint32_t boneId, uint32_t sequenceId, uint32_t variation, uint32_t time,
                                float speed, int32_t blend, int32_t primary);
    /* the start anchor (ms) of bone 0's sequence; 0 before the bone block exists */
    int32_t  (*seqStartAnchor)(void* instance);

    /* bone palettes: copy palette rows from the pose source into the instance following collToChar (count entries,
     * 0xFF = leave the row alone). Refused (0) unless the instances still hold the models the table was built for. */
    int  (*copyRemappedBones)(void* dst, void* src, const uint8_t* collToChar, uint32_t count, const void* dstShared,
                              const void* srcShared);
    /* 1 when the engine rebuilt the instance's palette during this frame; *stamp receives the frame stamp */
    int  (*paletteBuiltThisFrame)(void* instance, uint32_t* stamp);
    /* mirrors the freshly built palette across the model's local Y axis; 1 when applied */
    int  (*mirrorPalette)(void* instance);

    /* records. itemDisplayInfo: 1 when the ItemDisplayInfo record exists. raceClientPrefix: 0 ok, 1 race id out of range
     * (*maxRaceId = the largest valid id), 2 no record, 3 record without a prefix. itemDataDisplayId reads the display id
     * out of an OnItemSlotChange item pointer (0 when unreadable). */
    int      (*itemDisplayInfo)(uint32_t displayId, WxlItemDisplayInfo* out);
    int      (*raceClientPrefix)(uint32_t raceId, char* out, size_t cap, int32_t* maxRaceId);
    uint32_t (*itemDataDisplayId)(const void* itemData);

    /* the client asks these when it attaches / draws a held item; one function each, a later call replaces the earlier */
    void (*setHandItemModelResolver)(WxlHandItemResolverFn fn);
    void (*setMirroredModelQuery)(WxlMirroredModelQueryFn fn);

    /* the virtual-path key the client would mint for a per-character model; returns the length, 0 on truncation */
    size_t (*vpathBuildKey)(char* out, size_t outSz, void* cmo, const char* realMdxPath, const uint16_t* geoIds,
                            uint32_t geoCount, const char* texPath, uint32_t variantKey, const char* materialPatchSpec);

    /* ---- engine access, added for the outline module: check WXL_API_HAS(api, outlineBatchState) once ---- */

    /* The units under the mouse and the locked target (in that order, deduplicated), for the reaction-colored outline.
     * Fills up to `max` entries and returns how many. `model` is an opaque instance handle (only compared, never
     * dereferenced); reaction is 0 hostile (hated / hostile), 1 neutral (unfriendly / neutral), 2 friendly and up;
     * isPlayer is 1 for a player character. */
    uint32_t (*outlineUnits)(WxlOutlineUnit* out, uint32_t max);
    /* The model instances of the creatures and objects the open quests still need (the quest log, the client's quest
     * cache and what is visible now), up to `max`, returned as the count. The scan is not cheap: call it on a timer. */
    uint32_t (*outlineQuestTargets)(const void** models, uint32_t max);
    /* 1 when the model batch about to be drawn may be stamped into an outline mask (an opaque or alpha-tested batch
     * that writes depth); *alphaRef receives the cutout reference (0..1) the engine is using for it. */
    int (*outlineBatchState)(float* alphaRef);
} WxlApi;

#define WXL_API_HAS(api, member) \
    ((api) && (api)->structSize >= offsetof(WxlApi, member) + sizeof(((WxlApi*)0)->member))

/* ---- services the built-in modules offer ----------------------------------------------------- */

/* "weapon": the weapon module's answers to the equip extension, which decides what to attach to a hand. */
#define WXL_SERVICE_WEAPON "weapon"
typedef struct WxlWeaponService
{
    uint32_t structSize;
    int (*getVirtualPath)(uint32_t displayId, uint32_t column, char* out, size_t outSz);
    int (*getOffhandVirtualPath)(uint32_t displayId, uint32_t column, char* out, size_t outSz, int* outAutoMirrored);
    int (*mirrorIsGeometric)(void);
} WxlWeaponService;

/* ---- what a module exports -------------------------------------------------------------------- */

typedef struct WxlModuleInfo
{
    uint32_t structSize;           /* set by the core */
    char     name[32];             /* filled by the module: short name for the log */
    uint32_t versionMajor;
    uint32_t versionMinor;
} WxlModuleInfo;

/* Put this once in a module's source file. */
#define WXL_DEFINE_MODULE_ABI() \
    WXL_MODULE_API uint32_t WxlModuleAbiVersion(void) { return WXL_ABI_VERSION; }

typedef uint32_t (*WxlModuleAbiVersionFn)(void);
typedef int      (*WxlModuleInitFn)(const WxlApi* api, WxlModuleInfo* info);
typedef void     (*WxlModuleShutdownFn)(void);

#ifdef __cplusplus
}
#endif

#endif /* WXL_API_H */
