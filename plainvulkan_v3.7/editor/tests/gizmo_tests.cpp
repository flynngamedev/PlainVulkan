// gizmo_tests.cpp -- tests for viewport ray picking.
//
// The gizmo's picking maths is the part most likely to be silently wrong:
// a sign error in the NDC conversion produces a gizmo that looks perfect
// but grabs the wrong object, or one that works at screen centre and
// drifts toward the edges. The key test here is the project-then-pick
// round trip, which catches any disagreement between worldToScreen and
// screenRay about screen-space conventions.
//
// Build (from the repo root):
//   c++ -std=c++17 -Iruntime/include -Ieditor/tests/stub -Ieditor/src \
//       -o gizmo_tests editor/tests/gizmo_tests.cpp editor/src/gizmo.cpp \
//       runtime/src/pv_math.cpp
//
// The stub/ directory holds a ~25-line ImGui surface so this links without
// fetching ImGui or touching a GPU.
#include "gizmo.h"
#include <cstdio>
#include <cmath>
using namespace pv;
static int fails=0, checks=0;
#define CHECK(c,m) do{checks++; if(!(c)){printf("  FAIL: %s (line %d)\n",m,__LINE__);fails++;}}while(0)
static bool nf(float a,float b,float e=1e-3f){return std::fabs(a-b)<e;}

// Scene fns live in scene.cpp (needs Vulkan), so provide the two the gizmo
// uses. Only pickEntity/screenRay are under test here.
namespace pv {
  void markWorldDirty(Scene&, uint64_t){}
  Scene& activeScene(){ static Scene s; return s; }
}

int main(){
  printf("gizmo picking\n");
  const float W=800,H=600;
  Vec3 eye(0,0,10);
  Mat4 view = Mat4::lookAt(eye, Vec3(0,0,0), Vec3(0,1,0));
  Mat4 proj = Mat4::perspective(1.0f, W/H, 0.1f, 100.0f);

  // A ray through the screen centre must aim at the look-at target.
  auto center = gizmo::screenRay(view, proj, W/2, H/2, W, H);
  CHECK(nf(center.direction.x,0,1e-2f)&&nf(center.direction.y,0,1e-2f),
        "centre ray has no lateral component");
  CHECK(center.direction.z < -0.9f, "centre ray points down -Z toward the target");
  CHECK(nf(center.direction.length(),1.0f), "ray direction is normalized");

  // Rays must diverge in the correct screen direction. In Vulkan NDC (and
  // ImGui pixels) +Y is DOWN, so a click below centre aims downward (-Y).
  auto right = gizmo::screenRay(view, proj, W*0.9f, H/2, W, H);
  CHECK(right.direction.x > 0.05f, "a click right of centre aims +X");
  auto below = gizmo::screenRay(view, proj, W/2, H*0.9f, W, H);
  CHECK(below.direction.y < -0.05f, "a click below centre aims -Y (screen Y is down)");

  // Round trip: pick an entity placed off-axis and confirm the ray through
  // its projected pixel actually selects it.
  Scene s;
  auto place=[&](const char* n, Vec3 p){
    uint64_t id = s.entities.add(Entity{});
    Entity* e = s.entities.get(id);
    e->id=id; e->name=n; e->local.position=p; e->add(COMP_MESH);
    e->world = Mat4::fromTRS(p, Quat::identity(), Vec3(1,1,1));
    s.roots.push_back(id);
    return id;
  };
  uint64_t near_ = place("near", Vec3(0,0,0));
  uint64_t far_  = place("far",  Vec3(0,0,-6));
  uint64_t side  = place("side", Vec3(3,0,0));

  CHECK(gizmo::pickEntity(s, center)==near_, "the nearest entity along the ray wins");

  // Project 'side' to a pixel, then pick through that pixel.
  Mat4 vp = proj*view;
  Vec4 clip = vp*Vec4(Vec3(3,0,0),1.0f);
  float px=(clip.x/clip.w*0.5f+0.5f)*W, py=(clip.y/clip.w*0.5f+0.5f)*H;
  auto sideRay = gizmo::screenRay(view, proj, px, py, W, H);
  CHECK(gizmo::pickEntity(s, sideRay)==side, "project-then-pick round-trips to the same entity");

  // Empty space selects nothing.
  auto miss = gizmo::screenRay(view, proj, 5, 5, W, H);
  CHECK(gizmo::pickEntity(s, miss)==0, "clicking empty space picks nothing");

  // Inactive and non-visual entities are not selectable.
  if (Entity* e=s.entities.get(near_)) e->active=false;
  CHECK(gizmo::pickEntity(s, center)==far_, "an inactive entity is skipped");
  if (Entity* e=s.entities.get(near_)) e->active=true;

  // A scaled-up entity must have a correspondingly larger grab radius.
  if (Entity* e=s.entities.get(side)) {
    e->local.scale=Vec3(4,4,4);
    e->world=Mat4::fromTRS(Vec3(3,0,0),Quat::identity(),Vec3(4,4,4));
  }
  Vec4 c2 = vp*Vec4(Vec3(3.0f+2.0f,0,0),1.0f); // offset well outside unit radius
  auto edgeRay = gizmo::screenRay(view, proj, (c2.x/c2.w*0.5f+0.5f)*W, (c2.y/c2.w*0.5f+0.5f)*H, W, H);
  CHECK(gizmo::pickEntity(s, edgeRay)==side, "a scaled entity has a larger pick radius");

  // Orthographic cameras: all rays parallel, origins differ.
  Mat4 ortho = Mat4::orthographic(-8,8,-6,6,0.1f,100.0f);
  auto o1 = gizmo::screenRay(view, ortho, W*0.25f, H/2, W, H);
  auto o2 = gizmo::screenRay(view, ortho, W*0.75f, H/2, W, H);
  CHECK(nf(o1.direction.x,o2.direction.x,1e-3f)&&nf(o1.direction.y,o2.direction.y,1e-3f),
        "orthographic rays are parallel");
  CHECK(std::fabs(o1.origin.x-o2.origin.x)>1.0f, "orthographic ray origins differ across the screen");

  printf("\n%d checks, %d failure(s)\n%s\n", checks, fails, fails?"FAIL":"PASS");
  return fails?1:0;
}
