// physics_builtin.cpp -- the fallback physics backend.
//
// This is the engine's original hand-written solver, moved behind the
// PhysicsBackend interface rather than deleted. It exists so that a build
// without the PhysX SDK still simulates something instead of failing to
// link, and so a suspected PhysX bug can be bisected against the old
// behaviour by setting PV_PHYSICS_BACKEND=builtin.
//
// Its limits are the ones runtime/README.md always documented, now stated
// as capability flags the rest of the engine can query rather than as
// prose: no rotation (supportsRotation() returns false), capsules collide
// as spheres, and the contact model is discrete AABB overlap resolution
// with no friction or restitution beyond a velocity zero-out.
//
// The one thing added here beyond the original: fixed-timestep
// accumulation, so both backends step at the same rate and a scene tuned
// against one doesn't behave differently under the other purely because of
// framerate.
#include "pv/pv_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>

namespace pv {

namespace {

struct AABB {
    Vec3 lo, hi;
};

AABB colliderAABB(const ColliderRecord& c, Vec3 bodyPos) {
    Vec3 center = bodyPos + c.center;
    if (c.kind == ColliderRecord::Kind::Box) {
        return {center - c.halfExtents, center + c.halfExtents};
    }
    // Sphere and capsule alike: capsules are treated as spheres here, which
    // is the documented simplification of this backend.
    float r = c.radius;
    return {Vec3(center.x - r, center.y - r, center.z - r), Vec3(center.x + r, center.y + r, center.z + r)};
}

struct BuiltinBody {
    RigidBodyRecord rec;
    ColliderRecord col;
    Vec3 pendingForce;
    Vec3 pendingImpulse;
};

class BuiltinBackend final : public PhysicsBackend {
public:
    const char* name() const override { return "builtin"; }

    bool init() override {
        logLine("WARNING",
                "Physics: using the builtin fallback solver, not PhysX. It has no rotation, no friction, "
                "and treats capsules as spheres. Build with -DPV_WITH_PHYSX=ON for the real backend "
                "(see runtime/README.md).");
        bodies_.clear();
        contacts_.clear();
        return true;
    }

    void shutdown() override {
        bodies_.clear();
        contacts_.clear();
    }

    void setGravity(const Vec3& g) override { gravity_ = g; }

    void setFixedTimestep(float hz, int maxSubsteps) override {
        fixedDt_ = hz > 1.0f ? (1.0f / hz) : (1.0f / 60.0f);
        maxSubsteps_ = std::max(1, maxSubsteps);
    }

    void step(float dt) override {
        contacts_.clear();
        if (dt <= 0.0f) return;
        // Accumulate wall-clock time and consume it in fixed slices. A
        // variable timestep makes any iterative solver behave differently
        // frame to frame -- a stack stable at 144fps explodes at 30fps.
        accumulator_ += dt;
        // Cap the backlog so a long stall (a breakpoint, a window drag)
        // doesn't trigger a spiral of death where catching up costs more
        // time than it recovers.
        float maxBacklog = fixedDt_ * static_cast<float>(maxSubsteps_);
        if (accumulator_ > maxBacklog) accumulator_ = maxBacklog;

        int steps = 0;
        while (accumulator_ >= fixedDt_ && steps < maxSubsteps_) {
            substep(fixedDt_);
            accumulator_ -= fixedDt_;
            steps++;
        }
    }

    bool addBody(uint64_t body, const RigidBodyRecord& rec, const ColliderRecord& col) override {
        BuiltinBody b;
        b.rec = rec;
        b.col = col;
        bodies_[body] = b;
        return true;
    }

    void removeBody(uint64_t body) override { bodies_.erase(body); }

    void setPose(uint64_t body, const Transform& t) override {
        if (auto* b = find(body)) b->rec.pose = t;
    }
    void setLinearVelocity(uint64_t body, const Vec3& v) override {
        if (auto* b = find(body)) b->rec.velocity = v;
    }
    void setAngularVelocity(uint64_t body, const Vec3& v) override {
        // Stored so a script that sets it and reads it back is consistent,
        // but nothing integrates it -- see supportsRotation().
        if (auto* b = find(body)) b->rec.angularVelocity = v;
    }
    void addForce(uint64_t body, const Vec3& f) override {
        if (auto* b = find(body)) b->pendingForce += f;
    }
    void addImpulse(uint64_t body, const Vec3& i) override {
        if (auto* b = find(body)) b->pendingImpulse += i;
    }
    void addTorque(uint64_t, const Vec3&) override {
        warnOnce("addTorque", "the builtin backend has no rotational dynamics");
    }
    void setKinematicTarget(uint64_t body, const Transform& t) override {
        if (auto* b = find(body)) b->rec.pose = t;
    }
    void applyBodyFlags(uint64_t body, const RigidBodyRecord& rec) override {
        if (auto* b = find(body)) {
            b->rec.isStatic = rec.isStatic;
            b->rec.isKinematic = rec.isKinematic;
            b->rec.mass = rec.mass;
            b->rec.linearDamping = rec.linearDamping;
            b->rec.gravityEnabled = rec.gravityEnabled;
            b->rec.freezePosX = rec.freezePosX;
            b->rec.freezePosY = rec.freezePosY;
            b->rec.freezePosZ = rec.freezePosZ;
        }
    }

    void readBack(uint64_t body, RigidBodyRecord& rec) override {
        if (auto* b = find(body)) {
            rec.pose.position = b->rec.pose.position;
            rec.velocity = b->rec.velocity;
            // Rotation deliberately not written: this backend never changes
            // it, and overwriting the entity's authored rotation with a
            // stale identity would visibly reset placed objects.
        }
    }

    RaycastHit raycast(const Vec3& origin, const Vec3& dirRaw, float maxDist) override {
        RaycastHit best;
        Vec3 dir = dirRaw.normalized();
        if (dir.lengthSq() < 0.5f) return best;
        float bestT = std::numeric_limits<float>::max();

        for (auto& kv : bodies_) {
            const BuiltinBody& b = kv.second;
            Vec3 center = b.rec.pose.position + b.col.center;
            float t;
            Vec3 normal;
            bool hit = false;

            if (b.col.kind == ColliderRecord::Kind::Box) {
                hit = raySlab(origin, dir, colliderAABB(b.col, b.rec.pose.position), maxDist, t, normal);
            } else {
                hit = raySphere(origin, dir, center, b.col.radius, maxDist, t, normal);
            }
            if (hit && t < bestT) {
                bestT = t;
                best.hit = true;
                best.distance = t;
                best.point = origin + dir * t;
                best.normal = normal;
                best.body = kv.first;
            }
        }
        return best;
    }

    const std::vector<ContactEvent>& contacts() const override { return contacts_; }
    bool supportsRotation() const override { return false; }

private:
    BuiltinBody* find(uint64_t id) {
        auto it = bodies_.find(id);
        return it == bodies_.end() ? nullptr : &it->second;
    }

    void warnOnce(const char* fn, const char* why) {
        if (warned_.count(fn)) return;
        warned_.insert(fn);
        logLine("WARNING", std::string("Physics (builtin backend): ") + fn + " -- " + why +
                               ". Build with -DPV_WITH_PHYSX=ON for full support.");
    }

    void substep(float dt) {
        // --- integrate ---
        for (auto& kv : bodies_) {
            BuiltinBody& b = kv.second;
            if (b.rec.isStatic || b.rec.isKinematic) {
                b.pendingForce = Vec3();
                b.pendingImpulse = Vec3();
                continue;
            }
            float invMass = 1.0f / std::max(b.rec.mass, 0.0001f);
            if (b.rec.gravityEnabled) b.rec.velocity += gravity_ * dt;
            b.rec.velocity += b.pendingForce * (invMass * dt);
            b.rec.velocity += b.pendingImpulse * invMass;
            b.pendingForce = Vec3();
            b.pendingImpulse = Vec3();

            if (b.rec.linearDamping > 0.0f) {
                b.rec.velocity *= std::exp(-b.rec.linearDamping * dt);
            }
            if (b.rec.freezePosX) b.rec.velocity.x = 0;
            if (b.rec.freezePosY) b.rec.velocity.y = 0;
            if (b.rec.freezePosZ) b.rec.velocity.z = 0;

            b.rec.pose.position += b.rec.velocity * dt;
        }

        // --- resolve pairwise overlaps ---
        // O(n^2) over all bodies. Fine for the scale this backend targets;
        // PhysX does real broadphase.
        std::vector<uint64_t> ids;
        ids.reserve(bodies_.size());
        for (auto& kv : bodies_) ids.push_back(kv.first);

        for (size_t i = 0; i < ids.size(); i++) {
            for (size_t j = i + 1; j < ids.size(); j++) {
                BuiltinBody* a = find(ids[i]);
                BuiltinBody* b = find(ids[j]);
                if (!a || !b) continue;
                bool aFixed = a->rec.isStatic || a->rec.isKinematic;
                bool bFixed = b->rec.isStatic || b->rec.isKinematic;
                if (aFixed && bFixed) continue;

                AABB boxA = colliderAABB(a->col, a->rec.pose.position);
                AABB boxB = colliderAABB(b->col, b->rec.pose.position);
                float ox = std::min(boxA.hi.x, boxB.hi.x) - std::max(boxA.lo.x, boxB.lo.x);
                float oy = std::min(boxA.hi.y, boxB.hi.y) - std::max(boxA.lo.y, boxB.lo.y);
                float oz = std::min(boxA.hi.z, boxB.hi.z) - std::max(boxA.lo.z, boxB.lo.z);
                if (ox <= 0 || oy <= 0 || oz <= 0) continue;

                Vec3 centerA = a->rec.pose.position + a->col.center;
                Vec3 centerB = b->rec.pose.position + b->col.center;

                ContactEvent ev;
                ev.bodyA = ids[i];
                ev.bodyB = ids[j];
                ev.isTrigger = a->col.isTrigger || b->col.isTrigger;
                ev.point = (centerA + centerB) * 0.5f;

                if (ev.isTrigger) {
                    // Triggers report the overlap but never push.
                    ev.normal = (centerB - centerA).normalized();
                    contacts_.push_back(ev);
                    continue;
                }

                float invA = aFixed ? 0.0f : 1.0f / std::max(a->rec.mass, 0.0001f);
                float invB = bFixed ? 0.0f : 1.0f / std::max(b->rec.mass, 0.0001f);
                float totalInv = invA + invB;
                if (totalInv <= 0) continue;

                // Push apart along the axis of least penetration -- the
                // cheapest approximation of a contact normal that still
                // resolves box stacking correctly.
                Vec3 pushA, pushB, normal;
                auto resolveAxis = [&](float overlap, float Vec3::*axis, float ca, float cb) {
                    float sign = (ca < cb) ? -1.0f : 1.0f;
                    pushA.*axis = sign * overlap * (invA / totalInv);
                    pushB.*axis = -sign * overlap * (invB / totalInv);
                    normal.*axis = -sign;
                    if (!aFixed && ((sign < 0) == (a->rec.velocity.*axis > 0))) a->rec.velocity.*axis = 0;
                    if (!bFixed && ((sign > 0) == (b->rec.velocity.*axis < 0))) b->rec.velocity.*axis = 0;
                };

                if (ox <= oy && ox <= oz) resolveAxis(ox, &Vec3::x, centerA.x, centerB.x);
                else if (oy <= ox && oy <= oz) resolveAxis(oy, &Vec3::y, centerA.y, centerB.y);
                else resolveAxis(oz, &Vec3::z, centerA.z, centerB.z);

                if (!aFixed) a->rec.pose.position += pushA;
                if (!bFixed) b->rec.pose.position += pushB;

                ev.normal = normal;
                ev.impulse = (pushA - pushB).length();
                contacts_.push_back(ev);
            }
        }
    }

    static bool raySphere(const Vec3& o, const Vec3& d, const Vec3& center, float radius, float maxD, float& outT,
                          Vec3& outN) {
        Vec3 m = o - center;
        float b = dot(m, d);
        float c = m.lengthSq() - radius * radius;
        // Origin outside the sphere and pointing away: no hit, and skipping
        // the discriminant here avoids a needless sqrt.
        if (c > 0.0f && b > 0.0f) return false;
        float disc = b * b - c;
        if (disc < 0.0f) return false;
        float t = -b - std::sqrt(disc);
        if (t < 0.0f) t = 0.0f;
        if (t > maxD) return false;
        outT = t;
        outN = ((o + d * t) - center).normalized();
        return true;
    }

    static bool raySlab(const Vec3& o, const Vec3& d, const AABB& box, float maxD, float& outT, Vec3& outN) {
        float tmin = 0.0f, tmax = maxD;
        int hitAxis = 0;
        float hitSign = 1.0f;
        const float ov[3] = {o.x, o.y, o.z};
        const float dv[3] = {d.x, d.y, d.z};
        const float lo[3] = {box.lo.x, box.lo.y, box.lo.z};
        const float hi[3] = {box.hi.x, box.hi.y, box.hi.z};

        for (int i = 0; i < 3; i++) {
            if (std::fabs(dv[i]) < 1e-8f) {
                // Ray is parallel to this slab: it either starts inside the
                // slab (no constraint) or misses the box entirely.
                if (ov[i] < lo[i] || ov[i] > hi[i]) return false;
                continue;
            }
            float inv = 1.0f / dv[i];
            float t1 = (lo[i] - ov[i]) * inv;
            float t2 = (hi[i] - ov[i]) * inv;
            float sign = -1.0f;
            if (t1 > t2) {
                std::swap(t1, t2);
                sign = 1.0f;
            }
            if (t1 > tmin) {
                tmin = t1;
                hitAxis = i;
                hitSign = sign;
            }
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        }
        outT = tmin;
        outN = Vec3();
        (&outN.x)[hitAxis] = hitSign;
        return true;
    }

    std::unordered_map<uint64_t, BuiltinBody> bodies_;
    std::vector<ContactEvent> contacts_;
    std::set<std::string> warned_;
    Vec3 gravity_{0, -9.8f, 0};
    float fixedDt_ = 1.0f / 60.0f;
    int maxSubsteps_ = 4;
    float accumulator_ = 0.0f;
};

} // namespace

std::unique_ptr<PhysicsBackend> createBuiltinBackend() {
    return std::unique_ptr<PhysicsBackend>(new BuiltinBackend());
}

} // namespace pv
