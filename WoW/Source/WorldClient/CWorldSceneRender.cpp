#include <cfloat>
#include "CWorldSceneRender.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/World.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/GxuScene.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/Tempest/CMath.h"
#include "WoW/Source/LiquidSystem/Liquid.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/UIUtil/Cursor.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/WorldClient/CPortalView.h"
#include "WoW/Source/WorldClient/CWorldOcclusion.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/DetailDoodad.h"
#include "WoW/Source/WorldClient/MapEntity.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/MapRenderChunk.h"
#include "WoW/Source/WorldClient/MapWeather.h"
#include "WoW/Source/WorldClient/ShadowQuery.h"
#include "WoW/Source/WorldClient/SortTable.h"
#include "Engine/Source/Gx/CGxMatrixStack.h"
#include "Engine/Source/ShaderEffect/ShadowCache.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/WXL/Culling.h"
#include "Engine/Source/WXL/Log.h"
#include <cstdio>

// VERIFY: 78b69e1a manual
// OFFSET: 0x007D5610
void CWorldScene::RenderCollisionDebug() {
    if (!CMap::debugVertexArray.Count()) {
        return;
    }

    g_theGxDevicePtr->RsPush();
    GxRsSet(GxRs_PolygonOffset, 1.0f);

    C44Matrix matrix;
    matrix.Translate(C3Vector(-CWorldScene::s_activeWorldView.x, -CWorldScene::s_activeWorldView.y, -CWorldScene::s_activeWorldView.z));
    g_theGxDevicePtr->XformPushAndSet(GxXform_World, matrix);

    GxRsSet(GxRs_BlendingMode, 2);
    GxUpdateAlphaRef(GxRs_AlphaRef);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_Culling, 0);

    CGxVertexPC* vertices = CMap::debugVertexArray.m_data;
    GxPrimVertexPtr(CMap::debugVertexArray.Count(), &vertices->p, sizeof(CGxVertexPC), nullptr, 0, &vertices->c, sizeof(CGxVertexPC), nullptr, 0, nullptr, 0);
    GxPrimIndexPtr(CMap::debugIndexArray.Count(), CMap::debugIndexArray.m_data);

    CGxBatch batch;
    batch.m_count = CMap::debugIndexArray.Count();
    batch.m_maxIndex = CMap::debugVertexArray.Count() - 1;
    batch.m_primType = GxPrim_Triangles;
    batch.m_start = 0;
    batch.m_minIndex = 0;
    g_theGxDevicePtr->Draw(&batch, 1);

    GxXformPop(GxXform_World);
    g_theGxDevicePtr->RsPop();
}

// VERIFY: cd492c82 manual
// OFFSET: 0x0079A870
void CWorldScene::Render(const C3Vector& cameraPos, float time) {
    if (!CWorldScene::s_viewerMapObjDef) {
        CWorldScene::ComputeExteriorSortDistances();
    }

    CWorldScene::UpdateViewerLiquid();
    g_theGxDevicePtr->RsPush();
    g_theGxDevicePtr->XformPush(GxXform_World);

    CWorldScene::s_unused_CD8774 = 0;
    CWorldScene::s_chunksRendered = 0;
    CWorldScene::nDoodadsRendered = 0;
    CWorldScene::s_unused_CD8624 = 0;
    CWorldScene::frustumStack[CWorldScene::frustumIndex].CalcPlanesFromCorners(CWorldScene::s_frustumCorners);
    CWorldScene::s_occlusionFarDepth = World::s_farClip - 33.333332f;
    CWorldScene::s_hasPendingExteriorGroups = CWorldScene::sortTable.m_pendingExteriorGroupList.Head() != nullptr;

    CGCamera* camera = CGWorldFrame::GetActiveCamera();
    CWorldScene::s_fogFovPadding = CWorld::farFog / cosf(camera->FOV() * 0.5f) - CWorld::farFog;

    World::UpdateObjectFadeouts();
    CMapRenderChunk::UpdatePools();
    CMapObj::UpdatePools();
    CDetailDoodad::UpdatePools();
    memset(CWorldScene::s_clipBufferMask, 0, sizeof(CWorldScene::s_clipBufferMask));

    for (uint32_t i = 0; i < sizeof(CWorldScene::s_clipBuffer) / sizeof(CWorldScene::s_clipBuffer[0]); i++) {
        CWorldScene::s_clipBuffer[i] = -1000000.0f;
    }

    CWorldOcclusion::ClearVolumes();
    CWorldScene::AddWorldOccluders();

    if (CWorldScene::s_viewerMapObjDef) {
        CWorldScene::s_cullPassStamp++;

        if (CWorldScene::s_viewerMovedMapObjDef) {
            CWorldScene::s_viewerMovedMapObjDef->RenderWithCallback(&CWorldScene::s_viewerMovedMapObjGroups);
            CWorldScene::s_unk_ADF570.m_rect.minX = FLT_MAX;
            CWorldScene::s_unk_ADF570.m_rect.minY = FLT_MAX;
            CWorldScene::s_unk_ADF570.m_verts = nullptr;
            CWorldScene::s_unk_ADF570.m_rect.maxX = -FLT_MAX;
            CWorldScene::s_unk_ADF570.m_vertCount = 0;
            CWorldScene::s_unk_ADF570.m_rect.maxY = -FLT_MAX;
            CWorldScene::frustumPortalView.m_verts = nullptr;
            CWorldScene::frustumPortalView.m_vertCount = 0;
            CWorldScene::s_unk_ADF570.m_maxViewDepth = -1.0f;
            CWorldScene::frustumPortalView.m_rect.minX = FLT_MAX;
            CWorldScene::frustumPortalView.m_rect.minY = FLT_MAX;
            CWorldScene::frustumPortalView.m_rect.maxX = -FLT_MAX;
            CWorldScene::frustumPortalView.m_rect.maxY = -FLT_MAX;
            CWorldScene::frustumPortalView.m_maxViewDepth = -1.0f;
            CWorldScene::s_exteriorPortalViews.SetCount(0);
            CWorldScene::s_portalViews.SetCount(0);
        }

        CWorldScene::s_viewerMapObjDef->RenderWithCallback(&CWorldScene::s_viewerMapObjGroups);

        // Stock behaviour: the camera is inside a WMO. If none of its portals reach the outside
        // (m_maxViewDepth < 0) the whole exterior sort table (outdoor WMO groups, terrain chunks, liquid,
        // M2 entities and doodads) is emptied; otherwise the exterior is culled through the portal view.
        // WXLExtendedEquipment.ini [Culling] DisableViewerWmoCull=1 keeps the exterior, culled exactly as it
        // is when the camera stands outdoors (the branch below). Interiors still render through their
        // portals either way (RenderWithCallback above).
        const bool noWayOut = CWorldScene::frustumPortalView.m_maxViewDepth < 0.0f;
        wxl::culling::ExteriorAction exteriorAction;

        if (wxl::culling::KeepExteriorWhenInsideWmo()) {
            exteriorAction = wxl::culling::ExteriorAction::KeptByToggle;
            CRect exteriorRect = { 0.0f, 0.0f, 1.0f, 1.0f };
            CWorldScene::s_occlusionNearDepth = -10000.0f;
            CWorldScene::CullSortTable(&exteriorRect);
        } else if (noWayOut) {
            exteriorAction = wxl::culling::ExteriorAction::Cleared;
            CWorldScene::sortTable.Clear();
        } else {
            exteriorAction = wxl::culling::ExteriorAction::PortalCulled;
            CWorldScene::s_occlusionNearDepth = CWorldScene::frustumPortalView.m_maxViewDepth + 33.333332f;
            CWorldScene::CullSortTable(&CWorldScene::frustumPortalView.m_rect);
        }

        // [Culling] DisablePortalCulling=1: also draw this WMO's indoor groups that no portal chain from the camera
        // reaches (RenderWithCallback above only follows portals), when they are in view. A group already queued
        // by the portal walk is skipped (its sort-table link is set).
        if (wxl::culling::DisablePortalCulling()) {
            CMapObjDef* viewerDef = CWorldScene::s_viewerMapObjDef;
            CMapObj* viewerObj = viewerDef->m_owner;

            for (uint32_t i = 0; viewerObj && i < viewerObj->m_groupInfoCount; i++) {
                CMapObjDefGroup* defGroup = viewerDef->m_defGroups[i];
                CMapObjGroup* group = defGroup ? viewerObj->GetGroup(defGroup->m_groupNum, 0) : nullptr;

                if (!group || (group->m_flags & 0x10008) || defGroup->m_sortTableLink.m_next) {
                    continue;
                }

                if (!CWorldScene::frustumStack[CWorldScene::frustumIndex].Cull(defGroup->m_bbox)) {
                    continue;
                }

                s_curGroupIsInterior = 1;
                CWorldScene::AddMapObjDefGroupToSortTable(i, viewerDef);
            }
        }

        // Diagnostic (WXL.log, category CULL; only when logging is on): one line each time the camera's
        // WMO or what happened to the exterior changes.
        if (wxl::culling::ViewerChanged(CWorldScene::s_viewerMapObjDef, exteriorAction)) {
            CMapObj* owner = CWorldScene::s_viewerMapObjDef->m_owner;
            const char* wmoName = owner ? owner->m_wmoName : nullptr;

            if (wmoName) {
                const char* slash = strrchr(wmoName, '\\');
                wmoName = slash ? slash + 1 : wmoName;
            }

            char groups[256];
            size_t used = 0;
            groups[0] = 0;

            for (uint32_t i = 0; owner && i < CWorldScene::s_viewerMapObjGroups.Count() && i < 6 && used + 80 < sizeof(groups); i++) {
                const uint32_t num = CWorldScene::s_viewerMapObjGroups[i];
                CMapObjGroup* group = owner->GetGroup(num, 0);
                used += snprintf(groups + used, sizeof(groups) - used, "%s#%u '%s' flags 0x%X", i ? ", " : "", num,
                                 group && group->m_groupName ? group->m_groupName : "?", group ? group->m_flags : 0);
            }

            if (CWorldScene::s_viewerMapObjGroups.Count() > 6 && used + 12 < sizeof(groups)) {
                snprintf(groups + used, sizeof(groups) - used, ", +%u more", CWorldScene::s_viewerMapObjGroups.Count() - 6);
            }

            char detail[320];
            snprintf(detail, sizeof(detail), "%s%s", groups,
                     exteriorAction == wxl::culling::ExteriorAction::KeptByToggle
                         ? (noWayOut ? " (stock would have dropped the exterior)" : " (stock would have portal-culled it)")
                         : "");
            wxl::culling::LogViewer(exteriorAction, wmoName, detail, CWorldScene::s_activeWorldView.x,
                                    CWorldScene::s_activeWorldView.y, CWorldScene::s_activeWorldView.z);
        }

        CRect fullRect = { 0.0f, 0.0f, 1.0f, 1.0f };
        CWorldScene::CullThroughPortal(&fullRect);
    } else {
        CWorldScene::s_cullPassStamp++;
        CRect fullRect = { 0.0f, 0.0f, 1.0f, 1.0f };
        CWorldScene::s_unk_ADF570.m_rect = fullRect;
        CWorldScene::s_unk_ADF570.m_maxViewDepth = 0.0f;
        CWorldScene::s_unk_ADF570.m_vertCount = 0;
        CWorldScene::frustumPortalView.m_rect = fullRect;
        CWorldScene::frustumPortalView.m_maxViewDepth = 0.0f;
        CWorldScene::s_occlusionNearDepth = -10000.0f;
        CWorldScene::frustumPortalView.m_vertCount = 0;
        CWorldScene::CullSortTable(&CWorldScene::frustumPortalView.m_rect);

        if (wxl::culling::ViewerChanged(nullptr, wxl::culling::ExteriorAction::Outdoors)) {
            wxl::culling::LogViewer(wxl::culling::ExteriorAction::Outdoors, nullptr, nullptr, CWorldScene::s_activeWorldView.x,
                                    CWorldScene::s_activeWorldView.y, CWorldScene::s_activeWorldView.z);
        }
    }

    CWorldScene::CullMapObjDefGroup();
    CWorldScene::UpdateSortedModels();
    wxl::culling::TraceFlush(CWorldScene::s_activeWorldView.x, CWorldScene::s_activeWorldView.y, CWorldScene::s_activeWorldView.z);
    CWorldScene::sortTable.m_pendingExteriorGroupList.UnlinkAll();

    DayNight::DNInfo* info = DayNight::GetInfo();
    CImVector clearColor;

    if (!g_theGxDevicePtr->MasterEnable(GxMasterEnable_PolygonFill)) {
        clearColor.b = 0x00;
        clearColor.g = 0x00;
        clearColor.r = 0x00;
        clearColor.a = 0xFF;
    } else if (CWorldScene::s_unk_ADF570.m_maxViewDepth < 0.0f) {
        clearColor = info->m_fogInterior.m_color;
    } else if (s_liquidTypeId || (!info->m_showSky && DayNight::GetFogOverride())) {
        clearColor = info->m_fog.m_color;
    } else {
        clearColor.b = 0x00;
        clearColor.g = 0x00;
        clearColor.r = 0x00;
        clearColor.a = 0x00;
    }

    GxSceneClear(GxBufferBit_Color | GxBufferBit_Depth, clearColor);
    CWorldScene::s_unk_B2EB68.Render();

    if (CWorldScene::camTargetEntity) {
        C3Vector position = C3Vector(CWorldScene::camTargetEntity->m_position.x, CWorldScene::camTargetEntity->m_position.y, CWorldScene::camTargetEntity->m_position.z + 2.0f);
        CWorldScene::BuildViewMatrices(position);
    } else {
        CWorldScene::BuildViewMatrices(CWorldScene::s_activeWorldView);
    }

    CM2Scene* scene = World::s_m2Scene;

    if (scene) {
        scene->m_flags |= 0x1;
        scene->AdvanceTime(static_cast<uint32_t>(CMath::fint_(time * 1000.0f)));
        scene->Animate(cameraPos);
        scene->m_flags &= ~0x1;
    }

    CGUnit_C::UpdateMissileTrajectoryTargeting();
    CShadowQuery::Update();
    CShaderEffect::UpdateProjMatrix();
    CWorldScene::ApplyFogState();
    CWorldScene::RenderChunks();
    CWorldScene::RenderMapObjDefGroups();
    CWorldScene::RenderHorizon();

    if (CWorldScene::s_unk_ADF570.m_maxViewDepth >= 0.0f) {
        if (CWorldScene::s_exteriorPortalViews.Count()) {
            CWorldScene::CreateInversePortalList();
            CWorldScene::RenderPortalList(&CWorldScene::s_exteriorPortalViews, 1);
        }

        if (CWorldScene::s_portalViews.Count()) {
            CWorldScene::RenderPortalList(&CWorldScene::s_portalViews, 0);
        }

        if (CWorldScene::s_interiorSkybox) {
            DayNight::SetBlendSky(0, CWorldScene::s_interiorSkybox, 0, info->m_interiorFogBlend);
            DayNight::SetBlendSky(1, nullptr, 0, 0.0f);
        }

        if (!s_liquidTypeId) {
            DayNight::RenderSky(&CWorldScene::s_unk_ADF570);
        }
    }

    CWorldScene::RenderEntitys();
    Liquid::UpdateProceduralTextures();
    CWorldScene::PrepareLiquidInstances();

    if ((CWorld::enables & 0x1000000) && CWorldScene::s_liquidRenderer) {
        CWorldScene::s_liquidRenderer->Flush(CWorldScene::s_activeWorldView, 0);
    }

    CAaBox weatherBounds;
    weatherBounds.b = C3Vector(0.0f, 0.0f, 0.0f);
    weatherBounds.t = C3Vector(0.0f, 0.0f, 0.0f);

    if (World::s_weather && World::s_weather->GetActiveEffectBounds(&weatherBounds)) {
        weatherBounds.b.x = weatherBounds.b.x + CWorldScene::s_activeWorldView.x;
        weatherBounds.b.y = weatherBounds.b.y + CWorldScene::s_activeWorldView.y;
        weatherBounds.b.z = weatherBounds.b.z + CWorldScene::s_activeWorldView.z;
        weatherBounds.t.x = CWorldScene::s_activeWorldView.x + weatherBounds.t.x;
        weatherBounds.t.y = CWorldScene::s_activeWorldView.y + weatherBounds.t.y;
        weatherBounds.t.z = CWorldScene::s_activeWorldView.z + weatherBounds.t.z;
    }

    GxXformPop(GxXform_World);
    g_theGxDevicePtr->RsPop();
    CursorReleaseItemMips();

    if (CWorld::enables & 0x200000) {
        CWorldScene::RenderCollisionDebug();
        CMap::debugVertexArray.SetCount(0);
        CMap::debugIndexArray.m_count = 0;
    }
}


// VERIFY: 827be241 manual
// OFFSET: 0x007BB670
void CWorldScene::BuildViewMatrices(const C3Vector& position) {
    C44Matrix view;
    CGxMatrixStack& viewStack = g_theGxDevicePtr->m_xforms[GxXform_View];
    view = viewStack.m_mtx[viewStack.m_level];
    C44Matrix projection;
    projection = g_theGxDevicePtr->m_projection;
    C44Matrix viewProjection = view * projection;
    C3Vector normal = TransformDirection(C3Vector(0.0f, 0.0f, 1.0f), view);
    normal.Normalize();
    C3Vector relative = position - CWorldScene::s_activeWorldView;
    C3Vector projected = relative * viewProjection;
    CShadowCache::s_cutoffPlane.x = normal.x;
    CShadowCache::s_cutoffPlane.y = normal.y;
    CShadowCache::s_cutoffPlane.z = normal.z;
    CShadowCache::s_cutoffPlane.w = -(normal.x * projected.x + normal.y * projected.y + normal.z * projected.z);
    CShadowQuery::s_cutoffHeight = position.z + 2.0f;
}
