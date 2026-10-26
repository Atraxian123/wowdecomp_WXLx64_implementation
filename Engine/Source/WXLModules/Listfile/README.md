> **The listfile is a WXL module**: `wxl_listfile.dll`, built from this folder, loaded from `WXL\Modules\` by the
> 64-bit client. It sees the client only through `Engine/Source/WXL/Sdk/wxl_api.h`; its public interface for other
> modules is `Engine/Source/WXL/Sdk/wxl_listfile_api.h`.
> Test: `Tools/WxlModuleTest/listfile_host_test.cpp` (the module through the C ABI, `run_listfile_tests.sh` runs it all).

# Listfile: file id -> file path

Turns a file id into a path using `<client folder>\WXL\listfile.csv`. When the file is not found it hands back a
**"not found" name** that the client cannot open, so the client's own handling takes over: its default green texture
for a `.blp`, its default checkered cube for an `.m2`. Everything that is looked for and not found goes to the log.

## The listfile

```
1;interface/cinematics/logo_800.avi
53183;sound/music/citymusic/darnassus/darnassus intro.mp3
```

One entry per line: `<id>;<path>`. Read as the community listfiles write it:

* the delimiter is `;` or `,`; CRLF or LF; spaces in paths are kept; `/` is turned into `\` when the path is handed out
* a header row is optional: if the first non-blank line does not start with a number it is skipped
* a UTF-8 BOM is skipped; ids go up to 4294967295
* the same id twice: **the first entry wins**; a line that is not `<id>;<path>` is skipped and counted in the log

## Switching it on: `WXLExtendedEquipment.ini`

```ini
[Listfile]
Eager=1                          ; 1 = build the lookup table at load (default); 0 = read the file at run time
Path=WXL\listfile.csv            ; relative to the client folder, or absolute
CheckExists=1                    ; 1 = also ask the client whether the file the listfile names is really there
NotFoundName=WXL\NotFound        ; the name handed out for a file that is not found (.blp / .m2 is added by kind)
```

| | `Eager=1` (default) | `Eager=0` |
| --- | --- | --- |
| At load | reads the whole file into memory | reads nothing |
| A lookup | a binary search, ~0.5 µs | streams the file until the id is found (first lookup of an id near the end of a 185 MB file: ~0.07 s), then the answer is cached, hits and misses both |
| Memory | about the size of the file on disk (3,000,000 lines, 185 MB: ~150 MB held, ~350 MB at the peak of the build, 1 s) | a cache of up to 65,536 answers |
| An edit to the file | seen after `reload()` | seen after `reload()` (the cache is dropped) |

Neither mode needs the file to exist at load: without it the module still loads, logs that it cannot open it, and
answers "not found" for every id.

## Not found

A file counts as not found when its id is not in the listfile, or (with `CheckExists=1`) when the listfile names it but
the client cannot open it. The answer is `WXL_LISTFILE_NOTFOUND` and `out` holds the not-found name:

| The caller says it is | `out` | The client then |
| --- | --- | --- |
| a texture (`WXL_LISTFILE_TEXTURE`, or a path ending `.blp`) | `WXL\NotFound.blp` | cannot open it: uses its own default (green) texture |
| a model (`WXL_LISTFILE_MODEL`, or `.m2` / `.mdx`) | `WXL\NotFound.m2` | cannot open it: uses its own default (checkered cube) model |
| anything else (`.mp3`, `.avi`, ...) | `WXL\NotFound` | cannot open it |

The kind only picks the extension, so the engine sends the name down the loader that has the right fallback. The base
name is `[Listfile] NotFoundName`; pick one that is not a real file in any archive. Nothing is generated and the module
serves no files of its own.

Log category `LISTFILE`, one line per missing thing, once (not once per lookup):

```
file id 4000001 is not in the listfile (texture): answering 'WXL\NotFound.blp'
file id 2 -> 'interface\cinematics\wow_intro_800.avi' is not in the client (any type): answering 'WXL\NotFound'
'sound\music\nothing.mp3' is not in the client (any type): answering 'WXL\NotFound'
```

Note this is what the *lookup functions* answer. Nothing here intercepts the client's own file opens: a texture the client
asks for by itself, without going through this module, behaves as it always did.

## Using it from another module

```cpp
#include "wxl_listfile_api.h"

const WxlListfileServices* lf = WxlListfileGet(api);          // NULL when the listfile module is not loaded
char path[260];
switch (lf->resolve(fileId, WXL_LISTFILE_TEXTURE, path, sizeof(path)))
{
    case WXL_LISTFILE_FOUND:    /* path is the file's own path */                          break;
    case WXL_LISTFILE_NOTFOUND: /* path is the not-found name: open it anyway and the
                                   client shows its own missing texture / model */         break;
    default:                    /* WXL_LISTFILE_ERROR: bad arguments or buffer too small */ break;
}
```

| Function | |
| --- | --- |
| `lookupPath(id, out, size)` | the listfile's path for an id, nothing else: no existence check, no not-found name |
| `resolve(id, kind, out, size)` | the path to open for an id: its own, or the not-found name for `kind` |
| `resolvePath(path, kind, out, size)` | the same for a path you already have (`kind` `WXL_LISTFILE_ANY` = by extension) |
| `entryCount()`, `isEager()` | the table's size (0 at run time), and the mode |
| `reload()` | read the listfile again |

Modules load in file-name order: a module that wants the service *while it initializes* must sort after
`wxl_listfile.dll` (name it `20_mymodule.dll`), or fetch it on first use. Every function is thread-safe.
