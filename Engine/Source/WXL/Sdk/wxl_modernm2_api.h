/*
 * WarcraftXL modern M2 module: the service it publishes (ABI 1, see wxl_api.h).
 * Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
 *
 * wxl_modernm2.dll reads modern models (an MD21 container, inner version 272-274) for the client: it turns the
 * container into the flat MD20 body the client's own M2 loader walks, brings the records that grew in later builds
 * back to the client's record shape, rebuilds the skin contract, and names a texture the file leaves unnamed by its
 * TXID file id (through the listfile module, wxl_listfile_api.h). The client calls it through this table at the load
 * steps of CM2Shared (Engine/Source/Model2/ModernM2/ModernM2.cpp is the one caller):
 *
 *     const WxlModernM2Services* m2 = WxlModernM2Get(api);      // NULL when the module is not loaded
 *
 * The module never sees an engine type. It works on byte buffers and on the two blocks the client reads (the model
 * body and the skin block) through its own copy of the record layout; every block it needs comes from the CALLER,
 * through WxlM2Alloc, so each block is freed by the side that allocated it and the client's own free routines
 * (SIMD::FreeA16, OsFreeNear) stay the ones that release them.
 *
 * Plain C types only. Every function may be called from any thread (the client loads models on its async threads);
 * the module keeps no per-model state: whatever has to live with a model comes back in a result struct and is kept
 * by the client. Nothing here throws. The table only ever grows at its end (WXL_MODERNM2_HAS).
 */
#ifndef WXL_MODERNM2_API_H
#define WXL_MODERNM2_API_H

#include "wxl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_SERVICE_MODERNM2 "wxl.modernm2.v1"

/* Inner M2 versions the module reads (the client's own is 264). The client keeps its own copy of the window for the
 * hot path (every sequence load asks it) and compares it with these two once, when it first finds the service. */
#define WXL_M2_VERSION_MIN 272u
#define WXL_M2_VERSION_MAX 274u

/* Most TXID entries kept per model. */
#define WXL_M2_MAX_TXID 128u

/* The memory the module may ask the caller for during one call. `ctx` is handed back to each function. */
typedef struct WxlM2Alloc
{
    uint32_t structSize;     /* sizeof(WxlM2Alloc) in the caller */
    void*    ctx;
    /* A block of `size` bytes, 16-byte aligned, that the client frees with its own routine (the model body, the
     * skin block). NULL when out of memory. */
    void*  (*alloc)(void* ctx, uint32_t size);
    /* Gives back a block obtained from alloc(), or the block a call replaced (the old skin block). */
    void   (*release)(void* ctx, void* block);
    /* A block of `size` bytes within reach of a signed 32-bit offset from `anchor` (an M2 array reaches its data by
     * such an offset). It belongs to the model: the client releases it when the model goes. NULL when none. */
    void*  (*allocNear)(void* ctx, const void* anchor, size_t size);
} WxlM2Alloc;

/* ---- prepareLoad ---------------------------------------------------------------------------------------- */
enum
{
    WXL_M2_NOT_MODERN = 0,   /* not an MD21 container: nothing was touched, the stock loader goes ahead */
    WXL_M2_MODERN     = 1,   /* `body` holds the flat MD20 body in the client's shape */
    WXL_M2_FAILED     = 2    /* an MD21 file the module cannot read (logged): treat it as a corrupt model */
};

typedef struct WxlM2PrepareResult
{
    uint32_t structSize;     /* sizeof(WxlM2PrepareResult), set by the caller */
    uint32_t status;         /* WXL_M2_NOT_MODERN / _MODERN / _FAILED */
    void*    body;           /* MODERN: a block from alloc(): the body, then `reserveCap` spare zeroed bytes */
    uint32_t bodySize;       /* MODERN: bytes of the body (the model's new file size) */
    uint32_t reserveBase;    /* MODERN: where the spare room starts in the block (16-aligned, >= bodySize) */
    uint32_t reserveCap;     /* MODERN: bytes of spare room, for arrays the skin finalize rebuilds */
    uint32_t version;        /* MODERN: the inner version found */
    uint32_t txidCount;      /* MODERN: entries of txid[] (the TXID chunk's file ids, one per texture record) */
    uint32_t txid[WXL_M2_MAX_TXID];
} WxlM2PrepareResult;

/* ---- finalizeSkin --------------------------------------------------------------------------------------- */
typedef struct WxlM2SkinJob
{
    uint32_t    structSize;  /* sizeof(WxlM2SkinJob), set by the caller */
    const char* name;        /* the model's file name (log lines, and the extended-index-start name test) */
    void*       model;       /* IN: the model body after the client's walk (offsets are relative to each array) */
    void*       skin;        /* IN: the skin block as read, walked */
    uint32_t    skinBytes;   /* IN: bytes of that block */
    uint32_t    modern;      /* IN: 1 when the model went through prepareLoad (WXL_M2_MODERN) */
    uint32_t    reserveBase; /* IN: the model block's spare room (from prepareLoad; 0/0/0 for a stock model) */
    uint32_t    reserveCap;
    uint32_t    reserveUsed; /* IN/OUT: how much of it is taken */

    void*       newSkin;     /* OUT: the skin block to use from now on: `skin` when it was edited in place, else a
                                new block from alloc() (the old one went back through release()) */
    uint32_t    committed;   /* OUT: 0 when the rebuilt arrays could not be written back (logged; nothing changed) */
    uint32_t    extendedIndexStart; /* OUT: 1 when sections carry a full 32-bit index start beside the record's 16 bits */
    uint32_t    startCount;  /* OUT: entries of indexStart (0 unless extendedIndexStart) */
    const uint32_t* indexStart; /* OUT: the full index start of each section, in the skin's own section order. Owned by the
                                   module; valid until the next finalizeSkin call on the same thread: copy it. */
} WxlM2SkinJob;

/* TextureName results */
enum
{
    WXL_M2_TEXNAME_NONE       = 0, /* no answer (no listfile module): keep the solid white placeholder */
    WXL_M2_TEXNAME_FOUND      = 1, /* `out` is the texture's own path */
    WXL_M2_TEXNAME_NOTFOUND   = 2  /* `out` is the listfile module's not-found name: open it anyway, the client then
                                      shows its own missing texture */
};

typedef struct WxlModernM2Services
{
    uint32_t structSize;     /* sizeof(WxlModernM2Services) in the publisher */
    uint32_t versionMin;     /* WXL_M2_VERSION_MIN / _MAX the module was built with */
    uint32_t versionMax;

    /* An MD21 container in `file` (the bytes the client read) -> the flat body in a block from mem->alloc(). `file`
     * is only read: on WXL_M2_MODERN the caller frees it, on any other status it is untouched. Returns the status. */
    int  (*prepareLoad)(const char* name, const void* file, uint32_t fileSize, const WxlM2Alloc* mem,
                        WxlM2PrepareResult* out);

    /* After the client's walk of a MODERN body: reference clamps on the walked header (ribbon texture / material
     * indices, camera lookup sentinels). */
    void (*finishWalk)(void* model);

    /* The path to create texture `fileId` (one TXID entry) from, written to out[cap]. Returns WXL_M2_TEXNAME_*. */
    int  (*textureName)(uint32_t fileId, char* out, size_t cap);

    /* The skin finalize: the bone-budget split every model needs, the contract rebuild a modern model needs, the
     * boneInfluences fix, wide index starts. Edits the model's arrays and the skin in place or re-seats the skin;
     * everything it needs comes through `mem`. Returns 0 on success, nonzero when it could not run at all. */
    int  (*finalizeSkin)(const WxlM2Alloc* mem, WxlM2SkinJob* job);

    /* A modern external animation file wraps the flat track blob in one chunk: slides the payload onto the buffer
     * base and shortens *size. Returns 1 when it unwrapped, 0 when the buffer is not wrapped. */
    int  (*unwrapAnim)(void* buffer, uint32_t* size);
} WxlModernM2Services;

#define WXL_MODERNM2_HAS(table, member) \
    ((table) && (table)->structSize >= offsetof(WxlModernM2Services, member) + sizeof(((WxlModernM2Services*)0)->member))

/* The service, or NULL when the modern M2 module is not loaded (yet). */
static inline const WxlModernM2Services* WxlModernM2Get(const WxlApi* api)
{
    return (api && WXL_API_HAS(api, getService) && api->getService)
        ? (const WxlModernM2Services*)api->getService(WXL_SERVICE_MODERNM2, NULL) : NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* WXL_MODERNM2_API_H */
