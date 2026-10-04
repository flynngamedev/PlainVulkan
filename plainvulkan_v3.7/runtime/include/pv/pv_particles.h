// pv_particles.h -- CPU-simulated, GPU-batched particle emitters.
//
// Design notes worth stating up front:
//
// * Simulation is on the CPU. A compute-shader particle system is faster
//   in the large, but it needs a compute queue, storage buffers, an
//   indirect-draw path, and a per-frame readback story for anything that
//   wants to query particle state. For the particle counts a PlainVulkan
//   game realistically uses (thousands, not millions) a tight CPU loop is
//   simpler, portable to every device the runtime already supports, and
//   leaves particle state directly inspectable by the editor -- which
//   matters a lot for authoring.
//
// * Storage is a flat pre-allocated pool per emitter with a swap-remove
//   free list. No per-particle allocation ever happens at runtime, so a
//   burst of 2000 particles doesn't touch the allocator.
//
// * Rendering is one dynamic vertex buffer per frame-in-flight, filled
//   with camera-facing quads and drawn with a single call per emitter
//   (per texture, really). Particles are sorted back-to-front before
//   filling when the emitter uses alpha blending, because alpha-blended
//   geometry drawn out of order produces visibly wrong compositing.
//
// * Emitters live in the Engine's handle table and are *also* referenceable
//   from a scene entity, so an effect can be authored in the editor and
//   saved into a .pvscene.
#pragma once

#include "pv/pv_handle.h"
#include "pv/pv_math.h"

#include <string>
#include <vector>

namespace pv {

// How new particles are positioned and aimed at spawn time.
enum class EmitterShape {
    Point,      // all from the origin
    Sphere,     // random point inside radius, velocity radially outward
    Hemisphere, // upper half only -- the usual choice for ground impacts
    Box,        // random point inside halfExtents
    Cone,       // classic thruster/flame: aimed along +Y within coneAngle
    Circle      // ring on the XZ plane, for shockwaves
};

enum class ParticleBlend {
    Alpha,    // standard transparency; needs depth sorting
    Additive, // fire, sparks, magic. Order-independent, so sorting is skipped
    Opaque
};

// One live particle. Kept to 64 bytes so two fit in a cache line -- the
// simulation loop is memory-bound, not ALU-bound, and this measurably
// matters once an emitter is in the thousands.
struct Particle {
    Vec3 position;
    Vec3 velocity;
    float age = 0.0f;
    float lifetime = 1.0f;
    float rotation = 0.0f;      // radians, around the view axis
    float angularVelocity = 0.0f;
    float sizeSeed = 0.0f;      // per-particle random in [0,1], drives size/color variance
    uint32_t frame = 0;         // sprite-sheet cell, for animated particle textures
};

// A curve over a particle's normalized lifetime. Four control points is
// enough for every common shape (grow-then-shrink, fade-in-fade-out) and
// evaluates in a couple of comparisons -- much cheaper than a general
// spline, and far easier to edit in the inspector.
struct LifetimeCurve {
    float p0 = 1.0f, p1 = 1.0f, p2 = 1.0f, p3 = 1.0f;

    float eval(float t) const {
        t = clampf(t, 0.0f, 1.0f);
        float seg = t * 3.0f;
        int i = static_cast<int>(seg);
        if (i > 2) i = 2;
        float f = seg - static_cast<float>(i);
        const float pts[4] = {p0, p1, p2, p3};
        return lerpf(pts[i], pts[i + 1], f);
    }
    static LifetimeCurve constant(float v) { return {v, v, v, v}; }
    static LifetimeCurve fadeOut() { return {1.0f, 1.0f, 0.6f, 0.0f}; }
};

struct EmitterRecord {
    std::string name = "emitter";

    // --- transform ---
    Vec3 position;
    Quat rotation = Quat::identity();

    // --- emission ---
    bool enabled = true;
    bool looping = true;
    float rate = 20.0f;        // particles per second
    float duration = 0.0f;     // 0 = run forever (when looping)
    float elapsed = 0.0f;
    // Double, not float. Accumulating `rate * dt` in float leaves a residual
    // that creeps toward 1.0 but never crosses it, so an emitter
    // systematically spawns one fewer particle than its rate specifies and
    // every particle is late by a fraction of its interval. Measured: a
    // 0.5/sec emitter produced 29 particles in 60 seconds instead of 30,
    // with the residual stuck at 0.999982.
    double spawnAccumulator = 0.0; // fractional particle carried between frames
    int burstCount = 0;            // pending one-shot spawn, drained next update
    int maxParticles = 1000;

    // --- shape ---
    EmitterShape shape = EmitterShape::Cone;
    float radius = 0.5f;
    Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    float coneAngle = 25.0f * PV_DEG2RAD;

    // --- initial state (min/max give per-particle variance) ---
    float speedMin = 1.0f, speedMax = 3.0f;
    float lifetimeMin = 1.0f, lifetimeMax = 2.0f;
    float sizeMin = 0.1f, sizeMax = 0.2f;
    float rotationSpeedMin = 0.0f, rotationSpeedMax = 0.0f;

    // --- forces ---
    Vec3 gravity{0, -9.8f, 0};
    float gravityScale = 1.0f;
    float drag = 0.0f;          // velocity *= exp(-drag * dt), so it's framerate-independent
    Vec3 windForce;
    // Spins particles around the emitter's local Y axis. Cheap swirl that
    // reads as smoke/vortex without a real fluid sim.
    float vortexStrength = 0.0f;

    // --- appearance over lifetime ---
    Vec4 colorStart{1, 1, 1, 1};
    Vec4 colorEnd{1, 1, 1, 0};
    LifetimeCurve sizeCurve = LifetimeCurve::constant(1.0f);
    LifetimeCurve alphaCurve = LifetimeCurve::fadeOut();
    ParticleBlend blend = ParticleBlend::Alpha;
    uint64_t texture = 0;       // 0 = the engine's 1x1 white texture
    int sheetCols = 1, sheetRows = 1; // sprite-sheet animation over lifetime

    // --- simulation space ---
    // World space (the default) leaves particles behind as the emitter
    // moves, which is what a trail or exhaust plume should do. Local space
    // drags them along with it, which is what a shield shimmer or an aura
    // should do. Getting this wrong is the single most common reason a
    // particle effect "looks wrong when the object moves".
    bool localSpace = false;

    // --- runtime state ---
    std::vector<Particle> particles;
    Rng rng{0x1234ABCDu};
    bool warnedOverflow = false;

    // Bounding box of live particles, refreshed each update. Used to skip
    // simulating and drawing emitters entirely outside the view frustum.
    Vec3 boundsMin, boundsMax;
};

// Advances one emitter: ages and kills expired particles, integrates
// forces, then spawns new ones for this frame's share of `rate`.
void updateEmitter(EmitterRecord& e, float dt);

// Spawns `count` particles immediately, ignoring `rate`. Used by
// Pv::BurstParticles and by the editor's "preview" button.
void burstEmitter(EmitterRecord& e, int count);

// Removes every live particle without touching emitter settings.
void clearEmitter(EmitterRecord& e);

// Fills a camera-facing quad per particle into the shared 2D/3D vertex
// stream and issues the draw. Implemented in particles.cpp; declared here
// so draw3d.cpp and the scene renderer can both trigger it.
void drawEmitter(EmitterRecord& e);

} // namespace pv
