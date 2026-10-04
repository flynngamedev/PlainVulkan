// physics.cpp -- the `Pv::` physics commands, now a thin dispatch layer
// over whichever PhysicsBackend is active (PhysX by default; see
// pv_physics.h for why there are two).
//
// The engine-side ColliderRecord/RigidBodyRecord tables remain the source
// of truth for *authoring* data -- what shape, what mass, what flags -- and
// the backend owns the *simulation* state. After each step, readBack()
// copies the simulated pose and velocity into the engine record, so every
// script-facing query is a plain table lookup rather than a round trip
// into the SDK partway through a frame.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"
#include "pv/pv_terrain.h" // terrain collision is resolved in physicsStepAndSync

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace pv {

std::unique_ptr<PhysicsBackend> createDefaultPhysicsBackend() {
    // An explicit override always wins, so a suspected backend bug can be
    // bisected against the other implementation without a rebuild.
    std::string want;
    if (const char* env = std::getenv("PV_PHYSICS_BACKEND")) {
        want = env;
        for (char& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (want == "builtin") {
        logLine("INFO", "Physics: PV_PHYSICS_BACKEND=builtin -- using the fallback solver by request");
        auto b = createBuiltinBackend();
        if (b) b->init();
        return b;
    }

    if (want.empty() || want == "physx") {
        if (auto physx = createPhysXBackend()) return physx;
        if (want == "physx") {
            logLine("ERROR", "Physics: PV_PHYSICS_BACKEND=physx was requested but PhysX is unavailable "
                             "(either this build has -DPV_WITH_PHYSX=OFF, or initialization failed). "
                             "Falling back to the builtin solver.");
        }
    } else {
        logLine("WARNING", "Physics: unknown PV_PHYSICS_BACKEND='" + want + "' (expected 'physx' or 'builtin')");
    }

    auto b = createBuiltinBackend();
    if (b) b->init();
    return b;
}

// Used by the scene system, which steps physics itself rather than going
// through the Value-boxed command.
void physicsEnsureInitialized() {
    auto& e = engine();
    if (e.physicsInitialized && e.physics) return;
    e.physics = createDefaultPhysicsBackend();
    if (e.physics) {
        e.physics->setGravity(e.gravity);
        e.physics->setFixedTimestep(e.physicsFixedHz, e.physicsMaxSubsteps);
    }
    e.physicsInitialized = e.physics != nullptr;
}

// Registers a body that already exists in the engine tables with the
// backend. Shared by Pv::CreateRigidBody and by sceneStart().
bool physicsRegisterBody(uint64_t bodyHandle) {
    auto& e = engine();
    if (!e.physics) return false;
    RigidBodyRecord* rec = e.bodies.get(bodyHandle);
    if (!rec) return false;
    ColliderRecord* col = e.colliders.get(rec->collider);
    if (!col) {
        logLine("ERROR", "Physics: body " + std::to_string(bodyHandle) +
                             " references collider " + std::to_string(rec->collider) +
                             ", which doesn't exist -- was it created with Pv::CreateBoxCollider first?");
        return false;
    }
    if (e.physics->addBody(bodyHandle, *rec, *col)) {
        rec->backendHandle = bodyHandle;
        return true;
    }
    return false;
}

void physicsUnregisterBody(uint64_t bodyHandle) {
    auto& e = engine();
    if (e.physics) e.physics->removeBody(bodyHandle);
}

// Steps the simulation and syncs every body's pose back into the engine
// records. Called by Pv::StepPhysics and by sceneUpdate().
//
// Terrain collision is resolved here, after read-back, rather than inside
// a backend. Neither PhysicsBackend implementation knows terrain exists,
// and teaching both of them (one of which is PhysX, whose heightfield API
// is nothing like the builtin solver's) would mean widening the interface
// for a shape only one subsystem produces. Correcting the pose afterwards
// gets terrain collision on *both* backends for the same small amount of
// code -- the tradeoff is that it is a position correction, not a
// constraint, so a body resting on a slope is held up but not slid down
// it. Pv::TerrainRaycast is there for scripts that need the real surface
// interaction.
void physicsStepAndSync(float dt) {
    auto& e = engine();
    if (!e.physics) return;
    e.physics->step(dt);

    const bool terrainCollision = anyTerrainCollisionEnabled();
    for (auto& kv : e.bodies) {
        e.physics->readBack(kv.first, kv.second);
        if (!terrainCollision || kv.second.isStatic) continue;

        RigidBodyRecord& rec = kv.second;
        // Use the collider's radius as the body's standoff so a sphere
        // rests on the surface instead of half-sunk into it.
        float standoff = 0.0f;
        if (const ColliderRecord* col = e.colliders.get(rec.collider)) {
            switch (col->kind) {
                case ColliderRecord::Kind::Sphere: standoff = col->radius; break;
                case ColliderRecord::Kind::Capsule: standoff = col->radius + col->height * 0.5f; break;
                case ColliderRecord::Kind::Box: standoff = col->halfExtents.y; break;
            }
        }
        Vec3 corrected, normal;
        if (terrainResolvePoint(rec.pose.position, standoff, corrected, normal)) {
            rec.pose.position = corrected;
            // Kill only the downward component. Zeroing the whole vector
            // would stop a body dead the instant it brushed a slope,
            // which reads as sticking to the ground.
            if (rec.velocity.y < 0.0f) rec.velocity.y = 0.0f;
            e.physics->setPose(kv.first, rec.pose);
            e.physics->setLinearVelocity(kv.first, rec.velocity);
        }
    }
}

namespace {

RigidBodyRecord* getBody(const Value& v) {
    return engine().bodies.get(static_cast<uint64_t>(v.asInt()));
}

// Every physics command that touches the backend goes through this, so a
// script that forgot Pv::InitPhysics gets one clear message instead of a
// silent no-op or a null dereference.
bool requirePhysics(const char* fn) {
    auto& e = engine();
    if (e.physics) return true;
    static bool warned = false;
    if (!warned) {
        warned = true;
        logLine("WARNING", std::string(fn) + ": physics isn't initialized -- call Pv::InitPhysics() first. "
                                             "Auto-initializing now so this call still works.");
    }
    physicsEnsureInitialized();
    return e.physics != nullptr;
}

} // namespace

namespace rt {

Value InitPhysics() {
    physicsEnsureInitialized();
    auto& e = engine();
    if (e.physics) logLine("INFO", std::string("Physics backend: ") + e.physics->name());
    return Value(e.physics != nullptr);
}

Value SetGravity(const Value& x, const Value& y, const Value& z) {
    auto& e = engine();
    e.gravity = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                     static_cast<float>(z.asFloat()));
    // Stored unconditionally so SetGravity before InitPhysics still takes
    // effect once the backend comes up.
    if (e.physics) e.physics->setGravity(e.gravity);
    return Value();
}

Value StepPhysics(const Value& deltaTime) {
    if (!requirePhysics("Pv::StepPhysics")) return Value();
    physicsStepAndSync(static_cast<float>(deltaTime.asFloat()));
    return Value();
}

Value CreateBoxCollider(const Value& x, const Value& y, const Value& z, const Value& w, const Value& h,
                        const Value& d) {
    ColliderRecord c;
    c.kind = ColliderRecord::Kind::Box;
    c.center = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                    static_cast<float>(z.asFloat()));
    c.halfExtents = Vec3(static_cast<float>(w.asFloat()) * 0.5f, static_cast<float>(h.asFloat()) * 0.5f,
                         static_cast<float>(d.asFloat()) * 0.5f);
    return Value::MakeHandle(engine().colliders.add(c), "collider");
}

Value CreateSphereCollider(const Value& x, const Value& y, const Value& z, const Value& radius) {
    ColliderRecord c;
    c.kind = ColliderRecord::Kind::Sphere;
    c.center = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                    static_cast<float>(z.asFloat()));
    c.radius = static_cast<float>(radius.asFloat());
    return Value::MakeHandle(engine().colliders.add(c), "collider");
}

Value CreateCapsuleCollider(const Value& x, const Value& y, const Value& z, const Value& radius,
                            const Value& height) {
    // No longer a sphere in disguise: the PhysX backend builds a real
    // upright PxCapsuleGeometry (see physics_physx.cpp's createShape).
    ColliderRecord c;
    c.kind = ColliderRecord::Kind::Capsule;
    c.center = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                    static_cast<float>(z.asFloat()));
    c.radius = static_cast<float>(radius.asFloat());
    c.height = static_cast<float>(height.asFloat());
    return Value::MakeHandle(engine().colliders.add(c), "collider");
}

Value CreateRigidBody(const Value& collider, const Value& mass) {
    if (!requirePhysics("Pv::CreateRigidBody")) return Value::MakeHandle(0, "body");
    auto& e = engine();

    RigidBodyRecord body;
    body.collider = static_cast<uint64_t>(collider.asInt());
    body.mass = static_cast<float>(mass.asFloat());
    // Mass <= 0 has always meant "static" in this API; keeping that.
    body.isStatic = body.mass <= 0.0f;
    if (auto* c = e.colliders.get(body.collider)) body.pose.position = c->center;

    uint64_t handle = e.bodies.add(body);
    if (!physicsRegisterBody(handle)) {
        e.bodies.remove(handle);
        return Value::MakeHandle(0, "body");
    }
    return Value::MakeHandle(handle, "body");
}

Value SetBodyVelocity(const Value& body, const Value& x, const Value& y, const Value& z) {
    auto& e = engine();
    uint64_t h = static_cast<uint64_t>(body.asInt());
    if (auto* b = e.bodies.get(h)) {
        b->velocity = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                           static_cast<float>(z.asFloat()));
        if (e.physics) e.physics->setLinearVelocity(h, b->velocity);
    }
    return Value();
}

Value GetBodyVelocity(const Value& body) {
    if (auto* b = getBody(body)) {
        return make_array({Value(b->velocity.x), Value(b->velocity.y), Value(b->velocity.z)});
    }
    return make_array({Value(0.0), Value(0.0), Value(0.0)});
}

Value AddForce(const Value& body, const Value& x, const Value& y, const Value& z) {
    // Now a genuine force: PhysX integrates it over the step (PxForceMode::
    // eFORCE) rather than the old immediate velocity nudge, so the result no
    // longer depends on how many times per frame the script calls it.
    auto& e = engine();
    uint64_t h = static_cast<uint64_t>(body.asInt());
    if (e.bodies.get(h) && e.physics) {
        e.physics->addForce(h, Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                                    static_cast<float>(z.asFloat())));
    }
    return Value();
}

Value AddImpulse(const Value& body, const Value& x, const Value& y, const Value& z) {
    auto& e = engine();
    uint64_t h = static_cast<uint64_t>(body.asInt());
    if (e.bodies.get(h) && e.physics) {
        e.physics->addImpulse(h, Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                                      static_cast<float>(z.asFloat())));
    }
    return Value();
}

Value Raycast(const Value& ox, const Value& oy, const Value& oz, const Value& dx, const Value& dy, const Value& dz,
              const Value& maxDist) {
    Value result = Value::MakeObject();
    auto& e = engine();
    if (!e.physics) {
        member_ref(result, "hit") = Value(false);
        return result;
    }
    RaycastHit hit = e.physics->raycast(
        Vec3(static_cast<float>(ox.asFloat()), static_cast<float>(oy.asFloat()), static_cast<float>(oz.asFloat())),
        Vec3(static_cast<float>(dx.asFloat()), static_cast<float>(dy.asFloat()), static_cast<float>(dz.asFloat())),
        static_cast<float>(maxDist.asFloat()));

    member_ref(result, "hit") = Value(hit.hit);
    if (hit.hit) {
        member_ref(result, "x") = Value(static_cast<double>(hit.point.x));
        member_ref(result, "y") = Value(static_cast<double>(hit.point.y));
        member_ref(result, "z") = Value(static_cast<double>(hit.point.z));
        // New in the PhysX backend: a real surface normal, which is what
        // you need for decals, ricochets, and slope checks.
        member_ref(result, "nx") = Value(static_cast<double>(hit.normal.x));
        member_ref(result, "ny") = Value(static_cast<double>(hit.normal.y));
        member_ref(result, "nz") = Value(static_cast<double>(hit.normal.z));
        member_ref(result, "distance") = Value(static_cast<double>(hit.distance));
        member_ref(result, "body") = Value::MakeHandle(hit.body, "body");
    }
    return result;
}

// ---- commands new to the PhysX backend --------------------------------

Value GetBodyPosition(const Value& body) {
    if (auto* b = getBody(body)) {
        return make_array({Value(b->pose.position.x), Value(b->pose.position.y), Value(b->pose.position.z)});
    }
    return make_array({Value(0.0), Value(0.0), Value(0.0)});
}

Value SetBodyPosition(const Value& body, const Value& x, const Value& y, const Value& z) {
    auto& e = engine();
    uint64_t h = static_cast<uint64_t>(body.asInt());
    if (auto* b = e.bodies.get(h)) {
        b->pose.position = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                                static_cast<float>(z.asFloat()));
        if (e.physics) {
            // Kinematic bodies get a *target* rather than a teleport, so a
            // moving platform pushes what's standing on it.
            if (b->isKinematic) e.physics->setKinematicTarget(h, b->pose);
            else e.physics->setPose(h, b->pose);
        }
    }
    return Value();
}

Value GetBodyRotation(const Value& body) {
    if (auto* b = getBody(body)) {
        Vec3 e = b->pose.rotation.toEuler();
        return make_array({Value(e.x), Value(e.y), Value(e.z)});
    }
    return make_array({Value(0.0), Value(0.0), Value(0.0)});
}

Value SetBodyRotation(const Value& body, const Value& pitch, const Value& yaw, const Value& roll) {
    auto& e = engine();
    uint64_t h = static_cast<uint64_t>(body.asInt());
    if (auto* b = e.bodies.get(h)) {
        b->pose.rotation = Quat::fromEuler(static_cast<float>(pitch.asFloat()), static_cast<float>(yaw.asFloat()),
                                           static_cast<float>(roll.asFloat()));
        if (e.physics) e.physics->setPose(h, b->pose);
    }
    return Value();
}

Value AddTorque(const Value& body, const Value& x, const Value& y, const Value& z) {
    auto& e = engine();
    uint64_t h = static_cast<uint64_t>(body.asInt());
    if (e.bodies.get(h) && e.physics) {
        e.physics->addTorque(h, Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                                     static_cast<float>(z.asFloat())));
    }
    return Value();
}

Value SetBodyAngularVelocity(const Value& body, const Value& x, const Value& y, const Value& z) {
    auto& e = engine();
    uint64_t h = static_cast<uint64_t>(body.asInt());
    if (auto* b = e.bodies.get(h)) {
        b->angularVelocity = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                                  static_cast<float>(z.asFloat()));
        if (e.physics) e.physics->setAngularVelocity(h, b->angularVelocity);
    }
    return Value();
}

Value GetBodyAngularVelocity(const Value& body) {
    if (auto* b = getBody(body)) {
        return make_array({Value(b->angularVelocity.x), Value(b->angularVelocity.y),
                           Value(b->angularVelocity.z)});
    }
    return make_array({Value(0.0), Value(0.0), Value(0.0)});
}

Value SetBodyKinematic(const Value& body, const Value& enabled) {
    auto& e = engine();
    uint64_t h = static_cast<uint64_t>(body.asInt());
    if (auto* b = e.bodies.get(h)) {
        b->isKinematic = enabled.truthy();
        if (e.physics) e.physics->applyBodyFlags(h, *b);
    }
    return Value();
}

Value SetBodyDamping(const Value& body, const Value& linear, const Value& angular) {
    auto& e = engine();
    uint64_t h = static_cast<uint64_t>(body.asInt());
    if (auto* b = e.bodies.get(h)) {
        b->linearDamping = std::max(0.0f, static_cast<float>(linear.asFloat()));
        b->angularDamping = std::max(0.0f, static_cast<float>(angular.asFloat()));
        if (e.physics) e.physics->applyBodyFlags(h, *b);
    }
    return Value();
}

Value SetBodyGravityEnabled(const Value& body, const Value& enabled) {
    auto& e = engine();
    uint64_t h = static_cast<uint64_t>(body.asInt());
    if (auto* b = e.bodies.get(h)) {
        b->gravityEnabled = enabled.truthy();
        if (e.physics) e.physics->applyBodyFlags(h, *b);
    }
    return Value();
}

// Axis locks, taken as three booleans each. Invaluable for character
// controllers (lock all rotation so the capsule can't tip over) and 2.5D
// games (lock Z translation).
Value SetBodyFreezePosition(const Value& body, const Value& fx, const Value& fy, const Value& fz) {
    auto& e = engine();
    uint64_t h = static_cast<uint64_t>(body.asInt());
    if (auto* b = e.bodies.get(h)) {
        b->freezePosX = fx.truthy();
        b->freezePosY = fy.truthy();
        b->freezePosZ = fz.truthy();
        if (e.physics) e.physics->applyBodyFlags(h, *b);
    }
    return Value();
}

Value SetBodyFreezeRotation(const Value& body, const Value& fx, const Value& fy, const Value& fz) {
    auto& e = engine();
    uint64_t h = static_cast<uint64_t>(body.asInt());
    if (auto* b = e.bodies.get(h)) {
        b->freezeRotX = fx.truthy();
        b->freezeRotY = fy.truthy();
        b->freezeRotZ = fz.truthy();
        if (e.physics) e.physics->applyBodyFlags(h, *b);
    }
    return Value();
}

Value SetColliderMaterial(const Value& collider, const Value& staticFriction, const Value& dynamicFriction,
                          const Value& restitution) {
    if (auto* c = engine().colliders.get(static_cast<uint64_t>(collider.asInt()))) {
        c->staticFriction = clampf(static_cast<float>(staticFriction.asFloat()), 0.0f, 1.0f);
        c->dynamicFriction = clampf(static_cast<float>(dynamicFriction.asFloat()), 0.0f, 1.0f);
        c->restitution = clampf(static_cast<float>(restitution.asFloat()), 0.0f, 1.0f);
        // Applies to bodies created *after* this call. PhysX materials are
        // baked into the shape at attach time, so changing one retroactively
        // would mean rebuilding the actor.
    }
    return Value();
}

Value SetColliderTrigger(const Value& collider, const Value& enabled) {
    if (auto* c = engine().colliders.get(static_cast<uint64_t>(collider.asInt()))) {
        c->isTrigger = enabled.truthy();
    }
    return Value();
}

Value DestroyBody(const Value& body) {
    uint64_t h = static_cast<uint64_t>(body.asInt());
    physicsUnregisterBody(h);
    engine().bodies.remove(h);
    return Value();
}

Value SetPhysicsTimestep(const Value& hz, const Value& maxSubsteps) {
    auto& e = engine();
    e.physicsFixedHz = std::max(1.0f, static_cast<float>(hz.asFloat()));
    e.physicsMaxSubsteps = std::max(1, static_cast<int>(maxSubsteps.asInt()));
    if (e.physics) e.physics->setFixedTimestep(e.physicsFixedHz, e.physicsMaxSubsteps);
    return Value();
}

// Contacts from the most recent step, as an array of objects. Drained per
// frame, so a script reads this right after StepPhysics.
Value GetContacts() {
    Value arr = Value::MakeArray();
    auto& e = engine();
    if (!e.physics) return arr;
    for (const ContactEvent& c : e.physics->contacts()) {
        Value o = Value::MakeObject();
        member_ref(o, "bodyA") = Value::MakeHandle(c.bodyA, "body");
        member_ref(o, "bodyB") = Value::MakeHandle(c.bodyB, "body");
        member_ref(o, "x") = Value(static_cast<double>(c.point.x));
        member_ref(o, "y") = Value(static_cast<double>(c.point.y));
        member_ref(o, "z") = Value(static_cast<double>(c.point.z));
        member_ref(o, "nx") = Value(static_cast<double>(c.normal.x));
        member_ref(o, "ny") = Value(static_cast<double>(c.normal.y));
        member_ref(o, "nz") = Value(static_cast<double>(c.normal.z));
        member_ref(o, "impulse") = Value(static_cast<double>(c.impulse));
        member_ref(o, "trigger") = Value(c.isTrigger);
        arr.arrayRef().push_back(o);
    }
    return arr;
}

Value GetPhysicsBackend() {
    auto& e = engine();
    return Value(e.physics ? std::string(e.physics->name()) : std::string("none"));
}

} // namespace rt
} // namespace pv
