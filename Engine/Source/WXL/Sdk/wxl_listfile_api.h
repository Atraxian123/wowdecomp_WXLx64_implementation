/*
 * WarcraftXL listfile module: the service it publishes (ABI 1, see wxl_api.h).
 * Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
 *
 * wxl_listfile.dll turns a file id into a file path using <client folder>\WXL\listfile.csv. Any module (or the
 * client) can use it: fetch the table once,
 *
 *     const WxlListfileServices* lf = WxlListfileGet(api);      // NULL when the module is not loaded
 *     char path[260];
 *     lf->resolve(fileId, WXL_LISTFILE_TEXTURE, path, sizeof(path));
 *     ... open `path`: when the file was not found it holds a name the client cannot open, so the client's own
 *     ... missing-texture (green) / missing-model (cube) handling takes over ...
 *
 * A module that needs it while it initializes must load AFTER wxl_listfile.dll (modules load in file-name order,
 * so name it like "20_mymodule.dll"), or fetch the table on first use instead of in WxlModuleInit.
 *
 * Paths come back in the client's own form: backslashes, and otherwise exactly as the listfile spells them.
 * Plain C types only. Every function may be called from any thread. The table only ever grows at its end
 * (check WXL_LISTFILE_HAS before calling a member added after the first release).
 */
#ifndef WXL_LISTFILE_API_H
#define WXL_LISTFILE_API_H

#include "wxl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_SERVICE_LISTFILE "wxl.listfile.v1"

/* What the caller expects the file to be. Only matters when the file is not found: the not-found name then gets the
 * extension of that kind (.blp / .m2), so the client sends it down the loader that has the right fallback. */
enum
{
    WXL_LISTFILE_ANY     = 0,   /* unknown or other: the not-found name has no extension */
    WXL_LISTFILE_TEXTURE = 1,   /* a .blp */
    WXL_LISTFILE_MODEL   = 2    /* an .m2 */
};

/* What resolve / resolvePath return. */
enum
{
    WXL_LISTFILE_ERROR    = -1, /* bad arguments, or `out` too small for the path (nothing written) */
    WXL_LISTFILE_NOTFOUND = 0,  /* not found: `out` holds the NOT-FOUND NAME ([Listfile] NotFoundName, default
                                 * "WXL\\NotFound", plus .blp / .m2 by kind). It is not a file in the client: open it
                                 * and the client falls back to its own default texture / model. Logged. */
    WXL_LISTFILE_FOUND    = 1   /* `out` holds the file's own path */
};

typedef struct WxlListfileServices
{
    uint32_t structSize;        /* sizeof(WxlListfileServices) in the publisher */

    /* The path the listfile gives a file id, as written there (backslashes). Pure table lookup: no not-found name,
     * no check that the file exists. Returns 1 and fills out, 0 when the id is not in the listfile (logged once
     * per id), -1 when out is too small. */
    int  (*lookupPath)(uint32_t fileId, char* out, size_t outSize);

    /* The path to open for a file id. Looks the id up; when the id is in the listfile but the client cannot open
     * the file, or the id is not in the listfile at all, `out` gets the not-found name for `kind` and a line goes
     * to the log. Returns WXL_LISTFILE_FOUND / _NOTFOUND / _ERROR. */
    int  (*resolve)(uint32_t fileId, uint32_t kind, char* out, size_t outSize);

    /* The same for a path the caller already has: the path itself when the client can open it, otherwise the
     * not-found name for `kind` (WXL_LISTFILE_ANY: by the path's own extension, .blp or .m2, else none). */
    int  (*resolvePath)(const char* path, uint32_t kind, char* out, size_t outSize);

    /* Entries in the table: the eager table's size, 0 when the table is read at run time. */
    uint32_t (*entryCount)(void);

    /* 1 when the table was built at load ([Listfile] Eager=1), 0 when lookups read listfile.csv at run time. */
    int  (*isEager)(void);

    /* Throw away what was cached or built and read listfile.csv again (after it was edited). 1 on success. */
    int  (*reload)(void);
} WxlListfileServices;

#define WXL_LISTFILE_HAS(table, member) \
    ((table) && (table)->structSize >= offsetof(WxlListfileServices, member) + sizeof(((WxlListfileServices*)0)->member))

/* The service, or NULL when the listfile module is not loaded (yet). */
static inline const WxlListfileServices* WxlListfileGet(const WxlApi* api)
{
    return (api && WXL_API_HAS(api, getService))
        ? (const WxlListfileServices*)api->getService(WXL_SERVICE_LISTFILE, NULL) : NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* WXL_LISTFILE_API_H */
