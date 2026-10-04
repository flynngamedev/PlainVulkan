// pvedit.cpp -- the PlainVulkan scene editor.
//
// A separate executable from a game binary. It links the same pv_runtime
// static library, creates the same window and Vulkan device, and drives the
// same Scene the runtime does -- so what you see in the viewport is the
// engine rendering, not a preview that approximates it.
//
// Why Dear ImGui rather than the engine's own Pv::UI* layer: that layer is
// a genuinely nice ~250-line immediate-mode toolkit built on the 3x5 bitmap
// font, and it's the right tool for a game's HUD. An editor needs dockable
// panels, tree views, drag-reorder, text fields with selection, and a font
// that can render a file path legibly -- rebuilding all of that would be a
// project in itself and wouldn't make the engine better. ImGui's official
// Vulkan/GLFW backends attach to the descriptor pool and render pass this
// runtime already has, so the integration is small.
//
// The editor deliberately shares the runtime's Engine singleton rather than
// standing up its own: Play mode has to exercise the exact same code path a
// shipped game does, or the editor stops being a reliable preview.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

#include "gizmo.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using namespace pv;

// ======================================================================
// Editor state
// ======================================================================

struct EditorState {
    uint64_t selected = 0;
    uint64_t renaming = 0;
    char renameBuffer[128] = {};

    // Editor camera, independent of any scene camera so you can fly around
    // while the game's own camera is doing something else.
    Vec3 camPos{6, 5, 10};
    float camYaw = -0.6f;
    float camPitch = -0.35f;
    float camSpeed = 8.0f;
    bool flying = false;

    bool playing = false;
    // The scene is serialized to a string before Play and restored on Stop.
    // This is why Play->Stop returns you to exactly what you authored even
    // though Play mutates transforms via physics: the alternative (undoing
    // each change) is far harder to get right.
    std::string preplaySnapshot;

    std::string statusMessage;
    double statusExpiry = 0.0;

    std::string assetDir = "assets";
    char scenePathBuffer[512] = "scene.pvscene";

    bool showHierarchy = true;
    bool showInspector = true;
    bool showAssets = true;
    bool showParticles = false;
    bool showAnimation = false;
    bool showStats = true;

    gizmo::Mode gizmoMode = gizmo::Mode::Translate;
    gizmo::State gizmoState;
};

EditorState g;

void setStatus(const std::string& msg) {
    g.statusMessage = msg;
    g.statusExpiry = ImGui::GetTime() + 4.0;
    logLine("INFO", "Editor: " + msg);
}

// ======================================================================
// Editor camera
// ======================================================================

void updateEditorCamera(float dt) {
    auto& e = engine();

    // Right mouse held = fly mode, the convention every 3D editor uses.
    bool rmb = rt::IsMouseDown(Value(1)).truthy();
    if (rmb && !ImGui::GetIO().WantCaptureMouse) g.flying = true;
    if (!rmb) g.flying = false;

    if (g.flying) {
        float dx = static_cast<float>(rt::GetMouseDeltaX().asFloat());
        float dy = static_cast<float>(rt::GetMouseDeltaY().asFloat());
        g.camYaw -= dx * 0.005f;
        g.camPitch -= dy * 0.005f;
        // Clamp just short of straight up/down: at exactly +/-90 degrees the
        // forward vector becomes parallel to the world up axis and the view
        // matrix's cross product degenerates, flipping the camera.
        g.camPitch = clampf(g.camPitch, -1.55f, 1.55f);

        Vec3 forward(std::cos(g.camPitch) * std::sin(g.camYaw), std::sin(g.camPitch),
                     -std::cos(g.camPitch) * std::cos(g.camYaw));
        forward = forward.normalized();
        Vec3 right = cross(forward, Vec3(0, 1, 0)).normalized();

        float speed = g.camSpeed * dt;
        if (rt::IsKeyDown(Value(340)).truthy()) speed *= 3.0f; // left shift
        if (rt::IsKeyDown(Value(87)).truthy()) g.camPos += forward * speed;  // W
        if (rt::IsKeyDown(Value(83)).truthy()) g.camPos -= forward * speed;  // S
        if (rt::IsKeyDown(Value(65)).truthy()) g.camPos -= right * speed;    // A
        if (rt::IsKeyDown(Value(68)).truthy()) g.camPos += right * speed;    // D
        if (rt::IsKeyDown(Value(69)).truthy()) g.camPos.y += speed;          // E
        if (rt::IsKeyDown(Value(81)).truthy()) g.camPos.y -= speed;          // Q
    }

    Vec3 forward(std::cos(g.camPitch) * std::sin(g.camYaw), std::sin(g.camPitch),
                 -std::cos(g.camPitch) * std::cos(g.camYaw));
    e.camPos = g.camPos;
    e.camTarget = g.camPos + forward.normalized();
    e.camHasTarget = true;
}

// Frames the selected entity: pulls the camera back far enough to see it.
void focusSelected() {
    Scene& s = activeScene();
    Entity* ent = s.entities.get(g.selected);
    if (!ent) return;
    Vec3 target(ent->world.m[3][0], ent->world.m[3][1], ent->world.m[3][2]);
    Vec3 forward(std::cos(g.camPitch) * std::sin(g.camYaw), std::sin(g.camPitch),
                 -std::cos(g.camPitch) * std::cos(g.camYaw));
    g.camPos = target - forward.normalized() * 6.0f;
}

// ======================================================================
// Play / Stop
// ======================================================================

void enterPlayMode() {
    if (g.playing) return;
    Scene& s = activeScene();
    g.preplaySnapshot = sceneToJson(s);
    sceneStart(s);
    g.playing = true;
    setStatus("Play");
}

void exitPlayMode() {
    if (!g.playing) return;
    Scene& s = activeScene();
    sceneStop(s);
    if (!g.preplaySnapshot.empty()) {
        std::string err;
        if (!sceneFromJson(s, g.preplaySnapshot, &err)) {
            logLine("ERROR", "Editor: could not restore the pre-play scene: " + err);
        }
    }
    g.playing = false;
    g.selected = 0; // ids were reassigned by the reload
    setStatus("Stopped -- scene restored to its pre-play state");
}

// ======================================================================
// Panels
// ======================================================================

void drawHierarchyNode(Scene& s, uint64_t id) {
    Entity* e = s.entities.get(id);
    if (!e) return;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (e->children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (id == g.selected) flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushID(static_cast<int>(id));
    if (!e->active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

    bool open = ImGui::TreeNodeEx("##node", flags, "%s", e->name.c_str());

    if (!e->active) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) g.selected = id;

    // Drag-and-drop re-parenting. sceneSetParent rejects cycles, so
    // dropping a node onto its own descendant is safely a no-op.
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("PV_ENTITY", &id, sizeof(uint64_t));
        ImGui::Text("%s", e->name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PV_ENTITY")) {
            uint64_t dragged = *static_cast<const uint64_t*>(payload->Data);
            sceneSetParent(s, dragged, id, true);
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Add child")) {
            g.selected = sceneCreateEntity(s, "Entity", id);
        }
        if (ImGui::MenuItem("Duplicate")) {
            uint64_t copy = sceneCreateEntity(s, e->name + " copy", e->parent);
            if (Entity* c = s.entities.get(copy)) {
                Entity saved = *e;
                // Preserve the new entity's own identity and hierarchy links;
                // copying those wholesale would corrupt both trees.
                saved.id = c->id;
                saved.parent = c->parent;
                saved.children.clear();
                // Runtime handles belong to the original; the duplicate
                // re-creates its own at sceneStart.
                saved.body.body = 0;
                saved.light.runtimeLight = 0;
                saved.emitter.emitter = 0;
                saved.animator.animation = 0;
                *c = saved;
            }
            g.selected = copy;
        }
        if (ImGui::MenuItem("Unparent")) sceneSetParent(s, id, 0, true);
        ImGui::Separator();
        if (ImGui::MenuItem("Delete")) {
            sceneDestroyEntity(s, id);
            if (g.selected == id) g.selected = 0;
            ImGui::EndPopup();
            if (open && !(flags & ImGuiTreeNodeFlags_NoTreePushOnOpen)) ImGui::TreePop();
            ImGui::PopID();
            return;
        }
        ImGui::EndPopup();
    }

    if (open && !(flags & ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
        // Copy the child list: a context-menu delete inside the loop would
        // otherwise mutate the vector being iterated.
        std::vector<uint64_t> kids = e->children;
        for (uint64_t c : kids) drawHierarchyNode(s, c);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void drawHierarchyPanel() {
    if (!ImGui::Begin("Hierarchy", &g.showHierarchy)) {
        ImGui::End();
        return;
    }
    Scene& s = activeScene();

    if (ImGui::Button("+ Entity")) g.selected = sceneCreateEntity(s, "Entity", 0);
    ImGui::SameLine();
    if (ImGui::Button("+ Cube")) {
        uint64_t id = sceneCreateEntity(s, "Cube", 0);
        if (Entity* e = s.entities.get(id)) {
            e->add(COMP_MESH);
            e->mesh.primitive = "cube";
            e->mesh.primitiveParams = Vec3(1, 1, 1);
        }
        g.selected = id;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Light")) {
        uint64_t id = sceneCreateEntity(s, "Light", 0);
        if (Entity* e = s.entities.get(id)) {
            e->add(COMP_LIGHT);
            e->local.position = Vec3(0, 4, 0);
        }
        g.selected = id;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Emitter")) {
        uint64_t id = sceneCreateEntity(s, "Emitter", 0);
        if (Entity* e = s.entities.get(id)) {
            e->add(COMP_EMITTER);
            EmitterRecord er;
            er.name = "Emitter";
            e->emitter.emitter = engine().emitters.add(std::move(er));
        }
        g.selected = id;
        g.showParticles = true;
    }

    ImGui::Separator();
    // Root-level drop target, so an entity can be unparented by dragging it
    // into empty space.
    ImGui::BeginChild("tree", ImVec2(0, 0), false);
    std::vector<uint64_t> roots = s.roots;
    for (uint64_t id : roots) drawHierarchyNode(s, id);
    ImGui::EndChild();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PV_ENTITY")) {
            uint64_t dragged = *static_cast<const uint64_t*>(payload->Data);
            sceneSetParent(s, dragged, 0, true);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::End();
}

void drawTransformSection(Scene& s, Entity& e) {
    bool changed = false;
    float pos[3] = {e.local.position.x, e.local.position.y, e.local.position.z};
    if (ImGui::DragFloat3("Position", pos, 0.05f)) {
        e.local.position = Vec3(pos[0], pos[1], pos[2]);
        changed = true;
    }

    // The inspector edits Euler angles because nobody can author a
    // quaternion by hand, but the entity *stores* a quaternion. Converting
    // on every frame would let float error accumulate, so the Euler values
    // are only pushed back when the widget is actually edited.
    Vec3 euler = e.local.rotation.toEuler();
    float rot[3] = {euler.x * PV_RAD2DEG, euler.y * PV_RAD2DEG, euler.z * PV_RAD2DEG};
    if (ImGui::DragFloat3("Rotation", rot, 0.5f)) {
        e.local.rotation =
            Quat::fromEuler(rot[0] * PV_DEG2RAD, rot[1] * PV_DEG2RAD, rot[2] * PV_DEG2RAD);
        changed = true;
    }

    float scl[3] = {e.local.scale.x, e.local.scale.y, e.local.scale.z};
    if (ImGui::DragFloat3("Scale", scl, 0.02f)) {
        e.local.scale = Vec3(scl[0], scl[1], scl[2]);
        changed = true;
    }
    if (changed) {
        markWorldDirty(s, e.id);
        s.dirty = true;
    }
}

void drawInspectorPanel() {
    if (!ImGui::Begin("Inspector", &g.showInspector)) {
        ImGui::End();
        return;
    }
    Scene& s = activeScene();
    Entity* e = s.entities.get(g.selected);
    if (!e) {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::Separator();
        ImGui::TextUnformatted("Scene environment");
        float amb[3] = {s.ambientColor.x, s.ambientColor.y, s.ambientColor.z};
        if (ImGui::ColorEdit3("Ambient", amb)) s.ambientColor = Vec3(amb[0], amb[1], amb[2]);
        ImGui::DragFloat("Ambient intensity", &s.ambientIntensity, 0.01f, 0.0f, 4.0f);
        float clear[4] = {s.clearColor.x, s.clearColor.y, s.clearColor.z, s.clearColor.w};
        if (ImGui::ColorEdit4("Clear color", clear)) s.clearColor = Vec4(clear[0], clear[1], clear[2], clear[3]);
        float grav[3] = {s.gravity.x, s.gravity.y, s.gravity.z};
        if (ImGui::DragFloat3("Gravity", grav, 0.1f)) s.gravity = Vec3(grav[0], grav[1], grav[2]);
        ImGui::End();
        return;
    }

    char nameBuf[128];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", e->name.c_str());
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
        e->name = nameBuf;
        s.dirty = true;
    }
    ImGui::Checkbox("Active", &e->active);

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) drawTransformSection(s, *e);

    // --- mesh ---
    if (e->has(COMP_MESH) && ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Visible", &e->mesh.visible);
        const char* prims[] = {"(from file)", "cube", "sphere", "plane", "cylinder"};
        int cur = 0;
        for (int i = 1; i < 5; i++) {
            if (e->mesh.primitive == prims[i]) cur = i;
        }
        if (ImGui::Combo("Primitive", &cur, prims, 5)) {
            e->mesh.primitive = (cur == 0) ? "" : prims[cur];
            e->mesh.mesh = 0; // force a rebuild at the next start
            s.dirty = true;
        }
        char src[512];
        std::snprintf(src, sizeof(src), "%s", e->mesh.sourcePath.c_str());
        if (ImGui::InputText("Source", src, sizeof(src))) {
            e->mesh.sourcePath = src;
            e->mesh.mesh = 0;
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PV_ASSET_MESH")) {
                e->mesh.sourcePath = static_cast<const char*>(p->Data);
                e->mesh.primitive.clear();
                e->mesh.mesh = 0;
                s.dirty = true;
            }
            ImGui::EndDragDropTarget();
        }
        float params[3] = {e->mesh.primitiveParams.x, e->mesh.primitiveParams.y, e->mesh.primitiveParams.z};
        if (ImGui::DragFloat3("Params", params, 0.05f)) {
            e->mesh.primitiveParams = Vec3(params[0], params[1], params[2]);
            e->mesh.mesh = 0;
        }
        if (ImGui::SmallButton("Remove mesh")) e->remove(COMP_MESH);
    }

    // --- light ---
    if (e->has(COMP_LIGHT) && ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* kinds[] = {"point", "directional", "spot"};
        int k = static_cast<int>(e->light.kind);
        if (ImGui::Combo("Kind", &k, kinds, 3)) e->light.kind = static_cast<LightKindTag>(k);
        float col[3] = {e->light.color.x, e->light.color.y, e->light.color.z};
        if (ImGui::ColorEdit3("Color", col)) e->light.color = Vec3(col[0], col[1], col[2]);
        ImGui::DragFloat("Intensity", &e->light.intensity, 0.02f, 0.0f, 50.0f);
        ImGui::DragFloat("Radius", &e->light.radius, 0.1f, 0.0f, 500.0f);
        if (ImGui::SmallButton("Remove light")) e->remove(COMP_LIGHT);
    }

    // --- body ---
    if (e->has(COMP_BODY) && ImGui::CollapsingHeader("Rigid body", ImGuiTreeNodeFlags_DefaultOpen)) {
        ColliderRecord& col = e->body.colliderDesc;
        const char* shapes[] = {"box", "sphere", "capsule"};
        int sh = static_cast<int>(col.kind);
        if (ImGui::Combo("Shape", &sh, shapes, 3)) col.kind = static_cast<ColliderRecord::Kind>(sh);

        if (col.kind == ColliderRecord::Kind::Box) {
            float he[3] = {col.halfExtents.x, col.halfExtents.y, col.halfExtents.z};
            if (ImGui::DragFloat3("Half extents", he, 0.02f, 0.001f, 1000.0f)) {
                col.halfExtents = Vec3(he[0], he[1], he[2]);
            }
        } else {
            ImGui::DragFloat("Radius", &col.radius, 0.02f, 0.001f, 1000.0f);
            if (col.kind == ColliderRecord::Kind::Capsule) {
                ImGui::DragFloat("Height", &col.height, 0.02f, 0.001f, 1000.0f);
            }
        }
        ImGui::Checkbox("Static", &e->body.isStatic);
        ImGui::Checkbox("Kinematic", &e->body.isKinematic);
        if (!e->body.isStatic) ImGui::DragFloat("Mass", &e->body.mass, 0.05f, 0.001f, 10000.0f);
        ImGui::DragFloat("Static friction", &col.staticFriction, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Dynamic friction", &col.dynamicFriction, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Restitution", &col.restitution, 0.01f, 0.0f, 1.0f);
        ImGui::Checkbox("Trigger", &col.isTrigger);
        if (g.playing && e->body.body != 0) {
            if (const RigidBodyRecord* rec = engine().bodies.get(e->body.body)) {
                ImGui::TextDisabled("v = (%.2f, %.2f, %.2f)", rec->velocity.x, rec->velocity.y, rec->velocity.z);
            }
        }
        if (ImGui::SmallButton("Remove body")) e->remove(COMP_BODY);
    }

    // --- animator ---
    if (e->has(COMP_ANIMATOR) && ImGui::CollapsingHeader("Animator", ImGuiTreeNodeFlags_DefaultOpen)) {
        char src[512];
        std::snprintf(src, sizeof(src), "%s", e->animator.sourcePath.c_str());
        if (ImGui::InputText("File", src, sizeof(src))) {
            e->animator.sourcePath = src;
            e->animator.animation = 0;
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PV_ASSET_MESH")) {
                e->animator.sourcePath = static_cast<const char*>(p->Data);
                e->animator.animation = 0;
                s.dirty = true;
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::Button("Load")) {
            Value h = rt::LoadAnimation(Value(e->animator.sourcePath));
            e->animator.animation = static_cast<uint64_t>(h.asInt());
            g.showAnimation = true;
        }
        ImGui::Checkbox("Play on start", &e->animator.playOnStart);
        ImGui::Checkbox("Loop", &e->animator.looping);
        ImGui::Checkbox("Root motion", &e->animator.applyRootMotion);
        ImGui::DragFloat("Speed", &e->animator.speed, 0.01f, -4.0f, 4.0f);
        if (ImGui::SmallButton("Remove animator")) e->remove(COMP_ANIMATOR);
    }

    // --- camera ---
    if (e->has(COMP_CAMERA) && ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat("FOV", &e->camera.fovDegrees, 0.5f, 10.0f, 170.0f);
        ImGui::DragFloat("Near", &e->camera.nearZ, 0.005f, 0.001f, 10.0f);
        ImGui::DragFloat("Far", &e->camera.farZ, 1.0f, 1.0f, 10000.0f);
        ImGui::Checkbox("Orthographic", &e->camera.orthographic);
        if (ImGui::Button(s.activeCamera == e->id ? "Active camera" : "Make active")) {
            rt::SetActiveCamera(Value::MakeHandle(e->id, "entity"));
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Add component")) ImGui::OpenPopup("addcomp");
    if (ImGui::BeginPopup("addcomp")) {
        if (!e->has(COMP_MESH) && ImGui::MenuItem("Mesh")) {
            e->add(COMP_MESH);
            e->mesh.primitive = "cube";
        }
        if (!e->has(COMP_LIGHT) && ImGui::MenuItem("Light")) e->add(COMP_LIGHT);
        if (!e->has(COMP_BODY) && ImGui::MenuItem("Rigid body")) e->add(COMP_BODY);
        if (!e->has(COMP_EMITTER) && ImGui::MenuItem("Particle emitter")) {
            e->add(COMP_EMITTER);
            EmitterRecord er;
            er.name = e->name;
            e->emitter.emitter = engine().emitters.add(std::move(er));
        }
        if (!e->has(COMP_ANIMATOR) && ImGui::MenuItem("Animator")) e->add(COMP_ANIMATOR);
        if (!e->has(COMP_CAMERA) && ImGui::MenuItem("Camera")) e->add(COMP_CAMERA);
        ImGui::EndPopup();
    }
    ImGui::End();
}

// ======================================================================
// Particle editor
// ======================================================================

void drawParticlePanel() {
    if (!ImGui::Begin("Particles", &g.showParticles)) {
        ImGui::End();
        return;
    }
    Scene& s = activeScene();
    Entity* e = s.entities.get(g.selected);
    if (!e || !e->has(COMP_EMITTER) || e->emitter.emitter == 0) {
        ImGui::TextDisabled("Select an entity with a particle emitter.");
        ImGui::End();
        return;
    }
    EmitterRecord* em = engine().emitters.get(e->emitter.emitter);
    if (!em) {
        ImGui::TextDisabled("Emitter handle is stale.");
        ImGui::End();
        return;
    }

    ImGui::Text("Live particles: %d / %d", static_cast<int>(em->particles.size()), em->maxParticles);
    if (ImGui::Button("Burst 100")) burstEmitter(*em, 100);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) clearEmitter(*em);
    ImGui::SameLine();
    ImGui::Checkbox("Enabled", &em->enabled);

    ImGui::SeparatorText("Emission");
    ImGui::DragFloat("Rate/sec", &em->rate, 0.5f, 0.0f, 5000.0f);
    ImGui::DragInt("Max particles", &em->maxParticles, 1.0f, 1, 200000);
    ImGui::Checkbox("Looping", &em->looping);
    ImGui::Checkbox("Local space", &em->localSpace);
    ImGui::SetItemTooltip("World space leaves a trail behind a moving emitter; "
                          "local space drags particles along with it.");

    ImGui::SeparatorText("Shape");
    const char* shapes[] = {"point", "sphere", "hemisphere", "box", "cone", "circle"};
    int sh = static_cast<int>(em->shape);
    if (ImGui::Combo("Shape", &sh, shapes, 6)) em->shape = static_cast<EmitterShape>(sh);
    ImGui::DragFloat("Radius", &em->radius, 0.02f, 0.0f, 100.0f);
    if (em->shape == EmitterShape::Cone) {
        float deg = em->coneAngle * PV_RAD2DEG;
        if (ImGui::DragFloat("Cone angle", &deg, 0.5f, 0.0f, 180.0f)) em->coneAngle = deg * PV_DEG2RAD;
    }

    ImGui::SeparatorText("Particle");
    ImGui::DragFloatRange2("Lifetime", &em->lifetimeMin, &em->lifetimeMax, 0.02f, 0.01f, 60.0f);
    ImGui::DragFloatRange2("Speed", &em->speedMin, &em->speedMax, 0.05f, 0.0f, 200.0f);
    ImGui::DragFloatRange2("Size", &em->sizeMin, &em->sizeMax, 0.005f, 0.001f, 50.0f);
    ImGui::DragFloatRange2("Spin", &em->rotationSpeedMin, &em->rotationSpeedMax, 0.05f, -20.0f, 20.0f);

    ImGui::SeparatorText("Forces");
    float grav[3] = {em->gravity.x, em->gravity.y, em->gravity.z};
    if (ImGui::DragFloat3("Gravity", grav, 0.05f)) em->gravity = Vec3(grav[0], grav[1], grav[2]);
    ImGui::DragFloat("Gravity scale", &em->gravityScale, 0.01f, -5.0f, 5.0f);
    ImGui::DragFloat("Drag", &em->drag, 0.01f, 0.0f, 20.0f);
    float wind[3] = {em->windForce.x, em->windForce.y, em->windForce.z};
    if (ImGui::DragFloat3("Wind", wind, 0.05f)) em->windForce = Vec3(wind[0], wind[1], wind[2]);
    ImGui::DragFloat("Vortex", &em->vortexStrength, 0.05f, -50.0f, 50.0f);

    ImGui::SeparatorText("Appearance");
    float cs[4] = {em->colorStart.x, em->colorStart.y, em->colorStart.z, em->colorStart.w};
    if (ImGui::ColorEdit4("Start color", cs)) em->colorStart = Vec4(cs[0], cs[1], cs[2], cs[3]);
    float ce[4] = {em->colorEnd.x, em->colorEnd.y, em->colorEnd.z, em->colorEnd.w};
    if (ImGui::ColorEdit4("End color", ce)) em->colorEnd = Vec4(ce[0], ce[1], ce[2], ce[3]);

    const char* blends[] = {"alpha", "additive", "opaque"};
    int bl = static_cast<int>(em->blend);
    if (ImGui::Combo("Blend", &bl, blends, 3)) em->blend = static_cast<ParticleBlend>(bl);

    // Curve editors: four sliders each, matching LifetimeCurve's four
    // control points. A real spline widget would be nicer, but four labelled
    // values are unambiguous and take a fraction of the code.
    auto curveUI = [](const char* label, LifetimeCurve& c, float maxV) {
        ImGui::PushID(label);
        ImGui::TextUnformatted(label);
        ImGui::SliderFloat("##0", &c.p0, 0.0f, maxV, "start %.2f");
        ImGui::SliderFloat("##1", &c.p1, 0.0f, maxV, "1/3 %.2f");
        ImGui::SliderFloat("##2", &c.p2, 0.0f, maxV, "2/3 %.2f");
        ImGui::SliderFloat("##3", &c.p3, 0.0f, maxV, "end %.2f");
        ImGui::PopID();
    };
    curveUI("Size over lifetime", em->sizeCurve, 4.0f);
    curveUI("Alpha over lifetime", em->alphaCurve, 1.0f);

    ImGui::DragInt("Sheet columns", &em->sheetCols, 1.0f, 1, 16);
    ImGui::DragInt("Sheet rows", &em->sheetRows, 1.0f, 1, 16);

    ImGui::End();
}

// ======================================================================
// Animation timeline
// ======================================================================

void drawAnimationPanel() {
    if (!ImGui::Begin("Animation", &g.showAnimation)) {
        ImGui::End();
        return;
    }
    Scene& s = activeScene();
    Entity* e = s.entities.get(g.selected);
    uint64_t animHandle = (e && e->has(COMP_ANIMATOR)) ? e->animator.animation : 0;
    AnimRecord* rec = animHandle ? engine().anims.get(animHandle) : nullptr;
    if (!rec) {
        ImGui::TextDisabled("Select an entity with a loaded animator.");
        ImGui::End();
        return;
    }

    ImGui::Text("Source: %s", rec->sourcePath.empty() ? "(built from script)" : rec->sourcePath.c_str());
    ImGui::Text("%d clip(s), %d node(s)", static_cast<int>(rec->clips.size()),
                static_cast<int>(rec->nodes.size()));

    if (rec->clips.empty()) {
        ImGui::TextDisabled("This file contained no usable animation channels.");
        ImGui::End();
        return;
    }

    AnimLayer& layer = rec->primary();
    std::vector<const char*> names;
    for (const auto& c : rec->clips) names.push_back(c.name.c_str());
    int cur = std::max(0, layer.clip);
    if (ImGui::Combo("Clip", &cur, names.data(), static_cast<int>(names.size()))) {
        rt::PlayAnimation(Value::MakeHandle(animHandle, "animation"), Value(rec->clips[cur].name));
    }

    const AnimClip& clip = rec->clips[static_cast<size_t>(cur)];
    ImGui::Text("Duration: %.3fs, %d channel(s)", clip.duration, static_cast<int>(clip.channels.size()));

    if (ImGui::Button(layer.playing ? "Pause" : "Play")) {
        if (layer.playing) rt::PauseAnimation(Value::MakeHandle(animHandle, "animation"));
        else layer.playing = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) rt::StopAnimation(Value::MakeHandle(animHandle, "animation"));
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &layer.looping);

    // Scrubbing calls SetAnimationTime, which re-evaluates the pose
    // immediately -- so dragging the slider updates the viewport live even
    // while playback is paused.
    float t = layer.time;
    if (ImGui::SliderFloat("Time", &t, 0.0f, std::max(clip.duration, 0.001f), "%.3fs")) {
        rt::SetAnimationTime(Value::MakeHandle(animHandle, "animation"), Value(t));
    }
    ImGui::DragFloat("Speed", &layer.speed, 0.01f, -4.0f, 4.0f);

    // Per-channel keyframe ticks. A read-only view, but it's what tells you
    // at a glance whether an import actually produced keys and where.
    ImGui::SeparatorText("Channels");
    if (ImGui::BeginChild("channels", ImVec2(0, 160))) {
        for (const AnimChannel& ch : clip.channels) {
            const char* pathName = ch.path == AnimPath::Translation ? "translation"
                                   : ch.path == AnimPath::Rotation ? "rotation"
                                   : ch.path == AnimPath::Scale    ? "scale"
                                                                   : "weights";
            const char* nodeName = (ch.targetNode >= 0 && static_cast<size_t>(ch.targetNode) < rec->nodes.size())
                                       ? rec->nodes[static_cast<size_t>(ch.targetNode)].name.c_str()
                                       : "?";
            ImGui::Text("%-18s %-12s %3d keys", nodeName, pathName, static_cast<int>(ch.times.size()));

            ImVec2 p = ImGui::GetCursorScreenPos();
            float width = ImGui::GetContentRegionAvail().x - 8.0f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddLine(ImVec2(p.x, p.y + 4), ImVec2(p.x + width, p.y + 4), IM_COL32(90, 90, 100, 255));
            if (clip.duration > 1e-6f) {
                for (float keyTime : ch.times) {
                    float x = p.x + (keyTime / clip.duration) * width;
                    dl->AddCircleFilled(ImVec2(x, p.y + 4), 2.5f, IM_COL32(220, 190, 90, 255));
                }
                float px = p.x + (clampf(layer.time / clip.duration, 0.0f, 1.0f)) * width;
                dl->AddLine(ImVec2(px, p.y - 2), ImVec2(px, p.y + 10), IM_COL32(240, 90, 90, 255), 1.5f);
            }
            ImGui::Dummy(ImVec2(width, 12));
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// ======================================================================
// Asset browser
// ======================================================================

void drawAssetsPanel() {
    if (!ImGui::Begin("Assets", &g.showAssets)) {
        ImGui::End();
        return;
    }
    char dirBuf[512];
    std::snprintf(dirBuf, sizeof(dirBuf), "%s", g.assetDir.c_str());
    if (ImGui::InputText("Folder", dirBuf, sizeof(dirBuf))) g.assetDir = dirBuf;

    std::error_code ec;
    if (!fs::exists(g.assetDir, ec)) {
        ImGui::TextDisabled("'%s' doesn't exist.", g.assetDir.c_str());
        ImGui::End();
        return;
    }

    // Rebuilt each frame. A directory scan per frame would be wasteful for a
    // large tree, but an assets folder is small and this keeps newly added
    // files visible without a refresh button.
    for (const auto& entry : fs::recursive_directory_iterator(g.assetDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        std::string path = normalizeSlashes(entry.path().string());
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        const char* payloadType = nullptr;
        if (ext == ".obj" || ext == ".gltf" || ext == ".glb") payloadType = "PV_ASSET_MESH";
        else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") payloadType = "PV_ASSET_TEXTURE";
        else if (ext == ".pvscene") payloadType = "PV_ASSET_SCENE";

        ImGui::PushID(path.c_str());
        ImGui::Selectable(path.c_str());
        if (payloadType && ImGui::BeginDragDropSource()) {
            // The payload is the path including its null terminator, so the
            // receiving side can use it as a C string directly.
            ImGui::SetDragDropPayload(payloadType, path.c_str(), path.size() + 1);
            ImGui::Text("%s", path.c_str());
            ImGui::EndDragDropSource();
        }
        if (payloadType && std::strcmp(payloadType, "PV_ASSET_SCENE") == 0 &&
            ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (rt::LoadScene(Value(path)).truthy()) {
                g.selected = 0;
                std::snprintf(g.scenePathBuffer, sizeof(g.scenePathBuffer), "%s", path.c_str());
                setStatus("Loaded " + path);
            }
        }
        ImGui::PopID();
    }
    ImGui::End();
}

// ======================================================================
// Menu bar and stats
// ======================================================================

void drawMenuBar() {
    Scene& s = activeScene();
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New scene")) {
            rt::NewScene(Value("Untitled"));
            g.selected = 0;
            setStatus("New scene");
        }
        ImGui::InputText("Path", g.scenePathBuffer, sizeof(g.scenePathBuffer));
        if (ImGui::MenuItem("Open")) {
            if (rt::LoadScene(Value(std::string(g.scenePathBuffer))).truthy()) {
                g.selected = 0;
                setStatus("Loaded");
            } else {
                setStatus("Load failed -- see the log");
            }
        }
        if (ImGui::MenuItem("Save", "Ctrl+S")) {
            if (rt::SaveScene(Value(std::string(g.scenePathBuffer))).truthy()) setStatus("Saved");
            else setStatus("Save failed -- see the log");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) rt::CloseWindow();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Hierarchy", nullptr, &g.showHierarchy);
        ImGui::MenuItem("Inspector", nullptr, &g.showInspector);
        ImGui::MenuItem("Assets", nullptr, &g.showAssets);
        ImGui::MenuItem("Particles", nullptr, &g.showParticles);
        ImGui::MenuItem("Animation", nullptr, &g.showAnimation);
        ImGui::MenuItem("Stats", nullptr, &g.showStats);
        ImGui::EndMenu();
    }

    // Play controls, right-aligned-ish and colour-coded so the mode you're
    // in is unmistakable -- editing a scene while it's playing and losing
    // the changes on Stop is the classic editor papercut.
    // Gizmo mode toolbar.
    ImGui::Separator();
    {
        const char* modes[3] = {"Move", "Rotate", "Scale"};
        for (int i = 0; i < 3; i++) {
            bool active = static_cast<int>(g.gizmoMode) == i;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.45f, 0.70f, 1.0f));
            if (ImGui::Button(modes[i])) g.gizmoMode = static_cast<gizmo::Mode>(i);
            if (active) ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        if (ImGui::Button(g.gizmoState.localSpace ? "Local" : "World")) {
            g.gizmoState.localSpace = !g.gizmoState.localSpace;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::DragFloat("snap", &g.gizmoState.snap, 0.01f, 0.0f, 10.0f, "%.2f");
    }

    ImGui::Separator();
    if (g.playing) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.20f, 0.20f, 1.0f));
        if (ImGui::Button("Stop")) exitPlayMode();
        ImGui::PopStyleColor();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "PLAYING -- edits will be discarded on Stop");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.25f, 1.0f));
        if (ImGui::Button("Play")) enterPlayMode();
        ImGui::PopStyleColor();
        if (s.dirty) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "unsaved");
    }

    if (!g.statusMessage.empty() && ImGui::GetTime() < g.statusExpiry) {
        ImGui::Separator();
        ImGui::TextUnformatted(g.statusMessage.c_str());
    }
    ImGui::EndMainMenuBar();
}

void drawStatsPanel() {
    if (!ImGui::Begin("Stats", &g.showStats)) {
        ImGui::End();
        return;
    }
    auto& e = engine();
    ImGui::Text("%.1f FPS (%.2f ms)", e.fps, e.deltaTime * 1000.0f);
    ImGui::Text("Entities: %d", static_cast<int>(activeScene().entities.size()));
    ImGui::Text("Meshes: %d  Textures: %d", static_cast<int>(e.meshes.size()),
                static_cast<int>(e.textures.size()));

    size_t particles = 0;
    for (const auto& kv : e.emitters) particles += kv.second.particles.size();
    ImGui::Text("Emitters: %d  Particles: %d", static_cast<int>(e.emitters.size()),
                static_cast<int>(particles));
    ImGui::Text("Bodies: %d", static_cast<int>(e.bodies.size()));
    ImGui::Text("Physics backend: %s", e.physics ? e.physics->name() : "none");
    ImGui::Separator();
    ImGui::TextDisabled("Hold RMB to fly: WASD move, Q/E down/up, Shift sprint.");
    ImGui::TextDisabled("W/E/R gizmo mode, X world-local, F frame selection.");
    ImGui::TextDisabled("Click to select, Delete to remove, Ctrl+S to save.");
    ImGui::End();
}

// ======================================================================
// ImGui setup
// ======================================================================

void initImGui() {
    auto& e = engine();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForVulkan(e.window, true);

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = e.instance;
    info.PhysicalDevice = e.physicalDevice;
    info.Device = e.device;
    info.QueueFamily = e.graphicsFamily;
    info.Queue = e.graphicsQueue;
    // ImGui allocates its font atlas descriptor from this pool. The runtime's
    // existing pool is sized for per-texture sampler sets and has room.
    info.DescriptorPool = e.descriptorPool;
    info.RenderPass = e.renderPass;
    info.MinImageCount = 2;
    info.ImageCount = std::max(2u, static_cast<uint32_t>(e.swapchainImages.size()));
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.PipelineCache = VK_NULL_HANDLE;
    info.Subpass = 0;
    if (!ImGui_ImplVulkan_Init(&info)) {
        logLine("ERROR", "Editor: ImGui_ImplVulkan_Init failed");
        return;
    }
    ImGui_ImplVulkan_CreateFontsTexture();
}

void shutdownImGui() {
    vkDeviceWaitIdle(engine().device);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

} // namespace

// ======================================================================
// main
// ======================================================================

int main(int argc, char** argv) {
    pv::ensureConsole();

    std::string startupScene;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--assets" && i + 1 < argc) g.assetDir = argv[++i];
        else if (!arg.empty() && arg[0] != '-') startupScene = arg;
    }

    rt::Init();
    if (!rt::CreateWindow(Value(1600), Value(900), Value("PlainVulkan Editor")).truthy()) {
        logLine("ERROR", "Editor: could not create a window -- is a Vulkan driver installed?");
        return 1;
    }
    rt::InitPhysics();
    initImGui();

    if (!startupScene.empty()) {
        if (rt::LoadScene(Value(startupScene)).truthy()) {
            std::snprintf(g.scenePathBuffer, sizeof(g.scenePathBuffer), "%s", startupScene.c_str());
        }
    } else {
        // A brand-new editor session gets a floor and a light, so the
        // viewport isn't an unexplained black void on first launch.
        Scene& s = pv::activeScene();
        uint64_t floor = sceneCreateEntity(s, "Floor", 0);
        if (Entity* e = s.entities.get(floor)) {
            e->add(COMP_MESH);
            e->mesh.primitive = "plane";
            e->mesh.primitiveParams = Vec3(20, 20, 0);
            e->add(COMP_BODY);
            e->body.isStatic = true;
            e->body.mass = 0.0f;
            e->body.colliderDesc.kind = ColliderRecord::Kind::Box;
            e->body.colliderDesc.halfExtents = Vec3(10, 0.05f, 10);
        }
        uint64_t light = sceneCreateEntity(s, "Sun", 0);
        if (Entity* e = s.entities.get(light)) {
            e->add(COMP_LIGHT);
            e->light.kind = LightKindTag::Directional;
            e->light.intensity = 1.2f;
            e->local.position = Vec3(4, 8, 4);
            e->local.rotation = Quat::fromEuler(-0.9f, 0.6f, 0.0f);
        }
        s.dirty = false;
    }

    while (rt::IsWindowOpen().truthy()) {
        rt::BeginFrame();
        float dt = static_cast<float>(rt::GetDeltaTime().asFloat());

        // ImGui's Vulkan backend is told the swapchain image count at init.
        // A resize rebuilds the swapchain; keep the backend in sync.
        {
            uint32_t n = std::max(2u, static_cast<uint32_t>(engine().swapchainImages.size()));
            static uint32_t imguiImages = n;
            if (n != imguiImages) {
                ImGui_ImplVulkan_SetMinImageCount(n);
                imguiImages = n;
            }
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

        Scene& scene = pv::activeScene();

        // Keyboard shortcuts, suppressed while a text field has focus so
        // typing "s" into a name box doesn't save the scene.
        if (!ImGui::GetIO().WantTextInput) {
            if (rt::IsKeyPressed(Value(70)).truthy()) focusSelected();          // F
            // W/E/R are the near-universal gizmo mode bindings, but they
            // collide with the WASD fly controls -- so they only apply when
            // the right mouse button isn't held.
            if (!g.flying) {
                if (rt::IsKeyPressed(Value(87)).truthy()) g.gizmoMode = gizmo::Mode::Translate; // W
                if (rt::IsKeyPressed(Value(69)).truthy()) g.gizmoMode = gizmo::Mode::Rotate;    // E
                if (rt::IsKeyPressed(Value(82)).truthy()) g.gizmoMode = gizmo::Mode::Scale;     // R
                if (rt::IsKeyPressed(Value(88)).truthy()) {                                     // X
                    g.gizmoState.localSpace = !g.gizmoState.localSpace;
                    setStatus(g.gizmoState.localSpace ? "Gizmo: local space" : "Gizmo: world space");
                }
            }
            if (rt::IsKeyPressed(Value(261)).truthy() && g.selected) {          // Delete
                sceneDestroyEntity(scene, g.selected);
                g.selected = 0;
            }
            bool ctrl = rt::IsKeyDown(Value(341)).truthy();
            if (ctrl && rt::IsKeyPressed(Value(83)).truthy()) {                 // Ctrl+S
                if (rt::SaveScene(Value(std::string(g.scenePathBuffer))).truthy()) setStatus("Saved");
            }
        }

        updateEditorCamera(dt);

        // In Play mode the scene runs exactly as it would in a shipped game.
        // Outside it, transforms still refresh so gizmo edits are visible,
        // but physics and animation stay frozen.
        if (g.playing) {
            sceneUpdate(scene, dt);
        } else {
            updateSceneTransforms(scene);
            // Emitters still simulate when stopped, so tuning a particle
            // effect doesn't require entering Play.
            for (auto& kv : engine().emitters) updateEmitter(kv.second, dt);
        }

        // The scene's own camera takes over during Play; outside it, the
        // editor's fly camera wins, which is why this is re-applied after
        // sceneDraw would otherwise have overwritten it.
        uint64_t savedCamera = scene.activeCamera;
        if (!g.playing) scene.activeCamera = 0;
        sceneDraw(scene);
        scene.activeCamera = savedCamera;

        // --- gizmo and click-to-select ---------------------------------
        // Both need the same view/projection the renderer just used, so
        // this runs after sceneDraw rather than before it.
        {
            Mat4 view = buildViewMatrix();
            Mat4 proj = buildProjectionMatrix();
            float sw = static_cast<float>(engine().swapchainExtent.width);
            float sh = static_cast<float>(engine().swapchainExtent.height);
            float mx = static_cast<float>(rt::GetMouseX().asFloat());
            float my = static_cast<float>(rt::GetMouseY().asFloat());
            bool lmbDown = rt::IsMouseDown(Value(0)).truthy();
            bool lmbPressed = rt::IsMousePressed(Value(0)).truthy();

            bool overGizmo = false;
            if (g.selected != 0 && !g.flying) {
                overGizmo = gizmo::manipulate(scene, g.selected, g.gizmoMode, g.gizmoState, view, proj, sw, sh,
                                              lmbDown, lmbPressed, mx, my);
            }

            // Click-to-select, suppressed when the click landed on a gizmo
            // handle or on an ImGui panel -- otherwise grabbing an axis
            // would immediately deselect whatever you were manipulating.
            if (lmbPressed && !overGizmo && !g.flying && !ImGui::GetIO().WantCaptureMouse) {
                gizmo::Ray ray = gizmo::screenRay(view, proj, mx, my, sw, sh);
                g.selected = gizmo::pickEntity(scene, ray);
            }
        }

        drawMenuBar();
        if (g.showHierarchy) drawHierarchyPanel();
        if (g.showInspector) drawInspectorPanel();
        if (g.showAssets) drawAssetsPanel();
        if (g.showParticles) drawParticlePanel();
        if (g.showAnimation) drawAnimationPanel();
        if (g.showStats) drawStatsPanel();

        ImGui::Render();
        if (engine().frameActive) {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), engine().activeCmd);
        }

        rt::EndFrame();
    }

    shutdownImGui();
    rt::Shutdown();
    return 0;
}
