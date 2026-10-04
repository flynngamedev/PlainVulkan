// pv_ai.h -- navigation and agents: a walkable grid, Recast/Detour
// pathfinding, RVO2 local avoidance, and a small scriptable state
// machine with a per-agent blackboard.
//
// Like pv_terrain.h this is deliberately Vulkan-free, and for the same
// reason: pathfinding is exactly the kind of code that is worth testing
// against known answers (does the path go around the wall, does an agent
// stop at its goal), and none of that should need a GPU. ai.cpp implements
// all of it and every Pv:: AI command; there is no second, GPU-side file.
//
// Walkability is still a heightmap grid because that is how this engine's
// worlds are authored. Recast builds a polygon navmesh from those cells,
// Detour queries it, and RVO2 steers agents around each other while they
// follow the Detour corridor.
#pragma once

#include "pv/pv_handle.h"
#include "pv/pv_math.h"
#include "pv/pv_value.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pv {

struct NavMeshCache;

struct NavObstacle {
    float x = 0, z = 0, width = 0, depth = 0;
};

struct NavGrid {
    int cols = 0, rows = 0;
    float cellSize = 1.0f;
    Vec3 origin; // world position of cell (0,0)'s corner

    std::vector<uint8_t> walkable; // cols*rows, 1 = passable
    std::vector<float> cost;       // cols*rows, >=1, multiplies traversal cost
    std::vector<float> height;     // cols*rows, world Y of the cell centre

    // Kept so ClearNavObstacles and RebuildNavGrid can restore walkability
    // without re-deriving it from the terrain, which the grid may not have
    // come from at all.
    std::vector<uint8_t> baseWalkable;
    std::vector<NavObstacle> obstacles;

    // Bumped whenever walkability or costs change so the Recast/Detour mesh
    // is rebuilt on the next query instead of serving a stale tile.
    int meshSerial = 0;
    mutable std::unique_ptr<NavMeshCache> mesh;

    NavGrid();
    ~NavGrid();
    NavGrid(NavGrid&&) noexcept;
    NavGrid& operator=(NavGrid&&) noexcept;
    NavGrid(const NavGrid&) = delete;
    NavGrid& operator=(const NavGrid&) = delete;
};

struct Agent {
    uint64_t nav = 0;
    Vec3 position;
    Vec3 velocity;
    float heading = 0.0f; // radians, 0 = +X, increasing toward +Z
    float radius = 0.5f;
    float speed = 3.0f;
    float turnRate = 360.0f; // degrees per second
    float arriveRadius = 0.35f;

    std::vector<Vec3> path;
    size_t pathIndex = 0;
    bool hasTarget = false;
    bool arrived = false;
    Vec3 target;

    std::string state = "idle";
    float stateTime = 0.0f;
    std::map<std::string, Value> blackboard;

    // RVO2 agent index, or SIZE_MAX when the Agent was constructed in tests
    // without going through Pv::CreateAgent.
    size_t rvoIndex = static_cast<size_t>(-1);
};

HandleTable<NavGrid>& navGrids();
HandleTable<Agent>& agents();

bool navWorldToCell(const NavGrid& g, float worldX, float worldZ, int& outCol, int& outRow);
Vec3 navCellToWorld(const NavGrid& g, int col, int row);
bool navIsWalkable(const NavGrid& g, int col, int row);
void navApplyObstacles(NavGrid& g);
bool navLineOfSight(const NavGrid& g, int c0, int r0, int c1, int r1);
std::vector<Vec3> navFindPath(const NavGrid& g, float startX, float startZ, float goalX, float goalZ, bool smooth);
void navUpdateAgents(float dt);
void navRegisterRvo(Agent& a);
void navUnregisterRvo(Agent& a);

} // namespace pv
