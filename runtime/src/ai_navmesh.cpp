// ai_navmesh.cpp -- Recast/Detour navmesh build + query, and RVO2 stepping.
// Kept in its own translation unit so ai.cpp can stay the Pv:: command
// surface while the third-party libraries stay isolated.
#include "pv/pv_ai.h"
#include "pv/pv_math.h"
#include "pv/pv_platform.h"

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "DetourStatus.h"
#include "Recast.h"
#include "RVO.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace pv {

struct NavMeshCache {
    dtNavMesh* mesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    int serial = -1;

    ~NavMeshCache() {
        if (query) {
            dtFreeNavMeshQuery(query);
            query = nullptr;
        }
        if (mesh) {
            dtFreeNavMesh(mesh);
            mesh = nullptr;
        }
    }
};

NavGrid::NavGrid() = default;
NavGrid::~NavGrid() = default;
NavGrid::NavGrid(NavGrid&&) noexcept = default;
NavGrid& NavGrid::operator=(NavGrid&&) noexcept = default;

namespace {

RVO::RVOSimulator& rvoSim() {
    static RVO::RVOSimulator sim;
    return sim;
}

int cellIndex(const NavGrid& g, int col, int row) { return row * g.cols + col; }

bool rebuildNavMesh(const NavGrid& g) {
    g.mesh.reset();
    auto cache = std::make_unique<NavMeshCache>();

    std::vector<float> verts;
    std::vector<int> tris;
    std::vector<unsigned char> triAreas;
    verts.reserve(static_cast<size_t>(g.cols) * g.rows * 12);
    tris.reserve(static_cast<size_t>(g.cols) * g.rows * 6);
    triAreas.reserve(static_cast<size_t>(g.cols) * g.rows * 2);

    for (int r = 0; r < g.rows; ++r) {
        for (int c = 0; c < g.cols; ++c) {
            float x0 = g.origin.x + static_cast<float>(c) * g.cellSize;
            float z0 = g.origin.z + static_cast<float>(r) * g.cellSize;
            float x1 = x0 + g.cellSize;
            float z1 = z0 + g.cellSize;
            float y = g.origin.y;
            if (!g.height.empty()) y = g.height[static_cast<size_t>(cellIndex(g, c, r))];
            const bool walk = navIsWalkable(g, c, r);
            if (!walk) y -= 1.0f;
            // Inset walkable floors so conservative rasterization cannot
            // paint walkable spans into neighbouring blocked cells.
            if (walk) {
                float inset = g.cellSize * 0.08f;
                x0 += inset;
                z0 += inset;
                x1 -= inset;
                z1 -= inset;
            }

            int base = static_cast<int>(verts.size() / 3);
            auto push = [&](float x, float yy, float z) {
                verts.push_back(x);
                verts.push_back(yy);
                verts.push_back(z);
            };
            push(x0, y, z0);
            push(x1, y, z0);
            push(x1, y, z1);
            push(x0, y, z1);
            tris.push_back(base + 0);
            tris.push_back(base + 2);
            tris.push_back(base + 1);
            tris.push_back(base + 0);
            tris.push_back(base + 3);
            tris.push_back(base + 2);
            unsigned char area = walk ? RC_WALKABLE_AREA : 0;
            triAreas.push_back(area);
            triAreas.push_back(area);
        }
    }
    if (tris.empty()) {
        g.mesh = std::move(cache);
        g.mesh->serial = g.meshSerial;
        return false;
    }

    const int nverts = static_cast<int>(verts.size() / 3);
    const int ntris = static_cast<int>(tris.size() / 3);

    rcContext ctx(false);
    rcConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.cs = std::max(0.1f, g.cellSize);
    cfg.ch = 0.2f;
    cfg.walkableSlopeAngle = 60.0f;
    cfg.walkableHeight = 10; // >= 3 voxels
    cfg.walkableClimb = 1;
    cfg.walkableRadius = 0; // keep 1-cell corridors
    cfg.maxEdgeLen = static_cast<int>(g.cellSize * 4.0f / cfg.cs);
    cfg.maxSimplificationError = 1.2f;
    cfg.minRegionArea = 1;
    cfg.mergeRegionArea = 2;
    cfg.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
    cfg.detailSampleDist = cfg.cs * 2.0f;
    cfg.detailSampleMaxError = cfg.ch * 2.0f;

    rcCalcBounds(verts.data(), nverts, cfg.bmin, cfg.bmax);
    cfg.bmin[1] -= 1.0f;
    cfg.bmax[1] += 3.0f;
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
    if (cfg.width < 1 || cfg.height < 1) return false;

    rcHeightfield* solid = rcAllocHeightfield();
    if (!solid || !rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
        rcFreeHeightField(solid);
        return false;
    }

    if (!rcRasterizeTriangles(&ctx, verts.data(), nverts, tris.data(), triAreas.data(), ntris, *solid, cfg.walkableClimb)) {
        rcFreeHeightField(solid);
        return false;
    }

    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    if (!chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf)) {
        rcFreeCompactHeightfield(chf);
        rcFreeHeightField(solid);
        return false;
    }
    rcFreeHeightField(solid);

    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf)) {
        rcFreeCompactHeightfield(chf);
        return false;
    }
    if (!rcBuildRegionsMonotone(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) {
        rcFreeCompactHeightfield(chf);
        return false;
    }

    rcContourSet* cset = rcAllocContourSet();
    if (!cset || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset)) {
        rcFreeContourSet(cset);
        rcFreeCompactHeightfield(chf);
        return false;
    }

    rcPolyMesh* pmesh = rcAllocPolyMesh();
    if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh)) {
        rcFreePolyMesh(pmesh);
        rcFreeContourSet(cset);
        rcFreeCompactHeightfield(chf);
        return false;
    }

    rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
    if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh)) {
        rcFreePolyMeshDetail(dmesh);
        rcFreePolyMesh(pmesh);
        rcFreeContourSet(cset);
        rcFreeCompactHeightfield(chf);
        return false;
    }
    rcFreeCompactHeightfield(chf);
    rcFreeContourSet(cset);

    for (int i = 0; i < pmesh->npolys; ++i) {
        if (pmesh->areas[i] == RC_WALKABLE_AREA) pmesh->areas[i] = 1;
        pmesh->flags[i] = 1;
    }

    dtNavMeshCreateParams params;
    std::memset(&params, 0, sizeof(params));
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    params.polyFlags = pmesh->flags;
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = dmesh->meshes;
    params.detailVerts = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris = dmesh->tris;
    params.detailTriCount = dmesh->ntris;
    params.walkableHeight = 2.0f;
    params.walkableRadius = 0.1f;
    params.walkableClimb = 0.9f;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;

    unsigned char* navData = nullptr;
    int navDataSize = 0;
    if (!dtCreateNavMeshData(&params, &navData, &navDataSize)) {
        rcFreePolyMeshDetail(dmesh);
        rcFreePolyMesh(pmesh);
        return false;
    }
    rcFreePolyMeshDetail(dmesh);
    rcFreePolyMesh(pmesh);

    cache->mesh = dtAllocNavMesh();
    if (!cache->mesh || dtStatusFailed(cache->mesh->init(navData, navDataSize, DT_TILE_FREE_DATA))) {
        dtFree(navData);
        return false;
    }
    cache->query = dtAllocNavMeshQuery();
    if (!cache->query || dtStatusFailed(cache->query->init(cache->mesh, 2048))) {
        return false;
    }
    cache->serial = g.meshSerial;
    g.mesh = std::move(cache);
    return true;
}

const NavMeshCache* ensureNavMesh(const NavGrid& g) {
    if (!g.mesh || g.mesh->serial != g.meshSerial) rebuildNavMesh(g);
    return g.mesh.get();
}

bool gridReachable(const NavGrid& g, int sc, int sr, int gc, int gr) {
    if (!navIsWalkable(g, sc, sr) || !navIsWalkable(g, gc, gr)) return false;
    if (sc == gc && sr == gr) return true;
    const int n = g.cols * g.rows;
    std::vector<uint8_t> seen(static_cast<size_t>(n), 0);
    std::vector<int> q;
    q.push_back(sr * g.cols + sc);
    seen[static_cast<size_t>(q.back())] = 1;
    static const int dc[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int dr[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    for (size_t qi = 0; qi < q.size(); ++qi) {
        int i = q[qi];
        int c = i % g.cols, r = i / g.cols;
        if (c == gc && r == gr) return true;
        for (int k = 0; k < 8; ++k) {
            int nc = c + dc[k], nr = r + dr[k];
            if (!navIsWalkable(g, nc, nr)) continue;
            if (dc[k] != 0 && dr[k] != 0) {
                if (!navIsWalkable(g, c + dc[k], r) || !navIsWalkable(g, c, r + dr[k])) continue;
            }
            int ni = nr * g.cols + nc;
            if (seen[static_cast<size_t>(ni)]) continue;
            seen[static_cast<size_t>(ni)] = 1;
            q.push_back(ni);
        }
    }
    return false;
}

bool pathStaysWalkable(const NavGrid& g, const std::vector<Vec3>& path) {
    if (path.size() < 2) return !path.empty();
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        for (int s = 1; s < 8; ++s) {
            float t = static_cast<float>(s) / 8.0f;
            float x = path[i].x * (1.0f - t) + path[i + 1].x * t;
            float z = path[i].z * (1.0f - t) + path[i + 1].z * t;
            int c = 0, r = 0;
            if (!navWorldToCell(g, x, z, c, r) || !navIsWalkable(g, c, r)) return false;
        }
    }
    return true;
}

std::vector<Vec3> gridSearchPath(const NavGrid& g, int sc, int sr, int gc, int gr, bool smooth) {
    std::vector<Vec3> out;
    const int n = g.cols * g.rows;
    std::vector<int> came(static_cast<size_t>(n), -1);
    std::vector<uint8_t> seen(static_cast<size_t>(n), 0);
    std::vector<int> q;
    int start = sr * g.cols + sc;
    int goal = gr * g.cols + gc;
    q.push_back(start);
    seen[static_cast<size_t>(start)] = 1;
    static const int dc[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int dr[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    bool found = false;
    for (size_t qi = 0; qi < q.size(); ++qi) {
        int i = q[qi];
        if (i == goal) {
            found = true;
            break;
        }
        int c = i % g.cols, r = i / g.cols;
        for (int k = 0; k < 8; ++k) {
            int nc = c + dc[k], nr = r + dr[k];
            if (!navIsWalkable(g, nc, nr)) continue;
            if (dc[k] != 0 && dr[k] != 0) {
                if (!navIsWalkable(g, c + dc[k], r) || !navIsWalkable(g, c, r + dr[k])) continue;
            }
            int ni = nr * g.cols + nc;
            if (seen[static_cast<size_t>(ni)]) continue;
            seen[static_cast<size_t>(ni)] = 1;
            came[static_cast<size_t>(ni)] = i;
            q.push_back(ni);
        }
    }
    if (!found) return out;
    std::vector<int> cells;
    for (int i = goal; i != -1; i = came[static_cast<size_t>(i)]) cells.push_back(i);
    std::reverse(cells.begin(), cells.end());
    if (smooth && cells.size() > 2) {
        std::vector<int> pulled;
        pulled.push_back(cells.front());
        size_t anchor = 0;
        for (size_t i = 1; i + 1 < cells.size(); ++i) {
            int ac = cells[anchor] % g.cols, ar = cells[anchor] / g.cols;
            int nc = cells[i + 1] % g.cols, nr = cells[i + 1] / g.cols;
            if (!navLineOfSight(g, ac, ar, nc, nr)) {
                pulled.push_back(cells[i]);
                anchor = i;
            }
        }
        pulled.push_back(cells.back());
        cells.swap(pulled);
    }
    out.reserve(cells.size());
    for (int idx : cells) out.push_back(navCellToWorld(g, idx % g.cols, idx / g.cols));
    return out;
}

} // namespace

std::vector<Vec3> navFindPath(const NavGrid& g, float startX, float startZ, float goalX, float goalZ, bool smooth) {
    std::vector<Vec3> out;
    int sc = 0, sr = 0, gc = 0, gr = 0;
    if (!navWorldToCell(g, startX, startZ, sc, sr)) return out;
    if (!navWorldToCell(g, goalX, goalZ, gc, gr)) return out;
    if (!navIsWalkable(g, sc, sr) || !navIsWalkable(g, gc, gr)) return out;
    if (!gridReachable(g, sc, sr, gc, gr)) return out;

    const NavMeshCache* cache = ensureNavMesh(g);
    if (!cache || !cache->query || !cache->mesh) return gridSearchPath(g, sc, sr, gc, gr, smooth);

    Vec3 start = navCellToWorld(g, sc, sr);
    Vec3 goal = navCellToWorld(g, gc, gr);
    start.x = startX;
    start.z = startZ;
    goal.x = goalX;
    goal.z = goalZ;

    const float half[3] = {g.cellSize * 0.45f, 2.0f, g.cellSize * 0.45f};
    float spos[3] = {start.x, start.y, start.z};
    float epos[3] = {goal.x, goal.y, goal.z};
    dtQueryFilter filter;
    dtPolyRef startRef = 0, endRef = 0;
    cache->query->findNearestPoly(spos, half, &filter, &startRef, spos);
    cache->query->findNearestPoly(epos, half, &filter, &endRef, epos);
    if (!startRef || !endRef) return gridSearchPath(g, sc, sr, gc, gr, smooth);

    dtPolyRef polys[256];
    int npolys = 0;
    if (dtStatusFailed(cache->query->findPath(startRef, endRef, spos, epos, &filter, polys, &npolys, 256)) || npolys <= 0) {
        return gridSearchPath(g, sc, sr, gc, gr, smooth);
    }

    float straight[256 * 3];
    int nstraight = 0;
    unsigned int options = smooth ? 0u : static_cast<unsigned int>(DT_STRAIGHTPATH_ALL_CROSSINGS);
    if (dtStatusFailed(cache->query->findStraightPath(spos, epos, polys, npolys, straight, nullptr, nullptr, &nstraight, 256,
                                                      options)) ||
        nstraight <= 0) {
        return gridSearchPath(g, sc, sr, gc, gr, smooth);
    }

    out.reserve(static_cast<size_t>(nstraight));
    for (int i = 0; i < nstraight; ++i) {
        out.emplace_back(straight[i * 3 + 0], straight[i * 3 + 1], straight[i * 3 + 2]);
    }
    if (out.empty() || !pathStaysWalkable(g, out)) out = gridSearchPath(g, sc, sr, gc, gr, smooth);
    if (std::abs(sc - gc) == 1 && std::abs(sr - gr) == 1) {
        if (!navIsWalkable(g, sc, gr) || !navIsWalkable(g, gc, sr)) {
            out = gridSearchPath(g, sc, sr, gc, gr, smooth);
        }
    }
    return out;
}

void navUpdateAgents(float dt) {
    if (!(dt > 0.0f)) return;

    bool anyRvo = false;
    for (uint64_t id : agents().ids()) {
        Agent* a = agents().get(id);
        if (!a) continue;
        a->stateTime += dt;

        Vec3 pref(0, 0, 0);
        if (a->hasTarget && !a->path.empty() && a->pathIndex < a->path.size()) {
            Vec3 wp = a->path[a->pathIndex];
            Vec3 toWp(wp.x - a->position.x, 0.0f, wp.z - a->position.z);
            float dist = toWp.length();
            float accept = std::max(a->radius, a->speed * dt);
            if (dist <= accept) {
                ++a->pathIndex;
                if (a->pathIndex >= a->path.size()) {
                    a->position.x = wp.x;
                    a->position.z = wp.z;
                    a->position.y = wp.y;
                    a->velocity = Vec3(0, 0, 0);
                    a->arrived = true;
                    a->hasTarget = false;
                    if (a->rvoIndex != static_cast<size_t>(-1) && a->rvoIndex != RVO::RVO_ERROR) {
                        rvoSim().setAgentPrefVelocity(a->rvoIndex, RVO::Vector2(0, 0));
                        rvoSim().setAgentMaxSpeed(a->rvoIndex, 0.0f);
                    }
                    continue;
                }
                wp = a->path[a->pathIndex];
                toWp = Vec3(wp.x - a->position.x, 0.0f, wp.z - a->position.z);
                dist = toWp.length();
            }
            if (dist > 1e-6f) pref = (toWp / dist) * a->speed;
        }

        if (a->rvoIndex != static_cast<size_t>(-1) && a->rvoIndex != RVO::RVO_ERROR) {
            anyRvo = true;
            rvoSim().setAgentPosition(a->rvoIndex, RVO::Vector2(a->position.x, a->position.z));
            rvoSim().setAgentRadius(a->rvoIndex, std::max(0.05f, a->radius));
            rvoSim().setAgentMaxSpeed(a->rvoIndex, a->speed);
            rvoSim().setAgentPrefVelocity(a->rvoIndex, RVO::Vector2(pref.x, pref.z));
        } else {
            a->velocity = pref;
        }
    }

    if (anyRvo) {
        rvoSim().setTimeStep(dt);
        rvoSim().doStep();
        for (uint64_t id : agents().ids()) {
            Agent* a = agents().get(id);
            if (!a || a->rvoIndex == static_cast<size_t>(-1) || a->rvoIndex == RVO::RVO_ERROR) continue;
            if (!a->hasTarget) continue;
            RVO::Vector2 v = rvoSim().getAgentVelocity(a->rvoIndex);
            a->velocity = Vec3(v.x(), 0.0f, v.y());
        }
    }

    for (uint64_t id : agents().ids()) {
        Agent* a = agents().get(id);
        if (!a) continue;
        if (!a->hasTarget || a->path.empty() || a->pathIndex >= a->path.size()) continue;

        Vec3 desired = a->velocity;
        float dlen = desired.length();
        if (dlen < 1e-6f) {
            Vec3 wp = a->path[a->pathIndex];
            desired = Vec3(wp.x - a->position.x, 0.0f, wp.z - a->position.z);
            dlen = desired.length();
        }
        if (dlen < 1e-6f) continue;
        desired = desired / dlen;

        float desiredHeading = std::atan2(desired.z, desired.x);
        float delta = desiredHeading - a->heading;
        while (delta > PV_PI) delta -= 2.0f * PV_PI;
        while (delta < -PV_PI) delta += 2.0f * PV_PI;
        float maxTurn = a->turnRate * PV_PI / 180.0f * dt;
        a->heading += clampf(delta, -maxTurn, maxTurn);

        Vec3 wp = a->path[a->pathIndex];
        Vec3 toWp(wp.x - a->position.x, 0.0f, wp.z - a->position.z);
        float dist = toWp.length();
        Vec3 forward(std::cos(a->heading), 0.0f, std::sin(a->heading));
        float travel = std::min(a->speed * dt, dist);
        a->position += forward * travel;
        a->velocity = forward * (travel / dt);

        const NavGrid* grid = navGrids().get(a->nav);
        if (grid && !grid->height.empty()) {
            int c = 0, r = 0;
            if (navWorldToCell(*grid, a->position.x, a->position.z, c, r)) {
                a->position.y = grid->height[static_cast<size_t>(r * grid->cols + c)];
            }
        }
    }
}

void navRegisterRvo(Agent& a) {
    RVO::RVOSimulator& sim = rvoSim();
    size_t id = sim.addAgent(RVO::Vector2(a.position.x, a.position.z), 15.0f, 10, 10.0f, 5.0f, std::max(0.05f, a.radius),
                             a.speed);
    a.rvoIndex = id;
}

void navUnregisterRvo(Agent& a) {
    if (a.rvoIndex == static_cast<size_t>(-1) || a.rvoIndex == RVO::RVO_ERROR) return;
    rvoSim().setAgentPosition(a.rvoIndex, RVO::Vector2(1.0e6f, 1.0e6f));
    rvoSim().setAgentMaxSpeed(a.rvoIndex, 0.0f);
    rvoSim().setAgentRadius(a.rvoIndex, 0.01f);
    rvoSim().setAgentPrefVelocity(a.rvoIndex, RVO::Vector2(0, 0));
    a.rvoIndex = static_cast<size_t>(-1);
}

} // namespace pv
