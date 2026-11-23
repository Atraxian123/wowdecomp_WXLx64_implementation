/*
 * Shader 23 sidecar (.s23) on-disk format, version 1.
 * Copyright (C) 2026 WarcraftXL
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * Plain C on purpose (no namespaces, no <span>, no STL): this exact header is meant to be dropped into the
 * project that patches the 3.3.5a client, so the converter (writer) and the client (reader) cannot drift apart.
 *
 * WHY THIS EXISTS
 *   A shader 23 WMO material blends up to four tiling surface layers by per-vertex weights. The 3.3.5a material
 *   record holds two textures and the group holds two UV sets and no weights, so a normal conversion has to throw
 *   the rest away. The converter can instead write everything that would be lost into one "<root name>.s23" file
 *   beside each converted root WMO, for the patched client to read and draw the blend itself.
 *
 * WHAT IS IN IT
 *   Per root WMO:
 *     - one S23Material per shader 23 material of the root (its MOMT index, the four layer textures, the
 *       reflection/sheen texture, and the verbatim 64-byte source MOMT record so nothing is lost);
 *     - one S23Group per group file that draws at least one of those materials, with the group's UV sets 0..3
 *       (the source MOTV chunks) and the four per-vertex layer weights (decoded from the source MOC2 chunk).
 *
 * CONVENTIONS
 *   - Little endian, every field is a 32 bit value or a byte array, structures are 4 byte packed: no padding.
 *   - Layer k (0..3) is drawn with UV set k of the group. Layer 0 is the source's "texture 2" (MOMT+0x18), layer 1
 *     "texture 3" (+0x24), layer 2 "texture 4" (+0x28), layer 3 "texture 5" (+0x2C).
 *   - The vertex index space is the one of the CONVERTED group file's MOVT, which is the source's: extraction
 *     refuses to run together with the converter's own shader 23 modes that append/duplicate vertices.
 *   - Only the vertex range [firstVertex, firstVertex + vertexCount) that the shader 23 batches of the group
 *     reference is stored. Vertex v is found at index (v - firstVertex).
 *   - Group files named "<stem>_NNN.wmo" only (the 3.3.5a client has no LOD group files).
 *
 * FILE LAYOUT (all offsets from the start of the file)
 *   S23Header
 *   S23Material[materialCount]      at materialsOffset
 *   S23Group[groupCount]            at groupsOffset, ordered by groupIndex
 *   string blob                     at stringsOffset, stringsSize bytes, NUL terminated strings, padded to 4
 *   per group data blocks           at S23Group::dataOffset, in group order:
 *        float uv[uvSetCount][vertexCount][2]      UV set s, vertex i: u at uv[s][i][0], v at uv[s][i][1]
 *        uint8 weight[vertexCount][4]              only if S23_GROUP_HAS_WEIGHTS; layer k weight at weight[i][k]
 *   The four weights of a vertex sum to 255 (layer 3 is the remainder of the source's three stored weights).
 */
#ifndef WXL_SHADER23_FORMAT_H
#define WXL_SHADER23_FORMAT_H

#include <stdint.h>

#define S23_MAGIC        0x58333253u /* bytes "S23X" */
#define S23_VERSION      1u
#define S23_LAYERS       4
#define S23_NO_STRING    0xFFFFFFFFu /* S23Texture::pathOffset when the texture has no (resolvable) path */
#define S23_GROUP_HAS_WEIGHTS 0x1u   /* S23Group::flags: the data block carries per-vertex weights */
#define S23_MAX_UV_SETS  4

#pragma pack(push, 4)

typedef struct S23Header
{
    uint32_t magic;           /* S23_MAGIC */
    uint32_t version;         /* S23_VERSION */
    uint32_t materialCount;
    uint32_t groupCount;
    uint32_t materialsOffset;
    uint32_t groupsOffset;
    uint32_t stringsOffset;
    uint32_t stringsSize;
} S23Header;                  /* 32 bytes */

typedef struct S23Texture
{
    uint32_t fileDataId;      /* 0 = no texture in this slot */
    uint32_t pathOffset;      /* offset into the string blob, S23_NO_STRING if the id did not resolve to a path */
} S23Texture;                 /* 8 bytes */

typedef struct S23Material
{
    uint32_t materialIndex;   /* index into the root's MOMT, valid in the converted root too */
    uint32_t flags;           /* source MOMT flags (+0x00), unmasked */
    uint32_t blendMode;       /* source MOMT blend mode (+0x08) */
    uint32_t layerMask;       /* bit k set = layer k has a texture (layer 0 may be empty and then has no bit) */
    S23Texture layer[S23_LAYERS];
    S23Texture reflection;    /* source "texture 1" (MOMT+0x0C): the sheen / reflection map */
    uint8_t  rawMomt[64];     /* the source MOMT record, verbatim */
} S23Material;                /* 120 bytes */

typedef struct S23Group
{
    uint32_t groupIndex;      /* 0-based, the NNN of "<stem>_NNN.wmo" */
    uint32_t uvSetCount;      /* 1..4: how many UV sets the data block holds */
    uint32_t firstVertex;     /* first stored vertex index */
    uint32_t vertexCount;     /* stored vertices */
    uint32_t dataOffset;      /* start of this group's data block */
    uint32_t flags;           /* S23_GROUP_HAS_WEIGHTS */
    uint32_t reserved[2];     /* zero */
} S23Group;                   /* 32 bytes */

#pragma pack(pop)

#endif /* WXL_SHADER23_FORMAT_H */
