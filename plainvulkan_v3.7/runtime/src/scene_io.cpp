// scene_io.cpp -- reading and writing the `.pvscene` format.
//
// Format notes:
//   * JSON, pretty-printed, with vectors as flat arrays. A scene file that
//     a human can read, diff, and merge in git is worth more to a small
//     project than a faster binary load.
//   * Entities are stored as a flat array with explicit parent ids rather
//     than nested objects. Nesting reads nicer but makes a re-parent a
//     structural rewrite of the file; a flat list keeps the diff to one
//     changed field.
//   * Nothing runtime-derived is written -- no mesh/texture/body handles,
//     which are meaningless across a restart. The file records *intent*
//     (this entity is a 2m cube of mass 5) and sceneStart re-creates the
//     resources from it.
//   * Every field is optional on load, defaulting to the value in the
//     component struct. A scene written by an older build therefore still
//     loads in a newer one.
#include "pv/pv_internal.h"
#include "pv/pv_json.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace pv {

using json::Json;
using json::JsonArray;

namespace {

constexpr int kSceneFormatVersion = 1;

Json transformToJson(const Transform& t) {
    Json o = Json::object();
    o["position"] = json::vec3ToJson(t.position);
    // Rotation is stored as a quaternion, not Euler angles: Euler is
    // ambiguous (the same orientation has multiple representations, and
    // gimbal lock loses information), so round-tripping through it would
    // slowly corrupt an entity's rotation each save.
    JsonArray q{Json(t.rotation.x), Json(t.rotation.y), Json(t.rotation.z), Json(t.rotation.w)};
    o["rotation"] = Json(std::move(q));
    o["scale"] = json::vec3ToJson(t.scale);
    return o;
}

Transform transformFromJson(const Json& j) {
    Transform t;
    t.position = json::jsonToVec3<Vec3>(j["position"], Vec3(0, 0, 0));
    const Json& r = j["rotation"];
    if (r.isArray() && r.size() >= 4) {
        t.rotation = Quat(r[0].asFloat(0), r[1].asFloat(0), r[2].asFloat(0), r[3].asFloat(1)).normalized();
    }
    t.scale = json::jsonToVec3<Vec3>(j["scale"], Vec3(1, 1, 1));
    return t;
}

const char* lightKindName(LightKindTag k) {
    switch (k) {
        case LightKindTag::Directional: return "directional";
        case LightKindTag::Spot: return "spot";
        case LightKindTag::Point:
        default: return "point";
    }
}
LightKindTag lightKindFromName(const std::string& s) {
    if (s == "directional") return LightKindTag::Directional;
    if (s == "spot") return LightKindTag::Spot;
    return LightKindTag::Point;
}

const char* colliderKindName(ColliderRecord::Kind k) {
    switch (k) {
        case ColliderRecord::Kind::Sphere: return "sphere";
        case ColliderRecord::Kind::Capsule: return "capsule";
        case ColliderRecord::Kind::Box:
        default: return "box";
    }
}
ColliderRecord::Kind colliderKindFromName(const std::string& s) {
    if (s == "sphere") return ColliderRecord::Kind::Sphere;
    if (s == "capsule") return ColliderRecord::Kind::Capsule;
    return ColliderRecord::Kind::Box;
}

const char* emitterShapeName(EmitterShape s) {
    switch (s) {
        case EmitterShape::Point: return "point";
        case EmitterShape::Sphere: return "sphere";
        case EmitterShape::Hemisphere: return "hemisphere";
        case EmitterShape::Box: return "box";
        case EmitterShape::Circle: return "circle";
        case EmitterShape::Cone:
        default: return "cone";
    }
}
EmitterShape emitterShapeFromName(const std::string& s) {
    if (s == "point") return EmitterShape::Point;
    if (s == "sphere") return EmitterShape::Sphere;
    if (s == "hemisphere") return EmitterShape::Hemisphere;
    if (s == "box") return EmitterShape::Box;
    if (s == "circle") return EmitterShape::Circle;
    return EmitterShape::Cone;
}

const char* blendName(ParticleBlend b) {
    switch (b) {
        case ParticleBlend::Additive: return "additive";
        case ParticleBlend::Opaque: return "opaque";
        case ParticleBlend::Alpha:
        default: return "alpha";
    }
}
ParticleBlend blendFromName(const std::string& s) {
    if (s == "additive") return ParticleBlend::Additive;
    if (s == "opaque") return ParticleBlend::Opaque;
    return ParticleBlend::Alpha;
}

Json curveToJson(const LifetimeCurve& c) {
    JsonArray a{Json(c.p0), Json(c.p1), Json(c.p2), Json(c.p3)};
    return Json(std::move(a));
}
LifetimeCurve curveFromJson(const Json& j, LifetimeCurve def) {
    if (!j.isArray() || j.size() < 4) return def;
    return LifetimeCurve{j[0].asFloat(def.p0), j[1].asFloat(def.p1), j[2].asFloat(def.p2), j[3].asFloat(def.p3)};
}

// --- emitter settings -------------------------------------------------
// An emitter's authored settings are serialized from the live
// EmitterRecord when one exists (the scene is running and the user has been
// tweaking it), and skipped otherwise.

Json emitterSettingsToJson(const EmitterRecord& e) {
    Json o = Json::object();
    o["shape"] = Json(emitterShapeName(e.shape));
    o["radius"] = Json(e.radius);
    o["halfExtents"] = json::vec3ToJson(e.halfExtents);
    o["coneAngle"] = Json(e.coneAngle);
    o["rate"] = Json(e.rate);
    o["looping"] = Json(e.looping);
    o["duration"] = Json(e.duration);
    o["maxParticles"] = Json(e.maxParticles);
    o["speedMin"] = Json(e.speedMin);
    o["speedMax"] = Json(e.speedMax);
    o["lifetimeMin"] = Json(e.lifetimeMin);
    o["lifetimeMax"] = Json(e.lifetimeMax);
    o["sizeMin"] = Json(e.sizeMin);
    o["sizeMax"] = Json(e.sizeMax);
    o["rotationSpeedMin"] = Json(e.rotationSpeedMin);
    o["rotationSpeedMax"] = Json(e.rotationSpeedMax);
    o["gravity"] = json::vec3ToJson(e.gravity);
    o["gravityScale"] = Json(e.gravityScale);
    o["drag"] = Json(e.drag);
    o["wind"] = json::vec3ToJson(e.windForce);
    o["vortex"] = Json(e.vortexStrength);
    o["colorStart"] = json::vec4ToJson(e.colorStart);
    o["colorEnd"] = json::vec4ToJson(e.colorEnd);
    o["sizeCurve"] = curveToJson(e.sizeCurve);
    o["alphaCurve"] = curveToJson(e.alphaCurve);
    o["blend"] = Json(blendName(e.blend));
    o["sheetCols"] = Json(e.sheetCols);
    o["sheetRows"] = Json(e.sheetRows);
    o["localSpace"] = Json(e.localSpace);
    return o;
}

void emitterSettingsFromJson(const Json& j, EmitterRecord& e) {
    if (!j.isObject()) return;
    e.shape = emitterShapeFromName(j["shape"].asString(emitterShapeName(e.shape)));
    e.radius = j["radius"].asFloat(e.radius);
    e.halfExtents = json::jsonToVec3<Vec3>(j["halfExtents"], e.halfExtents);
    e.coneAngle = j["coneAngle"].asFloat(e.coneAngle);
    e.rate = j["rate"].asFloat(e.rate);
    e.looping = j["looping"].asBool(e.looping);
    e.duration = j["duration"].asFloat(e.duration);
    e.maxParticles = j["maxParticles"].asInt(e.maxParticles);
    e.speedMin = j["speedMin"].asFloat(e.speedMin);
    e.speedMax = j["speedMax"].asFloat(e.speedMax);
    e.lifetimeMin = j["lifetimeMin"].asFloat(e.lifetimeMin);
    e.lifetimeMax = j["lifetimeMax"].asFloat(e.lifetimeMax);
    e.sizeMin = j["sizeMin"].asFloat(e.sizeMin);
    e.sizeMax = j["sizeMax"].asFloat(e.sizeMax);
    e.rotationSpeedMin = j["rotationSpeedMin"].asFloat(e.rotationSpeedMin);
    e.rotationSpeedMax = j["rotationSpeedMax"].asFloat(e.rotationSpeedMax);
    e.gravity = json::jsonToVec3<Vec3>(j["gravity"], e.gravity);
    e.gravityScale = j["gravityScale"].asFloat(e.gravityScale);
    e.drag = j["drag"].asFloat(e.drag);
    e.windForce = json::jsonToVec3<Vec3>(j["wind"], e.windForce);
    e.vortexStrength = j["vortex"].asFloat(e.vortexStrength);
    e.colorStart = json::jsonToVec4<Vec4>(j["colorStart"], e.colorStart);
    e.colorEnd = json::jsonToVec4<Vec4>(j["colorEnd"], e.colorEnd);
    e.sizeCurve = curveFromJson(j["sizeCurve"], e.sizeCurve);
    e.alphaCurve = curveFromJson(j["alphaCurve"], e.alphaCurve);
    e.blend = blendFromName(j["blend"].asString(blendName(e.blend)));
    e.sheetCols = j["sheetCols"].asInt(e.sheetCols);
    e.sheetRows = j["sheetRows"].asInt(e.sheetRows);
    e.localSpace = j["localSpace"].asBool(e.localSpace);
}

Json entityToJson(const Scene& s, const Entity& e) {
    (void)s;
    Json o = Json::object();
    o["id"] = Json(e.id);
    o["name"] = Json(e.name);
    o["parent"] = Json(e.parent);
    o["active"] = Json(e.active);
    o["transform"] = transformToJson(e.local);

    if (!e.tags.empty()) {
        JsonArray tags;
        for (const auto& t : e.tags) tags.push_back(Json(t));
        o["tags"] = Json(std::move(tags));
    }

    if (e.has(COMP_MESH)) {
        Json c = Json::object();
        c["source"] = Json(normalizeSlashes(e.mesh.sourcePath));
        c["primitive"] = Json(e.mesh.primitive);
        c["params"] = json::vec3ToJson(e.mesh.primitiveParams);
        c["visible"] = Json(e.mesh.visible);
        c["castShadows"] = Json(e.mesh.castShadows);
        o["mesh"] = c;
    }
    if (e.has(COMP_LIGHT)) {
        Json c = Json::object();
        c["kind"] = Json(lightKindName(e.light.kind));
        c["color"] = json::vec3ToJson(e.light.color);
        c["intensity"] = Json(e.light.intensity);
        c["radius"] = Json(e.light.radius);
        c["coneAngle"] = Json(e.light.coneAngle);
        o["light"] = c;
    }
    if (e.has(COMP_BODY)) {
        Json c = Json::object();
        const ColliderRecord& col = e.body.colliderDesc;
        c["shape"] = Json(colliderKindName(col.kind));
        c["center"] = json::vec3ToJson(col.center);
        c["halfExtents"] = json::vec3ToJson(col.halfExtents);
        c["radius"] = Json(col.radius);
        c["height"] = Json(col.height);
        c["staticFriction"] = Json(col.staticFriction);
        c["dynamicFriction"] = Json(col.dynamicFriction);
        c["restitution"] = Json(col.restitution);
        c["trigger"] = Json(col.isTrigger);
        c["mass"] = Json(e.body.mass);
        c["static"] = Json(e.body.isStatic);
        c["kinematic"] = Json(e.body.isKinematic);
        o["body"] = c;
    }
    if (e.has(COMP_EMITTER)) {
        Json c = Json::object();
        c["playOnStart"] = Json(e.emitter.playOnStart);
        if (e.emitter.emitter != 0) {
            if (const EmitterRecord* er = engine().emitters.get(e.emitter.emitter)) {
                c["settings"] = emitterSettingsToJson(*er);
            }
        }
        o["emitter"] = c;
    }
    if (e.has(COMP_ANIMATOR)) {
        Json c = Json::object();
        c["source"] = Json(normalizeSlashes(e.animator.sourcePath));
        c["clip"] = Json(e.animator.autoPlayClip);
        c["playOnStart"] = Json(e.animator.playOnStart);
        c["speed"] = Json(e.animator.speed);
        c["looping"] = Json(e.animator.looping);
        c["rootMotion"] = Json(e.animator.applyRootMotion);
        o["animator"] = c;
    }
    if (e.has(COMP_CAMERA)) {
        Json c = Json::object();
        c["fov"] = Json(e.camera.fovDegrees);
        c["near"] = Json(e.camera.nearZ);
        c["far"] = Json(e.camera.farZ);
        c["orthographic"] = Json(e.camera.orthographic);
        c["orthoHeight"] = Json(e.camera.orthoHeight);
        c["active"] = Json(e.camera.active);
        o["camera"] = c;
    }
    if (e.has(COMP_SPRITE)) {
        Json c = Json::object();
        c["source"] = Json(normalizeSlashes(e.sprite.sourcePath));
        c["tint"] = json::vec4ToJson(e.sprite.tint);
        c["width"] = Json(e.sprite.width);
        c["height"] = Json(e.sprite.height);
        c["billboard"] = Json(e.sprite.billboard);
        o["sprite"] = c;
    }
    if (e.has(COMP_TEXT)) {
        Json c = Json::object();
        c["text"] = Json(e.text.text);
        c["size"] = Json(e.text.size);
        c["color"] = json::vec4ToJson(e.text.color);
        c["worldSpace"] = Json(e.text.worldSpace);
        o["text"] = c;
    }
    return o;
}

void entityFromJson(Scene& s, const Json& j, Entity& e) {
    e.name = j["name"].asString("Entity");
    e.active = j["active"].asBool(true);
    e.local = transformFromJson(j["transform"]);
    e.worldDirty = true;

    for (const auto& t : j["tags"].asArray()) e.tags.push_back(t.asString());

    if (j.has("mesh")) {
        const Json& c = j["mesh"];
        e.add(COMP_MESH);
        e.mesh.sourcePath = c["source"].asString();
        e.mesh.primitive = c["primitive"].asString();
        e.mesh.primitiveParams = json::jsonToVec3<Vec3>(c["params"], Vec3(1, 1, 1));
        e.mesh.visible = c["visible"].asBool(true);
        e.mesh.castShadows = c["castShadows"].asBool(true);
    }
    if (j.has("light")) {
        const Json& c = j["light"];
        e.add(COMP_LIGHT);
        e.light.kind = lightKindFromName(c["kind"].asString("point"));
        e.light.color = json::jsonToVec3<Vec3>(c["color"], Vec3(1, 1, 1));
        e.light.intensity = c["intensity"].asFloat(1.0f);
        e.light.radius = c["radius"].asFloat(10.0f);
        e.light.coneAngle = c["coneAngle"].asFloat(30.0f * PV_DEG2RAD);
    }
    if (j.has("body")) {
        const Json& c = j["body"];
        e.add(COMP_BODY);
        ColliderRecord& col = e.body.colliderDesc;
        col.kind = colliderKindFromName(c["shape"].asString("box"));
        col.center = json::jsonToVec3<Vec3>(c["center"], Vec3(0, 0, 0));
        col.halfExtents = json::jsonToVec3<Vec3>(c["halfExtents"], Vec3(0.5f, 0.5f, 0.5f));
        col.radius = c["radius"].asFloat(0.5f);
        col.height = c["height"].asFloat(1.0f);
        col.staticFriction = c["staticFriction"].asFloat(0.5f);
        col.dynamicFriction = c["dynamicFriction"].asFloat(0.5f);
        col.restitution = c["restitution"].asFloat(0.0f);
        col.isTrigger = c["trigger"].asBool(false);
        e.body.mass = c["mass"].asFloat(1.0f);
        e.body.isStatic = c["static"].asBool(false);
        e.body.isKinematic = c["kinematic"].asBool(false);
    }
    if (j.has("emitter")) {
        const Json& c = j["emitter"];
        e.add(COMP_EMITTER);
        e.emitter.playOnStart = c["playOnStart"].asBool(true);
        // The emitter's settings are instantiated here rather than at
        // sceneStart, so the editor can show and tweak them before Play.
        EmitterRecord er;
        er.name = e.name;
        emitterSettingsFromJson(c["settings"], er);
        er.enabled = e.emitter.playOnStart;
        e.emitter.emitter = engine().emitters.add(std::move(er));
    }
    if (j.has("animator")) {
        const Json& c = j["animator"];
        e.add(COMP_ANIMATOR);
        e.animator.sourcePath = c["source"].asString();
        e.animator.autoPlayClip = c["clip"].asString();
        e.animator.playOnStart = c["playOnStart"].asBool(true);
        e.animator.speed = c["speed"].asFloat(1.0f);
        e.animator.looping = c["looping"].asBool(true);
        e.animator.applyRootMotion = c["rootMotion"].asBool(false);
    }
    if (j.has("camera")) {
        const Json& c = j["camera"];
        e.add(COMP_CAMERA);
        e.camera.fovDegrees = c["fov"].asFloat(60.0f);
        e.camera.nearZ = c["near"].asFloat(0.05f);
        e.camera.farZ = c["far"].asFloat(1000.0f);
        e.camera.orthographic = c["orthographic"].asBool(false);
        e.camera.orthoHeight = c["orthoHeight"].asFloat(10.0f);
        e.camera.active = c["active"].asBool(false);
    }
    if (j.has("sprite")) {
        const Json& c = j["sprite"];
        e.add(COMP_SPRITE);
        e.sprite.sourcePath = c["source"].asString();
        e.sprite.tint = json::jsonToVec4<Vec4>(c["tint"], Vec4(1, 1, 1, 1));
        e.sprite.width = c["width"].asFloat(1.0f);
        e.sprite.height = c["height"].asFloat(1.0f);
        e.sprite.billboard = c["billboard"].asBool(true);
    }
    if (j.has("text")) {
        const Json& c = j["text"];
        e.add(COMP_TEXT);
        e.text.text = c["text"].asString("Text");
        e.text.size = c["size"].asFloat(16.0f);
        e.text.color = json::jsonToVec4<Vec4>(c["color"], Vec4(1, 1, 1, 1));
        e.text.worldSpace = c["worldSpace"].asBool(false);
    }
    (void)s;
}

} // namespace

std::string sceneToJson(const Scene& s) {
    Json root = Json::object();
    root["format"] = Json("pvscene");
    root["version"] = Json(kSceneFormatVersion);
    root["name"] = Json(s.name);

    Json env = Json::object();
    env["ambientColor"] = json::vec3ToJson(s.ambientColor);
    env["ambientIntensity"] = Json(s.ambientIntensity);
    env["clearColor"] = json::vec4ToJson(s.clearColor);
    env["fogEnabled"] = Json(s.fogEnabled);
    env["fogColor"] = json::vec3ToJson(s.fogColor);
    env["fogNear"] = Json(s.fogNear);
    env["fogFar"] = Json(s.fogFar);
    env["gravity"] = json::vec3ToJson(s.gravity);
    root["environment"] = env;
    root["activeCamera"] = Json(s.activeCamera);

    // Depth-first from the roots so the file's entity order matches the
    // hierarchy. That makes the file readable, and it means loading can
    // attach each entity to a parent that already exists.
    JsonArray entities;
    std::vector<uint64_t> stack(s.roots.rbegin(), s.roots.rend());
    while (!stack.empty()) {
        uint64_t id = stack.back();
        stack.pop_back();
        const Entity* e = s.entities.get(id);
        if (!e) continue;
        entities.push_back(entityToJson(s, *e));
        for (auto it = e->children.rbegin(); it != e->children.rend(); ++it) stack.push_back(*it);
    }
    root["entities"] = Json(std::move(entities));
    return root.dump(2);
}

bool sceneFromJson(Scene& s, const std::string& text, std::string* error) {
    std::string parseError;
    Json root = Json::parse(text, &parseError);
    if (!parseError.empty()) {
        if (error) *error = "malformed scene JSON: " + parseError;
        return false;
    }
    if (root["format"].asString() != "pvscene") {
        if (error) *error = "not a PlainVulkan scene file (missing \"format\": \"pvscene\")";
        return false;
    }
    int version = root["version"].asInt(0);
    if (version > kSceneFormatVersion) {
        // Load anyway: unknown fields are ignored and every known field has
        // a default, so a forward-version file degrades rather than fails.
        logLine("WARNING", "Scene: file is format version " + std::to_string(version) + " but this build knows " +
                               std::to_string(kSceneFormatVersion) +
                               " -- loading anyway; unrecognized fields will be dropped on the next save");
    }

    sceneStop(s);
    s.entities.clear();
    s.roots.clear();
    s.activeCamera = 0;
    s.running = false;

    s.name = root["name"].asString("Untitled");
    const Json& env = root["environment"];
    s.ambientColor = json::jsonToVec3<Vec3>(env["ambientColor"], Vec3(0.1f, 0.1f, 0.1f));
    s.ambientIntensity = env["ambientIntensity"].asFloat(1.0f);
    s.clearColor = json::jsonToVec4<Vec4>(env["clearColor"], Vec4(0.05f, 0.06f, 0.09f, 1.0f));
    s.fogEnabled = env["fogEnabled"].asBool(false);
    s.fogColor = json::jsonToVec3<Vec3>(env["fogColor"], Vec3(0.5f, 0.6f, 0.7f));
    s.fogNear = env["fogNear"].asFloat(50.0f);
    s.fogFar = env["fogFar"].asFloat(200.0f);
    s.gravity = json::jsonToVec3<Vec3>(env["gravity"], Vec3(0, -9.8f, 0));

    // Two passes. The file's ids are remapped to freshly allocated ones,
    // because ids from the file could collide with handles already issued in
    // this process (the editor loads a second scene without restarting).
    std::unordered_map<uint64_t, uint64_t> idMap;
    const JsonArray& arr = root["entities"].asArray();

    for (const Json& je : arr) {
        uint64_t fileId = je["id"].asUInt64(0);
        uint64_t newId = sceneCreateEntity(s, je["name"].asString("Entity"), 0);
        idMap[fileId] = newId;
    }

    size_t i = 0;
    for (const Json& je : arr) {
        if (i >= arr.size()) break;
        uint64_t fileId = je["id"].asUInt64(0);
        auto it = idMap.find(fileId);
        if (it == idMap.end()) { i++; continue; }
        Entity* e = s.entities.get(it->second);
        if (!e) { i++; continue; }

        entityFromJson(s, je, *e);

        uint64_t fileParent = je["parent"].asUInt64(0);
        if (fileParent != 0) {
            auto pit = idMap.find(fileParent);
            if (pit != idMap.end()) {
                // keepWorldTransform=false: the stored local transform is
                // already relative to the parent, so preserving world space
                // here would double-apply the parent's transform.
                sceneSetParent(s, it->second, pit->second, false);
            }
        }
        i++;
    }

    uint64_t fileCam = root["activeCamera"].asUInt64(0);
    if (fileCam != 0) {
        auto it = idMap.find(fileCam);
        if (it != idMap.end()) s.activeCamera = it->second;
    }
    if (s.activeCamera == 0) {
        for (const auto& kv : s.entities) {
            if (kv.second.has(COMP_CAMERA) && kv.second.camera.active) {
                s.activeCamera = kv.first;
                break;
            }
        }
    }

    updateSceneTransforms(s);
    s.dirty = false;
    if (error) error->clear();
    return true;
}

bool sceneSaveToFile(const Scene& s, const std::string& path, std::string* error) {
    // Write to a temporary and rename over the target, so an interrupted
    // save (full disk, crash, power loss) can't leave a truncated scene file
    // where the user's work used to be.
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            if (error) *error = "could not open '" + tmp + "' for writing";
            return false;
        }
        f << sceneToJson(s);
        if (!f.good()) {
            if (error) *error = "write error while saving '" + tmp + "'";
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // Windows' rename fails if the destination exists, unlike POSIX.
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            if (error) *error = "could not replace '" + path + "': " + ec.message();
            return false;
        }
    }
    if (error) error->clear();
    return true;
}

bool sceneLoadFromFile(Scene& s, const std::string& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        if (error) *error = "could not open '" + path + "'";
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!sceneFromJson(s, ss.str(), error)) return false;
    s.path = path;
    return true;
}

} // namespace pv
