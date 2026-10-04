// scene.cpp -- the scene graph's lifecycle and per-frame work.
// Serialization lives next door in scene_io.cpp.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

#include <algorithm>
#include <cmath>

namespace pv {

// Declared in physics.cpp.
void physicsEnsureInitialized();
bool physicsRegisterBody(uint64_t bodyHandle);
void physicsUnregisterBody(uint64_t bodyHandle);
void physicsStepAndSync(float dt);

// Declared in animation.cpp.
void animAdvance(AnimRecord& rec, float dt);

// ======================================================================
// Hierarchy
// ======================================================================

uint64_t sceneCreateEntity(Scene& s, const std::string& name, uint64_t parent) {
    Entity e;
    e.name = name;
    e.parent = parent;
    uint64_t id = s.entities.add(std::move(e));
    // The id isn't known until after add(), so it's written back here rather
    // than being part of the value passed in.
    if (Entity* stored = s.entities.get(id)) stored->id = id;

    if (parent != 0) {
        if (Entity* p = s.entities.get(parent)) {
            p->children.push_back(id);
        } else {
            // Dangling parent: attach at the root rather than creating an
            // entity that nothing can reach.
            if (Entity* stored = s.entities.get(id)) stored->parent = 0;
            s.roots.push_back(id);
        }
    } else {
        s.roots.push_back(id);
    }
    s.dirty = true;
    return id;
}

namespace {

void detachFromParent(Scene& s, uint64_t id) {
    Entity* e = s.entities.get(id);
    if (!e) return;
    if (e->parent != 0) {
        if (Entity* p = s.entities.get(e->parent)) {
            p->children.erase(std::remove(p->children.begin(), p->children.end(), id), p->children.end());
        }
    } else {
        s.roots.erase(std::remove(s.roots.begin(), s.roots.end(), id), s.roots.end());
    }
}

// Releases the runtime resources an entity's components own, without
// touching the authored description -- so an entity can be stopped and
// restarted (editor Play/Stop) without losing its configuration.
void releaseRuntimeResources(Scene& s, Entity& e) {
    (void)s;
    auto& eng = engine();
    if (e.has(COMP_BODY) && e.body.body != 0) {
        physicsUnregisterBody(e.body.body);
        eng.bodies.remove(e.body.body);
        e.body.body = 0;
    }
    if (e.has(COMP_LIGHT) && e.light.runtimeLight != 0) {
        eng.lights.remove(e.light.runtimeLight);
        e.light.runtimeLight = 0;
    }
    if (e.has(COMP_EMITTER) && e.emitter.emitter != 0) {
        eng.emitters.remove(e.emitter.emitter);
        e.emitter.emitter = 0;
    }
    if (e.has(COMP_ANIMATOR) && e.animator.animation != 0) {
        eng.anims.remove(e.animator.animation);
        e.animator.animation = 0;
    }
}

} // namespace

void sceneDestroyEntity(Scene& s, uint64_t id) {
    Entity* e = s.entities.get(id);
    if (!e) return;

    // Copy the child list before recursing: the recursive call mutates the
    // parent's vector via detachFromParent, which would invalidate an
    // iterator held across the loop.
    std::vector<uint64_t> kids = e->children;
    for (uint64_t child : kids) sceneDestroyEntity(s, child);

    e = s.entities.get(id); // re-fetch; the table may have rehashed
    if (!e) return;
    releaseRuntimeResources(s, *e);
    detachFromParent(s, id);
    if (s.activeCamera == id) s.activeCamera = 0;
    s.entities.remove(id);
    s.dirty = true;
}

void markWorldDirty(Scene& s, uint64_t id) {
    Entity* e = s.entities.get(id);
    if (!e || e->worldDirty) return; // already marked: subtree is too
    e->worldDirty = true;
    for (uint64_t c : e->children) markWorldDirty(s, c);
}

void sceneSetParent(Scene& s, uint64_t child, uint64_t parent, bool keepWorldTransform) {
    Entity* c = s.entities.get(child);
    if (!c || child == parent) return;

    // Reject a cycle: walking up from the prospective parent must not reach
    // the child. Without this, re-parenting a node under its own descendant
    // makes updateSceneTransforms recurse forever.
    for (uint64_t walk = parent; walk != 0;) {
        if (walk == child) {
            logLine("WARNING", "Scene: refusing to parent '" + c->name +
                                   "' under its own descendant (that would make a cycle)");
            return;
        }
        Entity* w = s.entities.get(walk);
        walk = w ? w->parent : 0;
    }

    updateSceneTransforms(s);
    Mat4 worldBefore = c->world;

    detachFromParent(s, child);
    c->parent = parent;
    if (parent == 0) {
        s.roots.push_back(child);
    } else if (Entity* p = s.entities.get(parent)) {
        p->children.push_back(child);
    } else {
        c->parent = 0;
        s.roots.push_back(child);
    }

    if (keepWorldTransform) {
        // Recompute the local transform so the entity doesn't visibly jump
        // when it changes parent -- local = inverse(parentWorld) * world.
        Mat4 parentWorld = Mat4::identity();
        if (c->parent != 0) {
            if (Entity* p = s.entities.get(c->parent)) parentWorld = p->world;
        }
        Mat4 local = parentWorld.inverse() * worldBefore;
        decomposeTRS(local, c->local.position, c->local.rotation, c->local.scale);
    }
    markWorldDirty(s, child);
    s.dirty = true;
}

uint64_t sceneFindByName(const Scene& s, const std::string& name) {
    for (const auto& kv : s.entities) {
        if (kv.second.name == name) return kv.first;
    }
    return 0;
}

std::vector<uint64_t> sceneFindByTag(const Scene& s, const std::string& tag) {
    std::vector<uint64_t> out;
    for (const auto& kv : s.entities) {
        const auto& tags = kv.second.tags;
        if (std::find(tags.begin(), tags.end(), tag) != tags.end()) out.push_back(kv.first);
    }
    // Sorted so iteration order is stable between runs -- the entity table
    // is a hash map, and unstable ordering makes gameplay non-reproducible.
    std::sort(out.begin(), out.end());
    return out;
}

void updateSceneTransforms(Scene& s) {
    // Iterative depth-first from the roots rather than recursion, so a deep
    // hierarchy can't overflow the stack, and each entity is visited once
    // with its parent already resolved.
    std::vector<uint64_t> stack(s.roots.rbegin(), s.roots.rend());
    while (!stack.empty()) {
        uint64_t id = stack.back();
        stack.pop_back();
        Entity* e = s.entities.get(id);
        if (!e) continue;

        Mat4 parentWorld = Mat4::identity();
        bool parentChanged = false;
        if (e->parent != 0) {
            if (Entity* p = s.entities.get(e->parent)) {
                parentWorld = p->world;
                parentChanged = !p->worldDirty; // parent was just recomputed
            }
        }
        if (e->worldDirty || parentChanged) {
            e->world = parentWorld * e->local.matrix();
            e->worldDirty = false;
        }
        for (uint64_t c : e->children) stack.push_back(c);
    }
}

// ======================================================================
// Start / stop
// ======================================================================

namespace {

// Rebuilds the mesh a MeshComponent describes. Scene files record either a
// source path or a procedural primitive, never a runtime handle -- a handle
// is meaningless across a restart.
uint64_t realizeMesh(const MeshComponent& mc) {
    if (!mc.sourcePath.empty()) {
        return static_cast<uint64_t>(rt::LoadMesh(Value(mc.sourcePath)).asInt());
    }
    if (mc.primitive == "cube") {
        return static_cast<uint64_t>(rt::CreateCube(Value(mc.primitiveParams.x)).asInt());
    }
    if (mc.primitive == "sphere") {
        return static_cast<uint64_t>(
            rt::CreateSphere(Value(mc.primitiveParams.x), Value(static_cast<int>(mc.primitiveParams.y))).asInt());
    }
    if (mc.primitive == "plane") {
        return static_cast<uint64_t>(
            rt::CreatePlane(Value(mc.primitiveParams.x), Value(mc.primitiveParams.y)).asInt());
    }
    if (mc.primitive == "cylinder") {
        return static_cast<uint64_t>(
            rt::CreateCylinder(Value(mc.primitiveParams.x), Value(mc.primitiveParams.y)).asInt());
    }
    return 0;
}

} // namespace

void sceneStart(Scene& s) {
    if (s.running) return;
    auto& eng = engine();

    updateSceneTransforms(s);
    physicsEnsureInitialized();

    eng.gravity = s.gravity;
    if (eng.physics) eng.physics->setGravity(s.gravity);
    rt::SetAmbient(Value(s.ambientColor.x), Value(s.ambientColor.y), Value(s.ambientColor.z),
                   Value(s.ambientIntensity));
    rt::SetClearColor(Value(s.clearColor.x), Value(s.clearColor.y), Value(s.clearColor.z), Value(s.clearColor.w));

    for (uint64_t id : s.entities.ids()) {
        Entity* e = s.entities.get(id);
        if (!e) continue;

        // --- meshes ---
        if (e->has(COMP_MESH) && e->mesh.mesh == 0) e->mesh.mesh = realizeMesh(e->mesh);

        // --- sprites ---
        if (e->has(COMP_SPRITE) && e->sprite.texture == 0 && !e->sprite.sourcePath.empty()) {
            e->sprite.texture = static_cast<uint64_t>(rt::LoadTexture(Value(e->sprite.sourcePath)).asInt());
        }

        // --- lights ---
        if (e->has(COMP_LIGHT) && e->light.runtimeLight == 0) {
            LightRecord lr;
            lr.kind = e->light.kind;
            Vec3 wp(e->world.m[3][0], e->world.m[3][1], e->world.m[3][2]);
            lr.position = wp;
            // A light's direction is its local -Z axis in world space, which
            // is the same forward convention the camera uses.
            lr.direction = e->world.transformDirection(Vec3(0, 0, -1)).normalized();
            lr.r = e->light.color.x;
            lr.g = e->light.color.y;
            lr.b = e->light.color.z;
            lr.intensity = e->light.intensity;
            lr.radius = e->light.radius;
            lr.angle = e->light.coneAngle;
            e->light.runtimeLight = eng.lights.add(lr);
        }

        // --- physics bodies ---
        if (e->has(COMP_BODY) && e->body.body == 0) {
            uint64_t colliderHandle = eng.colliders.add(e->body.colliderDesc);
            RigidBodyRecord rec;
            rec.collider = colliderHandle;
            rec.mass = e->body.mass;
            rec.isStatic = e->body.isStatic;
            rec.isKinematic = e->body.isKinematic;
            // Seeded from the entity's authored world transform, so a body
            // starts exactly where the editor placed it.
            Vec3 t, sc;
            Quat r;
            decomposeTRS(e->world, t, r, sc);
            rec.pose.position = t;
            rec.pose.rotation = r;
            rec.ownerEntity = id;

            uint64_t bodyHandle = eng.bodies.add(rec);
            if (physicsRegisterBody(bodyHandle)) {
                e->body.body = bodyHandle;
                e->body.collider = colliderHandle;
            } else {
                eng.bodies.remove(bodyHandle);
                eng.colliders.remove(colliderHandle);
            }
        }

        // --- emitters ---
        if (e->has(COMP_EMITTER) && e->emitter.emitter == 0) {
            EmitterRecord er;
            er.name = e->name;
            e->emitter.emitter = eng.emitters.add(std::move(er));
        }
        if (e->has(COMP_EMITTER) && e->emitter.emitter != 0) {
            if (auto* er = eng.emitters.get(e->emitter.emitter)) {
                er->enabled = e->emitter.playOnStart;
            }
        }

        // --- animators ---
        if (e->has(COMP_ANIMATOR) && e->animator.animation == 0 && !e->animator.sourcePath.empty()) {
            Value handle = rt::LoadAnimation(Value(e->animator.sourcePath));
            e->animator.animation = static_cast<uint64_t>(handle.asInt());
            if (e->animator.playOnStart) {
                rt::PlayAnimation(handle, Value(e->animator.autoPlayClip));
                rt::SetAnimationSpeed(handle, Value(e->animator.speed));
                rt::SetAnimationLoop(handle, Value(e->animator.looping));
            }
        }

        // --- camera ---
        if (e->has(COMP_CAMERA) && e->camera.active && s.activeCamera == 0) s.activeCamera = id;
    }

    s.running = true;
}

void sceneStop(Scene& s) {
    if (!s.running) return;
    for (uint64_t id : s.entities.ids()) {
        if (Entity* e = s.entities.get(id)) releaseRuntimeResources(s, *e);
    }
    s.running = false;
}

// ======================================================================
// Per-frame update
// ======================================================================

void sceneUpdate(Scene& s, float dt) {
    if (!s.running) {
        // Not playing: still refresh transforms so the editor's viewport
        // reflects gizmo edits immediately.
        updateSceneTransforms(s);
        return;
    }
    auto& eng = engine();

    updateSceneTransforms(s);

    // --- physics: push kinematic/script-moved poses in, step, read back ---
    for (uint64_t id : s.entities.ids()) {
        Entity* e = s.entities.get(id);
        if (!e || !e->active || !e->has(COMP_BODY) || e->body.body == 0) continue;
        if (!e->body.isKinematic) continue;
        RigidBodyRecord* rec = eng.bodies.get(e->body.body);
        if (!rec || !eng.physics) continue;
        Vec3 t, sc;
        Quat r;
        decomposeTRS(e->world, t, r, sc);
        rec->pose.position = t;
        rec->pose.rotation = r;
        eng.physics->setKinematicTarget(e->body.body, rec->pose);
    }

    physicsStepAndSync(dt);

    const bool physicsHasRotation = eng.physics && eng.physics->supportsRotation();
    for (uint64_t id : s.entities.ids()) {
        Entity* e = s.entities.get(id);
        if (!e || !e->active || !e->has(COMP_BODY) || e->body.body == 0) continue;
        if (e->body.isStatic || e->body.isKinematic) continue;
        RigidBodyRecord* rec = eng.bodies.get(e->body.body);
        if (!rec) continue;

        // The simulated pose is in world space; the entity stores a local
        // transform, so a parented body needs the parent's inverse applied.
        Transform world;
        world.position = rec->pose.position;
        world.rotation = physicsHasRotation ? rec->pose.rotation : e->local.rotation;
        world.scale = e->local.scale;

        if (e->parent == 0) {
            e->local.position = world.position;
            if (physicsHasRotation) e->local.rotation = world.rotation;
        } else if (Entity* p = s.entities.get(e->parent)) {
            Mat4 local = p->world.inverse() * Mat4::fromTRS(world.position, world.rotation, world.scale);
            Vec3 t, sc;
            Quat r;
            decomposeTRS(local, t, r, sc);
            e->local.position = t;
            if (physicsHasRotation) e->local.rotation = r;
        }
        markWorldDirty(s, id);
    }

    updateSceneTransforms(s);

    // --- animators ---
    for (uint64_t id : s.entities.ids()) {
        Entity* e = s.entities.get(id);
        if (!e || !e->active || !e->has(COMP_ANIMATOR) || e->animator.animation == 0) continue;
        AnimRecord* rec = eng.anims.get(e->animator.animation);
        if (!rec) continue;
        animAdvance(*rec, dt);

        if (e->animator.applyRootMotion && !rec->localPose.empty()) {
            e->local = rec->localPose[0];
            markWorldDirty(s, id);
        }
    }

    // --- emitters: follow their entity, then simulate ---
    for (uint64_t id : s.entities.ids()) {
        Entity* e = s.entities.get(id);
        if (!e || !e->active || !e->has(COMP_EMITTER) || e->emitter.emitter == 0) continue;
        EmitterRecord* er = eng.emitters.get(e->emitter.emitter);
        if (!er) continue;
        Vec3 t, sc;
        Quat r;
        decomposeTRS(e->world, t, r, sc);
        er->position = t;
        er->rotation = r;
        updateEmitter(*er, dt);
    }

    // --- lights follow their entity ---
    for (uint64_t id : s.entities.ids()) {
        Entity* e = s.entities.get(id);
        if (!e || !e->has(COMP_LIGHT) || e->light.runtimeLight == 0) continue;
        LightRecord* lr = eng.lights.get(e->light.runtimeLight);
        if (!lr) continue;
        lr->position = Vec3(e->world.m[3][0], e->world.m[3][1], e->world.m[3][2]);
        lr->direction = e->world.transformDirection(Vec3(0, 0, -1)).normalized();
        lr->r = e->light.color.x;
        lr->g = e->light.color.y;
        lr->b = e->light.color.z;
        lr->intensity = e->active ? e->light.intensity : 0.0f;
        lr->radius = e->light.radius;
    }

    updateSceneTransforms(s);
}

// ======================================================================
// Draw
// ======================================================================

void sceneDraw(Scene& s) {
    auto& eng = engine();
    updateSceneTransforms(s);

    // Apply the active camera before anything is submitted.
    if (s.activeCamera != 0) {
        if (Entity* cam = s.entities.get(s.activeCamera)) {
            Vec3 pos(cam->world.m[3][0], cam->world.m[3][1], cam->world.m[3][2]);
            Vec3 fwd = cam->world.transformDirection(Vec3(0, 0, -1)).normalized();
            eng.camPos = pos;
            eng.camTarget = pos + fwd;
            eng.camHasTarget = true;
            eng.fovDegrees = cam->camera.fovDegrees;
            eng.nearZ = cam->camera.nearZ;
            eng.farZ = cam->camera.farZ;
            eng.orthographic = cam->camera.orthographic;
        }
    }

    // Opaque meshes first, then transparent sprites and particles, so
    // alpha-blended geometry composites over a complete depth buffer.
    for (const auto& kv : s.entities) {
        const Entity& e = kv.second;
        if (!e.active || !e.has(COMP_MESH) || !e.mesh.visible || e.mesh.mesh == 0) continue;

        Mat4 model = e.world;
        // An animator with root motion disabled still animates: its sampled
        // root transform composes onto the entity's placed transform.
        if (e.has(COMP_ANIMATOR) && !e.animator.applyRootMotion && e.animator.animation != 0) {
            if (const AnimRecord* rec = eng.anims.get(e.animator.animation)) {
                if (rec->posed && !rec->modelPose.empty()) model = model * rec->modelPose[0];
            }
        }
        drawSceneMesh(e.mesh.mesh, model, e.mesh.material);
    }

    for (const auto& kv : s.entities) {
        const Entity& e = kv.second;
        if (!e.active || !e.has(COMP_SPRITE) || e.sprite.texture == 0) continue;
        Vec3 p(e.world.m[3][0], e.world.m[3][1], e.world.m[3][2]);
        rt::DrawBillboard(Value::MakeHandle(e.sprite.texture, "texture"), Value(p.x), Value(p.y), Value(p.z),
                          Value(e.sprite.width), Value(e.sprite.height));
    }

    for (const auto& kv : s.entities) {
        const Entity& e = kv.second;
        if (!e.active || !e.has(COMP_EMITTER) || e.emitter.emitter == 0) continue;
        if (EmitterRecord* er = eng.emitters.get(e.emitter.emitter)) drawEmitter(*er);
    }
}

} // namespace pv
