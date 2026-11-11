#include "debugConsole.h"
#include "engine/modelPreviewRequest.h"

#include <algorithm>
#include <conio.h>
#include <cctype>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "engine/runtimeCommand.h"
#include "engine/editorHost.h"
#include "hair/hairConsoleCommand.h"
#include "hair/tressfxLiveParams.h"
#include "particles/particleDump.h"
#include "particles/particleRuntimeCommand.h"
#include "world/world.h"

DebugConsole::DebugConsole(VL::CommandBus& commandBus, VL::IEditorConsoleSink* sink)
    : commandBus(commandBus), editorConsoleSink(sink)
{
}

void DebugConsole::Initialize()
{
    std::cout << "Debug console ready. Type 'help' for available commands." << std::endl;
    PrintPrompt();
}

void DebugConsole::Update()
{
    for (auto iterator = pendingEditorResults.begin(); iterator != pendingEditorResults.end();)
    {
        if (editorConsoleSink == nullptr)
        {
            iterator = pendingEditorResults.erase(iterator);
            continue;
        }
        bool isFinal = false;
        const auto text = editorConsoleSink->PollResult(*iterator, isFinal);
        if (text)
            std::cout << *text << std::endl;
        if (isFinal) iterator = pendingEditorResults.erase(iterator);
        else ++iterator;
    }
    while (_kbhit())
    {
        const int ch = _getch();
        HandleInput(ch);
    }
}

// 启动脚本入口：直接走 ProcessCommand，避开 _kbhit/_getch 交互输入路径。
// 这里刻意不做任何额外解析或校验，脚本命令与手输命令必须完全同构。
void DebugConsole::SubmitCommandLine(const std::string& line)
{
    if (line.empty())
    {
        return;
    }

    std::cout << line << std::endl;
    ProcessCommand(line);
}

void DebugConsole::HandleInput(int ch)
{
    if (ch == 0 || ch == 224)
    {
        if (_kbhit())
        {
            (void)_getch();
        }
        return;
    }

    if (ch == '\r' || ch == '\n')
    {
        std::cout << std::endl;
        if (!currentLine.empty())
        {
            ProcessCommand(currentLine);
            currentLine.clear();
        }
        PrintPrompt();
        return;
    }

    if (ch == '\b')
    {
        if (!currentLine.empty())
        {
            currentLine.pop_back();
            std::cout << "\b \b" << std::flush;
        }
        return;
    }

    if (std::isprint(static_cast<unsigned char>(ch)))
    {
        currentLine.push_back(static_cast<char>(ch));
        std::cout << static_cast<char>(ch) << std::flush;
    }
}

void DebugConsole::PrintHelp() const
{
    std::cout << "Available commands:\n";
    std::cout << "  animation <get|play|pause|seek seconds|clip index> - control native skeletal playback\n";
    std::cout << "  script <play|pause|stop> - control C# blueprint play state (same path as the editor Play button)\n";
    std::cout << "  motiondebug <0|1> - actual history and predicted trajectory overlay\n";
    std::cout << "  debugdraw <on|off|status|demo [overlay/depth]|category name on/off|owner id/*|clear [owner/category name]>\n";
    std::cout << "  editor <JSON> - submit an editor asset command and print its structured result\n";
    std::cout << "  debugview <mode> - set debug view mode\n";
    std::cout << "    0: Full\n";
    std::cout << "    1: BaseColor\n";
    std::cout << "    2: Emissive\n";
    std::cout << "    3: Normal\n";
    std::cout << "    4: Roughness\n";
    std::cout << "    5: Metallic\n";
    std::cout << "    6: AO\n";
    std::cout << "    7: Shadow\n";
    std::cout << "    8: Direct Lighting\n";
    std::cout << "    9: Indirect Diffuse\n";
    std::cout << "    10: Indirect Specular\n";
    std::cout << "    11: Shadow Cascade Index\n";
    std::cout << "    12: Shading Model\n";
    std::cout << "    13: Subsurface Weight\n";
    std::cout << "    14: Transmission Weight\n";
    std::cout << "    15: SSS Asset ID\n";
    std::cout << "    16: Local SSS Response\n";
    std::cout << "    17: Diffuse Before SSS\n";
    std::cout << "    18: Diffuse After SSS\n";
    std::cout << "    19: SSS Pixel Radius\n";
    std::cout << "    20: SSS Valid Weight\n";
    std::cout << "    64: Cloth Shading Model\n";
    std::cout << "    65: Cloth Sheen Color\n";
    std::cout << "    66: Cloth Sheen Roughness\n";
    std::cout << "    67: Cloth Charlie D\n";
    std::cout << "    68: Cloth Neubelt Visibility\n";
    std::cout << "    69: Cloth Directional Albedo\n";
    std::cout << "    70: Cloth Base Energy Scale\n";
    std::cout << "    71: Cloth Direct Sheen\n";
    std::cout << "    72: Cloth Indirect Sheen\n";
    std::cout << "    73: Cloth IBL Fallback\n";
    std::cout << "    74: Skin Direct Diffuse\n";
    std::cout << "    75: Skin Transmission\n";
    std::cout << "    76: Skin Shadow Visibility\n";
    std::cout << "    77: Skin IBL Diffuse\n";
    std::cout << "    78: Skin IBL Specular\n";
    std::cout << "    79: Skin Virtual Light\n";
    std::cout << "    90: Deferred Velocity (zero=0.5, XY gain=20)\n";
    std::cout << "  tonemap <mode> - set tone mapping mode\n";
    std::cout << "    0: Linear clamp\n";
    std::cout << "    1: Reinhard\n";
    std::cout << "    2: Hable\n";
    std::cout << "    3: ACES\n";
    std::cout << "  bloom strength <value> - set bloom composite strength\n";
    std::cout << "  bloom threshold <value> - set bloom threshold\n";
    std::cout << "  bloom knee <value> - set bloom soft knee\n";
    std::cout << "  bloom clamp <value> - set bloom fireflies clamp\n";
    std::cout << "  loadworld <scene-path> - request scene load through the World transaction\n";
    std::cout << "  mesh transform <identity> <px> <py> <pz> [rx ry rz] - absolute placement (cm, degrees; scale=1)\n";
    std::cout << "  mesh offset <identity> <dx> <dy> <dz> - add a delta to the current placement\n";
    std::cout << "  mesh sweep <identityA> <identityB> <dx> <dy> <dz> <seconds> - linear placement over GT frames\n";
    std::cout << "  hair trace <path> <frames> <guide-stride> - one-shot read-only GPU guide trace (max 256 guides)\n";
    std::cout << "  hair reset [objectName] - queue one ResetPositions for the next positive-delta frame\n";
    std::cout << "  hair set <objectName> <field> <value...> - override one live simulation parameter (GPU only)\n";
    std::cout << "  offline <on|off|samples N|bounces N|seed N|lightSamples N|aov MODE|exposure X|pause|resume|stop|restart|export>\n";
    std::cout << "  environment <value> - set unified sky and IBL intensity\n";
    std::cout << "  screenshot [file.bmp] - capture the presented swapchain image\n";
    std::cout << "  particledump [file.json] [maxPerEmitter] - read back GPU particle pools into Generated/ParticleDumps\n";
    std::cout << "  particlestats - print last frame particle emitter/draw/sort budget plus particle pass GPU ms\n";
    std::cout << "  particlespawn <name> <PS_asset_path> [x y z] - add a particle system instance to the active World\n";
    std::cout << "  particledestroy <name> - remove a particle system instance from the active World\n";
    std::cout << "  particlerestart <name> - reset one particle system instance (simulation time, prewarm, cull)\n";
    std::cout << "  particlekill <system> <emitter> <particleIndex> - clear one stable GPU particle slot\n";
    std::cout << "  particlespawnat <system> <emitter> <x> <y> <z> [lifetime] - spawn one GPU particle at a world position\n";
    std::cout << "  particleparam <name> <spawnRateScale <scale>|colorTint <r> <g> <b> <a>|emitter <index> <0|1>|reset> - runtime-only particle overrides\n";
    std::cout << "  particleemitter <system> <spawn <sourceIndex> [name]|destroy <index|name>|restart <index|name>|enable <index|name> <0|1>> - emitter lifecycle\n";
    std::cout << "  particlebudget <system|world> <maxTotal> <emitterScale> - runtime particle budget\n";
    std::cout << "  particleuser <system> [<key> <value>|clear <key>] - runtime named user parameter\n";
    std::cout << "  particledata <name> <value> | <name> vec2/vec3/vec4/quat/bool/int ... | <name> clear | particledata - World-global typed particle data channel\n";
    std::cout << "  particleattach <system> <mesh <target>|component <blueprint> <component>|detach> [x y z [rx ry rz [sx sy sz]]] - attach to scene object/component\n";
    std::cout << "  particletelemetry [system] - print CPU-authored counts and renderer-fed alive/budget data\n";
    std::cout << "  softbody impact <balloon-name> [speed] - launch a Jolt sphere at a soft body\n";
    std::cout << "  glass impact <pane-name> [speed] - launch a Jolt sphere at a glass pane\n";
    std::cout << "  shoot - fire a visible arrow from the render camera\n";
    std::cout << "  camera get - print the active camera pose\n";
    std::cout << "  camera position <x> <y> <z> - set camera position\n";
    std::cout << "  camera lookat <x> <y> <z> - aim camera at a world-space target\n";
    std::cout << "  camera pose <px> <py> <pz> <tx> <ty> <tz> - set position and look-at\n";
    std::cout << "  shaderreload <changed|all> - transactionally publish live Graphics, Compute, and UI shaders\n";
    std::cout << "  reloadassembly - reload the configured Gameplay assembly in the live World\n";
    std::cout << "  injectinput <action> <down|up> - inject a gameplay action for automation (no OS focus required)\n";
    std::cout << "  shaderreload help - show hot-reload triggers by resource type\n";
    std::cout << "  shadercache stats - report shader build cache statistics\n";
    std::cout << "  terrain stats - report terrain residency/upload statistics\n";
    std::cout << "  terrain parity [x z] - compare CPU/GPU bake for one page\n";
    std::cout << "  terrain capacity N - set resident page budget for the active World\n";
    std::cout << "  terrainfeedback - report completed terrain feedback page statistics (read-only)\n";
    std::cout << "  terrainlayerstats - report layer-page atlas residency/format statistics\n";
    std::cout << "  terrainlayer stats - report layer-page residency/visibility/budget statistics\n";
    std::cout << "  terrainlayer capacity N - set layer-page resident page budget\n";
    std::cout << "  terrainlayer preload - request every layer page (full-preload comparison)\n";
    std::cout << "  terrainlayer forcemip N - force target layer mip (-1 auto, 0..6)\n";
    std::cout << "  terraincollision stats - report play-mode terrain collision bodies\n";
    std::cout << "  terrainlod stats - report per-chunk terrain LOD level/distance/switches\n";
    std::cout << "  terrainchunk stats - report terrain chunk residency/draw/geometry bytes\n";
    std::cout << "  gpustats - print the latest per-pass GPU timing table (enables collection if needed)\n";
    std::cout << "  perf json - print one line of JSON with FPS, CPU frame ms, GPU total and per-pass ms\n";
    std::cout << "  quit - request a clean application exit (exit code 0)\n";
}

void DebugConsole::PrintShaderReloadHelp() const
{
    std::cout << "Shader hot-reload triggers:\n";
    std::cout << "  Material/Graphics - save .vert, .frag, or a depended-on .glsl file\n";
    std::cout << "  Compute           - save .comp or a depended-on .glsl file\n";
    std::cout << "  UI Overlay        - save uiOverlay .vert/.frag or a depended-on .glsl file\n";
    std::cout << "  Material M_*.json - save the definition under shader/glsl; rebuilds World/Graph resources and migrates compatible live MI state\n";
    std::cout << "  Shared include    - save .glsl; only live dependents are rebuilt\n";
    std::cout << "Manual commands:\n";
    std::cout << "  shaderreload changed - rebuild live dependents changed since the committed manifest\n";
    std::cout << "  shaderreload all     - rebuild all live Graphics, Compute, and UI shaders\n";
    std::cout << "  M_*.json schema reload is automatic; it has no manual shaderreload scope\n";
    std::cout << "Not watched by this system: MI_*.json, scene JSON, RenderGraph JSON, shader/spv outputs\n";
    std::cout << "Regular Graphics/Compute/UI replacement requires ABI compatibility.\n";
}

void DebugConsole::PrintPrompt() const
{
    std::cout << "debug> " << std::flush;
}
