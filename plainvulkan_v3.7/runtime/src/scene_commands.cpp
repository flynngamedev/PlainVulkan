// scene_commands.cpp -- the `Pv::` scene API.
//
// These are what a .pv script uses to work with a scene the editor
// authored, or to build one from scratch. The engine holds one active
// scene (Engine::scene); every command here operates on it and
// auto-creates an empty one if the script never called Pv::NewScene, so
// nothing here can fail on a null pointer.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

#include <algorithm>

namespace pv {

Scene& activeScene() {
    auto& e = engine();
    if (!e.scene) e.scene.reset(new Scene());
    return *e.scene;
}

namespace rt {

namespace {

// pv::Value::asString() has no defaulted-fallback overload, and adding one
// to the core Value type for the sake of this file would widen a
// widely-used API for a local convenience. This does the same job locally.
std::string strOr(const Value& v, const char* fallback) {
    std::string s = v.asString();
    return s.empty() ? std::string(fallback) : s;
}

Entity* getEntity(const Value& v) {
    return activeScene().entities.get(static_cast<uint64_t>(v.asInt()));
}

// Every mutation that moves an entity has to mark its subtree dirty, or the
// cached world matrices go stale and children stop following their parent.
void touch(uint64_t id) {
    Scene& s = activeScene();
    markWorldDirty(s, id);
    s.dirty = true;
}

} // namespace

// ---- scene lifecycle --------------------------------------------------

Value NewScene(const Value& name) {
    auto& e = engine();
    if (e.scene) sceneStop(*e.scene);
    e.scene.reset(new Scene());
    e.scene->name = strOr(name, "Untitled");
    return Value(true);
}

Value LoadScene(const Value& path) {
    Scene& s = activeScene();
    std::string error;
    if (!sceneLoadFromFile(s, path.asString(), &error)) {
        logLine("ERROR", "Pv::LoadScene: " + error);
        return Value(false);
    }
    logLine("INFO", "Pv::LoadScene: '" + path.asString() + "' -- " + std::to_string(s.entities.size()) +
                        " entities");
    return Value(true);
}

Value SaveScene(const Value& path) {
    Scene& s = activeScene();
    std::string target = path.asString();
    if (target.empty()) target = s.path;
    if (target.empty()) {
        logLine("ERROR", "Pv::SaveScene: no path given and this scene has never been saved");
        return Value(false);
    }
    std::string error;
    if (!sceneSaveToFile(s, target, &error)) {
        logLine("ERROR", "Pv::SaveScene: " + error);
        return Value(false);
    }
    s.path = target;
    s.dirty = false;
    return Value(true);
}

Value StartScene() {
    sceneStart(activeScene());
    return Value(true);
}

Value StopScene() {
    sceneStop(activeScene());
    return Value(true);
}

Value UpdateScene(const Value& deltaTime) {
    sceneUpdate(activeScene(), static_cast<float>(deltaTime.asFloat()));
    return Value();
}

Value DrawScene() {
    sceneDraw(activeScene());
    return Value();
}

// ---- entities ---------------------------------------------------------

Value CreateEntity(const Value& name) {
    return Value::MakeHandle(sceneCreateEntity(activeScene(), strOr(name, "Entity"), 0), "entity");
}

Value CreateChildEntity(const Value& name, const Value& parent) {
    return Value::MakeHandle(
        sceneCreateEntity(activeScene(), strOr(name, "Entity"), static_cast<uint64_t>(parent.asInt())), "entity");
}

Value DestroyEntity(const Value& entity) {
    sceneDestroyEntity(activeScene(), static_cast<uint64_t>(entity.asInt()));
    return Value();
}

Value FindEntity(const Value& name) {
    return Value::MakeHandle(sceneFindByName(activeScene(), name.asString()), "entity");
}

Value FindEntitiesByTag(const Value& tag) {
    Value arr = Value::MakeArray();
    for (uint64_t id : sceneFindByTag(activeScene(), tag.asString())) {
        arr.arrayRef().push_back(Value::MakeHandle(id, "entity"));
    }
    return arr;
}

Value SetEntityParent(const Value& child, const Value& parent) {
    sceneSetParent(activeScene(), static_cast<uint64_t>(child.asInt()),
                   static_cast<uint64_t>(parent.asInt()), true);
    return Value();
}

Value SetEntityPosition(const Value& entity, const Value& x, const Value& y, const Value& z) {
    uint64_t id = static_cast<uint64_t>(entity.asInt());
    if (Entity* e = activeScene().entities.get(id)) {
        e->local.position = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                                 static_cast<float>(z.asFloat()));
        touch(id);
    }
    return Value();
}

Value GetEntityPosition(const Value& entity) {
    if (Entity* e = getEntity(entity)) {
        return make_array({Value(e->local.position.x), Value(e->local.position.y), Value(e->local.position.z)});
    }
    return make_array({Value(0.0), Value(0.0), Value(0.0)});
}

Value SetEntityRotation(const Value& entity, const Value& pitch, const Value& yaw, const Value& roll) {
    uint64_t id = static_cast<uint64_t>(entity.asInt());
    if (Entity* e = activeScene().entities.get(id)) {
        e->local.rotation = Quat::fromEuler(static_cast<float>(pitch.asFloat()),
                                            static_cast<float>(yaw.asFloat()),
                                            static_cast<float>(roll.asFloat()));
        touch(id);
    }
    return Value();
}

Value GetEntityRotation(const Value& entity) {
    if (Entity* e = getEntity(entity)) {
        Vec3 euler = e->local.rotation.toEuler();
        return make_array({Value(euler.x), Value(euler.y), Value(euler.z)});
    }
    return make_array({Value(0.0), Value(0.0), Value(0.0)});
}

Value SetEntityScale(const Value& entity, const Value& x, const Value& y, const Value& z) {
    uint64_t id = static_cast<uint64_t>(entity.asInt());
    if (Entity* e = activeScene().entities.get(id)) {
        e->local.scale = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                              static_cast<float>(z.asFloat()));
        touch(id);
    }
    return Value();
}

Value SetEntityActive(const Value& entity, const Value& active) {
    if (Entity* e = getEntity(entity)) e->active = active.truthy();
    return Value();
}

Value AddEntityTag(const Value& entity, const Value& tag) {
    if (Entity* e = getEntity(entity)) {
        std::string t = tag.asString();
        if (std::find(e->tags.begin(), e->tags.end(), t) == e->tags.end()) e->tags.push_back(t);
    }
    return Value();
}

Value GetEntityCount() { return Value(static_cast<int64_t>(activeScene().entities.size())); }

// ---- components -------------------------------------------------------

Value SetEntityMesh(const Value& entity, const Value& mesh) {
    if (Entity* e = getEntity(entity)) {
        e->add(COMP_MESH);
        e->mesh.mesh = static_cast<uint64_t>(mesh.asInt());
        // A handle assigned from script has no source path, so saving the
        // scene would lose it. Warn once rather than silently dropping it on
        // the next save.
        if (e->mesh.sourcePath.empty() && e->mesh.primitive.empty()) {
            e->mesh.primitive = "";
        }
        activeScene().dirty = true;
    }
    return Value();
}

Value SetEntityMaterial(const Value& entity, const Value& material) {
    if (Entity* e = getEntity(entity)) {
        e->add(COMP_MESH);
        e->mesh.material = static_cast<uint64_t>(material.asInt());
    }
    return Value();
}

Value AddEntityLight(const Value& entity, const Value& kind, const Value& r, const Value& g, const Value& b,
                     const Value& intensity) {
    if (Entity* e = getEntity(entity)) {
        e->add(COMP_LIGHT);
        std::string k = strOr(kind, "point");
        e->light.kind = (k == "directional") ? LightKindTag::Directional
                        : (k == "spot")      ? LightKindTag::Spot
                                             : LightKindTag::Point;
        e->light.color = Vec3(static_cast<float>(r.asFloat()), static_cast<float>(g.asFloat()),
                              static_cast<float>(b.asFloat()));
        e->light.intensity = static_cast<float>(intensity.asFloat());
        activeScene().dirty = true;
    }
    return Value();
}

Value AddEntityBody(const Value& entity, const Value& shape, const Value& mass, const Value& sizeX,
                    const Value& sizeY, const Value& sizeZ) {
    Entity* e = getEntity(entity);
    if (!e) return Value(false);
    e->add(COMP_BODY);
    ColliderRecord& col = e->body.colliderDesc;
    std::string s = strOr(shape, "box");
    float sx = static_cast<float>(sizeX.asFloat());
    float sy = static_cast<float>(sizeY.asFloat());
    float sz = static_cast<float>(sizeZ.asFloat());

    if (s == "sphere") {
        col.kind = ColliderRecord::Kind::Sphere;
        col.radius = sx * 0.5f;
    } else if (s == "capsule") {
        col.kind = ColliderRecord::Kind::Capsule;
        col.radius = sx * 0.5f;
        col.height = sy;
    } else {
        col.kind = ColliderRecord::Kind::Box;
        // The API takes full extents, matching CreateBoxCollider.
        col.halfExtents = Vec3(sx * 0.5f, sy * 0.5f, sz * 0.5f);
    }
    e->body.mass = static_cast<float>(mass.asFloat());
    e->body.isStatic = e->body.mass <= 0.0f;
    activeScene().dirty = true;

    // If the scene is already running, instantiate immediately so a body
    // spawned mid-game starts simulating this frame rather than at the next
    // Play.
    if (activeScene().running) {
        Scene& sc = activeScene();
        sceneStop(sc);
        sceneStart(sc);
    }
    return Value(true);
}

Value GetEntityBody(const Value& entity) {
    Entity* e = getEntity(entity);
    return Value::MakeHandle(e && e->has(COMP_BODY) ? e->body.body : 0, "body");
}

Value AddEntityEmitter(const Value& entity) {
    Entity* e = getEntity(entity);
    if (!e) return Value::MakeHandle(0, "emitter");
    e->add(COMP_EMITTER);
    if (e->emitter.emitter == 0) {
        EmitterRecord er;
        er.name = e->name;
        e->emitter.emitter = engine().emitters.add(std::move(er));
    }
    activeScene().dirty = true;
    return Value::MakeHandle(e->emitter.emitter, "emitter");
}

Value GetEntityEmitter(const Value& entity) {
    Entity* e = getEntity(entity);
    return Value::MakeHandle(e && e->has(COMP_EMITTER) ? e->emitter.emitter : 0, "emitter");
}

Value AddEntityAnimator(const Value& entity, const Value& path, const Value& clip) {
    Entity* e = getEntity(entity);
    if (!e) return Value::MakeHandle(0, "animation");
    e->add(COMP_ANIMATOR);
    e->animator.sourcePath = path.asString();
    e->animator.autoPlayClip = clip.asString();
    if (e->animator.animation == 0 && !e->animator.sourcePath.empty()) {
        Value handle = LoadAnimation(Value(e->animator.sourcePath));
        e->animator.animation = static_cast<uint64_t>(handle.asInt());
        if (e->animator.playOnStart) PlayAnimation(handle, Value(e->animator.autoPlayClip));
    }
    activeScene().dirty = true;
    return Value::MakeHandle(e->animator.animation, "animation");
}

Value GetEntityAnimation(const Value& entity) {
    Entity* e = getEntity(entity);
    return Value::MakeHandle(e && e->has(COMP_ANIMATOR) ? e->animator.animation : 0, "animation");
}

Value AddEntityCamera(const Value& entity, const Value& fov) {
    uint64_t id = static_cast<uint64_t>(entity.asInt());
    Scene& s = activeScene();
    if (Entity* e = s.entities.get(id)) {
        e->add(COMP_CAMERA);
        e->camera.fovDegrees = static_cast<float>(fov.asFloat());
        // First camera added becomes active, so a script that adds exactly
        // one doesn't also have to call SetActiveCamera.
        if (s.activeCamera == 0) {
            s.activeCamera = id;
            e->camera.active = true;
        }
        s.dirty = true;
    }
    return Value();
}

Value SetActiveCamera(const Value& entity) {
    Scene& s = activeScene();
    uint64_t id = static_cast<uint64_t>(entity.asInt());
    for (auto& kv : s.entities) {
        if (kv.second.has(COMP_CAMERA)) kv.second.camera.active = (kv.first == id);
    }
    s.activeCamera = id;
    return Value();
}

} // namespace rt
} // namespace pv
