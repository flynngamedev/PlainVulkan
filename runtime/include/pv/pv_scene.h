// pv_scene.h -- the scene graph: entities, their components, and the
// serialized `.pvscene` format the editor reads and writes.
//
// PlainVulkan had no notion of a scene before this: a .pv script called
// Pv::DrawMesh in a loop and that was the whole world model. That works
// for a demo and falls apart the moment you want to *author* something,
// because there's nothing for an editor to select, inspect, or save.
//
// The model chosen here is a plain hierarchical entity/component tree, not
// an archetype ECS. That's a deliberate call: an ECS's win is cache-coherent
// iteration over huge homogeneous entity counts, and it costs a lot of
// indirection to get there. A PlainVulkan scene is hundreds of entities
// authored by hand, the per-frame work is dominated by draw submission, and
// the thing that actually matters is that an editor can point at one entity
// and show you its fields. A struct with optional components does that
// directly and stays readable.
//
// Components are `std::optional`-shaped via a bitmask + inline storage
// rather than heap-allocated polymorphic parts, so an Entity is one
// contiguous object and serialization is a straight field walk.
#pragma once

#include "pv/pv_anim.h"
#include "pv/pv_handle.h"
#include "pv/pv_math.h"
#include "pv/pv_particles.h"
#include "pv/pv_physics.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace pv {

// Which components an entity carries. A bitmask rather than one bool per
// component so "does this entity have anything renderable" is one AND.
enum ComponentBits : uint32_t {
    COMP_NONE      = 0,
    COMP_MESH      = 1u << 0,
    COMP_LIGHT     = 1u << 1,
    COMP_BODY      = 1u << 2,
    COMP_EMITTER   = 1u << 3,
    COMP_ANIMATOR  = 1u << 4,
    COMP_CAMERA    = 1u << 5,
    COMP_SPRITE    = 1u << 6,
    COMP_TEXT      = 1u << 7,
};

// Light kinds live here rather than in pv_internal.h because both the
// scene's authoring-time LightComponent and the renderer's runtime
// LightRecord need them, and pv_internal.h (which pulls in Vulkan) is
// downstream of this header. pv_internal.h aliases LightKind to this.
enum class LightKindTag { Point, Directional, Spot };

// --- component payloads -------------------------------------------------

struct MeshComponent {
    uint64_t mesh = 0;
    uint64_t material = 0;
    bool castShadows = true;
    bool visible = true;
    // Set when the mesh came from a file, so the editor can show the source
    // and reload it, and so saving a scene records the asset rather than an
    // opaque runtime handle that means nothing on the next launch.
    std::string sourcePath;
    // Which procedural primitive to rebuild, when sourcePath is empty.
    // Serialized so a scene made of cubes and spheres round-trips without
    // needing any asset files at all.
    std::string primitive; // "cube" | "sphere" | "plane" | "cylinder" | ""
    Vec3 primitiveParams{1, 1, 1};
};

struct LightComponent {
    LightKindTag kind = LightKindTag::Point;
    Vec3 color{1, 1, 1};
    float intensity = 1.0f;
    float radius = 10.0f;
    float coneAngle = 30.0f * PV_DEG2RAD;
    uint64_t runtimeLight = 0; // handle in Engine::lights, re-created on scene load
};

struct BodyComponent {
    uint64_t body = 0;
    uint64_t collider = 0;
    // Authoring-time description, kept separately from the runtime handles
    // so the scene file describes the *intent* (a 2x2x2 box, 5kg, dynamic)
    // and loading re-creates real physics objects from it.
    ColliderRecord colliderDesc;
    float mass = 1.0f;
    bool isStatic = false;
    bool isKinematic = false;
};

struct EmitterComponent {
    uint64_t emitter = 0;
    bool playOnStart = true;
};

struct AnimatorComponent {
    uint64_t animation = 0;
    std::string sourcePath;
    std::string autoPlayClip;
    bool playOnStart = true;
    float speed = 1.0f;
    bool looping = true;
    // When set, the sampled root node transform drives the entity's own
    // transform. Off by default, because most props want the animation to
    // play *within* their placed transform rather than teleport them to
    // wherever the animation was authored.
    bool applyRootMotion = false;
};

struct CameraComponent {
    float fovDegrees = 60.0f;
    float nearZ = 0.05f, farZ = 1000.0f;
    bool orthographic = false;
    float orthoHeight = 10.0f;
    bool active = false; // exactly one camera per scene should be active
};

struct SpriteComponent {
    uint64_t texture = 0;
    std::string sourcePath;
    Vec4 tint{1, 1, 1, 1};
    float width = 1.0f, height = 1.0f;
    bool billboard = true; // face the camera, as opposed to lying in the XY plane
};

struct TextComponent {
    std::string text = "Text";
    float size = 16.0f;
    Vec4 color{1, 1, 1, 1};
    bool worldSpace = false;
};

// --- the entity ---------------------------------------------------------

struct Entity {
    uint64_t id = 0;
    std::string name = "Entity";
    uint64_t parent = 0;              // 0 = root-level
    std::vector<uint64_t> children;

    Transform local;
    // Cached world matrix, rebuilt each frame by updateSceneTransforms.
    // Cached rather than recomputed on access because a deep hierarchy
    // would otherwise recompose the same parent chain once per query.
    Mat4 world = Mat4::identity();
    bool worldDirty = true;

    bool active = true;               // inactive entities skip update and draw
    std::vector<std::string> tags;    // cheap grouping, queryable via Pv::FindEntitiesByTag

    uint32_t components = COMP_NONE;
    MeshComponent mesh;
    LightComponent light;
    BodyComponent body;
    EmitterComponent emitter;
    AnimatorComponent animator;
    CameraComponent camera;
    SpriteComponent sprite;
    TextComponent text;

    bool has(ComponentBits c) const { return (components & c) != 0; }
    void add(ComponentBits c) { components |= c; }
    void remove(ComponentBits c) { components &= ~static_cast<uint32_t>(c); }
};

// --- the scene ----------------------------------------------------------

struct Scene {
    std::string name = "Untitled";
    std::string path;                 // where it was loaded from / will save to
    HandleTable<Entity> entities;
    std::vector<uint64_t> roots;      // entities with parent == 0, in author order

    // Environment settings, saved with the scene so lighting is part of the
    // authored content rather than something the script has to re-set.
    Vec3 ambientColor{0.1f, 0.1f, 0.1f};
    float ambientIntensity = 1.0f;
    Vec4 clearColor{0.05f, 0.06f, 0.09f, 1.0f};
    bool fogEnabled = false;
    Vec3 fogColor{0.5f, 0.6f, 0.7f};
    float fogNear = 50.0f, fogFar = 200.0f;
    Vec3 gravity{0, -9.8f, 0};

    uint64_t activeCamera = 0;

    // True once the scene has been started (physics bodies instantiated,
    // playOnStart emitters and animators kicked off). The editor toggles
    // this on Play/Stop.
    bool running = false;
    bool dirty = false; // unsaved changes, for the editor's title bar

    Entity* find(uint64_t id) { return entities.get(id); }
    const Entity* find(uint64_t id) const { return entities.get(id); }
};

// --- lifecycle ----------------------------------------------------------

uint64_t sceneCreateEntity(Scene& s, const std::string& name, uint64_t parent = 0);
void sceneDestroyEntity(Scene& s, uint64_t id);          // recursive, detaches from parent
void sceneSetParent(Scene& s, uint64_t child, uint64_t parent, bool keepWorldTransform = true);
uint64_t sceneFindByName(const Scene& s, const std::string& name);
std::vector<uint64_t> sceneFindByTag(const Scene& s, const std::string& tag);

// Recomputes `world` for every dirty entity, parents before children.
void updateSceneTransforms(Scene& s);

// Marks an entity and its whole subtree as needing a world recompute.
void markWorldDirty(Scene& s, uint64_t id);

// Instantiates runtime resources the scene describes: creates physics
// bodies, registers lights, starts playOnStart emitters/animators.
// Idempotent -- calling it twice won't double-create.
void sceneStart(Scene& s);

// Tears the runtime resources back down, leaving the authored data intact
// so the editor can Play -> Stop -> Play without reloading from disk.
void sceneStop(Scene& s);

// Per-frame: advances animators, emitters, and physics, then writes
// simulated poses back onto entity transforms.
void sceneUpdate(Scene& s, float dt);

// Submits every visible renderable in the scene.
void sceneDraw(Scene& s);

// --- serialization ------------------------------------------------------
// Format is JSON, written by pv_json.h. Chosen over a binary format
// because a scene file that a human can diff and merge in git is worth far
// more to a small project than the load-time saving, and over the engine's
// own .pv syntax because a scene is data, not code.

// The engine's single active scene, created on first use. Declared here so
// the editor and the Pv:: scene commands share one accessor.
Scene& activeScene();

bool sceneSaveToFile(const Scene& s, const std::string& path, std::string* error = nullptr);
bool sceneLoadFromFile(Scene& s, const std::string& path, std::string* error = nullptr);
std::string sceneToJson(const Scene& s);
bool sceneFromJson(Scene& s, const std::string& json, std::string* error = nullptr);

} // namespace pv
