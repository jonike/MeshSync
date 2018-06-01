﻿#include "pch.h"
#include "MeshSyncClient3dsMax.h"
#include "msmaxUtils.h"
#include "msmaxCallbacks.h"

#ifdef _WIN32
#pragma comment(lib, "core.lib")
#pragma comment(lib, "geom.lib")
#pragma comment(lib, "mesh.lib")
#pragma comment(lib, "poly.lib")
#pragma comment(lib, "mnmath.lib")
#pragma comment(lib, "maxutil.lib")
#pragma comment(lib, "maxscrpt.lib")
#pragma comment(lib, "paramblk2.lib")
#endif


static void OnStartup(void *param, NotifyInfo *info)
{
    ((MeshSyncClient3dsMax*)param)->onStartup();
}
static void OnNodeRenamed(void *param, NotifyInfo *info)
{
    ((MeshSyncClient3dsMax*)param)->onSceneUpdated();
}


void MeshSyncClient3dsMax::NodeRecord::clearState()
{
    dst_obj = nullptr;
    dst_anim = nullptr;
}

void MeshSyncClient3dsMax::AnimationRecord::operator()(MeshSyncClient3dsMax * _this)
{
    (_this->*extractor)(*dst, src);
}


MeshSyncClient3dsMax & MeshSyncClient3dsMax::getInstance()
{
    static MeshSyncClient3dsMax s_plugin;
    return s_plugin;
}

MeshSyncClient3dsMax::MeshSyncClient3dsMax()
{
    RegisterNotification(OnStartup, this, NOTIFY_SYSTEM_STARTUP);
}

MeshSyncClient3dsMax::~MeshSyncClient3dsMax()
{
    waitAsyncSend();
}

void MeshSyncClient3dsMax::onStartup()
{
    GetCOREInterface()->RegisterViewportDisplayCallback(TRUE, &msmaxViewportDisplayCallback::getInstance());
    GetCOREInterface()->RegisterTimeChangeCallback(&msmaxTimeChangeCallback::getInstance());
    RegisterNotification(OnNodeRenamed, this, NOTIFY_NODE_RENAMED);
    m_cbkey = GetISceneEventManager()->RegisterCallback(msmaxNodeCallback::getInstance().GetINodeEventCallback());
}

void MeshSyncClient3dsMax::onSceneUpdated()
{
    m_scene_updated = true;
}

void MeshSyncClient3dsMax::onTimeChanged()
{
    mscTraceW(L"MeshSyncClient3dsMax::onTimeChanged()\n");
}

void MeshSyncClient3dsMax::onNodeAdded(INode * n)
{
    m_scene_updated = true;
}

void MeshSyncClient3dsMax::onNodeDeleted(INode * n)
{
    m_scene_updated = true;

    m_deleted.push_back(GetPath(n));
    m_node_records.erase(n);
}

void MeshSyncClient3dsMax::onNodeUpdated(INode * n)
{
    mscTraceW(L"MeshSyncClient3dsMax::onNodeUpdated(): %s\n", n->GetName());
    m_node_records[n].dirty = true;
}

void MeshSyncClient3dsMax::onRepaint()
{
    mscTraceW(L"MeshSyncClient3dsMax::onRepaint()\n");
}


void MeshSyncClient3dsMax::update()
{
    if (m_pending_request != SendScope::None) {
        if (sendScene(m_pending_request)) {
            m_pending_request = SendScope::None;
        }
    }
    else if (m_settings.auto_sync && m_dirty) {
        sendScene(SendScope::Updated);
    }
}

bool MeshSyncClient3dsMax::sendScene(SendScope scope)
{
    if (isSending()) {
        return false;
    }

    int num_exported = 0;
    if (scope == SendScope::All) {
        EnumerateAllNode([&](INode *n) {
            auto it = m_node_records.find(n);
            if (it != m_node_records.end())
                it->second.dirty = false;

            if (exportObject(n))
                ++num_exported;
        });
    }
    else if (scope == SendScope::Updated) {
        for (auto& kvp : m_node_records) {
            auto& rec = kvp.second;
            if (rec.dirty) {
                rec.dirty = false;
                if (exportObject(kvp.first))
                    ++num_exported;
            }
        }
    }
    m_dirty = false;

    if (num_exported > 0)
        kickAsyncSend();
    return true;
}

bool MeshSyncClient3dsMax::sendAnimations(SendScope scope)
{
    if (isSending()) {
        waitAsyncSend();
    }

    // create default clip
    m_animations.push_back(ms::AnimationClip::create());

    // gather target data
    int num_exported = 0;
    if (scope == SendScope::All) {
        EnumerateAllNode([&](INode *n) {
            if (exportAnimations(n))
                ++num_exported;
        });
    }
    else {
        // todo:
    }

    // advance frame and record animation
    auto time_range = GetCOREInterface()->GetAnimRange();
    auto interval = SecToTicks(1.0f / m_settings.animation_sps);
    for (TimeValue t = time_range.Start(); t <= time_range.End(); t += interval) {
        // advance frame and record
        m_current_time = t;
        for (auto& kvp : m_anim_records)
            kvp.second(this);
    }

    // cleanup intermediate data
    m_anim_records.clear();

    // keyframe reduction
    for (auto& clip : m_animations)
        clip->reduction();

    // erase empty animation
    m_animations.erase(
        std::remove_if(m_animations.begin(), m_animations.end(), [](ms::AnimationClipPtr& p) { return p->empty(); }),
        m_animations.end());

    if (num_exported > 0)
        kickAsyncSend();
    else
        m_animations.clear();
    return true;
}

bool MeshSyncClient3dsMax::recvScene()
{
    return false;
}


bool MeshSyncClient3dsMax::isSending() const
{
    if (m_future_send.valid()) {
        return m_future_send.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout;
    }
    return false;
}

void MeshSyncClient3dsMax::waitAsyncSend()
{
    if (m_future_send.valid()) {
        m_future_send.wait_for(std::chrono::milliseconds(m_settings.timeout_ms));
    }
}

void MeshSyncClient3dsMax::kickAsyncSend()
{
    for (auto& kvp : m_node_records)
        kvp.second.clearState();

    // begin async send
    m_future_send = std::async(std::launch::async, [this]() {
        ms::Client client(m_settings.client_settings);

        ms::SceneSettings scene_settings;
        scene_settings.handedness = ms::Handedness::Left;
        scene_settings.scale_factor = m_settings.scale_factor;

        // notify scene begin
        {
            ms::FenceMessage fence;
            fence.type = ms::FenceMessage::FenceType::SceneBegin;
            client.send(fence);
        }

        // send delete message
        size_t num_deleted = m_deleted.size();
        if (num_deleted) {
            ms::DeleteMessage del;
            del.targets.resize(num_deleted);
            for (uint32_t i = 0; i < num_deleted; ++i)
                del.targets[i].path = m_deleted[i];
            m_deleted.clear();

            client.send(del);
        }

        // send scene data
        {
            ms::SetMessage set;
            set.scene.settings = scene_settings;
            set.scene.objects = m_objects;
            set.scene.materials = m_materials;
            client.send(set);

            m_objects.clear();
            m_materials.clear();
        }

        // send meshes one by one to Unity can respond quickly
        for (auto& mesh : m_meshes) {
            ms::SetMessage set;
            set.scene.settings = scene_settings;
            set.scene.objects = { mesh };
            client.send(set);
        };
        m_meshes.clear();

        // send animations and constraints
        if (!m_animations.empty() || !m_constraints.empty()) {
            ms::SetMessage set;
            set.scene.settings = scene_settings;
            set.scene.animations = m_animations;
            set.scene.constraints = m_constraints;
            client.send(set);

            m_animations.clear();
            m_constraints.clear();
        }

        // notify scene end
        {
            ms::FenceMessage fence;
            fence.type = ms::FenceMessage::FenceType::SceneEnd;
            client.send(fence);
        }
    });
}

void MeshSyncClient3dsMax::exportMaterials()
{
    auto mtllib = GetCOREInterface()->GetSceneMtls();
    int count = mtllib->Count();

    m_materials.clear();
    for (int mi = 0; mi < count; ++mi) {
        auto mtl = (Mtl*)(*mtllib)[mi];
        auto dst = ms::Material::create();
        m_materials.push_back(dst);
        dst->name = mu::ToMBS(mtl->GetName().data());
        dst->color = to_color(mtl->GetDiffuse());
    }
}


static void ExtractTransform(INode * node, TimeValue t, mu::float3& pos, mu::quatf& rot, mu::float3& scale)
{
    auto mat = to_float4x4(node->GetObjTMAfterWSM(t));
    if (auto parent = node->GetParentNode()) {
        auto pmat = to_float4x4(parent->GetObjTMAfterWSM(t));
        mat *= mu::invert(pmat);
    }
    pos = to_lhs(mu::extract_position(mat));
    rot = to_lhs(mu::extract_rotation(mat));
    scale = to_lhs(mu::extract_scale(mat));
}


ms::TransformPtr MeshSyncClient3dsMax::exportObject(INode * n)
{
    ms::TransformPtr ret;

    auto obj = n->GetObjectRef();
    if (obj->IsSubClassOf(polyObjectClassID) || obj->CanConvertToType(triObjectClassID)) {
        auto dst = ms::Mesh::create();
        ret = dst;
        m_meshes.push_back(dst);
        extractMeshData(*dst, n);
    }

    if (!ret) {
        switch (n->SuperClassID()) {
        case CAMERA_CLASS_ID:
            if (m_settings.sync_cameras) {
                auto dst = ms::Camera::create();
                ret = dst;
                m_objects.push_back(dst);
                extractCameraData(*dst, n);
            }
            break;
        case LIGHT_CLASS_ID:
            if (m_settings.sync_lights) {
                auto dst = ms::Light::create();
                ret = dst;
                m_objects.push_back(dst);
                extractLightData(*dst, n);
            }
            break;
        default:
            if (m_settings.sync_meshes) {
                auto dst = ms::Transform::create();
                ret = dst;
                m_objects.push_back(dst);
                extractTransformData(*dst, n);
            }
            break;
        }
    }

    if (ret) {
        ret->path = GetPath(n);
        ret->index = ++m_index_seed;
        m_node_records[n].dst_obj = ret.get();
    }
    return ret;
}

bool MeshSyncClient3dsMax::extractTransformData(ms::Transform & dst, INode * src)
{
    ExtractTransform(src, GetTime(), dst.position, dst.rotation, dst.scale);
    return true;
}

bool MeshSyncClient3dsMax::extractCameraData(ms::Camera & dst, INode * src)
{
    extractTransformData(dst, src);

    auto cam = (CameraObject*)src->GetObjectRef();
    return true;
}

bool MeshSyncClient3dsMax::extractLightData(ms::Light & dst, INode * src)
{
    extractTransformData(dst, src);
    return true;
}

bool MeshSyncClient3dsMax::extractMeshData(ms::Mesh & dst, INode * src)
{
    extractTransformData(dst, src);

    if (m_materials.empty()) {
        exportMaterials();
    }

    bool ret = false;
    auto obj = src->GetObjectRef();
    if (obj->IsSubClassOf(polyObjectClassID)) {
        ret = extractMeshData(dst, static_cast<PolyObject*>(obj)->GetMesh());
    }
    else {
        auto obj = src->GetObjectRef();
        if (auto tri = (TriObject*)obj->ConvertToType(GetTime(), triObjectClassID)) {
            ret = extractMeshData(dst, tri->GetMesh());
        }
    }

    if (ret) {
        for (auto& v : dst.points) v = to_lhs(v);
        for (auto& v : dst.normals) v = to_lhs(v);
        dst.setupFlags();
    }
    return ret;
}

bool MeshSyncClient3dsMax::extractMeshData(ms::Mesh & dst, MNMesh & mesh)
{
    // faces
    int num_faces = mesh.numf;
    int num_indices = 0;
    {
        dst.counts.resize_discard(num_faces);
        dst.material_ids.resize_discard(num_faces);
        dst.indices.reserve(num_faces * 4);
        for (int fi = 0; fi < num_faces; ++fi) {
            auto& face = mesh.f[fi];
            dst.counts[fi] = face.deg;
            dst.material_ids[fi] = face.material;
            for (int i = 0; i < face.deg; ++i)
                dst.indices.push_back(face.vtx[i]);
            num_indices += face.deg;
        }
    }

    // points
    int num_vertices = mesh.numv;
    dst.points.resize_discard(num_vertices);
    for (int vi = 0; vi < num_vertices; ++vi) {
        dst.points[vi] = to_float3(mesh.v[vi].p);
    }

    if (m_settings.sync_normals) {
        auto *nspec = mesh.GetSpecifiedNormals();
        if (nspec) {
            if (num_faces != nspec->GetNumFaces()) {
                mscTrace("should not be here\n");
            }
            else {
                dst.normals.resize_discard(num_indices);
                auto *faces = nspec->GetFaceArray();
                auto *normals = nspec->GetNormalArray();
                int ii = 0;
                for (int fi = 0; fi < num_faces; ++fi) {
                    auto& face = faces[fi];
                    int num = face.GetDegree();
                    auto *nids = face.GetNormalIDArray();
                    for (int i = 0; i < num; ++i)
                        dst.normals[ii++] = to_float3(normals[nids[i]]);
                }
            }
        }
    }

    // uv
    if (m_settings.sync_uvs && mesh.MNum() > 0) {
        auto *map = mesh.M(0);
        if (num_faces != map->numf) {
            mscTrace("should not be here\n");
        }
        else {
            dst.uv0.resize_discard(num_indices);
            int ii = 0;
            for (int fi = 0; fi < num_faces; ++fi) {
                auto& face = map->f[fi];
                for (int i = 0; i < face.deg; ++i)
                    dst.uv0[ii++] = to_float2(map->v[face.tv[i]]);
            }
        }
    }

    return true;
}

bool MeshSyncClient3dsMax::extractMeshData(ms::Mesh & dst, Mesh & mesh)
{
    // faces
    int num_faces = mesh.numFaces;
    int num_indices = num_faces * 3; // Max's Face is triangle
    {
        dst.counts.clear();
        dst.counts.resize(num_faces, 3);
        dst.material_ids.resize_discard(num_faces);

        const auto *faces = mesh.faces;
        dst.indices.resize_discard(num_indices);
        for (int fi = 0; fi < num_faces; ++fi) {
            auto& face = faces[fi];
            dst.material_ids[fi] = const_cast<Face&>(face).getMatID(); // :(
            for (int i = 0; i < 3; ++i)
                dst.indices[fi * 3 + i] = face.v[i];
        }
    }

    // points
    int num_vertices = mesh.numVerts;
    dst.points.resize_discard(num_vertices);
    dst.points.assign((mu::float3*)mesh.verts, (mu::float3*)mesh.verts + num_vertices);

    // normals
    if (m_settings.sync_normals) {
        auto get_normal = [&mesh](int face_index, int vertex_index) -> mu::float3 {
            const auto& rv = mesh.getRVert(vertex_index);
            const auto& face = mesh.faces[face_index];
            DWORD smGroup = face.smGroup;
            int num_normals = 0;
            Point3 ret;

            // Is normal specified
            // SPCIFIED is not currently used, but may be used in future versions.
            if (rv.rFlags & SPECIFIED_NORMAL) {
                ret = rv.rn.getNormal();
            }
            // If normal is not specified it's only available if the face belongs
            // to a smoothing group
            else if ((num_normals = rv.rFlags & NORCT_MASK) != 0 && smGroup) {
                // If there is only one vertex is found in the rn member.
                if (num_normals == 1) {
                    ret = rv.rn.getNormal();
                }
                else {
                    // If two or more vertices are there you need to step through them
                    // and find the vertex with the same smoothing group as the current face.
                    // You will find multiple normals in the ern member.
                    for (int i = 0; i < num_normals; i++) {
                        if (rv.ern[i].getSmGroup() & smGroup) {
                            ret = rv.ern[i].getNormal();
                        }
                    }
                }
            }
            else {
                // Get the normal from the Face if no smoothing groups are there
                ret = mesh.getFaceNormal(face_index);
            }
            return to_float3(ret);
        };

        // make sure normal is allocated
        mesh.checkNormals(TRUE);

        const auto *faces = mesh.faces;
        dst.normals.resize_discard(num_indices);
        for (int fi = 0; fi < num_faces; ++fi) {
            auto& face = faces[fi];
            for (int i = 0; i < 3; ++i) {
                int vi = face.v[i];
                dst.normals[fi * 3 + i] = get_normal(fi, face.v[i]);
            }
        }
    }

    // uv
    if (m_settings.sync_uvs) {
        int num_uv = mesh.numTVerts;
        auto *uv_faces = mesh.tvFace;
        auto *uv_vertices = mesh.tVerts;
        if (num_uv && uv_faces && uv_vertices) {
            dst.uv0.resize_discard(num_indices);
            for (int fi = 0; fi < num_faces; ++fi) {
                for (int i = 0; i < 3; ++i) {
                    dst.uv0[fi * 3 + i] = to_float2(uv_vertices[uv_faces[fi].t[i]]);
                }
            }
        }
    }

    // colors
    if (m_settings.sync_colors) {
        int num_colors = mesh.numCVerts;
        auto *vc_faces = mesh.vcFace;
        auto *vc_vertices = mesh.vertCol;
        if (num_colors && vc_faces && vc_vertices) {
            dst.colors.resize_discard(num_indices);
            for (int fi = 0; fi < num_faces; ++fi) {
                for (int i = 0; i < 3; ++i) {
                    dst.colors[fi * 3 + i] = to_color(vc_vertices[vc_faces[fi].t[i]]);
                }
            }
        }
    }

    return true;
}

ms::AnimationPtr MeshSyncClient3dsMax::exportAnimations(INode * n)
{
    auto it = m_anim_records.find(n);
    if (it != m_anim_records.end())
        return ms::AnimationPtr();

    auto& animations = m_animations[0]->animations;
    ms::AnimationPtr ret;
    AnimationRecord::extractor_t extractor = nullptr;

    auto obj = n->GetObjectRef();
    if (obj->CanConvertToType(triObjectClassID)) {
        auto dst = ms::MeshAnimation::create();
        animations.push_back(dst);
        ret = dst;
        extractor = &MeshSyncClient3dsMax::extractMeshAnimation;
    }

    if (!ret) {
        switch (n->SuperClassID()) {
        case CAMERA_CLASS_ID:
            if (m_settings.sync_cameras) {
                auto dst = ms::CameraAnimation::create();
                animations.push_back(dst);
                ret = dst;
                extractor = &MeshSyncClient3dsMax::extractCameraAnimation;
            }
            break;
        case LIGHT_CLASS_ID:
            if (m_settings.sync_lights) {
                auto dst = ms::LightAnimation::create();
                animations.push_back(dst);
                ret = dst;
                extractor = &MeshSyncClient3dsMax::extractLightAnimation;
            }
            break;
        default:
            if (m_settings.sync_meshes) {
                auto dst = ms::TransformAnimation::create();
                animations.push_back(dst);
                ret = dst;
                extractor = &MeshSyncClient3dsMax::extractTransformAnimation;
            }
            break;
        }
    }

    if (ret) {
        ret->path = GetPath(n);

        auto& rec = m_anim_records[n];
        rec.dst = ret.get();
        rec.src = n;
        rec.extractor = extractor;
    }
    return ret;
}

void MeshSyncClient3dsMax::extractTransformAnimation(ms::Animation& dst_, INode *src)
{
    auto& dst = (ms::TransformAnimation&)dst_;

    mu::float3 pos;
    mu::quatf rot;
    mu::float3 scale;
    ExtractTransform(src, m_current_time, pos, rot, scale);

    float t = TicksToSec(m_current_time) * m_settings.animation_time_scale;
    dst.translation.push_back({ t, pos });
    dst.rotation.push_back({ t, rot });
    dst.scale.push_back({ t, scale });
}

void MeshSyncClient3dsMax::extractCameraAnimation(ms::Animation& dst_, INode *src)
{
    extractTransformAnimation(dst_, src);

    auto& dst = (ms::CameraAnimation&)dst_;
}

void MeshSyncClient3dsMax::extractLightAnimation(ms::Animation& dst_, INode *src)
{
    extractTransformAnimation(dst_, src);

    auto& dst = (ms::LightAnimation&)dst_;
}

void MeshSyncClient3dsMax::extractMeshAnimation(ms::Animation& dst_, INode *src)
{
    extractTransformAnimation(dst_, src);

    auto& dst = (ms::MeshAnimation&)dst_;
}
