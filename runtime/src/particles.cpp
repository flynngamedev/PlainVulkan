// particles.cpp -- emitter simulation and batched rendering, plus the
// `Pv::` particle commands.
//
// The simulation half (updateEmitter/burstEmitter/clearEmitter) has no
// Vulkan dependency and is unit-tested in tests/particle_tests.cpp. The
// rendering half builds camera-facing quads into a per-frame ring buffer
// and issues one draw per emitter.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pv {

// ======================================================================
// Simulation (no GPU involvement)
// ======================================================================

namespace {

// Picks a spawn position and direction for the emitter's shape, in the
// emitter's local space. The caller rotates/translates into world space.
void sampleShape(EmitterRecord& e, Vec3& outPos, Vec3& outDir) {
    switch (e.shape) {
        case EmitterShape::Point:
            outPos = Vec3(0, 0, 0);
            outDir = e.rng.onUnitSphere();
            break;
        case EmitterShape::Sphere: {
            Vec3 p = e.rng.insideUnitSphere();
            outPos = p * e.radius;
            // Radially outward, falling back to a random direction when the
            // sample landed exactly on the origin.
            outDir = p.lengthSq() > 1e-6f ? p.normalized() : e.rng.onUnitSphere();
            break;
        }
        case EmitterShape::Hemisphere: {
            Vec3 p = e.rng.insideUnitSphere();
            p.y = std::fabs(p.y);
            outPos = p * e.radius;
            outDir = p.lengthSq() > 1e-6f ? p.normalized() : Vec3(0, 1, 0);
            break;
        }
        case EmitterShape::Box:
            outPos = Vec3(e.rng.range(-e.halfExtents.x, e.halfExtents.x),
                          e.rng.range(-e.halfExtents.y, e.halfExtents.y),
                          e.rng.range(-e.halfExtents.z, e.halfExtents.z));
            outDir = Vec3(0, 1, 0);
            break;
        case EmitterShape::Cone: {
            // Uniform over the spherical cap around +Y. Sampling cos(theta)
            // uniformly (rather than theta) is what makes the distribution
            // even across the cap instead of bunched at the axis.
            float cosMax = std::cos(clampf(e.coneAngle, 0.0f, PV_PI));
            float cosTheta = e.rng.range(cosMax, 1.0f);
            float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
            float phi = e.rng.range(0.0f, PV_TAU);
            outDir = Vec3(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
            outPos = Vec3(outDir.x, 0.0f, outDir.z) * (e.radius * e.rng.unit());
            break;
        }
        case EmitterShape::Circle: {
            float a = e.rng.range(0.0f, PV_TAU);
            Vec3 onRing(std::cos(a), 0.0f, std::sin(a));
            outPos = onRing * e.radius;
            outDir = onRing;
            break;
        }
    }
}

void spawnOne(EmitterRecord& e) {
    if (static_cast<int>(e.particles.size()) >= e.maxParticles) {
        if (!e.warnedOverflow) {
            e.warnedOverflow = true;
            logLine("WARNING", "Particle emitter '" + e.name + "' hit its maxParticles cap (" +
                                   std::to_string(e.maxParticles) +
                                   "); raise it with Pv::SetEmitterMaxParticles or lower the rate");
        }
        return;
    }

    Vec3 localPos, localDir;
    sampleShape(e, localPos, localDir);

    Particle p;
    // World-space emitters bake the emitter transform in at spawn time and
    // then ignore it, which is what leaves a trail behind a moving object.
    // Local-space emitters store local coordinates and get transformed at
    // draw time, so they follow the object instead.
    if (e.localSpace) {
        p.position = localPos;
        p.velocity = localDir * e.rng.range(e.speedMin, e.speedMax);
    } else {
        p.position = e.position + e.rotation.rotate(localPos);
        p.velocity = e.rotation.rotate(localDir) * e.rng.range(e.speedMin, e.speedMax);
    }

    p.age = 0.0f;
    p.lifetime = std::max(0.01f, e.rng.range(e.lifetimeMin, e.lifetimeMax));
    p.rotation = e.rng.range(0.0f, PV_TAU);
    p.angularVelocity = e.rng.range(e.rotationSpeedMin, e.rotationSpeedMax);
    p.sizeSeed = e.rng.unit();
    p.frame = 0;
    e.particles.push_back(p);
}

} // namespace

void clearEmitter(EmitterRecord& e) {
    e.particles.clear();
    e.spawnAccumulator = 0.0f;
    e.warnedOverflow = false;
}

void burstEmitter(EmitterRecord& e, int count) {
    // Deferred to the next update rather than spawned here, so a burst
    // triggered from script mid-frame is integrated with the same dt as
    // everything else instead of sitting frozen for one frame.
    e.burstCount += std::max(0, count);
}

void updateEmitter(EmitterRecord& e, float dt) {
    if (dt <= 0.0f) return;
    // A long stall (window drag, a breakpoint, the editor recompiling) would
    // otherwise deliver a multi-second dt that teleports every particle and
    // spawns thousands at once. Clamping to ~3 frames at 20fps keeps the
    // effect continuous through a hitch.
    dt = std::min(dt, 0.15f);

    e.elapsed += dt;

    // --- age, integrate, and compact in one pass ---
    const Vec3 g = e.gravity * e.gravityScale;
    const float dragFactor = e.drag > 0.0f ? std::exp(-e.drag * dt) : 1.0f;
    const Vec3 vortexAxis = e.localSpace ? Vec3(0, 1, 0) : e.rotation.rotate(Vec3(0, 1, 0));
    const Vec3 vortexCenter = e.localSpace ? Vec3(0, 0, 0) : e.position;

    size_t write = 0;
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);

    for (size_t read = 0; read < e.particles.size(); read++) {
        Particle p = e.particles[read];
        p.age += dt;
        if (p.age >= p.lifetime) continue; // dead: simply not copied down

        p.velocity += g * dt;
        p.velocity += e.windForce * dt;

        if (e.vortexStrength != 0.0f) {
            // Tangential force around the emitter axis. cross(axis, radial)
            // gives the tangent direction; scaling by 1/(r+eps) keeps
            // particles near the axis from acquiring absurd speed.
            Vec3 radial = p.position - vortexCenter;
            radial -= vortexAxis * dot(radial, vortexAxis); // project onto the plane
            float r = radial.length();
            if (r > 1e-4f) {
                Vec3 tangent = cross(vortexAxis, radial / r);
                p.velocity += tangent * (e.vortexStrength * dt / (r + 0.5f));
            }
        }

        p.velocity *= dragFactor;
        p.position += p.velocity * dt;
        p.rotation += p.angularVelocity * dt;

        const int cells = std::max(1, e.sheetCols * e.sheetRows);
        if (cells > 1) {
            float t = p.age / p.lifetime;
            int cell = static_cast<int>(t * static_cast<float>(cells));
            p.frame = static_cast<uint32_t>(std::min(cells - 1, std::max(0, cell)));
        }

        lo = minv(lo, p.position);
        hi = maxv(hi, p.position);
        e.particles[write++] = p;
    }
    e.particles.resize(write);

    if (write == 0) {
        e.boundsMin = e.boundsMax = e.localSpace ? Vec3(0, 0, 0) : e.position;
    } else {
        e.boundsMin = lo;
        e.boundsMax = hi;
    }

    // --- spawning ---
    int toSpawn = e.burstCount;
    e.burstCount = 0;

    bool emitting = e.enabled;
    if (emitting && e.duration > 0.0f && !e.looping && e.elapsed > e.duration) emitting = false;

    if (emitting && e.rate > 0.0f) {
        // The fractional accumulator is what makes a rate of 0.5/sec work at
        // all: without it, int(rate * dt) truncates to zero every frame and
        // the emitter silently never spawns anything.
        e.spawnAccumulator += static_cast<double>(e.rate) * static_cast<double>(dt);
        int whole = static_cast<int>(e.spawnAccumulator);
        e.spawnAccumulator -= static_cast<double>(whole);
        toSpawn += whole;
    }

    // Cap per-frame spawns so a huge rate can't stall a frame outright.
    toSpawn = std::min(toSpawn, e.maxParticles);
    for (int i = 0; i < toSpawn; i++) spawnOne(e);
}

// ======================================================================
// Rendering
// ======================================================================

namespace {

struct ParticleVertex {
    float pos[3];
    float color[4];
    float uv[2];
};

// One quad's worth of scratch, reused across draws so a frame with many
// emitters doesn't repeatedly grow and free a vector.
std::vector<ParticleVertex>& vertexScratch() {
    static std::vector<ParticleVertex> s;
    return s;
}

struct SortKey {
    float depth;
    uint32_t index;
};

} // namespace

void drawEmitter(EmitterRecord& e) {
    auto& eng = engine();
    if (!eng.frameActive || e.particles.empty()) return;
    if (eng.pipelineParticleAlpha == VK_NULL_HANDLE) return; // runtime built before pipelines came up

    Mat4 view = buildViewMatrix();
    Mat4 proj = buildProjectionMatrix();
    Mat4 viewProj = proj * view;

    // Camera basis from the view matrix's rows, exactly as DrawBillboard
    // does -- these are the world-space right/up axes of the camera, which
    // is what makes every quad face the viewer.
    Vec3 camRight(view.m[0][0], view.m[1][0], view.m[2][0]);
    Vec3 camUp(view.m[0][1], view.m[1][1], view.m[2][1]);
    Vec3 camFwd(view.m[0][2], view.m[1][2], view.m[2][2]);

    // For local-space emitters every particle is transformed by the emitter
    // pose at draw time.
    Mat4 emitterModel = e.localSpace ? Mat4::fromTRS(e.position, e.rotation, Vec3(1, 1, 1))
                                     : Mat4::identity();

    const size_t count = e.particles.size();

    // Alpha-blended particles must be drawn back-to-front or the blending
    // composites in the wrong order and produces visible hard edges where
    // quads overlap. Additive blending is order-independent (addition is
    // commutative), so that sort is skipped entirely -- which is a real
    // reason to prefer additive for large sparks/fire effects.
    static std::vector<SortKey> order;
    order.clear();
    order.reserve(count);
    for (size_t i = 0; i < count; i++) {
        Vec3 wp = e.localSpace ? emitterModel.transformPoint(e.particles[i].position) : e.particles[i].position;
        // Distance along the camera's forward axis, not Euclidean distance:
        // it's the actual depth the rasterizer will compare, and it's a dot
        // product instead of a square root.
        order.push_back(SortKey{dot(wp, camFwd), static_cast<uint32_t>(i)});
    }
    if (e.blend == ParticleBlend::Alpha) {
        std::sort(order.begin(), order.end(),
                  [](const SortKey& a, const SortKey& b) { return a.depth < b.depth; });
    }

    auto& verts = vertexScratch();
    verts.clear();
    verts.reserve(count * 6);

    const float invCols = 1.0f / static_cast<float>(std::max(1, e.sheetCols));
    const float invRows = 1.0f / static_cast<float>(std::max(1, e.sheetRows));

    for (const SortKey& key : order) {
        const Particle& p = e.particles[key.index];
        float t = clampf(p.age / p.lifetime, 0.0f, 1.0f);

        float baseSize = lerpf(e.sizeMin, e.sizeMax, p.sizeSeed);
        float size = baseSize * e.sizeCurve.eval(t);
        if (size <= 1e-5f) continue;

        Vec4 color = lerp(e.colorStart, e.colorEnd, t);
        color.w *= e.alphaCurve.eval(t);
        if (color.w <= 0.002f && e.blend == ParticleBlend::Alpha) continue;

        Vec3 center = e.localSpace ? emitterModel.transformPoint(p.position) : p.position;

        // Rotate the billboard basis around the view axis by the particle's
        // own spin. Doing it here (rather than with a per-particle model
        // matrix) keeps everything in one vertex stream and one draw call.
        float c = std::cos(p.rotation), s = std::sin(p.rotation);
        Vec3 right = (camRight * c + camUp * s) * (size * 0.5f);
        Vec3 up = (camUp * c - camRight * s) * (size * 0.5f);

        // Sprite-sheet cell UVs.
        uint32_t cell = p.frame;
        uint32_t cx = e.sheetCols > 0 ? (cell % static_cast<uint32_t>(e.sheetCols)) : 0;
        uint32_t cy = e.sheetCols > 0 ? (cell / static_cast<uint32_t>(e.sheetCols)) : 0;
        float u0 = static_cast<float>(cx) * invCols;
        float v0 = static_cast<float>(cy) * invRows;
        float u1 = u0 + invCols;
        float v1 = v0 + invRows;

        Vec3 corners[4] = {
            center - right - up, // bottom-left
            center + right - up, // bottom-right
            center + right + up, // top-right
            center - right + up  // top-left
        };
        float uvs[4][2] = {{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}};

        auto push = [&](int i) {
            ParticleVertex v;
            v.pos[0] = corners[i].x;
            v.pos[1] = corners[i].y;
            v.pos[2] = corners[i].z;
            v.color[0] = color.x;
            v.color[1] = color.y;
            v.color[2] = color.z;
            v.color[3] = color.w;
            v.uv[0] = uvs[i][0];
            v.uv[1] = uvs[i][1];
            verts.push_back(v);
        };
        // Two triangles; no index buffer, because a dynamic per-frame index
        // buffer would cost more to maintain than the 2 duplicated vertices.
        push(0); push(1); push(2);
        push(0); push(2); push(3);
    }

    if (verts.empty()) return;
    drawParticleVerts(verts.data(), static_cast<uint32_t>(verts.size()), viewProj,
                      e.texture, e.blend == ParticleBlend::Additive);
}

// ======================================================================
// Pv:: commands
// ======================================================================
namespace rt {

namespace {

EmitterRecord* getEmitter(const Value& v) {
    return engine().emitters.get(static_cast<uint64_t>(v.asInt()));
}

EmitterShape parseShape(const std::string& s) {
    if (s == "point") return EmitterShape::Point;
    if (s == "sphere") return EmitterShape::Sphere;
    if (s == "hemisphere") return EmitterShape::Hemisphere;
    if (s == "box") return EmitterShape::Box;
    if (s == "circle") return EmitterShape::Circle;
    if (s == "cone") return EmitterShape::Cone;
    logLine("WARNING", "particle emitter shape '" + s +
                           "' not recognized (expected point/sphere/hemisphere/box/cone/circle) -- using cone");
    return EmitterShape::Cone;
}

} // namespace

Value CreateEmitter(const Value& x, const Value& y, const Value& z) {
    EmitterRecord e;
    e.position = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                      static_cast<float>(z.asFloat()));
    // Seed from the handle counter so two emitters created at the same
    // position don't produce identical particle streams, while a given
    // emitter still replays deterministically.
    e.rng = Rng(static_cast<uint32_t>(engine().emitters.size() * 2654435761u + 12345u));
    e.particles.reserve(64);
    return Value::MakeHandle(engine().emitters.add(std::move(e)), "emitter");
}

Value DestroyEmitter(const Value& emitter) {
    engine().emitters.remove(static_cast<uint64_t>(emitter.asInt()));
    return Value();
}

Value SetEmitterPosition(const Value& emitter, const Value& x, const Value& y, const Value& z) {
    if (auto* e = getEmitter(emitter)) {
        e->position = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                           static_cast<float>(z.asFloat()));
    }
    return Value();
}

Value SetEmitterRotation(const Value& emitter, const Value& pitch, const Value& yaw, const Value& roll) {
    if (auto* e = getEmitter(emitter)) {
        e->rotation = Quat::fromEuler(static_cast<float>(pitch.asFloat()), static_cast<float>(yaw.asFloat()),
                                      static_cast<float>(roll.asFloat()));
    }
    return Value();
}

Value SetEmitterShape(const Value& emitter, const Value& shape, const Value& radius, const Value& angleDegrees) {
    if (auto* e = getEmitter(emitter)) {
        e->shape = parseShape(shape.asString());
        e->radius = static_cast<float>(radius.asFloat());
        e->coneAngle = static_cast<float>(angleDegrees.asFloat()) * PV_DEG2RAD;
        e->halfExtents = Vec3(e->radius, e->radius, e->radius);
    }
    return Value();
}

Value SetEmitterRate(const Value& emitter, const Value& perSecond) {
    if (auto* e = getEmitter(emitter)) e->rate = std::max(0.0f, static_cast<float>(perSecond.asFloat()));
    return Value();
}

Value SetEmitterLifetime(const Value& emitter, const Value& minSeconds, const Value& maxSeconds) {
    if (auto* e = getEmitter(emitter)) {
        e->lifetimeMin = static_cast<float>(minSeconds.asFloat());
        e->lifetimeMax = std::max(e->lifetimeMin, static_cast<float>(maxSeconds.asFloat()));
    }
    return Value();
}

Value SetEmitterSpeed(const Value& emitter, const Value& minSpeed, const Value& maxSpeed) {
    if (auto* e = getEmitter(emitter)) {
        e->speedMin = static_cast<float>(minSpeed.asFloat());
        e->speedMax = std::max(e->speedMin, static_cast<float>(maxSpeed.asFloat()));
    }
    return Value();
}

Value SetEmitterSize(const Value& emitter, const Value& minSize, const Value& maxSize) {
    if (auto* e = getEmitter(emitter)) {
        e->sizeMin = static_cast<float>(minSize.asFloat());
        e->sizeMax = std::max(e->sizeMin, static_cast<float>(maxSize.asFloat()));
    }
    return Value();
}

Value SetEmitterColor(const Value& emitter, const Value& startHex, const Value& endHex) {
    auto unpack = [](int64_t hex) {
        // 0xRRGGBBAA, matching Pv::Clear's existing color convention.
        return Vec4(static_cast<float>((hex >> 24) & 0xFF) / 255.0f,
                    static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
                    static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
                    static_cast<float>(hex & 0xFF) / 255.0f);
    };
    if (auto* e = getEmitter(emitter)) {
        e->colorStart = unpack(startHex.asInt());
        e->colorEnd = unpack(endHex.asInt());
    }
    return Value();
}

Value SetEmitterGravity(const Value& emitter, const Value& x, const Value& y, const Value& z) {
    if (auto* e = getEmitter(emitter)) {
        e->gravity = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                          static_cast<float>(z.asFloat()));
        e->gravityScale = 1.0f;
    }
    return Value();
}

Value SetEmitterDrag(const Value& emitter, const Value& drag) {
    if (auto* e = getEmitter(emitter)) e->drag = std::max(0.0f, static_cast<float>(drag.asFloat()));
    return Value();
}

Value SetEmitterTexture(const Value& emitter, const Value& texture) {
    if (auto* e = getEmitter(emitter)) e->texture = static_cast<uint64_t>(texture.asInt());
    return Value();
}

Value SetEmitterBlend(const Value& emitter, const Value& mode) {
    if (auto* e = getEmitter(emitter)) {
        std::string m = mode.asString();
        if (m == "additive") e->blend = ParticleBlend::Additive;
        else if (m == "opaque") e->blend = ParticleBlend::Opaque;
        else e->blend = ParticleBlend::Alpha;
    }
    return Value();
}

Value SetEmitterMaxParticles(const Value& emitter, const Value& maxCount) {
    if (auto* e = getEmitter(emitter)) {
        e->maxParticles = std::max(1, static_cast<int>(maxCount.asInt()));
        e->warnedOverflow = false;
        if (static_cast<int>(e->particles.size()) > e->maxParticles) {
            e->particles.resize(static_cast<size_t>(e->maxParticles));
        }
    }
    return Value();
}

Value SetEmitterLocalSpace(const Value& emitter, const Value& enabled) {
    if (auto* e = getEmitter(emitter)) e->localSpace = enabled.truthy();
    return Value();
}

Value SetEmitterEnabled(const Value& emitter, const Value& enabled) {
    if (auto* e = getEmitter(emitter)) {
        e->enabled = enabled.truthy();
        if (e->enabled) e->elapsed = 0.0f;
    }
    return Value();
}

Value BurstParticles(const Value& emitter, const Value& count) {
    if (auto* e = getEmitter(emitter)) burstEmitter(*e, static_cast<int>(count.asInt()));
    return Value();
}

Value ClearParticles(const Value& emitter) {
    if (auto* e = getEmitter(emitter)) clearEmitter(*e);
    return Value();
}

Value GetParticleCount(const Value& emitter) {
    auto* e = getEmitter(emitter);
    return Value(static_cast<int64_t>(e ? e->particles.size() : 0));
}

Value UpdateParticles(const Value& deltaTime) {
    float dt = static_cast<float>(deltaTime.asFloat());
    for (auto& kv : engine().emitters) updateEmitter(kv.second, dt);
    return Value();
}

Value DrawEmitter(const Value& emitter) {
    if (auto* e = getEmitter(emitter)) drawEmitter(*e);
    return Value();
}

Value DrawParticles() {
    for (auto& kv : engine().emitters) drawEmitter(kv.second);
    return Value();
}

} // namespace rt
} // namespace pv
