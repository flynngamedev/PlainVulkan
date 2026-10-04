// ai.cpp -- navigation grid, Recast/Detour/RVO2 commands, and a small
// scriptable state machine + blackboard. Path queries and agent stepping
// live in ai_navmesh.cpp.
//
// No Vulkan, by design (see pv_ai.h): every interesting decision here --
// does the path route around the wall, is it the shortest one, does
// smoothing preserve validity, does an agent stop at its goal instead of
// orbiting it -- is testable against a known answer, and runtime_tests
// links this file directly to do exactly that.
#include "pv/pv_ai.h"
#include "pv/pv_runtime.h"
#include "pv/pv_terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pv {

HandleTable<NavGrid>& navGrids() {
    static HandleTable<NavGrid> table;
    return table;
}

HandleTable<Agent>& agents() {
    static HandleTable<Agent> table;
    return table;
}

namespace {

void aiError(const char* fn, const std::string& msg) {
    std::fprintf(stderr, "[PlainVulkan][ERROR] %s: %s\n", fn, msg.c_str());
}

NavGrid* lookupNav(const Value& handle) {
    if (!handle.isHandle()) return nullptr;
    return navGrids().get(handle.asHandle().id);
}

Agent* lookupAgent(const Value& handle) {
    if (!handle.isHandle()) return nullptr;
    return agents().get(handle.asHandle().id);
}

inline int cellIndex(const NavGrid& g, int col, int row) { return row * g.cols + col; }

} // namespace

// ---------------------------------------------------------------------
// Grid helpers
// ---------------------------------------------------------------------

bool navWorldToCell(const NavGrid& g, float worldX, float worldZ, int& outCol, int& outRow) {
    if (g.cols <= 0 || g.rows <= 0 || g.cellSize <= 0.0f) return false;
    float fx = (worldX - g.origin.x) / g.cellSize;
    float fz = (worldZ - g.origin.z) / g.cellSize;
    int c = static_cast<int>(std::floor(fx));
    int r = static_cast<int>(std::floor(fz));
    outCol = c;
    outRow = r;
    return c >= 0 && r >= 0 && c < g.cols && r < g.rows;
}

Vec3 navCellToWorld(const NavGrid& g, int col, int row) {
    // Cell centre, not corner: an agent standing "in" a cell should be in
    // the middle of it, and a path made of corners hugs walls.
    float x = g.origin.x + (static_cast<float>(col) + 0.5f) * g.cellSize;
    float z = g.origin.z + (static_cast<float>(row) + 0.5f) * g.cellSize;
    float y = g.origin.y;
    if (col >= 0 && row >= 0 && col < g.cols && row < g.rows && !g.height.empty()) {
        y = g.height[static_cast<size_t>(cellIndex(g, col, row))];
    }
    return Vec3(x, y, z);
}

bool navIsWalkable(const NavGrid& g, int col, int row) {
    if (col < 0 || row < 0 || col >= g.cols || row >= g.rows) return false;
    return g.walkable[static_cast<size_t>(cellIndex(g, col, row))] != 0;
}

void navApplyObstacles(NavGrid& g) {
    g.walkable = g.baseWalkable;
    for (const NavObstacle& o : g.obstacles) {
        float halfW = o.width * 0.5f;
        float halfD = o.depth * 0.5f;
        int c0 = static_cast<int>(std::floor((o.x - halfW - g.origin.x) / g.cellSize));
        int c1 = static_cast<int>(std::floor((o.x + halfW - g.origin.x) / g.cellSize));
        int r0 = static_cast<int>(std::floor((o.z - halfD - g.origin.z) / g.cellSize));
        int r1 = static_cast<int>(std::floor((o.z + halfD - g.origin.z) / g.cellSize));
        c0 = std::max(c0, 0);
        r0 = std::max(r0, 0);
        c1 = std::min(c1, g.cols - 1);
        r1 = std::min(r1, g.rows - 1);
        for (int r = r0; r <= r1; ++r) {
            for (int c = c0; c <= c1; ++c) g.walkable[static_cast<size_t>(cellIndex(g, c, r))] = 0;
        }
    }
    g.meshSerial++;
}

bool navLineOfSight(const NavGrid& g, int c0, int r0, int c1, int r1) {
    int dx = std::abs(c1 - c0), sx = c0 < c1 ? 1 : -1;
    int dy = -std::abs(r1 - r0), sy = r0 < r1 ? 1 : -1;
    int err = dx + dy;
    int c = c0, r = r0;
    for (;;) {
        if (!navIsWalkable(g, c, r)) return false;
        if (c == c1 && r == r1) return true;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            c += sx;
        }
        if (e2 <= dx) {
            err += dx;
            r += sy;
        }
    }
}

// Pathfinding and agent stepping live in ai_navmesh.cpp (Recast/Detour/RVO2).


// =====================================================================
// Pv:: commands
// =====================================================================
namespace rt {

// ---- Navigation grid ------------------------------------------------

Value CreateNavGrid(const Value& originX, const Value& originZ, const Value& cols, const Value& rows,
                    const Value& cellSize) {
    NavGrid g;
    g.cols = static_cast<int>(cols.asInt());
    g.rows = static_cast<int>(rows.asInt());
    g.cellSize = static_cast<float>(cellSize.asFloat());
    if (g.cols < 1 || g.rows < 1) {
        aiError("Pv::CreateNavGrid", "cols and rows must each be at least 1");
        return Value::MakeHandle(0, "navgrid");
    }
    if (static_cast<int64_t>(g.cols) * g.rows > 16u * 1024u * 1024u) {
        aiError("Pv::CreateNavGrid", "cols*rows exceeds 16M cells; use a coarser cellSize");
        return Value::MakeHandle(0, "navgrid");
    }
    if (!(g.cellSize > 0.0f)) {
        aiError("Pv::CreateNavGrid", "cellSize must be greater than zero");
        return Value::MakeHandle(0, "navgrid");
    }
    g.origin = Vec3(static_cast<float>(originX.asFloat()), 0.0f, static_cast<float>(originZ.asFloat()));
    size_t n = static_cast<size_t>(g.cols) * g.rows;
    g.baseWalkable.assign(n, 1);
    g.walkable.assign(n, 1);
    g.cost.assign(n, 1.0f);
    g.height.assign(n, 0.0f);
    return Value::MakeHandle(navGrids().add(std::move(g)), "navgrid");
}

Value CreateNavGridFromTerrain(const Value& terrain, const Value& maxSlopeDeg) {
    if (!terrain.isHandle()) {
        aiError("Pv::CreateNavGridFromTerrain", "expected a terrain handle");
        return Value::MakeHandle(0, "navgrid");
    }
    Terrain* t = terrains().get(terrain.asHandle().id);
    if (!t || t->cols < 2 || t->rows < 2) {
        aiError("Pv::CreateNavGridFromTerrain", "invalid or empty terrain handle");
        return Value::MakeHandle(0, "navgrid");
    }

    NavGrid g;
    // One nav cell per terrain cell (not per vertex), so a cell centre is
    // a real point on the surface rather than a grid corner.
    g.cols = t->cols - 1;
    g.rows = t->rows - 1;
    g.cellSize = t->cellSize;
    g.origin = t->origin;

    size_t n = static_cast<size_t>(g.cols) * g.rows;
    g.baseWalkable.assign(n, 1);
    g.cost.assign(n, 1.0f);
    g.height.assign(n, 0.0f);

    float maxSlope = static_cast<float>(maxSlopeDeg.asFloat());
    for (int r = 0; r < g.rows; ++r) {
        for (int c = 0; c < g.cols; ++c) {
            float wx = g.origin.x + (static_cast<float>(c) + 0.5f) * g.cellSize;
            float wz = g.origin.z + (static_cast<float>(r) + 0.5f) * g.cellSize;
            float slope = terrainSlopeDegAt(*t, wx, wz);
            size_t i = static_cast<size_t>(r) * g.cols + c;
            g.height[i] = terrainHeightAt(*t, wx, wz);
            g.baseWalkable[i] = (slope <= maxSlope) ? 1 : 0;
            // Steeper ground stays passable but costs more, so a path
            // prefers the gentle way round when one exists and still
            // finds the steep way when it doesn't.
            g.cost[i] = 1.0f + (slope / std::max(maxSlope, 1.0f)) * 2.0f;
        }
    }
    g.walkable = g.baseWalkable;
    return Value::MakeHandle(navGrids().add(std::move(g)), "navgrid");
}

Value DestroyNavGrid(const Value& nav) {
    if (!nav.isHandle()) return Value(false);
    navGrids().remove(nav.asHandle().id);
    return Value(true);
}

Value SetNavCellWalkable(const Value& nav, const Value& col, const Value& row, const Value& walkable) {
    NavGrid* g = lookupNav(nav);
    if (!g) return Value(false);
    int c = static_cast<int>(col.asInt());
    int r = static_cast<int>(row.asInt());
    if (c < 0 || r < 0 || c >= g->cols || r >= g->rows) {
        aiError("Pv::SetNavCellWalkable", "cell index out of range; the write was discarded");
        return Value(false);
    }
    uint8_t v = walkable.truthy() ? 1 : 0;
    g->baseWalkable[static_cast<size_t>(cellIndex(*g, c, r))] = v;
    navApplyObstacles(*g);
    return Value(true);
}

Value SetNavCellCost(const Value& nav, const Value& col, const Value& row, const Value& cost) {
    NavGrid* g = lookupNav(nav);
    if (!g) return Value(false);
    int c = static_cast<int>(col.asInt());
    int r = static_cast<int>(row.asInt());
    if (c < 0 || r < 0 || c >= g->cols || r >= g->rows) {
        aiError("Pv::SetNavCellCost", "cell index out of range; the write was discarded");
        return Value(false);
    }
    // Clamped at 1: A*'s heuristic assumes a minimum step cost of 1, and a
    // cheaper cell would make it inadmissible and the "shortest" path
    // quietly wrong.
    g->cost[static_cast<size_t>(cellIndex(*g, c, r))] = std::max(1.0f, static_cast<float>(cost.asFloat()));
    g->meshSerial++;
    return Value(true);
}

Value AddNavObstacle(const Value& nav, const Value& x, const Value& z, const Value& width, const Value& depth) {
    NavGrid* g = lookupNav(nav);
    if (!g) return Value(static_cast<int64_t>(-1));
    NavObstacle o;
    o.x = static_cast<float>(x.asFloat());
    o.z = static_cast<float>(z.asFloat());
    o.width = static_cast<float>(width.asFloat());
    o.depth = static_cast<float>(depth.asFloat());
    g->obstacles.push_back(o);
    navApplyObstacles(*g);
    return Value(static_cast<int64_t>(g->obstacles.size() - 1));
}

Value ClearNavObstacles(const Value& nav) {
    NavGrid* g = lookupNav(nav);
    if (!g) return Value(false);
    g->obstacles.clear();
    navApplyObstacles(*g);
    return Value(true);
}

Value IsNavPointWalkable(const Value& nav, const Value& x, const Value& z) {
    NavGrid* g = lookupNav(nav);
    if (!g) return Value(false);
    int c = 0, r = 0;
    if (!navWorldToCell(*g, static_cast<float>(x.asFloat()), static_cast<float>(z.asFloat()), c, r)) {
        return Value(false);
    }
    return Value(navIsWalkable(*g, c, r));
}

Value NavLineOfSight(const Value& nav, const Value& x1, const Value& z1, const Value& x2, const Value& z2) {
    NavGrid* g = lookupNav(nav);
    if (!g) return Value(false);
    int c0 = 0, r0 = 0, c1 = 0, r1 = 0;
    if (!navWorldToCell(*g, static_cast<float>(x1.asFloat()), static_cast<float>(z1.asFloat()), c0, r0)) {
        return Value(false);
    }
    if (!navWorldToCell(*g, static_cast<float>(x2.asFloat()), static_cast<float>(z2.asFloat()), c1, r1)) {
        return Value(false);
    }
    return Value(navLineOfSight(*g, c0, r0, c1, r1));
}

// Internal, and `static` deliberately: scripts/check_command_coverage.sh
// treats any `^Value Name(` in runtime/src as a Pv:: command that must
// have a pv_runtime.h declaration, which is exactly the check that catches
// a real command being added without one. Keeping helpers off column 0's
// `Value ` form keeps that check honest instead of growing its exception
// list.
static Value pathToValue(const std::vector<Vec3>& path) {
    Value out = Value::MakeArray();
    for (const Vec3& p : path) {
        Value pt;
        member_ref(pt, "x") = Value(static_cast<double>(p.x));
        member_ref(pt, "y") = Value(static_cast<double>(p.y));
        member_ref(pt, "z") = Value(static_cast<double>(p.z));
        out.arrayRef().push_back(pt);
    }
    return out;
}

Value FindPath(const Value& nav, const Value& startX, const Value& startZ, const Value& goalX, const Value& goalZ) {
    NavGrid* g = lookupNav(nav);
    if (!g) return Value::MakeArray();
    return pathToValue(navFindPath(*g, static_cast<float>(startX.asFloat()), static_cast<float>(startZ.asFloat()),
                                   static_cast<float>(goalX.asFloat()), static_cast<float>(goalZ.asFloat()), false));
}

Value FindPathSmoothed(const Value& nav, const Value& startX, const Value& startZ, const Value& goalX,
                       const Value& goalZ) {
    NavGrid* g = lookupNav(nav);
    if (!g) return Value::MakeArray();
    return pathToValue(navFindPath(*g, static_cast<float>(startX.asFloat()), static_cast<float>(startZ.asFloat()),
                                   static_cast<float>(goalX.asFloat()), static_cast<float>(goalZ.asFloat()), true));
}

Value GetNavGridStats(const Value& nav) {
    Value out;
    NavGrid* g = lookupNav(nav);
    if (!g) return out;
    int64_t walkableCount = 0;
    for (uint8_t w : g->walkable) walkableCount += (w != 0) ? 1 : 0;
    member_ref(out, "cols") = Value(static_cast<int64_t>(g->cols));
    member_ref(out, "rows") = Value(static_cast<int64_t>(g->rows));
    member_ref(out, "cellSize") = Value(static_cast<double>(g->cellSize));
    member_ref(out, "cells") = Value(static_cast<int64_t>(g->cols) * g->rows);
    member_ref(out, "walkableCells") = Value(walkableCount);
    member_ref(out, "obstacles") = Value(static_cast<int64_t>(g->obstacles.size()));
    return out;
}

// ---- Agents ---------------------------------------------------------

Value CreateAgent(const Value& nav, const Value& x, const Value& z) {
    NavGrid* g = lookupNav(nav);
    if (!g) {
        aiError("Pv::CreateAgent", "invalid navgrid handle");
        return Value::MakeHandle(0, "agent");
    }
    Agent a;
    a.nav = nav.asHandle().id;
    a.position = Vec3(static_cast<float>(x.asFloat()), g->origin.y, static_cast<float>(z.asFloat()));
    int c = 0, r = 0;
    if (!g->height.empty() && navWorldToCell(*g, a.position.x, a.position.z, c, r)) {
        a.position.y = g->height[static_cast<size_t>(cellIndex(*g, c, r))];
    }
    uint64_t id = agents().add(std::move(a));
    if (Agent* created = agents().get(id)) navRegisterRvo(*created);
    return Value::MakeHandle(id, "agent");
}

Value DestroyAgent(const Value& agent) {
    if (!agent.isHandle()) return Value(false);
    if (Agent* a = agents().get(agent.asHandle().id)) navUnregisterRvo(*a);
    agents().remove(agent.asHandle().id);
    return Value(true);
}

Value SetAgentSpeed(const Value& agent, const Value& speed) {
    Agent* a = lookupAgent(agent);
    if (!a) return Value(false);
    a->speed = std::max(0.0f, static_cast<float>(speed.asFloat()));
    return Value(true);
}

Value SetAgentTurnRate(const Value& agent, const Value& degreesPerSecond) {
    Agent* a = lookupAgent(agent);
    if (!a) return Value(false);
    a->turnRate = std::max(0.0f, static_cast<float>(degreesPerSecond.asFloat()));
    return Value(true);
}

Value SetAgentRadius(const Value& agent, const Value& radius) {
    Agent* a = lookupAgent(agent);
    if (!a) return Value(false);
    a->radius = std::max(0.0f, static_cast<float>(radius.asFloat()));
    return Value(true);
}

Value SetAgentPosition(const Value& agent, const Value& x, const Value& z) {
    Agent* a = lookupAgent(agent);
    if (!a) return Value(false);
    a->position.x = static_cast<float>(x.asFloat());
    a->position.z = static_cast<float>(z.asFloat());
    const NavGrid* g = navGrids().get(a->nav);
    int c = 0, r = 0;
    if (g && !g->height.empty() && navWorldToCell(*g, a->position.x, a->position.z, c, r)) {
        a->position.y = g->height[static_cast<size_t>(cellIndex(*g, c, r))];
    }
    // Teleporting invalidates any path that was computed from the old
    // position, so drop it rather than let the agent walk back to it.
    a->path.clear();
    a->pathIndex = 0;
    a->hasTarget = false;
    return Value(true);
}

Value SetAgentTarget(const Value& agent, const Value& x, const Value& z) {
    Agent* a = lookupAgent(agent);
    if (!a) return Value(false);
    NavGrid* g = navGrids().get(a->nav);
    if (!g) return Value(false);

    float tx = static_cast<float>(x.asFloat());
    float tz = static_cast<float>(z.asFloat());
    a->path = navFindPath(*g, a->position.x, a->position.z, tx, tz, true);
    a->pathIndex = 0;
    a->target = Vec3(tx, 0.0f, tz);
    a->arrived = false;
    a->hasTarget = !a->path.empty();
    if (a->path.empty()) return Value(false);

    // navFindPath starts at the agent's own cell centre; keeping it would
    // make the agent step backwards to that centre before setting off.
    if (a->path.size() > 1) a->pathIndex = 1;
    return Value(true);
}

Value StopAgent(const Value& agent) {
    Agent* a = lookupAgent(agent);
    if (!a) return Value(false);
    a->path.clear();
    a->pathIndex = 0;
    a->hasTarget = false;
    a->velocity = Vec3(0, 0, 0);
    return Value(true);
}

Value UpdateAgents(const Value& deltaTime) {
    navUpdateAgents(static_cast<float>(deltaTime.asFloat()));
    return Value();
}

Value GetAgentPosition(const Value& agent) {
    Agent* a = lookupAgent(agent);
    if (!a) return make_array({Value(0.0), Value(0.0), Value(0.0)});
    return make_array({Value(static_cast<double>(a->position.x)), Value(static_cast<double>(a->position.y)),
                       Value(static_cast<double>(a->position.z))});
}

Value GetAgentVelocity(const Value& agent) {
    Agent* a = lookupAgent(agent);
    if (!a) return make_array({Value(0.0), Value(0.0), Value(0.0)});
    return make_array({Value(static_cast<double>(a->velocity.x)), Value(static_cast<double>(a->velocity.y)),
                       Value(static_cast<double>(a->velocity.z))});
}

Value GetAgentHeading(const Value& agent) {
    Agent* a = lookupAgent(agent);
    if (!a) return Value(0.0);
    return Value(static_cast<double>(a->heading * 180.0f / PV_PI));
}

Value HasAgentArrived(const Value& agent) {
    Agent* a = lookupAgent(agent);
    if (!a) return Value(false);
    return Value(a->arrived);
}

Value GetAgentPath(const Value& agent) {
    Agent* a = lookupAgent(agent);
    if (!a) return Value::MakeArray();
    return pathToValue(a->path);
}

Value AgentCanSee(const Value& agent, const Value& x, const Value& z, const Value& fovDegrees,
                  const Value& maxDistance) {
    Agent* a = lookupAgent(agent);
    if (!a) return Value(false);
    NavGrid* g = navGrids().get(a->nav);
    if (!g) return Value(false);

    float tx = static_cast<float>(x.asFloat());
    float tz = static_cast<float>(z.asFloat());
    Vec3 to(tx - a->position.x, 0.0f, tz - a->position.z);
    float dist = to.length();
    if (dist > static_cast<float>(maxDistance.asFloat())) return Value(false);
    if (dist < 1e-6f) return Value(true);

    float angle = std::atan2(to.z, to.x) - a->heading;
    while (angle > PV_PI) angle -= 2.0f * PV_PI;
    while (angle < -PV_PI) angle += 2.0f * PV_PI;
    float halfFov = static_cast<float>(fovDegrees.asFloat()) * 0.5f * PV_PI / 180.0f;
    if (std::fabs(angle) > halfFov) return Value(false);

    int c0 = 0, r0 = 0, c1 = 0, r1 = 0;
    if (!navWorldToCell(*g, a->position.x, a->position.z, c0, r0)) return Value(false);
    if (!navWorldToCell(*g, tx, tz, c1, r1)) return Value(false);
    return Value(navLineOfSight(*g, c0, r0, c1, r1));
}

Value FindNearestAgent(const Value& x, const Value& z, const Value& maxDistance) {
    float px = static_cast<float>(x.asFloat());
    float pz = static_cast<float>(z.asFloat());
    float maxD = static_cast<float>(maxDistance.asFloat());
    uint64_t best = 0;
    float bestD = maxD;
    for (const auto& kv : agents()) {
        float dx = kv.second.position.x - px;
        float dz = kv.second.position.z - pz;
        float d = std::sqrt(dx * dx + dz * dz);
        if (d <= bestD) {
            bestD = d;
            best = kv.first;
        }
    }
    return Value::MakeHandle(best, "agent");
}

// ---- State machine / blackboard -------------------------------------

Value SetAgentState(const Value& agent, const Value& state) {
    Agent* a = lookupAgent(agent);
    if (!a) return Value(false);
    std::string next = state.asString();
    // Re-entering the same state deliberately does not reset stateTime:
    // scripts poll it to drive timeouts ("been chasing for 5s"), and a
    // per-frame SetAgentState("chase") would otherwise pin it at zero.
    if (next != a->state) {
        a->state = next;
        a->stateTime = 0.0f;
    }
    return Value(true);
}

Value GetAgentState(const Value& agent) {
    Agent* a = lookupAgent(agent);
    if (!a) return Value(std::string());
    return Value(a->state);
}

Value GetAgentStateTime(const Value& agent) {
    Agent* a = lookupAgent(agent);
    if (!a) return Value(0.0);
    return Value(static_cast<double>(a->stateTime));
}

Value SetAgentBlackboard(const Value& agent, const Value& key, const Value& value) {
    Agent* a = lookupAgent(agent);
    if (!a) return Value(false);
    a->blackboard[key.asString()] = value;
    return Value(true);
}

Value GetAgentBlackboard(const Value& agent, const Value& key) {
    Agent* a = lookupAgent(agent);
    if (!a) return Value();
    auto it = a->blackboard.find(key.asString());
    return it == a->blackboard.end() ? Value() : it->second;
}

Value GetAgentCount() { return Value(static_cast<int64_t>(agents().size())); }

} // namespace rt
} // namespace pv
