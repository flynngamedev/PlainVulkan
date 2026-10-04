# PlainVulkan 3.7 (`.pv`) Reference

A small, dynamically-typed scripting language that compiles (via
`transpiler/` + `compiler/`) to native C++ rendered with Vulkan. This is
the same reference the parser (`transpiler/tests/parser_tests.rs`) is
tested against.

## Language syntax

### Comments
```
// line comment
/* block comment */
```
Comments are stripped by the lexer and never reach the parser -- there is
no comment AST node.

### Variables
```
var name = value;
let name = value;
const name = value;   // must be initialized
var name;              // no initializer (var/let only)
```
Top-level declarations are visible from every function (they compile to
globals); declarations inside a function or block are ordinary local
variables scoped the way you'd expect.

### Assignment
```
name = value;
name[index] = value;
object.member = value;
a.b[0].c = value;       // chains freely
```
Operators: `= += -= *= /= %=`

### Values / literals
```
123        3.14        -10
0xFF       0b1010       1.5e3       1.5e-3
"double-quoted"   'single-quoted'
true   false   null
```
Strings support `\n \t \r \\ \" \' \0` escapes.

### Arrays
```
[]
[1, 2, 3]
[1, 2, 3,]        // trailing comma OK
name[index]
name[index] = value
```

### Functions
```
function name(param1, param2) {
    return value;   // or bare `return;`, or fall off the end (returns null)
}
name(arg1, arg2);
```
Functions can be declared at the top level or nested inside another
function/block; a nested function can call itself recursively.

### Control flow
```
if (condition) { } elif (condition) { } else { }
while (condition) { }
for (init; condition; increment) { }
break;
continue;
```

### Operators (loosest to tightest precedence)
```
=  +=  -=  *=  /=  %=      (assignment, right-associative)
?:                            (ternary, right-associative)
||
&&
==  !=
<  >  <=  >=
+  -
*  /  %
-x  !x  ~x                    (unary, right-associative)
```

### Member / scope access
```
object.member
object.member = value
Namespace::name              // generic scope operator -- Pv:: is just
Namespace::name(args)        // the built-in engine using this generically
```

## Engine commands (`Pv::`)

Every function below is a real, callable C++ symbol in the runtime; see
`runtime/README.md` for exactly which are fully real, which are simplified,
and which are tracked-but-not-yet-effective stubs.

### Core / Window
```
Pv::Init();  Pv::Shutdown();
Pv::CreateWindow(width, height, "title");   Pv::CloseWindow();
Pv::SetWindowTitle("title");  Pv::SetWindowSize(width, height);
Pv::IsWindowOpen();  Pv::SetVSync(bool);  Pv::SetFullscreen(bool);
```

### Frame / Time
```
Pv::BeginFrame();  Pv::EndFrame();
Pv::Clear(colorHex);  Pv::SetClearColor(r, g, b, a);
Pv::GetDeltaTime();  Pv::GetFPS();
```

### Camera
```
Pv::SetCamera(x, y, z);  Pv::LookAt(x, y, z);
Pv::SetFOV(degrees);  Pv::SetNearFar(near, far);
Pv::SetOrthographic(bool);
Pv::MoveCamera(x, y, z);  Pv::RotateCamera(pitch, yaw, roll);
```

### 2D Drawing
```
Pv::DrawTriangle(x, y, color);
Pv::DrawRect(x, y, w, h, color);
Pv::DrawCircle(x, y, radius, color);
Pv::DrawLine(x1, y1, x2, y2, color);
Pv::DrawSprite(texture, x, y);
Pv::DrawSpriteEx(texture, x, y, w, h);
Pv::DrawText("text", x, y, color);
Pv::DrawTextEx("text", x, y, "font", size, color);
```

### 3D Drawing
```
Pv::DrawMesh(mesh, x, y, z);
Pv::DrawMeshEx(mesh, x, y, z, rx, ry, rz, scale);
Pv::DrawSkybox(cubemap);  Pv::DrawTerrain(heightmap, scale);
Pv::DrawBillboard(texture, x, y, z, w, h);
Pv::SetWireframe(bool);
```

### Mesh management
```
Pv::LoadMesh("filepath");     // .obj .gltf .glb
Pv::UnloadMesh(mesh);
Pv::CreateMesh(vertices, indices);   Pv::UpdateMesh(mesh, vertices);
Pv::CreateCube(size);  Pv::CreateSphere(radius, slices);
Pv::CreatePlane(width, height);  Pv::CreateCylinder(radius, height);
```

### Textures
```
Pv::LoadTexture("filepath");   // .png .jpg
Pv::UnloadTexture(texture);
Pv::CreateTexture(width, height, format);
Pv::SetTextureFilter(texture, filter);  Pv::SetTextureWrap(texture, wrap);
Pv::LoadCubemap("filepath");  Pv::CreateRenderTexture(width, height);
```

### Shaders
```
Pv::LoadShader("vertPath", "fragPath");  Pv::UnloadShader(shader);
Pv::SetShader(shader);  Pv::ResetShader();
Pv::SetUniformInt/Float(shader, "name", value);
Pv::SetUniformVec2(shader, "name", x, y);
Pv::SetUniformVec3(shader, "name", x, y, z);
Pv::SetUniformVec4(shader, "name", x, y, z, w);
Pv::SetUniformMat4(shader, "name", matrix);
Pv::SetUniformTexture(shader, "name", texture, slot);
```

### Lighting
```
Pv::SetAmbient(r, g, b, intensity);
Pv::AddPointLight(x, y, z, r, g, b, intensity, radius);
Pv::AddDirectionalLight(dx, dy, dz, r, g, b, intensity);
Pv::AddSpotLight(x, y, z, dx, dy, dz, angle, intensity);
Pv::RemoveLight(light);
Pv::SetShadows(bool);  Pv::SetShadowResolution(resolution);
Pv::SetFog(r, g, b, near, far);
```

### Materials
```
Pv::CreateMaterial();
Pv::SetMaterialAlbedo(material, texture);
Pv::SetMaterialNormal(material, texture);
Pv::SetMaterialRoughness(material, value);
Pv::SetMaterialMetallic(material, value);
Pv::SetMaterialEmissive(material, texture, intensity);
Pv::SetMaterialColor(material, r, g, b, a);
```

### Audio
```
Pv::LoadSound("filepath");    // .wav .ogg (+ .mp3/.flac for free)
Pv::UnloadSound(sound);  Pv::PlaySound(sound);  Pv::StopSound(sound);
Pv::PauseSound(sound);
Pv::SetSoundVolume(sound, volume);  Pv::SetSoundPitch(sound, pitch);
Pv::LoadMusic("filepath");  Pv::PlayMusic(music);  Pv::StopMusic(music);
Pv::SetMusicVolume(music, volume);
Pv::SetSound3D(sound, x, y, z);
```

### Input -- keyboard

`key` is a **GLFW integer** (`GLFW_KEY_*`), not a letter string. A string
such as `"w"` coerces to `0` and will not match the W key. Typical codes:

| Key | Code |
|---|---|
| A–Z | 65–90 (`W`=87, `A`=65, `S`=83, `D`=68) |
| Space | 32 |
| Escape | 256 |
| Enter | 257 |
| Arrow left / right / up / down | 263 / 262 / 265 / 264 |

```
Pv::IsKeyDown(key);  Pv::IsKeyUp(key);
Pv::IsKeyPressed(key);  Pv::IsKeyReleased(key);
Pv::GetKeyPressed();
```

### Input -- mouse (button 0=left, 1=right, 2=middle, matching GLFW)
```
Pv::GetMouseX();  Pv::GetMouseY();
Pv::GetMouseDeltaX();  Pv::GetMouseDeltaY();
Pv::IsMouseDown(button);  Pv::IsMousePressed(button);  Pv::IsMouseReleased(button);
Pv::GetMouseScroll();
Pv::SetMouseVisible(bool);  Pv::SetMouseLocked(bool);
Pv::SetMousePosition(x, y);
```

### Input -- gamepad
```
Pv::IsGamepadConnected(id);
Pv::IsGamepadButtonDown(id, button);  Pv::IsGamepadButtonPressed(id, button);
Pv::GetGamepadAxis(id, axis);
Pv::SetGamepadVibration(id, left, right);
```

### Physics
Backed by NVIDIA PhysX (see `runtime/README.md` for the backend selection
rules and the fallback solver).
```
Pv::InitPhysics();  Pv::SetGravity(x, y, z);  Pv::StepPhysics(deltaTime);
Pv::SetPhysicsTimestep(hz, maxSubsteps);   // fixed-step accumulation
Pv::GetPhysicsBackend();                   // -> "physx" | "builtin" | "none"

Pv::CreateBoxCollider(x, y, z, w, h, d);
Pv::CreateSphereCollider(x, y, z, radius);
Pv::CreateCapsuleCollider(x, y, z, radius, height);   // a real capsule now
Pv::SetColliderMaterial(collider, staticFriction, dynamicFriction, restitution);
Pv::SetColliderTrigger(collider, bool);

Pv::CreateRigidBody(collider, mass);       // mass <= 0 means static
Pv::DestroyBody(body);
Pv::GetBodyPosition(body);  Pv::SetBodyPosition(body, x, y, z);   // -> [x,y,z]
Pv::GetBodyRotation(body);  Pv::SetBodyRotation(body, pitch, yaw, roll);
Pv::SetBodyVelocity(body, x, y, z);  Pv::GetBodyVelocity(body);
Pv::SetBodyAngularVelocity(body, x, y, z);  Pv::GetBodyAngularVelocity(body);
Pv::AddForce(body, x, y, z);  Pv::AddImpulse(body, x, y, z);
Pv::AddTorque(body, x, y, z);
Pv::SetBodyKinematic(body, bool);          // moved by script, still pushes
Pv::SetBodyDamping(body, linear, angular);
Pv::SetBodyGravityEnabled(body, bool);
Pv::SetBodyFreezePosition(body, fx, fy, fz);   // per-axis locks
Pv::SetBodyFreezeRotation(body, fx, fy, fz);

Pv::Raycast(ox, oy, oz, dx, dy, dz, maxDist);
// -> {hit, x, y, z, nx, ny, nz, distance, body}  (nx/ny/nz = surface normal)
Pv::GetContacts();
// -> array of {bodyA, bodyB, x, y, z, nx, ny, nz, impulse, trigger}
```

### Particles
```
Pv::CreateEmitter(x, y, z);  Pv::DestroyEmitter(emitter);
Pv::SetEmitterPosition(emitter, x, y, z);
Pv::SetEmitterRotation(emitter, pitch, yaw, roll);
Pv::SetEmitterShape(emitter, "cone", radius, angleDegrees);
//   shapes: "point" "sphere" "hemisphere" "box" "cone" "circle"
Pv::SetEmitterRate(emitter, perSecond);
Pv::SetEmitterLifetime(emitter, minSeconds, maxSeconds);
Pv::SetEmitterSpeed(emitter, minSpeed, maxSpeed);
Pv::SetEmitterSize(emitter, minSize, maxSize);
Pv::SetEmitterColor(emitter, startHex, endHex);   // 0xRRGGBBAA
Pv::SetEmitterGravity(emitter, x, y, z);
Pv::SetEmitterDrag(emitter, drag);
Pv::SetEmitterTexture(emitter, texture);
Pv::SetEmitterBlend(emitter, "alpha" | "additive" | "opaque");
Pv::SetEmitterMaxParticles(emitter, count);
Pv::SetEmitterLocalSpace(emitter, bool);   // false leaves a trail behind
Pv::SetEmitterEnabled(emitter, bool);
Pv::BurstParticles(emitter, count);  Pv::ClearParticles(emitter);
Pv::GetParticleCount(emitter);
Pv::UpdateParticles(deltaTime);
Pv::DrawEmitter(emitter);  Pv::DrawParticles();   // all emitters
```

### Scene
```
Pv::NewScene("name");  Pv::LoadScene("path.pvscene");  Pv::SaveScene("path");
Pv::StartScene();  Pv::StopScene();
Pv::UpdateScene(deltaTime);  Pv::DrawScene();

Pv::CreateEntity("name");  Pv::CreateChildEntity("name", parent);
Pv::DestroyEntity(entity);
Pv::FindEntity("name");  Pv::FindEntitiesByTag("tag");   // -> array
Pv::SetEntityParent(child, parent);
Pv::SetEntityPosition(entity, x, y, z);  Pv::GetEntityPosition(entity);
Pv::SetEntityRotation(entity, pitch, yaw, roll);  Pv::GetEntityRotation(entity);
Pv::SetEntityScale(entity, x, y, z);
Pv::SetEntityActive(entity, bool);  Pv::AddEntityTag(entity, "tag");
Pv::GetEntityCount();

Pv::SetEntityMesh(entity, mesh);  Pv::SetEntityMaterial(entity, material);
Pv::AddEntityLight(entity, "point", r, g, b, intensity);
Pv::AddEntityBody(entity, "box", mass, sizeX, sizeY, sizeZ);
Pv::GetEntityBody(entity);
Pv::AddEntityEmitter(entity);  Pv::GetEntityEmitter(entity);
Pv::AddEntityAnimator(entity, "file.glb", "clipName");
Pv::GetEntityAnimation(entity);
Pv::AddEntityCamera(entity, fovDegrees);  Pv::SetActiveCamera(entity);
```

### File system
```
Pv::FileExists("filepath");  Pv::ReadFile("filepath");
Pv::WriteFile("filepath", "data");  Pv::DeleteFile("filepath");
Pv::GetWorkingDir();  Pv::SetWorkingDir("path");
```

### UI
```
Pv::BeginUI();  Pv::EndUI();
Pv::UIButton("label", x, y, w, h);              // -> bool clicked
Pv::UIText("text", x, y, size, color);
Pv::UIImage(texture, x, y, w, h);
Pv::UISlider("label", x, y, min, max, value);    // -> updated value
Pv::UICheckbox("label", x, y, checked);          // -> updated bool
Pv::UIInputText("label", x, y, buffer);          // -> updated string
Pv::UIProgressBar(x, y, w, h, value);            // value in [0,1]
```

### Debug
```
Pv::DrawDebugLine(x1, y1, z1, x2, y2, z2, color);
Pv::DrawDebugBox(x, y, z, w, h, d, color);
Pv::DrawDebugSphere(x, y, z, radius, color);
Pv::Log("message");  Pv::LogWarning("message");  Pv::LogError("message");
Pv::Assert(condition, "message");
```

### Animation
```
Pv::LoadAnimation("filepath");  Pv::UnloadAnimation(anim);
Pv::PlayAnimation(anim, "clipName");
Pv::PauseAnimation(anim);  Pv::StopAnimation(anim);
Pv::SetAnimationSpeed(anim, speed);  Pv::SetAnimationLoop(anim, bool);
Pv::IsAnimationPlaying(anim);
Pv::GetAnimationTime(anim);  Pv::SetAnimationTime(anim, time);
Pv::GetAnimationDuration(anim);
Pv::BlendAnimation(anim, "clipA", weight);   // real layered blending
Pv::CrossFadeAnimation(anim, "clipName", fadeSeconds);
Pv::UpdateAnimations(deltaTime);

// Introspection
Pv::GetAnimationClipCount(anim);  Pv::GetAnimationClipName(anim, index);
Pv::GetAnimationTransform(anim);
// -> {valid, x, y, z, pitch, yaw, roll, scaleX, scaleY, scaleZ}

// Building a clip from script, with no asset file
Pv::CreateAnimation();
Pv::AddAnimationClip(anim, "clipName");
Pv::AddAnimationKey(anim, "clipName", kind, time, a1, a2, a3);
//   kind: "position" | "rotation" (Euler radians) | "scale"
```

### Networking (client)

Connects to a **PlainServer** (`.pls` / `pls.py`) process -- the sibling
server toolchain -- or to another PlainVulkan client acting as a peer.
Transport is TCP with length-prefixed framing. The byte layout is shared
verbatim with PlainServer (`runtime/include/pv/pv_net.h` here,
`runtime/include/pls/pls_net.h` there):

```
[0..4)  uint32 big-endian: byte length of everything that follows
[4]     uint8 frame kind:  0=JSON  1=raw  2=PING  3=PONG
[5..)   payload
```

The bundled pair is `examples/multiplayer_client` in this repo and
`examples/mp_server` in the PlainServer tree. Default address is
`127.0.0.1:8080`. Start the server first:

```
python pls.py run examples/mp_server           # from the PlainServer directory
python pv.py run examples/multiplayer_client   # from this directory
```

The client must not send its own position. It sends **input**; the server
integrates it and broadcasts world `state`.

Typical server → client JSON:

- `{"type":"welcome","player_id":...,"initial_position":[x,y,z],"tickrate":...}`
- `{"type":"state", ...}` -- feed this to `Pv::DeserializeState`
- `{"type":"player_killed","victim":...,"killer":...}`

Typical client → server JSON (build with `Pv::SerializeInput`):

- `{action:"move"|"shoot"|"jump", x, y, z}` -- `x,y,z` is a **direction**,
  not a world position

`Pv::DeserializeState` mirrors remote entities as `Net_<serverId>`. An
entity the server marks `"dead"` is deactivated, not destroyed, so a
respawn under the same id keeps script handles valid.

```
// Connection
Pv::Connect(host, port);          // -> connection handle, or null on failure
Pv::Disconnect();
Pv::IsConnected();                // -> bool
Pv::GetConnectionStatus();        // -> "connecting"|"connected"|"disconnected"|"error"
Pv::GetConnectionError();         // -> last error message

// Sending
Pv::Send(data);                   // any value; serialized to JSON
Pv::SendRaw(bytes);               // string, or array of byte values
Pv::Flush();                      // no-op: sends are unbuffered (see below)

// Receiving
Pv::Receive();                    // blocks until a message arrives
Pv::ReceiveNonBlocking();         // -> message, or null
Pv::ReceiveTimeout(milliseconds);
Pv::PollMessages();               // -> count of pending messages
Pv::ClearMessageQueue();          // -> number discarded

// Building and applying packets
Pv::SerializeInput(action, x, y, z);   // -> {action, x, y, z}
Pv::SerializePlayerState(entity);      // -> {id, position, rotation}
Pv::DeserializeState(data);            // applies a server state packet
Pv::DeserializePlayerPosition(data, entity);

// Callbacks -- see the note below
Pv::OnConnect(callback);          // callback()
Pv::OnDisconnect(callback);       // callback(reason)
Pv::OnReceive(callback);          // callback(data)
Pv::OnNetworkError(callback);     // callback(error)

// Instrumentation
Pv::GetLatency();                 // -> float, milliseconds round-trip
Pv::GetPacketLoss();              // -> float 0.0-1.0 (see below)
Pv::GetBytesSent();  Pv::GetBytesReceived();
Pv::EnableNetworkDebug(enabled);
Pv::GetNetworkStats();
// -> {latency, packetLoss, bytesSent, bytesReceived, status, pendingMessages}

// Peer-to-peer -- lets a client also accept connections
Pv::Listen(port);
Pv::AcceptConnection();           // -> peer_id, or null if none waiting
Pv::SendToPeer(peer_id, data);
Pv::ReceiveFromPeer();            // -> {peer_id, data}
```

**Callbacks must be top-level functions.** They are passed as C++ function
pointers, because a `pv::Value` has no callable case. A function declared
inside a block compiles to a capturing lambda, which has no function-pointer
form, and the C++ compiler will reject it.

**Callbacks are dispatched on the main thread**, from `Pv::BeginFrame` and
from every `Pv::` networking command. Reads happen on a background thread,
but your handler never runs there -- so it is safe for a handler to touch
scene entities. In a normal game loop this means handlers fire once per
frame, at a predictable point.

**`Pv::OnReceive` takes over the message stream.** With a handler
registered, messages go to it and *not* to `Pv::Receive` / `PollMessages`.
Picking one style is deliberate: delivering to both would make a script that
mixes them process every message twice.

**Registration order does not matter.** Connecting before registering
`Pv::OnConnect` still fires the handler -- the event is held until someone
is listening.

**`Pv::Flush` is a no-op.** Sends are written to the socket before `Pv::Send`
returns, and `TCP_NODELAY` is set, so there is nothing buffered to force. It
is kept so scripts written against the buffered-send model still run.

**`Pv::GetPacketLoss` measures unanswered latency probes**, not datagram
loss. TCP retransmits until data arrives or the connection dies, so it never
exposes a loss rate; this rises when the link is bad enough that
retransmission is stalling the stream.

**Building packets without object literals.** `.pv` has no `{...}` literal.
Use `Pv::SerializeInput` for input packets, or build a value field by field
(member assignment auto-vivifies):

```
var packet;
packet.type = "chat";
packet.text = "hello";
Pv::Send(packet);
```

### Terrain

Heightmap terrain with chunked level of detail. A terrain of `cols` x `rows`
*vertices* covers `(cols-1)*cellSize` by `(rows-1)*cellSize` world units from
its origin, on the X/Z plane with Y up.

```
var t = Pv::CreateTerrain(cols, rows, cellSize);
Pv::GenerateTerrain(t, seed, roughness, heightScale);   // fBm value noise
Pv::LoadTerrainHeightmap(t, "assets/heights.png");      // greyscale image
Pv::DestroyTerrain(t);

Pv::SetTerrainOrigin(t, x, y, z);
Pv::SetTerrainHeight(t, col, row, height);   // edit one grid vertex
Pv::SetTerrainLOD(t, chunkSize, lodDistance); // chunkSize must be a power of 2
Pv::SetTerrainCollision(t, true);             // honoured by both physics backends
```

Queries take **world** X/Z and clamp outside the terrain rather than failing:

```
Pv::GetTerrainHeight(t, x, z);   // number
Pv::GetTerrainNormal(t, x, z);   // [nx, ny, nz]
Pv::GetTerrainSlope(t, x, z);    // degrees from flat
Pv::GetTerrainSize(t);           // {cols, rows, cellSize, width, depth, minHeight, maxHeight}
Pv::GetTerrainStats(t);          // {chunks, lodLevels, trianglesDrawnLastFrame, ...}
Pv::TerrainRaycast(t, ox,oy,oz, dx,dy,dz, maxDist);  // {hit, point[3], normal[3], distance}
```

Layers colour the terrain by height *and* slope, so cliffs read as rock at
any altitude. The layers are baked into one texture on the CPU, which means
they work with the existing material pipeline and need no custom shader:

```
var rock = Pv::AddTerrainLayer(t, r,g,b, minHeight, maxHeight, minSlope, maxSlope);
Pv::SetTerrainLayerBlend(t, rock, 8.0);   // soft edge width
Pv::ClearTerrainLayers(t);
Pv::SampleTerrainLayer(t, x, z, layerIndex);  // that layer's weight, 0..1

var tex = Pv::BakeTerrainTexture(t, 512);
var mat = Pv::CreateMaterial();
Pv::SetMaterialAlbedo(mat, tex);
Pv::SetTerrainMaterial(t, mat);

Pv::BuildTerrainMesh(t);    // upload per-chunk LOD meshes; call after any edit
Pv::DrawTerrain(t, 1.0);    // LOD chosen per chunk from the camera
```

### AI -- navigation grid

```
var nav = Pv::CreateNavGrid(originX, originZ, cols, rows, cellSize);
var nav = Pv::CreateNavGridFromTerrain(terrain, maxSlopeDegrees);
Pv::DestroyNavGrid(nav);

Pv::SetNavCellWalkable(nav, col, row, bool);
Pv::SetNavCellCost(nav, col, row, cost);   // >= 1; higher is avoided
Pv::AddNavObstacle(nav, x, z, width, depth);
Pv::ClearNavObstacles(nav);

Pv::IsNavPointWalkable(nav, x, z);
Pv::NavLineOfSight(nav, x1, z1, x2, z2);
Pv::GetNavGridStats(nav);   // {cols, rows, cells, walkableCells, obstacles}
```

`FindPath` is A* over 8-connected cells with a true diagonal cost, so it
returns the shortest route rather than merely a plausible one, and it refuses
to cut the corner between two diagonally-touching blockers. `FindPathSmoothed`
adds string pulling, which drops every waypoint the previous one can already
see past -- that is what turns A*'s staircase into straight runs.

```
var path = Pv::FindPathSmoothed(nav, sx, sz, gx, gz);  // [{x,y,z}, ...], empty if no route
```

### AI -- agents

```
var a = Pv::CreateAgent(nav, x, z);
Pv::DestroyAgent(a);
Pv::SetAgentSpeed(a, unitsPerSecond);
Pv::SetAgentTurnRate(a, degreesPerSecond);
Pv::SetAgentRadius(a, r);
Pv::SetAgentPosition(a, x, z);      // teleport; clears the current path
Pv::SetAgentTarget(a, x, z);        // paths there; false when no route exists
Pv::StopAgent(a);
Pv::UpdateAgents(deltaTime);        // steps every agent

Pv::GetAgentPosition(a);   // [x, y, z] -- Y follows the ground
Pv::GetAgentVelocity(a);   // [x, y, z]
Pv::GetAgentHeading(a);    // degrees
Pv::HasAgentArrived(a);
Pv::GetAgentPath(a);
Pv::GetAgentCount();
Pv::AgentCanSee(a, x, z, fovDegrees, maxDistance);  // cone + line of sight
Pv::FindNearestAgent(x, z, maxDistance);
```

### AI -- state machine and blackboard

Deliberately a **polling** model. A `.pv` script already runs a per-frame
loop, so the engine only stores a state name and how long the agent has been
in it; the script decides what the states mean and when to switch. That keeps
the whole state machine visible in the script instead of split between it and
an engine-side graph.

```
Pv::SetAgentState(a, "chase");     // re-entering the same state does NOT reset the timer
Pv::GetAgentState(a);
Pv::GetAgentStateTime(a);          // seconds in the current state
Pv::SetAgentBlackboard(a, "target", entityId);
Pv::GetAgentBlackboard(a, "target");   // null when unset
```

See `examples/terrain_ai` for both systems working together.

## CLI
```
pv new <name>       Create assets/, scripts/, main.pv, pvproject.json
pv build <dir>       Parse -> generate C++ -> compile a native binary
pv run <dir>         Build (if needed) and run it
pv check <file.pv>   Parse a single file and report syntax errors
pv version
```
