// Layout parity: the modern M2 module carries its own copy of the client's M2 record layout
// (Engine/Source/WXLModules/ModernM2/M2Records.hpp, no engine header allowed in a module). This program includes
// BOTH that copy (namespace wxl::m2rec) and the engine's M2Data.h (global names) and fails to compile, or fails to
// run, when a size or a field offset differs. Generated from M2Data.h; regenerate the lists when the engine header
// gains a record. Built and run by run_modernm2_tests.sh.
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "Engine/Source/Model2/M2Data.h"
#include "M2Records.hpp"

namespace mod = wxl::m2rec;

static int g_fail = 0, g_pass = 0;
#define SAME_SIZE(T) do { if (sizeof(::T) == sizeof(mod::T)) ++g_pass; else { ++g_fail; std::printf("  [FAIL] sizeof(%s): engine %zu, module %zu\n", #T, sizeof(::T), sizeof(mod::T)); } } while (0)
#define SAME_AT(T, F) do { if (offsetof(::T, F) == offsetof(mod::T, F)) ++g_pass; else { ++g_fail; std::printf("  [FAIL] offsetof(%s, %s): engine %zu, module %zu\n", #T, #F, offsetof(::T, F), offsetof(mod::T, F)); } } while (0)

int main()
{
    std::printf("M2 record layout: module copy vs engine M2Data.h\n");
    SAME_SIZE(fixed16);
    SAME_AT(fixed16, m_value);
    SAME_SIZE(ubyte4);
    SAME_AT(ubyte4, b);
    SAME_SIZE(M2CompQuat);
    SAME_AT(M2CompQuat, m_values);
    SAME_SIZE(M2TrackBase);
    SAME_AT(M2TrackBase, m_trackType);
    SAME_AT(M2TrackBase, m_loopIndex);
    SAME_AT(M2TrackBase, m_sequenceTimes);
    SAME_SIZE(M2Bounds);
    SAME_AT(M2Bounds, m_extent);
    SAME_AT(M2Bounds, m_radius);
    SAME_SIZE(M2Loop);
    SAME_AT(M2Loop, m_length);
    SAME_SIZE(M2Sequence);
    SAME_AT(M2Sequence, m_id);
    SAME_AT(M2Sequence, m_variationIndex);
    SAME_AT(M2Sequence, m_duration);
    SAME_AT(M2Sequence, m_moveSpeed);
    SAME_AT(M2Sequence, m_flags);
    SAME_AT(M2Sequence, m_frequency);
    SAME_AT(M2Sequence, m_replay);
    SAME_AT(M2Sequence, m_blendTime);
    SAME_AT(M2Sequence, m_bounds);
    SAME_AT(M2Sequence, m_variationNext);
    SAME_AT(M2Sequence, m_aliasNext);
    SAME_SIZE(M2CompBone);
    SAME_AT(M2CompBone, m_boneId);
    SAME_AT(M2CompBone, m_flags);
    SAME_AT(M2CompBone, m_parentIndex);
    SAME_AT(M2CompBone, m_uDistToParent);
    SAME_AT(M2CompBone, m_boneNameCRC);
    SAME_AT(M2CompBone, m_translationTrack);
    SAME_AT(M2CompBone, m_rotationTrack);
    SAME_AT(M2CompBone, m_scaleTrack);
    SAME_AT(M2CompBone, m_pivot);
    SAME_SIZE(M2VertexPNT2);
    SAME_AT(M2VertexPNT2, m_position);
    SAME_AT(M2VertexPNT2, m_normal);
    SAME_AT(M2VertexPNT2, m_texcoord);
    SAME_SIZE(M2Vertex);
    SAME_AT(M2Vertex, m_position);
    SAME_AT(M2Vertex, m_weights);
    SAME_AT(M2Vertex, m_indices);
    SAME_AT(M2Vertex, m_normal);
    SAME_AT(M2Vertex, m_texcoord);
    SAME_SIZE(M2Color);
    SAME_AT(M2Color, m_color);
    SAME_AT(M2Color, m_alpha);
    SAME_SIZE(M2Texture);
    SAME_AT(M2Texture, m_type);
    SAME_AT(M2Texture, m_flags);
    SAME_AT(M2Texture, m_filename);
    SAME_SIZE(M2TextureWeight);
    SAME_AT(M2TextureWeight, m_weight);
    SAME_SIZE(M2TextureTransform);
    SAME_AT(M2TextureTransform, m_translation);
    SAME_AT(M2TextureTransform, m_rotation);
    SAME_AT(M2TextureTransform, m_scaling);
    SAME_SIZE(M2Material);
    SAME_AT(M2Material, m_flags);
    SAME_AT(M2Material, m_blendMode);
    SAME_SIZE(M2Attachment);
    SAME_AT(M2Attachment, m_id);
    SAME_AT(M2Attachment, m_bone);
    SAME_AT(M2Attachment, unused_0006);
    SAME_AT(M2Attachment, m_position);
    SAME_AT(M2Attachment, m_animateAttached);
    SAME_SIZE(M2Event);
    SAME_AT(M2Event, m_eventId);
    SAME_AT(M2Event, m_data);
    SAME_AT(M2Event, m_bone);
    SAME_AT(M2Event, unused_000A);
    SAME_AT(M2Event, m_position);
    SAME_AT(M2Event, m_enabled);
    SAME_SIZE(M2Light);
    SAME_AT(M2Light, m_type);
    SAME_AT(M2Light, m_bone);
    SAME_AT(M2Light, m_position);
    SAME_AT(M2Light, m_ambientColor);
    SAME_AT(M2Light, m_ambientIntensity);
    SAME_AT(M2Light, m_diffuseColor);
    SAME_AT(M2Light, m_diffuseIntensity);
    SAME_AT(M2Light, m_attenuationStart);
    SAME_AT(M2Light, m_attenuationEnd);
    SAME_AT(M2Light, m_visibility);
    SAME_SIZE(M2Camera);
    SAME_AT(M2Camera, m_type);
    SAME_AT(M2Camera, m_fieldOfView);
    SAME_AT(M2Camera, m_farClip);
    SAME_AT(M2Camera, m_nearClip);
    SAME_AT(M2Camera, m_positions);
    SAME_AT(M2Camera, m_positionBase);
    SAME_AT(M2Camera, m_targetPositions);
    SAME_AT(M2Camera, m_targetPositionBase);
    SAME_AT(M2Camera, m_roll);
    SAME_SIZE(M2Ribbon);
    SAME_AT(M2Ribbon, m_ribbonId);
    SAME_AT(M2Ribbon, m_boneIndex);
    SAME_AT(M2Ribbon, unused_0006);
    SAME_AT(M2Ribbon, m_position);
    SAME_AT(M2Ribbon, m_textureIndices);
    SAME_AT(M2Ribbon, m_materialIndices);
    SAME_AT(M2Ribbon, m_colorTrack);
    SAME_AT(M2Ribbon, m_alphaTrack);
    SAME_AT(M2Ribbon, m_heightAboveTrack);
    SAME_AT(M2Ribbon, m_heightBelowTrack);
    SAME_AT(M2Ribbon, m_edgesPerSecond);
    SAME_AT(M2Ribbon, m_edgeLifetime);
    SAME_AT(M2Ribbon, m_gravity);
    SAME_AT(M2Ribbon, m_textureRows);
    SAME_AT(M2Ribbon, m_textureCols);
    SAME_AT(M2Ribbon, m_textureSlotTrack);
    SAME_AT(M2Ribbon, m_visibilityTrack);
    SAME_AT(M2Ribbon, m_priorityPlane);
    SAME_AT(M2Ribbon, unused_00AE);
    SAME_SIZE(M2Particle);
    SAME_AT(M2Particle, m_particleId);
    SAME_AT(M2Particle, m_flags);
    SAME_AT(M2Particle, m_position);
    SAME_AT(M2Particle, m_boneIndex);
    SAME_AT(M2Particle, m_textureIndex);
    SAME_AT(M2Particle, m_geometryMdl);
    SAME_AT(M2Particle, m_recursionMdl);
    SAME_AT(M2Particle, m_blendMode);
    SAME_AT(M2Particle, m_emitterType);
    SAME_AT(M2Particle, m_colorIndex);
    SAME_AT(M2Particle, unused_002C);
    SAME_AT(M2Particle, m_priorityPlane);
    SAME_AT(M2Particle, m_rows);
    SAME_AT(M2Particle, m_cols);
    SAME_AT(M2Particle, m_speedTrack);
    SAME_AT(M2Particle, m_variationTrack);
    SAME_AT(M2Particle, m_latitudeTrack);
    SAME_AT(M2Particle, m_longitudeTrack);
    SAME_AT(M2Particle, m_gravityTrack);
    SAME_AT(M2Particle, m_lifeTrack);
    SAME_AT(M2Particle, m_lifeVariation);
    SAME_AT(M2Particle, m_emissionRateTrack);
    SAME_AT(M2Particle, m_emissionRateVariation);
    SAME_AT(M2Particle, m_widthTrack);
    SAME_AT(M2Particle, m_lengthTrack);
    SAME_AT(M2Particle, m_zsourceTrack);
    SAME_AT(M2Particle, m_colorTrack);
    SAME_AT(M2Particle, m_alphaTrack);
    SAME_AT(M2Particle, m_scaleTrack);
    SAME_AT(M2Particle, m_scaleVariation);
    SAME_AT(M2Particle, m_headCellTrack);
    SAME_AT(M2Particle, m_tailCellTrack);
    SAME_AT(M2Particle, m_tailLength);
    SAME_AT(M2Particle, m_twinkleFPS);
    SAME_AT(M2Particle, m_twinkleOnOff);
    SAME_AT(M2Particle, m_twinkleScale);
    SAME_AT(M2Particle, m_ivelScale);
    SAME_AT(M2Particle, m_drag);
    SAME_AT(M2Particle, m_initialSpin);
    SAME_AT(M2Particle, m_initialSpinVariation);
    SAME_AT(M2Particle, m_spin);
    SAME_AT(M2Particle, m_spinVariation);
    SAME_AT(M2Particle, m_tumble);
    SAME_AT(M2Particle, m_windVector);
    SAME_AT(M2Particle, m_windTime);
    SAME_AT(M2Particle, m_followSpeed1);
    SAME_AT(M2Particle, m_followScale1);
    SAME_AT(M2Particle, m_followSpeed2);
    SAME_AT(M2Particle, m_followScale2);
    SAME_AT(M2Particle, m_spline);
    SAME_AT(M2Particle, m_visibilityTrack);
    SAME_SIZE(M2Batch);
    SAME_AT(M2Batch, m_flags);
    SAME_AT(M2Batch, m_priorityPlane);
    SAME_AT(M2Batch, m_shader);
    SAME_AT(M2Batch, m_skinSectionIndex);
    SAME_AT(M2Batch, m_geosetIndex);
    SAME_AT(M2Batch, m_colorIndex);
    SAME_AT(M2Batch, m_materialIndex);
    SAME_AT(M2Batch, m_materialLayer);
    SAME_AT(M2Batch, m_textureCount);
    SAME_AT(M2Batch, m_textureComboIndex);
    SAME_AT(M2Batch, m_textureCoordComboIndex);
    SAME_AT(M2Batch, m_textureWeightComboIndex);
    SAME_AT(M2Batch, m_textureTransformComboIndex);
    SAME_SIZE(M2SkinSection);
    SAME_AT(M2SkinSection, m_skinSectionId);
    SAME_AT(M2SkinSection, m_vertexStart);
    SAME_AT(M2SkinSection, m_vertexCount);
    SAME_AT(M2SkinSection, m_indexStart);
    SAME_AT(M2SkinSection, m_indexCount);
    SAME_AT(M2SkinSection, m_boneCount);
    SAME_AT(M2SkinSection, m_boneComboIndex);
    SAME_AT(M2SkinSection, m_boneInfluences);
    SAME_AT(M2SkinSection, m_centerBoneIndex);
    SAME_AT(M2SkinSection, m_centerPosition);
    SAME_AT(M2SkinSection, m_sortCenterPosition);
    SAME_AT(M2SkinSection, m_sortRadius);
    SAME_SIZE(M2Data);
    SAME_AT(M2Data, m_magic);
    SAME_AT(M2Data, m_version);
    SAME_AT(M2Data, m_name);
    SAME_AT(M2Data, m_flags);
    SAME_AT(M2Data, m_loops);
    SAME_AT(M2Data, m_sequences);
    SAME_AT(M2Data, m_sequenceIdxHashById);
    SAME_AT(M2Data, m_bones);
    SAME_AT(M2Data, m_boneIndicesById);
    SAME_AT(M2Data, m_vertices);
    SAME_AT(M2Data, m_numSkinProfiles);
    SAME_AT(M2Data, m_colors);
    SAME_AT(M2Data, m_textures);
    SAME_AT(M2Data, m_textureWeights);
    SAME_AT(M2Data, m_textureTransforms);
    SAME_AT(M2Data, m_textureIndicesById);
    SAME_AT(M2Data, m_materials);
    SAME_AT(M2Data, m_boneCombos);
    SAME_AT(M2Data, m_textureCombos);
    SAME_AT(M2Data, m_textureCoordCombos);
    SAME_AT(M2Data, m_textureWeightCombos);
    SAME_AT(M2Data, m_textureTransformCombos);
    SAME_AT(M2Data, m_bounds);
    SAME_AT(M2Data, m_collisionBounds);
    SAME_AT(M2Data, m_collisionIndices);
    SAME_AT(M2Data, m_collisionPositions);
    SAME_AT(M2Data, m_collisionFaceNormals);
    SAME_AT(M2Data, m_attachments);
    SAME_AT(M2Data, m_attachmentIndicesById);
    SAME_AT(M2Data, m_events);
    SAME_AT(M2Data, m_lights);
    SAME_AT(M2Data, m_cameras);
    SAME_AT(M2Data, m_cameraIndicesById);
    SAME_AT(M2Data, m_ribbons);
    SAME_AT(M2Data, m_particles);
    SAME_AT(M2Data, m_textureCombinerCombos);
    SAME_SIZE(M2SkinProfile);
    SAME_AT(M2SkinProfile, m_magic);
    SAME_AT(M2SkinProfile, m_vertices);
    SAME_AT(M2SkinProfile, m_indices);
    SAME_AT(M2SkinProfile, m_bones);
    SAME_AT(M2SkinProfile, m_skinSections);
    SAME_AT(M2SkinProfile, m_batches);
    SAME_AT(M2SkinProfile, m_boneCountMax);
    // template instantiations the reader walks
    SAME_SIZE(M2TrackBase);
    {
        using E1 = ::M2Array<char>;  using M1 = mod::M2Array<char>;
        if (sizeof(E1) == sizeof(M1)) ++g_pass; else { ++g_fail; std::printf("  [FAIL] sizeof(M2Array)\n"); }
        using E2 = ::M2Track<float>; using M2 = mod::M2Track<float>;
        if (sizeof(E2) == sizeof(M2) && offsetof(E2, m_sequenceKeys) == offsetof(M2, m_sequenceKeys)) ++g_pass; else { ++g_fail; std::printf("  [FAIL] M2Track<float>\n"); }
        using E3 = ::M2Track<::C3Vector>; using M3 = mod::M2Track<mod::C3Vector>;
        if (sizeof(E3) == sizeof(M3)) ++g_pass; else { ++g_fail; std::printf("  [FAIL] M2Track<C3Vector>\n"); }
        using E4 = ::M2PartTrack<::C3Vector>; using M4 = mod::M2PartTrack<mod::C3Vector>;
        if (sizeof(E4) == sizeof(M4)) ++g_pass; else { ++g_fail; std::printf("  [FAIL] M2PartTrack<C3Vector>\n"); }
        using E5 = ::M2SplineKey<::C3Vector>; using M5 = mod::M2SplineKey<mod::C3Vector>;
        if (sizeof(E5) == sizeof(M5)) ++g_pass; else { ++g_fail; std::printf("  [FAIL] M2SplineKey<C3Vector>\n"); }
    }
    std::printf("%d matched, %d differ\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
