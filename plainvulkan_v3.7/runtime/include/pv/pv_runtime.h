// pv_runtime.h -- every `Pv::Command(...)` in the language reference,
// declared here as `pv::rt::Command(...)`. The C++ code generator maps
// `Pv::Foo(args)` to `pv::rt::Foo(args)` mechanically (see compiler's
// codegen.rs): `::` is a generic scope operator at the *grammar* level, so
// nothing about parsing depends on this list, but codegen and the linker
// obviously need every name the reference promises to actually exist.
//
// Every function takes/returns `pv::Value` uniformly (even ones that are
// conceptually "void", like `Pv::Log`) so code generation never has to
// special-case "is this call used as an expression or a statement" --
// unused return values are simply discarded, same as in any dynamically
// typed language.
//
// See runtime/README.md for exactly which of these are: fully implemented,
// implemented at a deliberately simplified level, or stubbed with a
// LogWarning("not yet implemented") body. Nothing here is silently
// missing -- everything declared has *some* body, even if simple.
#pragma once

#include "pv/pv_value.h"

// Generated game code includes this header but not pv_platform.h, and it may
// already have seen <windows.h> transitively. Undo the Win32 macros
// (CreateWindow, DrawText, PlaySound, DeleteFile, ...) that would otherwise
// mangle the declarations -- and the call sites -- below.
#include "pv/pv_win32_undef.h"

namespace pv {
namespace rt {

// ---- Core / Window ----------------------------------------------------
Value Init();
Value Shutdown();
Value CreateWindow(const Value& width, const Value& height, const Value& title);
Value CloseWindow();
Value SetWindowTitle(const Value& title);
Value SetWindowSize(const Value& width, const Value& height);
Value IsWindowOpen();
Value SetVSync(const Value& enabled);
Value SetFullscreen(const Value& enabled);

// ---- Frame / Time -------------------------------------------------------
Value BeginFrame();
Value EndFrame();
Value Clear(const Value& colorHex);
Value SetClearColor(const Value& r, const Value& g, const Value& b, const Value& a);
Value GetDeltaTime();
Value GetFPS();

// ---- Camera -------------------------------------------------------------
Value SetCamera(const Value& x, const Value& y, const Value& z);
Value LookAt(const Value& x, const Value& y, const Value& z);
Value SetFOV(const Value& degrees);
Value SetNearFar(const Value& near, const Value& far);
Value SetOrthographic(const Value& enabled);
Value MoveCamera(const Value& x, const Value& y, const Value& z);
Value RotateCamera(const Value& pitch, const Value& yaw, const Value& roll);

// ---- 2D Drawing -----------------------------------------------------------
Value DrawTriangle(const Value& x, const Value& y, const Value& color);
Value DrawRect(const Value& x, const Value& y, const Value& w, const Value& h, const Value& color);
Value DrawCircle(const Value& x, const Value& y, const Value& radius, const Value& color);
Value DrawLine(const Value& x1, const Value& y1, const Value& x2, const Value& y2, const Value& color);
Value DrawSprite(const Value& texture, const Value& x, const Value& y);
Value DrawSpriteEx(const Value& texture, const Value& x, const Value& y, const Value& w, const Value& h);
Value DrawText(const Value& text, const Value& x, const Value& y, const Value& color);
Value DrawTextEx(const Value& text, const Value& x, const Value& y, const Value& font, const Value& size, const Value& color);

// ---- 3D Drawing -----------------------------------------------------------
Value DrawMesh(const Value& mesh, const Value& x, const Value& y, const Value& z);
Value DrawMeshEx(const Value& mesh, const Value& x, const Value& y, const Value& z,
                  const Value& rx, const Value& ry, const Value& rz, const Value& scale);
Value DrawSkybox(const Value& cubemap);
Value DrawTerrain(const Value& heightmap, const Value& scale);
Value DrawBillboard(const Value& texture, const Value& x, const Value& y, const Value& z, const Value& w, const Value& h);
Value SetWireframe(const Value& enabled);

// ---- Mesh management --------------------------------------------------
Value LoadMesh(const Value& filepath);
Value UnloadMesh(const Value& mesh);
Value CreateMesh(const Value& vertices, const Value& indices);
Value UpdateMesh(const Value& mesh, const Value& vertices);
Value CreateCube(const Value& size);
Value CreateSphere(const Value& radius, const Value& slices);
Value CreatePlane(const Value& width, const Value& height);
Value CreateCylinder(const Value& radius, const Value& height);

// ---- Textures -------------------------------------------------------------
Value LoadTexture(const Value& filepath);
Value UnloadTexture(const Value& texture);
Value CreateTexture(const Value& width, const Value& height, const Value& format);
Value SetTextureFilter(const Value& texture, const Value& filter);
Value SetTextureWrap(const Value& texture, const Value& wrap);
Value LoadCubemap(const Value& filepath);
Value CreateRenderTexture(const Value& width, const Value& height);

// ---- Shaders ------------------------------------------------------------
Value LoadShader(const Value& vertPath, const Value& fragPath);
Value UnloadShader(const Value& shader);
Value SetShader(const Value& shader);
Value ResetShader();
Value SetUniformInt(const Value& shader, const Value& name, const Value& value);
Value SetUniformFloat(const Value& shader, const Value& name, const Value& value);
Value SetUniformVec2(const Value& shader, const Value& name, const Value& x, const Value& y);
Value SetUniformVec3(const Value& shader, const Value& name, const Value& x, const Value& y, const Value& z);
Value SetUniformVec4(const Value& shader, const Value& name, const Value& x, const Value& y, const Value& z, const Value& w);
Value SetUniformMat4(const Value& shader, const Value& name, const Value& matrix);
Value SetUniformTexture(const Value& shader, const Value& name, const Value& texture, const Value& slot);

// ---- Lighting -----------------------------------------------------------
Value SetAmbient(const Value& r, const Value& g, const Value& b, const Value& intensity);
Value AddPointLight(const Value& x, const Value& y, const Value& z, const Value& r, const Value& g,
                     const Value& b, const Value& intensity, const Value& radius);
Value AddDirectionalLight(const Value& dx, const Value& dy, const Value& dz, const Value& r,
                           const Value& g, const Value& b, const Value& intensity);
Value AddSpotLight(const Value& x, const Value& y, const Value& z, const Value& dx, const Value& dy,
                    const Value& dz, const Value& angle, const Value& intensity);
Value RemoveLight(const Value& light);
Value SetShadows(const Value& enabled);
Value SetShadowResolution(const Value& resolution);
Value SetFog(const Value& r, const Value& g, const Value& b, const Value& near, const Value& far);

// ---- Materials ------------------------------------------------------------
Value CreateMaterial();
Value SetMaterialAlbedo(const Value& material, const Value& texture);
Value SetMaterialNormal(const Value& material, const Value& texture);
Value SetMaterialRoughness(const Value& material, const Value& value);
Value SetMaterialMetallic(const Value& material, const Value& value);
Value SetMaterialEmissive(const Value& material, const Value& texture, const Value& intensity);
Value SetMaterialColor(const Value& material, const Value& r, const Value& g, const Value& b, const Value& a);

Value SetMaterialHeight(const Value& material, const Value& texture, const Value& scale);

// ---- Audio --------------------------------------------------------------
Value LoadSound(const Value& filepath);
Value UnloadSound(const Value& sound);
Value PlaySound(const Value& sound);
Value StopSound(const Value& sound);
Value PauseSound(const Value& sound);
Value SetSoundVolume(const Value& sound, const Value& volume);
Value SetSoundPitch(const Value& sound, const Value& pitch);
Value LoadMusic(const Value& filepath);
Value PlayMusic(const Value& music);
Value StopMusic(const Value& music);
Value SetMusicVolume(const Value& music, const Value& volume);
Value SetSound3D(const Value& sound, const Value& x, const Value& y, const Value& z);

// ---- Input: keyboard --------------------------------------------------
Value IsKeyDown(const Value& key);
Value IsKeyUp(const Value& key);
Value IsKeyPressed(const Value& key);
Value IsKeyReleased(const Value& key);
Value GetKeyPressed();

// ---- Input: mouse -------------------------------------------------------
Value GetMouseX();
Value GetMouseY();
Value GetMouseDeltaX();
Value GetMouseDeltaY();
Value IsMouseDown(const Value& button);
Value IsMousePressed(const Value& button);
Value IsMouseReleased(const Value& button);
Value GetMouseScroll();
Value SetMouseVisible(const Value& visible);
Value SetMouseLocked(const Value& locked);
Value SetMousePosition(const Value& x, const Value& y);

// ---- Input: gamepad -------------------------------------------------------
Value IsGamepadConnected(const Value& id);
Value IsGamepadButtonDown(const Value& id, const Value& button);
Value IsGamepadButtonPressed(const Value& id, const Value& button);
Value GetGamepadAxis(const Value& id, const Value& axis);
Value SetGamepadVibration(const Value& id, const Value& left, const Value& right);

// ---- Physics --------------------------------------------------------------
Value InitPhysics();
Value SetGravity(const Value& x, const Value& y, const Value& z);
Value StepPhysics(const Value& deltaTime);
Value CreateBoxCollider(const Value& x, const Value& y, const Value& z, const Value& w, const Value& h, const Value& d);
Value CreateSphereCollider(const Value& x, const Value& y, const Value& z, const Value& radius);
Value CreateCapsuleCollider(const Value& x, const Value& y, const Value& z, const Value& radius, const Value& height);
Value CreateRigidBody(const Value& collider, const Value& mass);
Value SetBodyVelocity(const Value& body, const Value& x, const Value& y, const Value& z);
Value GetBodyVelocity(const Value& body);
Value AddForce(const Value& body, const Value& x, const Value& y, const Value& z);
Value AddImpulse(const Value& body, const Value& x, const Value& y, const Value& z);
Value Raycast(const Value& ox, const Value& oy, const Value& oz, const Value& dx, const Value& dy, const Value& dz, const Value& maxDist);

// ---- File system ------------------------------------------------------
Value FileExists(const Value& filepath);
Value ReadFile(const Value& filepath);
Value WriteFile(const Value& filepath, const Value& data);
Value DeleteFile(const Value& filepath);
Value GetWorkingDir();
Value SetWorkingDir(const Value& path);

// ---- UI -------------------------------------------------------------------
Value BeginUI();
Value EndUI();
Value UIButton(const Value& label, const Value& x, const Value& y, const Value& w, const Value& h);
Value UIText(const Value& text, const Value& x, const Value& y, const Value& size, const Value& color);
Value UIImage(const Value& texture, const Value& x, const Value& y, const Value& w, const Value& h);
Value UISlider(const Value& label, const Value& x, const Value& y, const Value& min, const Value& max, const Value& value);
Value UICheckbox(const Value& label, const Value& x, const Value& y, const Value& checked);
Value UIInputText(const Value& label, const Value& x, const Value& y, const Value& buffer);
Value UIProgressBar(const Value& x, const Value& y, const Value& w, const Value& h, const Value& value);

// ---- Debug ----------------------------------------------------------------
Value DrawDebugLine(const Value& x1, const Value& y1, const Value& z1, const Value& x2, const Value& y2, const Value& z2, const Value& color);
Value DrawDebugBox(const Value& x, const Value& y, const Value& z, const Value& w, const Value& h, const Value& d, const Value& color);
Value DrawDebugSphere(const Value& x, const Value& y, const Value& z, const Value& radius, const Value& color);
Value Log(const Value& message);
Value LogWarning(const Value& message);
Value LogError(const Value& message);
Value Assert(const Value& condition, const Value& message);

// ---- Animation ------------------------------------------------------------
Value LoadAnimation(const Value& filepath);
Value UnloadAnimation(const Value& anim);
Value PlayAnimation(const Value& anim, const Value& clipName);
Value PauseAnimation(const Value& anim);
Value StopAnimation(const Value& anim);
Value SetAnimationSpeed(const Value& anim, const Value& speed);
Value SetAnimationLoop(const Value& anim, const Value& loop_);
Value IsAnimationPlaying(const Value& anim);
Value GetAnimationTime(const Value& anim);
Value SetAnimationTime(const Value& anim, const Value& time);
Value GetAnimationDuration(const Value& anim);
Value BlendAnimation(const Value& anim, const Value& clipA, const Value& weight);
Value UpdateAnimations(const Value& deltaTime);


// ---- Physics: new with the PhysX backend ---------------------------------
// The commands above are unchanged in meaning; these expose capabilities the
// old hand-written solver simply didn't have (rotation, torque, materials,
// triggers, contact reporting). See runtime/README.md.
Value GetBodyPosition(const Value& body);
Value SetBodyPosition(const Value& body, const Value& x, const Value& y, const Value& z);
Value GetBodyRotation(const Value& body);
Value SetBodyRotation(const Value& body, const Value& pitch, const Value& yaw, const Value& roll);
Value AddTorque(const Value& body, const Value& x, const Value& y, const Value& z);
Value SetBodyAngularVelocity(const Value& body, const Value& x, const Value& y, const Value& z);
Value GetBodyAngularVelocity(const Value& body);
Value SetBodyKinematic(const Value& body, const Value& enabled);
Value SetBodyDamping(const Value& body, const Value& linear, const Value& angular);
Value SetBodyGravityEnabled(const Value& body, const Value& enabled);
Value SetBodyFreezePosition(const Value& body, const Value& fx, const Value& fy, const Value& fz);
Value SetBodyFreezeRotation(const Value& body, const Value& fx, const Value& fy, const Value& fz);
Value SetColliderMaterial(const Value& collider, const Value& staticFriction, const Value& dynamicFriction,
                          const Value& restitution);
Value SetColliderTrigger(const Value& collider, const Value& enabled);
Value DestroyBody(const Value& body);
Value SetPhysicsTimestep(const Value& hz, const Value& maxSubsteps);
Value GetContacts();
Value GetPhysicsBackend();

// ---- Particles -----------------------------------------------------------
Value CreateEmitter(const Value& x, const Value& y, const Value& z);
Value DestroyEmitter(const Value& emitter);
Value SetEmitterPosition(const Value& emitter, const Value& x, const Value& y, const Value& z);
Value SetEmitterRotation(const Value& emitter, const Value& pitch, const Value& yaw, const Value& roll);
Value SetEmitterShape(const Value& emitter, const Value& shape, const Value& radius, const Value& angleDegrees);
Value SetEmitterRate(const Value& emitter, const Value& perSecond);
Value SetEmitterLifetime(const Value& emitter, const Value& minSeconds, const Value& maxSeconds);
Value SetEmitterSpeed(const Value& emitter, const Value& minSpeed, const Value& maxSpeed);
Value SetEmitterSize(const Value& emitter, const Value& minSize, const Value& maxSize);
Value SetEmitterColor(const Value& emitter, const Value& startHex, const Value& endHex);
Value SetEmitterGravity(const Value& emitter, const Value& x, const Value& y, const Value& z);
Value SetEmitterDrag(const Value& emitter, const Value& drag);
Value SetEmitterTexture(const Value& emitter, const Value& texture);
Value SetEmitterBlend(const Value& emitter, const Value& mode);
Value SetEmitterMaxParticles(const Value& emitter, const Value& maxCount);
Value SetEmitterLocalSpace(const Value& emitter, const Value& enabled);
Value SetEmitterEnabled(const Value& emitter, const Value& enabled);
Value BurstParticles(const Value& emitter, const Value& count);
Value ClearParticles(const Value& emitter);
Value GetParticleCount(const Value& emitter);
Value UpdateParticles(const Value& deltaTime);
Value DrawEmitter(const Value& emitter);
Value DrawParticles();

// ---- Animation: new ------------------------------------------------------
Value CrossFadeAnimation(const Value& anim, const Value& clipName, const Value& fadeSeconds);
Value GetAnimationClipCount(const Value& anim);
Value GetAnimationClipName(const Value& anim, const Value& index);
Value GetAnimationTransform(const Value& anim);
Value CreateAnimation();
Value AddAnimationClip(const Value& anim, const Value& clipName);
Value AddAnimationKey(const Value& anim, const Value& clipName, const Value& kind, const Value& time,
                      const Value& a1, const Value& a2, const Value& a3);

// ---- Scene ---------------------------------------------------------------
Value NewScene(const Value& name);
Value LoadScene(const Value& path);
Value SaveScene(const Value& path);
Value StartScene();
Value StopScene();
Value UpdateScene(const Value& deltaTime);
Value DrawScene();
Value CreateEntity(const Value& name);
Value CreateChildEntity(const Value& name, const Value& parent);
Value DestroyEntity(const Value& entity);
Value FindEntity(const Value& name);
Value FindEntitiesByTag(const Value& tag);
Value SetEntityParent(const Value& child, const Value& parent);
Value SetEntityPosition(const Value& entity, const Value& x, const Value& y, const Value& z);
Value GetEntityPosition(const Value& entity);
Value SetEntityRotation(const Value& entity, const Value& pitch, const Value& yaw, const Value& roll);
Value GetEntityRotation(const Value& entity);
Value SetEntityScale(const Value& entity, const Value& x, const Value& y, const Value& z);
Value SetEntityActive(const Value& entity, const Value& active);
Value AddEntityTag(const Value& entity, const Value& tag);
Value SetEntityMesh(const Value& entity, const Value& mesh);
Value SetEntityMaterial(const Value& entity, const Value& material);
Value AddEntityLight(const Value& entity, const Value& kind, const Value& r, const Value& g, const Value& b,
                     const Value& intensity);
Value AddEntityBody(const Value& entity, const Value& shape, const Value& mass, const Value& sizeX,
                    const Value& sizeY, const Value& sizeZ);
Value AddEntityEmitter(const Value& entity);
Value GetEntityEmitter(const Value& entity);
Value AddEntityAnimator(const Value& entity, const Value& path, const Value& clip);
Value GetEntityAnimation(const Value& entity);
Value AddEntityCamera(const Value& entity, const Value& fov);
Value SetActiveCamera(const Value& entity);
Value GetEntityBody(const Value& entity);
Value GetEntityCount();

// ---- Terrain (see runtime/src/terrain.cpp + terrain_render.cpp) ----------
// Heightmap terrain with chunked LOD, height/normal/slope queries, a
// slope- and height-driven splat bake, and optional collision that both
// physics backends honour. Pv::DrawTerrain (declared with the other 3D
// drawing commands above) is the draw call.
Value CreateTerrain(const Value& cols, const Value& rows, const Value& cellSize);
Value GenerateTerrain(const Value& terrain, const Value& seed, const Value& roughness, const Value& heightScale);
Value LoadTerrainHeightmap(const Value& terrain, const Value& filepath);
Value DestroyTerrain(const Value& terrain);
Value SetTerrainOrigin(const Value& terrain, const Value& x, const Value& y, const Value& z);
Value SetTerrainHeight(const Value& terrain, const Value& col, const Value& row, const Value& height);
Value GetTerrainHeight(const Value& terrain, const Value& x, const Value& z);
Value GetTerrainNormal(const Value& terrain, const Value& x, const Value& z);
Value GetTerrainSlope(const Value& terrain, const Value& x, const Value& z);
Value GetTerrainSize(const Value& terrain);
Value TerrainRaycast(const Value& terrain, const Value& ox, const Value& oy, const Value& oz, const Value& dx,
                     const Value& dy, const Value& dz, const Value& maxDist);
Value AddTerrainLayer(const Value& terrain, const Value& r, const Value& g, const Value& b, const Value& minHeight,
                      const Value& maxHeight, const Value& minSlope, const Value& maxSlope);
Value SetTerrainLayerBlend(const Value& terrain, const Value& layer, const Value& blend);
Value ClearTerrainLayers(const Value& terrain);
Value SampleTerrainLayer(const Value& terrain, const Value& x, const Value& z, const Value& layer);
Value SetTerrainLOD(const Value& terrain, const Value& chunkSize, const Value& lodDistance);
Value SetTerrainCollision(const Value& terrain, const Value& enabled);
Value GetTerrainStats(const Value& terrain);
Value BuildTerrainMesh(const Value& terrain);
Value BakeTerrainTexture(const Value& terrain, const Value& resolution);
Value SetTerrainMaterial(const Value& terrain, const Value& material);

// ---- AI: navigation grid (see runtime/src/ai.cpp) ------------------------
Value CreateNavGrid(const Value& originX, const Value& originZ, const Value& cols, const Value& rows,
                    const Value& cellSize);
Value CreateNavGridFromTerrain(const Value& terrain, const Value& maxSlopeDeg);
Value DestroyNavGrid(const Value& nav);
Value SetNavCellWalkable(const Value& nav, const Value& col, const Value& row, const Value& walkable);
Value SetNavCellCost(const Value& nav, const Value& col, const Value& row, const Value& cost);
Value AddNavObstacle(const Value& nav, const Value& x, const Value& z, const Value& width, const Value& depth);
Value ClearNavObstacles(const Value& nav);
Value IsNavPointWalkable(const Value& nav, const Value& x, const Value& z);
Value NavLineOfSight(const Value& nav, const Value& x1, const Value& z1, const Value& x2, const Value& z2);
Value FindPath(const Value& nav, const Value& startX, const Value& startZ, const Value& goalX, const Value& goalZ);
Value FindPathSmoothed(const Value& nav, const Value& startX, const Value& startZ, const Value& goalX,
                       const Value& goalZ);
Value GetNavGridStats(const Value& nav);

// ---- AI: agents ---------------------------------------------------------
Value CreateAgent(const Value& nav, const Value& x, const Value& z);
Value DestroyAgent(const Value& agent);
Value SetAgentSpeed(const Value& agent, const Value& speed);
Value SetAgentTurnRate(const Value& agent, const Value& degreesPerSecond);
Value SetAgentRadius(const Value& agent, const Value& radius);
Value SetAgentPosition(const Value& agent, const Value& x, const Value& z);
Value SetAgentTarget(const Value& agent, const Value& x, const Value& z);
Value StopAgent(const Value& agent);
Value UpdateAgents(const Value& deltaTime);
Value GetAgentPosition(const Value& agent);
Value GetAgentVelocity(const Value& agent);
Value GetAgentHeading(const Value& agent);
Value HasAgentArrived(const Value& agent);
Value GetAgentPath(const Value& agent);
Value AgentCanSee(const Value& agent, const Value& x, const Value& z, const Value& fovDegrees,
                  const Value& maxDistance);
Value FindNearestAgent(const Value& x, const Value& z, const Value& maxDistance);
Value GetAgentCount();

// ---- AI: state machine and blackboard -----------------------------------
// Deliberately a polling model rather than script callbacks: a .pv script
// already runs a per-frame loop, and giving each agent a named state plus
// a time-in-state is enough to write a state machine in the script itself
// without pvcc needing to special-case yet another function-pointer
// command the way the networking callbacks are special-cased.
Value SetAgentState(const Value& agent, const Value& state);
Value GetAgentState(const Value& agent);
Value GetAgentStateTime(const Value& agent);
Value SetAgentBlackboard(const Value& agent, const Value& key, const Value& value);
Value GetAgentBlackboard(const Value& agent, const Value& key);

// ---- Networking: client (see runtime/src/network.cpp) --------------------
//
// Callbacks are raw function pointers, not Value. A pv::Value has no
// callable case (the compiler warns on a bare `Pv::Foo` reference for
// exactly this reason), but a top-level `.pv` function compiles to a real
// C++ function -- so `Pv::OnReceive(OnMessage)` passes its address with no
// codegen support needed. The practical limit this implies: a callback must
// be a *top-level* function. One nested inside a block compiles to a
// capturing std::function, which has no function-pointer form and will be
// rejected by the C++ compiler.
using NetCallback = Value (*)();       // OnConnect
using NetCallback1 = Value (*)(Value); // OnDisconnect / OnReceive / OnNetworkError

// Connection management
Value Connect(const Value& host, const Value& port);
Value Disconnect();
Value IsConnected();
Value GetConnectionStatus();
Value GetConnectionError();

Value GetSessionToken();

// Sending
Value Send(const Value& data);
Value SendRaw(const Value& bytes);
Value Flush();

// Receiving
Value Receive();
Value ReceiveNonBlocking();
Value ReceiveTimeout(const Value& milliseconds);
Value PollMessages();
Value ClearMessageQueue();

// Input serialization helpers (network_scene.cpp -- these touch the scene)
Value SerializeInput(const Value& action, const Value& x, const Value& y, const Value& z);
Value SerializePlayerState(const Value& entity);
Value DeserializeState(const Value& data);
Value DeserializePlayerPosition(const Value& data, const Value& entity);

// Networking callbacks
Value OnConnect(NetCallback callback);
Value OnDisconnect(NetCallback1 callback);
Value OnReceive(NetCallback1 callback);
Value OnNetworkError(NetCallback1 callback);

// Latency & debugging
Value GetLatency();
Value GetPacketLoss();
Value GetBytesReceived();
Value GetBytesSent();
Value EnableNetworkDebug(const Value& enabled);
Value GetNetworkStats();

// Peer-to-peer
Value Listen(const Value& port);
Value AcceptConnection();
Value SendToPeer(const Value& peer_id, const Value& data);
Value ReceiveFromPeer();

} // namespace rt
} // namespace pv
