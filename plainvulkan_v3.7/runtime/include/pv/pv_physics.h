// pv_physics.h -- the physics abstraction the `Pv::` physics commands are
// written against.
//
// Why an interface rather than calling PhysX directly from physics.cpp:
// PhysX is a large native SDK that has to be fetched and built per
// platform, and a first-time contributor cloning PlainVulkan on a laptop
// should still get a runtime that compiles and runs. So the build has two
// backends behind one vtable:
//
//   PhysX (default, -DPV_WITH_PHYSX=ON)  -- physics_physx.cpp. Real
//       PxScene, real PxRigidDynamic/PxRigidStatic actors, real convex and
//       capsule shapes, real continuous collision detection, real
//       constraint solver, real scene-query raycasts. This is what
//       "replace the physics with PhysX" means and it's what ships.
//
//   Builtin (fallback, -DPV_WITH_PHYSX=OFF) -- physics_builtin.cpp. The
//       original hand-written AABB/sphere solver, kept verbatim so a build
//       without the PhysX SDK still simulates *something* rather than
//       failing to link. It logs a clear warning at InitPhysics so nobody
//       mistakes it for the real backend.
//
// The interface is deliberately shaped around what PhysX natively does
// (poses as position+quaternion, shapes owned separately from actors,
// scene-wide raycast queries) rather than around what the old solver did,
// so the PhysX path is the straightforward one and the fallback is the one
// doing extra work to emulate.
#pragma once

#include "pv/pv_handle.h"
#include "pv/pv_math.h"

#include <memory>
#include <string>
#include <vector>

namespace pv {

// ------------------------------------------------------------------
// Shape description. Created by Pv::CreateBoxCollider and friends, which
// in the PlainVulkan API happen *before* a body exists -- so a collider is
// a description that a later CreateRigidBody call instantiates, not a live
// PhysX object. `center` is the offset from the body origin, which maps
// onto PxShape's local pose.
// ------------------------------------------------------------------
struct ColliderRecord {
    enum class Kind { Box, Sphere, Capsule } kind = Kind::Box;
    Vec3 center;
    Vec3 halfExtents{0.5f, 0.5f, 0.5f}; // box
    float radius = 0.5f;                 // sphere / capsule
    float height = 1.0f;                 // capsule: length of the cylindrical section

    // Surface properties. PhysX takes these as a PxMaterial; the builtin
    // backend uses restitution only. Defaults match PhysX's own defaults so
    // a scene authored against one backend behaves plausibly on the other.
    float staticFriction = 0.5f;
    float dynamicFriction = 0.5f;
    float restitution = 0.0f;

    // A trigger reports overlaps but doesn't push anything -- PxShapeFlag::
    // eTRIGGER_SHAPE. Used by the scene system for pickup/zone volumes.
    bool isTrigger = false;
};

// ------------------------------------------------------------------
// Body. `backendHandle` is an opaque id owned by the active backend
// (PhysX stores the PxActor* in a side table keyed by it). The cached
// pose/velocity are written back by the backend every step so that script
// reads (Pv::GetBodyPosition) are a plain lookup and never round-trip into
// the SDK mid-frame.
// ------------------------------------------------------------------
struct RigidBodyRecord {
    uint64_t collider = 0;
    float mass = 1.0f;
    bool isStatic = false;
    bool isKinematic = false; // moved by script, but still pushes dynamics

    Transform pose;   // position + rotation, kept in sync with the backend
    Vec3 velocity;    // linear
    Vec3 angularVelocity;

    float linearDamping = 0.0f;
    float angularDamping = 0.05f;
    bool gravityEnabled = true;

    // Per-axis motion locks. Cheap and extremely useful for character
    // controllers and 2.5D games; PhysX exposes them natively as
    // PxRigidDynamicLockFlags, and the builtin backend zeroes the axis.
    bool freezePosX = false, freezePosY = false, freezePosZ = false;
    bool freezeRotX = false, freezeRotY = false, freezeRotZ = false;

    uint64_t backendHandle = 0;

    // Set by the scene system so a physics step can write the simulated
    // pose straight back onto the owning entity's transform. 0 = not owned
    // by any entity (a body created directly from script).
    uint64_t ownerEntity = 0;
};

// ------------------------------------------------------------------
// Query results
// ------------------------------------------------------------------
struct RaycastHit {
    bool hit = false;
    Vec3 point;
    Vec3 normal;
    float distance = 0.0f;
    uint64_t body = 0;
};

// A contact reported by the backend during the last step. The scene system
// drains these each frame so script can react to collisions; PhysX fills
// them from a PxSimulationEventCallback, the builtin backend from its own
// overlap resolution pass.
struct ContactEvent {
    uint64_t bodyA = 0;
    uint64_t bodyB = 0;
    Vec3 point;
    Vec3 normal;
    float impulse = 0.0f;
    bool isTrigger = false;
};

// ------------------------------------------------------------------
// The backend interface
// ------------------------------------------------------------------
class PhysicsBackend {
public:
    virtual ~PhysicsBackend() = default;

    virtual const char* name() const = 0;
    virtual bool init() = 0;
    virtual void shutdown() = 0;

    virtual void setGravity(const Vec3& g) = 0;

    // Advances the simulation. Implementations run a fixed internal
    // timestep and accumulate the remainder, because a variable timestep
    // makes any constraint solver (PhysX's included) behave differently
    // frame to frame -- stacks that are stable at 144fps explode at 30fps.
    // The caller still passes wall-clock dt; the fixing happens here.
    virtual void step(float dt) = 0;

    virtual void setFixedTimestep(float hz, int maxSubsteps) = 0;

    // Actor lifecycle. `body` is the engine-side handle; the backend keeps
    // its own map from that to its native object.
    virtual bool addBody(uint64_t body, const RigidBodyRecord& rec, const ColliderRecord& col) = 0;
    virtual void removeBody(uint64_t body) = 0;

    // Per-body state. The backend is the source of truth during simulation;
    // these push script-driven changes in.
    virtual void setPose(uint64_t body, const Transform& t) = 0;
    virtual void setLinearVelocity(uint64_t body, const Vec3& v) = 0;
    virtual void setAngularVelocity(uint64_t body, const Vec3& v) = 0;
    virtual void addForce(uint64_t body, const Vec3& f) = 0;
    virtual void addImpulse(uint64_t body, const Vec3& i) = 0;
    virtual void addTorque(uint64_t body, const Vec3& t) = 0;
    virtual void setKinematicTarget(uint64_t body, const Transform& t) = 0;
    virtual void applyBodyFlags(uint64_t body, const RigidBodyRecord& rec) = 0;

    // Pulls the simulated pose/velocity back out after a step, into the
    // engine-side record. Called once per body per frame by physics.cpp.
    virtual void readBack(uint64_t body, RigidBodyRecord& rec) = 0;

    virtual RaycastHit raycast(const Vec3& origin, const Vec3& dir, float maxDist) = 0;

    // Contacts generated during the most recent step(). Cleared by the
    // next step, so callers must drain it each frame.
    virtual const std::vector<ContactEvent>& contacts() const = 0;

    // Whether this backend actually simulates rotation. The builtin one
    // does not; the scene system checks this so it can avoid writing a
    // meaningless identity rotation over an entity's authored one.
    virtual bool supportsRotation() const = 0;
};

// Factories. createPhysXBackend returns nullptr when the runtime was built
// without PV_WITH_PHYSX, which is how physics.cpp decides to fall back.
std::unique_ptr<PhysicsBackend> createPhysXBackend();
std::unique_ptr<PhysicsBackend> createBuiltinBackend();

// Chooses PhysX when available, otherwise builtin. Honours the
// PV_PHYSICS_BACKEND environment variable ("physx" / "builtin") so a bug
// can be bisected against the old solver without a rebuild.
std::unique_ptr<PhysicsBackend> createDefaultPhysicsBackend();

} // namespace pv
