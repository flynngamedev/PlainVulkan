// Minimal ImGui surface used only to compile-test gizmo.cpp off-GPU.
#pragma once
#include <cmath>
#include <cstdio>
struct ImVec2 { float x=0,y=0; ImVec2()=default; ImVec2(float a,float b):x(a),y(b){} };
struct ImVec4 { float x=0,y=0,z=0,w=0; ImVec4()=default; ImVec4(float a,float b,float c,float d):x(a),y(b),z(c),w(d){} };
typedef unsigned int ImU32;
#define IM_COL32(r,g,b,a) ((ImU32)((a)<<24|(b)<<16|(g)<<8|(r)))
struct ImDrawList {
  void AddLine(ImVec2,ImVec2,ImU32,float=1.0f){}
  void AddTriangleFilled(ImVec2,ImVec2,ImVec2,ImU32){}
  void AddRectFilled(ImVec2,ImVec2,ImU32,float=0.0f){}
  void AddCircle(ImVec2,float,ImU32,int=0,float=1.0f){}
  void AddCircleFilled(ImVec2,float,ImU32){}
  void AddText(ImVec2,ImU32,const char*){}
};
struct ImGuiIO { bool WantCaptureMouse=false; bool WantTextInput=false; };
namespace ImGui {
  inline ImDrawList* GetForegroundDrawList(){ static ImDrawList d; return &d; }
  inline ImGuiIO& GetIO(){ static ImGuiIO io; return io; }
  inline ImVec2 CalcTextSize(const char*){ return ImVec2(40,12); }
}
