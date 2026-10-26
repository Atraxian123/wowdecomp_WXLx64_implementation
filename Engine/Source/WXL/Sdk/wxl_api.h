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

/* ---- files ------------------------------------------------------------------------------------ */

typedef struct WxlBuffer
{
    void*  data;
    size_t size;
} WxlBuffer;

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
