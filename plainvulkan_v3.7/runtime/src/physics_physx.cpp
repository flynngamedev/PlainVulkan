// physics_physx.cpp -- the real PhysX backend.
//
// Compiled only when the build found the PhysX SDK (-DPV_WITH_PHYSX=ON,
// which is the default; see runtime/cmake/FindPhysX.cmake). When it isn't
// available this file still compiles, but createPhysXBackend() returns
// nullptr and physics.cpp falls back to the builtin solver.
//
// What this buys over the solver it replaces:
//   * a real constraint solver, so stacks, joints, and resting contact are
//     stable instead of jittering apart
//   * real rotational dynamics -- torque, angular velocity, inertia tensors
//     computed from the shape. The old solver had no rotation at all.
//   * genuine capsule and convex shapes rather than capsules-as-spheres
//   * continuous collision detection, so a fast projectile no longer
//     tunnels straight through a thin wall between two frames
//   * friction and restitution via PxMaterial
//   * a real broadphase, so body count stops being O(n^2)
//   * scene-query raycasts with proper filtering
//
// Threading: PhysX gets a CPU dispatcher sized to hardware concurrency
// minus one (leaving a core for the render thread). simulate()/fetchResults()
// are called back-to-back within step() rather than overlapped with
// rendering -- overlapping is the real performance win, but it requires the
// whole engine to agree on when body poses are readable, and the Pv:: API
// lets script read a body's position at any point in a frame. Correctness
// first; the pipelining is a contained follow-up.
#include "pv/pv_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>

#if defined(PV_WITH_PHYSX)

// PhysX headers pull in windows.h on Windows; pv_platform.h (via
// pv_internal.h above) has already set NOMINMAX/WIN32_LEAN_AND_MEAN, which
// is exactly why it must be included first.
#include <PxPhysicsAPI.h>

namespace pv {
namespace {

using namespace physx;

// --- conversions -------------------------------------------------------
// pv::Quat is deliberately (x, y, z, w) with w last, matching PxQuat's
// constructor order, so these are plain component copies with no shuffle.
inline PxVec3 toPx(const Vec3& v) { return PxVec3(v.x, v.y, v.z); }
inline Vec3 fromPx(const PxVec3& v) { return Vec3(v.x, v.y, v.z); }
inline PxQuat toPx(const Quat& q) { return PxQuat(q.x, q.y, q.z, q.w); }
inline Quat fromPx(const PxQuat& q) { return Quat(q.x, q.y, q.z, q.w); }
inline PxTransform toPx(const Transform& t) { return PxTransform(toPx(t.position), toPx(t.rotation)); }

// PhysX routes its own diagnostics through this rather than printing to
// stdout, so SDK warnings show up in the same log stream as everything else.
class PvErrorCallback final : public PxErrorCallback {
public:
    void reportError(PxErrorCode::Enum code, const char* message, const char* file, int line) override {
        const char* level = "ERROR";
        if (code == PxErrorCode::eDEBUG_INFO) level = "INFO";
        else if (code == PxErrorCode::eDEBUG_WARNING || code == PxErrorCode::ePERF_WARNING) level = "WARNING";
        logLine(level, std::string("PhysX: ") + (message ? message : "(no message)") + " [" +
                           (file ? file : "?") + ":" + std::to_string(line) + "]");
    }
};

// Collects contact and trigger events during fetchResults so the scene
// system can react to collisions from script.
class PvSimulationCallback final : public PxSimulationEventCallback {
public:
    std::vector<ContactEvent>* sink = nullptr;
    std::unordered_map<const PxActor*, uint64_t>* actorToHandle = nullptr;

    void onContact(const PxContactPairHeader& header, const PxContactPair* pairs, PxU32 count) override {
        if (!sink || !actorToHandle) return;
        for (PxU32 i = 0; i < count; i++) {
            const PxContactPair& pair = pairs[i];
            if (!(pair.events & PxPairFlag::eNOTIFY_TOUCH_FOUND)) continue;
            // An actor removed during this same step has its entry in the
            // header flagged; reading its pose would be a use-after-free.
            if (header.flags & (PxContactPairHeaderFlag::eREMOVED_ACTOR_0 |
                                PxContactPairHeaderFlag::eREMOVED_ACTOR_1)) {
                continue;
            }

            ContactEvent ev;
            ev.bodyA = lookup(header.actors[0]);
            ev.bodyB = lookup(header.actors[1]);
            if (ev.bodyA == 0 && ev.bodyB == 0) continue;

            PxContactPairPoint points[8];
            PxU32 n = pair.extractContacts(points, 8);
            if (n > 0) {
                ev.point = fromPx(points[0].position);
                ev.normal = fromPx(points[0].normal);
                float total = 0.0f;
                for (PxU32 k = 0; k < n; k++) total += points[k].impulse.magnitude();
                ev.impulse = total;
            }
            sink->push_back(ev);
        }
    }

    void onTrigger(PxTriggerPair* pairs, PxU32 count) override {
        if (!sink || !actorToHandle) return;
        for (PxU32 i = 0; i < count; i++) {
            const PxTriggerPair& p = pairs[i];
            // Same removal hazard as contacts.
            if (p.flags & (PxTriggerPairFlag::eREMOVED_SHAPE_TRIGGER | PxTriggerPairFlag::eREMOVED_SHAPE_OTHER)) {
                continue;
            }
            if (p.status != PxPairFlag::eNOTIFY_TOUCH_FOUND) continue;
            ContactEvent ev;
            ev.isTrigger = true;
            ev.bodyA = lookup(p.triggerActor);
            ev.bodyB = lookup(p.otherActor);
            sink->push_back(ev);
        }
    }

    void onConstraintBreak(PxConstraintInfo*, PxU32) override {}
    void onWake(PxActor**, PxU32) override {}
    void onSleep(PxActor**, PxU32) override {}
    void onAdvance(const PxRigidBody* const*, const PxTransform*, const PxU32) override {}

private:
    uint64_t lookup(const PxActor* a) const {
        auto it = actorToHandle->find(a);
        return it == actorToHandle->end() ? 0 : it->second;
    }
};

// Enables contact reporting for every pair. PhysX defaults to reporting
// nothing, so without this onContact never fires and collision callbacks
// silently do nothing -- a common first-time PhysX integration bug.
PxFilterFlags pvFilterShader(PxFilterObjectAttributes a0, PxFilterData, PxFilterObjectAttributes a1, PxFilterData,
                             PxPairFlags& pairFlags, const void*, PxU32) {
    if (PxFilterObjectIsTrigger(a0) || PxFilterObjectIsTrigger(a1)) {
        pairFlags = PxPairFlag::eTRIGGER_DEFAULT;
        return PxFilterFlag::eDEFAULT;
    }
    pairFlags = PxPairFlag::eCONTACT_DEFAULT | PxPairFlag::eNOTIFY_TOUCH_FOUND |
                PxPairFlag::eNOTIFY_CONTACT_POINTS;
    return PxFilterFlag::eDEFAULT;
}

struct PhysXBody {
    PxRigidActor* actor = nullptr;
    PxMaterial* material = nullptr;
    bool dynamic = false;
};

class PhysXBackend final : public PhysicsBackend {
public:
    const char* name() const override { return "physx"; }

    bool init() override {
        foundation_ = PxCreateFoundation(PX_PHYSICS_VERSION, allocator_, errorCallback_);
        if (!foundation_) {
            logLine("ERROR", "PhysX: PxCreateFoundation failed");
            return false;
        }

        // The PVD (PhysX Visual Debugger) connection is opt-in via an env
        // var: it's genuinely useful for debugging a physics problem, but it
        // opens a socket, so it should never be on by default.
        if (const char* pvdHost = std::getenv("PV_PHYSX_PVD")) {
            pvd_ = PxCreatePvd(*foundation_);
            PxPvdTransport* transport = PxDefaultPvdSocketTransportCreate(pvdHost, 5425, 10);
            if (pvd_->connect(*transport, PxPvdInstrumentationFlag::eALL)) {
                logLine("INFO", std::string("PhysX: connected to PVD at ") + pvdHost);
            }
        }

        physics_ = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation_, PxTolerancesScale(), true, pvd_);
        if (!physics_) {
            logLine("ERROR", "PhysX: PxCreatePhysics failed");
            return false;
        }

        // One worker short of hardware concurrency, so the render thread
        // isn't fighting the physics workers for a core.
        unsigned hw = std::thread::hardware_concurrency();
        int workers = static_cast<int>(hw > 2 ? hw - 1 : 1);
        dispatcher_ = PxDefaultCpuDispatcherCreate(static_cast<PxU32>(workers));

        PxSceneDesc desc(physics_->getTolerancesScale());
        desc.gravity = toPx(gravity_);
        desc.cpuDispatcher = dispatcher_;
        desc.filterShader = pvFilterShader;
        desc.simulationEventCallback = &eventCallback_;
        // Enabling CCD scene-wide: without it a fast-moving small body
        // passes straight through thin geometry between two steps, which is
        // the single most reported "my bullet went through the wall" bug.
        desc.flags |= PxSceneFlag::eENABLE_CCD;
        desc.flags |= PxSceneFlag::eENABLE_ACTIVE_ACTORS;

        scene_ = physics_->createScene(desc);
        if (!scene_) {
            logLine("ERROR", "PhysX: createScene failed");
            return false;
        }

        eventCallback_.sink = &contacts_;
        eventCallback_.actorToHandle = &actorToHandle_;

        defaultMaterial_ = physics_->createMaterial(0.5f, 0.5f, 0.0f);
        logLine("INFO", std::string("PhysX ") + std::to_string(PX_PHYSICS_VERSION / 10000000) + "." +
                            std::to_string((PX_PHYSICS_VERSION / 100000) % 100) + " initialized with " +
                            std::to_string(workers) + " worker thread(s)");
        return true;
    }

    void shutdown() override {
        for (auto& kv : bodies_) {
            if (kv.second.actor) {
                scene_->removeActor(*kv.second.actor);
                kv.second.actor->release();
            }
            if (kv.second.material) kv.second.material->release();
        }
        bodies_.clear();
        actorToHandle_.clear();

        // Release order matters and is the reverse of creation: releasing
        // the foundation while a scene still references it is a crash on
        // exit that only shows up sometimes.
        if (defaultMaterial_) { defaultMaterial_->release(); defaultMaterial_ = nullptr; }
        if (scene_) { scene_->release(); scene_ = nullptr; }
        if (dispatcher_) { dispatcher_->release(); dispatcher_ = nullptr; }
        if (physics_) { physics_->release(); physics_ = nullptr; }
        if (pvd_) {
            PxPvdTransport* t = pvd_->getTransport();
            pvd_->release();
            pvd_ = nullptr;
            if (t) t->release();
        }
        if (foundation_) { foundation_->release(); foundation_ = nullptr; }
    }

    void setGravity(const Vec3& g) override {
        gravity_ = g;
        if (scene_) scene_->setGravity(toPx(g));
    }

    void setFixedTimestep(float hz, int maxSubsteps) override {
        fixedDt_ = hz > 1.0f ? (1.0f / hz) : (1.0f / 60.0f);
        maxSubsteps_ = std::max(1, maxSubsteps);
    }

    void step(float dt) override {
        contacts_.clear();
        if (!scene_ || dt <= 0.0f) return;

        accumulator_ += dt;
        float maxBacklog = fixedDt_ * static_cast<float>(maxSubsteps_);
        if (accumulator_ > maxBacklog) accumulator_ = maxBacklog;

        int steps = 0;
        while (accumulator_ >= fixedDt_ && steps < maxSubsteps_) {
            scene_->simulate(fixedDt_);
            // Blocking fetch. See the threading note at the top of the file
            // for why this isn't overlapped with rendering yet.
            scene_->fetchResults(true);
            accumulator_ -= fixedDt_;
            steps++;
        }
    }

    bool addBody(uint64_t handle, const RigidBodyRecord& rec, const ColliderRecord& col) override {
        if (!scene_ || !physics_) return false;

        PxMaterial* mat = physics_->createMaterial(col.staticFriction, col.dynamicFriction, col.restitution);
        if (!mat) mat = defaultMaterial_;

        PxTransform pose = toPx(rec.pose);
        // A non-finite pose (a NaN that crept in from script arithmetic)
        // makes PhysX assert and abort the process. Catching it here turns a
        // hard crash into a log line.
        if (!pose.isSane()) {
            logLine("ERROR", "PhysX: rejected a body with a non-finite transform; check for NaN in its position");
            return false;
        }

        PhysXBody entry;
        entry.material = mat;
        entry.dynamic = !rec.isStatic;

        PxShape* shape = createShape(col, *mat);
        if (!shape) return false;

        // Shapes are attached at the collider's local offset, which is what
        // makes CreateBoxCollider(x,y,z,...) place the box relative to the
        // body rather than at the world origin.
        shape->setLocalPose(PxTransform(toPx(col.center)));
        if (col.isTrigger) {
            // A shape can't be both a simulation shape and a trigger.
            shape->setFlag(PxShapeFlag::eSIMULATION_SHAPE, false);
            shape->setFlag(PxShapeFlag::eTRIGGER_SHAPE, true);
        }

        if (rec.isStatic) {
            PxRigidStatic* actor = physics_->createRigidStatic(pose);
            actor->attachShape(*shape);
            entry.actor = actor;
        } else {
            PxRigidDynamic* actor = physics_->createRigidDynamic(pose);
            actor->attachShape(*shape);
            // Distributes mass across the attached shapes to get a real
            // inertia tensor -- without this, angular motion is wrong in a
            // way that reads as objects spinning far too easily.
            PxRigidBodyExt::updateMassAndInertia(*actor, std::max(rec.mass, 0.0001f));
            actor->setLinearDamping(rec.linearDamping);
            actor->setAngularDamping(rec.angularDamping);
            actor->setLinearVelocity(toPx(rec.velocity));
            actor->setAngularVelocity(toPx(rec.angularVelocity));
            actor->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, true);
            entry.actor = actor;
        }
        shape->release(); // the actor holds its own reference now

        applyFlagsTo(entry, rec);
        scene_->addActor(*entry.actor);
        actorToHandle_[entry.actor] = handle;
        bodies_[handle] = entry;
        return true;
    }

    void removeBody(uint64_t handle) override {
        auto it = bodies_.find(handle);
        if (it == bodies_.end()) return;
        if (it->second.actor) {
            actorToHandle_.erase(it->second.actor);
            scene_->removeActor(*it->second.actor);
            it->second.actor->release();
        }
        if (it->second.material && it->second.material != defaultMaterial_) it->second.material->release();
        bodies_.erase(it);
    }

    void setPose(uint64_t handle, const Transform& t) override {
        if (auto* b = find(handle)) {
            PxTransform p = toPx(t);
            if (p.isSane()) b->actor->setGlobalPose(p);
        }
    }

    void setLinearVelocity(uint64_t handle, const Vec3& v) override {
        if (auto* d = findDynamic(handle)) d->setLinearVelocity(toPx(v));
    }
    void setAngularVelocity(uint64_t handle, const Vec3& v) override {
        if (auto* d = findDynamic(handle)) d->setAngularVelocity(toPx(v));
    }
    void addForce(uint64_t handle, const Vec3& f) override {
        // eFORCE is mass-dependent and integrated over the step, which is
        // the correct meaning of "force" (as opposed to addImpulse below).
        if (auto* d = findDynamic(handle)) d->addForce(toPx(f), PxForceMode::eFORCE);
    }
    void addImpulse(uint64_t handle, const Vec3& i) override {
        if (auto* d = findDynamic(handle)) d->addForce(toPx(i), PxForceMode::eIMPULSE);
    }
    void addTorque(uint64_t handle, const Vec3& t) override {
        if (auto* d = findDynamic(handle)) d->addTorque(toPx(t), PxForceMode::eFORCE);
    }

    void setKinematicTarget(uint64_t handle, const Transform& t) override {
        if (auto* d = findDynamic(handle)) {
            if (d->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) {
                // A kinematic target (rather than a pose set) is what makes a
                // moving platform push dynamic bodies instead of passing
                // through them.
                PxTransform p = toPx(t);
                if (p.isSane()) d->setKinematicTarget(p);
            } else {
                setPose(handle, t);
            }
        }
    }

    void applyBodyFlags(uint64_t handle, const RigidBodyRecord& rec) override {
        if (auto* b = find(handle)) applyFlagsTo(*b, rec);
    }

    void readBack(uint64_t handle, RigidBodyRecord& rec) override {
        auto* b = find(handle);
        if (!b || !b->actor) return;
        PxTransform p = b->actor->getGlobalPose();
        rec.pose.position = fromPx(p.p);
        rec.pose.rotation = fromPx(p.q);
        if (b->dynamic) {
            auto* d = static_cast<PxRigidDynamic*>(b->actor);
            rec.velocity = fromPx(d->getLinearVelocity());
            rec.angularVelocity = fromPx(d->getAngularVelocity());
        }
    }

    RaycastHit raycast(const Vec3& origin, const Vec3& dirRaw, float maxDist) override {
        RaycastHit out;
        if (!scene_) return out;
        Vec3 dir = dirRaw.normalized();
        if (dir.lengthSq() < 0.5f) return out;
        // PhysX asserts on a zero or non-positive distance.
        if (!(maxDist > 0.0f)) return out;

        PxRaycastBuffer hit;
        PxHitFlags flags = PxHitFlag::ePOSITION | PxHitFlag::eNORMAL;
        if (scene_->raycast(toPx(origin), toPx(dir), maxDist, hit, flags) && hit.hasBlock) {
            out.hit = true;
            out.point = fromPx(hit.block.position);
            out.normal = fromPx(hit.block.normal);
            out.distance = hit.block.distance;
            auto it = actorToHandle_.find(hit.block.actor);
            out.body = it == actorToHandle_.end() ? 0 : it->second;
        }
        return out;
    }

    const std::vector<ContactEvent>& contacts() const override { return contacts_; }
    bool supportsRotation() const override { return true; }

private:
    PhysXBody* find(uint64_t h) {
        auto it = bodies_.find(h);
        return it == bodies_.end() ? nullptr : &it->second;
    }
    PxRigidDynamic* findDynamic(uint64_t h) {
        auto* b = find(h);
        if (!b || !b->dynamic || !b->actor) return nullptr;
        return static_cast<PxRigidDynamic*>(b->actor);
    }

    PxShape* createShape(const ColliderRecord& col, PxMaterial& mat) {
        switch (col.kind) {
            case ColliderRecord::Kind::Box:
                return physics_->createShape(
                    PxBoxGeometry(std::max(col.halfExtents.x, 0.001f), std::max(col.halfExtents.y, 0.001f),
                                  std::max(col.halfExtents.z, 0.001f)),
                    mat, true);
            case ColliderRecord::Kind::Sphere:
                return physics_->createShape(PxSphereGeometry(std::max(col.radius, 0.001f)), mat, true);
            case ColliderRecord::Kind::Capsule: {
                // PhysX capsules are X-axis aligned by default, but every
                // other engine (and every character controller anyone will
                // write) expects them upright, so rotate 90 degrees about Z.
                // `height` in the Pv:: API is the total cylindrical section,
                // and PxCapsuleGeometry wants the half-height.
                PxShape* s = physics_->createShape(
                    PxCapsuleGeometry(std::max(col.radius, 0.001f), std::max(col.height * 0.5f, 0.001f)), mat, true);
                if (s) s->setLocalPose(PxTransform(PxQuat(PxHalfPi, PxVec3(0, 0, 1))));
                return s;
            }
        }
        return nullptr;
    }

    void applyFlagsTo(PhysXBody& b, const RigidBodyRecord& rec) {
        if (!b.dynamic || !b.actor) return;
        auto* d = static_cast<PxRigidDynamic*>(b.actor);
        d->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, rec.isKinematic);
        d->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !rec.gravityEnabled);
        d->setLinearDamping(rec.linearDamping);
        d->setAngularDamping(rec.angularDamping);

        PxRigidDynamicLockFlags locks = PxRigidDynamicLockFlags(0);
        if (rec.freezePosX) locks |= PxRigidDynamicLockFlag::eLOCK_LINEAR_X;
        if (rec.freezePosY) locks |= PxRigidDynamicLockFlag::eLOCK_LINEAR_Y;
        if (rec.freezePosZ) locks |= PxRigidDynamicLockFlag::eLOCK_LINEAR_Z;
        if (rec.freezeRotX) locks |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_X;
        if (rec.freezeRotY) locks |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Y;
        if (rec.freezeRotZ) locks |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Z;
        d->setRigidDynamicLockFlags(locks);
    }

    PxDefaultAllocator allocator_;
    PvErrorCallback errorCallback_;
    PvSimulationCallback eventCallback_;

    PxFoundation* foundation_ = nullptr;
    PxPhysics* physics_ = nullptr;
    PxDefaultCpuDispatcher* dispatcher_ = nullptr;
    PxScene* scene_ = nullptr;
    PxPvd* pvd_ = nullptr;
    PxMaterial* defaultMaterial_ = nullptr;

    std::unordered_map<uint64_t, PhysXBody> bodies_;
    std::unordered_map<const PxActor*, uint64_t> actorToHandle_;
    std::vector<ContactEvent> contacts_;

    Vec3 gravity_{0, -9.8f, 0};
    float fixedDt_ = 1.0f / 60.0f;
    int maxSubsteps_ = 4;
    float accumulator_ = 0.0f;
};

} // namespace

std::unique_ptr<PhysicsBackend> createPhysXBackend() {
    auto backend = std::unique_ptr<PhysicsBackend>(new PhysXBackend());
    if (!backend->init()) {
        backend->shutdown();
        return nullptr;
    }
    return backend;
}

} // namespace pv

#else // !PV_WITH_PHYSX

namespace pv {

std::unique_ptr<PhysicsBackend> createPhysXBackend() {
    // Not an error: the build simply didn't have the SDK. physics.cpp
    // reports the fallback once, at InitPhysics.
    return nullptr;
}

} // namespace pv

#endif
