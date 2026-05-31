param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
    param([string]$RelativePath)
    return Join-Path $RepoRoot $RelativePath
}

function Add-Failure {
    param(
        [System.Collections.Generic.List[string]]$Failures,
        [string]$Message
    )

    $Failures.Add($Message) | Out-Null
}

function Test-FilePatternsAbsent {
    param(
        [System.Collections.Generic.List[string]]$Failures,
        [string[]]$RelativePaths,
        [string[]]$Patterns,
        [string]$RuleName
    )

    foreach ($relativePath in $RelativePaths) {
        $path = Resolve-RepoPath $relativePath
        if (!(Test-Path -LiteralPath $path)) {
            Add-Failure $Failures "${RuleName}: missing expected file '$relativePath'."
            continue
        }

        foreach ($pattern in $Patterns) {
            $matches = Select-String -LiteralPath $path -Pattern $pattern
            foreach ($match in $matches) {
                Add-Failure $Failures "${RuleName}: '${relativePath}:$($match.LineNumber)' matches forbidden pattern '$pattern'."
            }
        }
    }
}

function Test-FilePatternsPresent {
    param(
        [System.Collections.Generic.List[string]]$Failures,
        [string]$RelativePath,
        [string[]]$Patterns,
        [string]$RuleName
    )

    $path = Resolve-RepoPath $RelativePath
    if (!(Test-Path -LiteralPath $path)) {
        Add-Failure $Failures "${RuleName}: missing expected file '$RelativePath'."
        return
    }

    foreach ($pattern in $Patterns) {
        $matches = Select-String -LiteralPath $path -Pattern $pattern
        if ($matches.Count -eq 0) {
            Add-Failure $Failures "${RuleName}: '$RelativePath' is missing required pattern '$pattern'."
        }
    }
}

function Test-PathsAbsent {
    param(
        [System.Collections.Generic.List[string]]$Failures,
        [string[]]$RelativePaths,
        [string]$RuleName
    )

    foreach ($relativePath in $RelativePaths) {
        $path = Resolve-RepoPath $relativePath
        if (Test-Path -LiteralPath $path) {
            Add-Failure $Failures "${RuleName}: legacy path '$relativePath' still exists."
        }
    }
}

function Test-SourcePatternsAbsent {
    param(
        [System.Collections.Generic.List[string]]$Failures,
        [string[]]$Patterns,
        [string]$RuleName
    )

    $sourceRoot = Resolve-RepoPath "source"
    $files = Get-ChildItem -LiteralPath $sourceRoot -Recurse -File -Include *.h,*.cpp
    foreach ($file in $files) {
        foreach ($pattern in $Patterns) {
            $matches = Select-String -LiteralPath $file.FullName -Pattern $pattern
            foreach ($match in $matches) {
                $relative = Resolve-Path -LiteralPath $file.FullName -Relative
                Add-Failure $Failures "${RuleName}: '${relative}:$($match.LineNumber)' matches forbidden pattern '$pattern'."
            }
        }
    }
}

function Test-VulkanManagerBoundary {
    param([System.Collections.Generic.List[string]]$Failures)

    $renderRoot = Resolve-RepoPath "source/render"
    $allowedFile = (Resolve-RepoPath "source/render/rhi/vulkan/rhiDeviceVulkan.cpp").ToLowerInvariant()
    $files = Get-ChildItem -LiteralPath $renderRoot -Recurse -File -Include *.h,*.cpp
    foreach ($file in $files) {
        $matches = Select-String -LiteralPath $file.FullName -Pattern "VulkanManager::GetInstance\("
        if ($matches.Count -eq 0) {
            continue
        }

        if ($file.FullName.ToLowerInvariant() -ne $allowedFile) {
            foreach ($match in $matches) {
                $relative = Resolve-Path -LiteralPath $file.FullName -Relative
                Add-Failure $Failures "VulkanManager boundary: '${relative}:$($match.LineNumber)' bypasses RHIDeviceVulkan."
            }
        }
    }
}

function Test-RendererBackendRawGetterBoundary {
    param([System.Collections.Generic.List[string]]$Failures)

    $sourceRoot = Resolve-RepoPath "source"
    $files = Get-ChildItem -LiteralPath $sourceRoot -Recurse -File -Include *.h,*.cpp
    foreach ($file in $files) {
        $matches = Select-String `
            -LiteralPath $file.FullName `
            -Pattern 'rendererBackend(\.|->)Get(Device|GpuMemoryProperties)\('
        foreach ($match in $matches) {
            $relative = Resolve-Path -LiteralPath $file.FullName -Relative
            Add-Failure $Failures "RendererBackend raw getter boundary: '${relative}:$($match.LineNumber)' pulls raw Vulkan device state from the backend facade."
        }
    }
}

function Test-RendererSubmitBoundary {
    param([System.Collections.Generic.List[string]]$Failures)

    $sourceRoot = Resolve-RepoPath "source"
    $allowedFile = (Resolve-RepoPath "source/render/backend/rendererBackendVulkan.cpp").ToLowerInvariant()
    $files = Get-ChildItem -LiteralPath $sourceRoot -Recurse -File -Include *.h,*.cpp
    foreach ($file in $files) {
        if ($file.FullName.ToLowerInvariant() -eq $allowedFile) {
            continue
        }

        $matches = Select-String `
            -LiteralPath $file.FullName `
            -Pattern 'RendererSubmitPlan', 'BuildSubmitPlan\(', 'SubmitAndPresent\('
        foreach ($match in $matches) {
            $relative = Resolve-Path -LiteralPath $file.FullName -Relative
            Add-Failure $Failures "Renderer submit boundary: '${relative}:$($match.LineNumber)' exposes backend-owned submit/present details outside RendererBackendVulkan."
        }
    }
}

$failures = [System.Collections.Generic.List[string]]::new()

Test-PathsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/sceneLoader.h",
        "source/sceneLoader.cpp",
        "source/lightManager.h",
        "source/lightManager.cpp"
    ) `
    -RuleName "Legacy scene/light singleton cutover"

Test-SourcePatternsAbsent `
    -Failures $failures `
    -Patterns @(
        '#include\s+"sceneLoader\.h"',
        '#include\s+"lightManager\.h"',
        '\bSceneLoader\b',
        '\bLightManager\b'
    ) `
    -RuleName "Legacy scene/light singleton references"

Test-SourcePatternsAbsent `
    -Failures $failures `
    -Patterns @(
        'TODO',
        'FIXME',
        '\bmaybe\b',
        '\u8FD9\u91CC\u6709\u95EE\u9898',
        '\u770B\u540E\u7EED',
        '\u53EF\u80FD\u9700\u8981',
        '\u9700\u8981\u5904\u7406\s+sdl',
        '\u5E94\u8BE5\u5148\u68C0\u6D4B',
        '\u76EE\u524D\u6682\u65F6',
        '\u4E34\u65F6'
    ) `
    -RuleName "Source comment clarity"

# Draw execution, pass runtime, and resolved scene data are on the render side
# of the boundary. They may consume frozen draw packets and backend resource
# entries, but must not pull mutable World or SceneObject data back in.
$drawFrameFiles = @(
    "source/render/backend/rendererDrawExecutor.h",
    "source/render/backend/rendererDrawExecutor.cpp",
    "source/render/pass/passRuntime.h",
    "source/render/pass/passRuntime.cpp",
    "source/render/backend/resolvedRenderScene.h",
    "source/render/backend/resolvedRenderScene.cpp"
)

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths $drawFrameFiles `
    -Patterns @(
        '#include\s+"sceneObject\.h"',
        '#include\s+"world/world\.h"',
        '\bSceneObject\b',
        '\bWorld\b',
        'GetSceneObject\(',
        'GetSceneObjects\(',
        '->\s*GetRenderableObject\(',
        '->\s*GetMaterialInstance\(',
        'SnapshotPreviousModelMatrix\(',
        'GetPreviousModelMatrix\('
    ) `
    -RuleName "Draw frame boundary"

# RenderSystem may coordinate the frame handoff, but object GPU resource lookup
# and binding belong to RendererObjectResourceRegistry/backend ownership.
Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @("source/renderSystem.cpp", "source/renderSystem.h") `
    -Patterns @(
        'CapturePreviousFrameTransforms',
        'SnapshotPreviousModelMatrix\(',
        'GetPreviousModelMatrix\(',
        'GetObjectResource\(',
        'BindObjectResource\(',
        'GetSceneObject\(',
        'GetSceneObjects\(',
        'draw\.object(?!ResourceEntry)'
    ) `
    -RuleName "RenderSystem frame boundary"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/world/worldSnapshotBuilder.h" `
    -Patterns @(
        'previousObjectModels'
    ) `
    -RuleName "Snapshot previous model ownership"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/render/backend/resolvedRenderScene.h" `
    -Patterns @(
        'objectResourceEntry'
    ) `
    -RuleName "Resolved draw resource ownership"

# Runtime validation hooks must apply pressure through CommandBus. If they call
# world loading or renderer internals directly, reloadstress stops proving the
# same path that users and console commands exercise.
Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/engine/runtimeTestHooks.h",
        "source/engine/runtimeTestHooks.cpp"
    ) `
    -Patterns @(
        '#include\s+"world/loading/worldTransitionCoordinator\.h"',
        '#include\s+"renderSystem\.h"',
        '#include\s+"renderGraph\.h"',
        'RequestWorldLoad\(',
        'RenderSystem::',
        'RenderGraph::'
    ) `
    -RuleName "Runtime test hook command boundary"

# RHIDevice is the public device-facing contract. Concrete Vulkan internals may
# still use raw handles while the migration is in flight, but the abstract RHI
# surface must not hand those escape hatches to higher layers.
Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @("source/render/rhi/rhiDevice.h") `
    -Patterns @(
        'virtual\s+vk::Device&\s+GetDevice\(',
        'virtual\s+vk::PhysicalDevice&\s+GetPhysicalDevice\(',
        'virtual\s+vk::PhysicalDeviceMemoryProperties&\s+GetGpuMemoryProperties\(',
        'virtual\s+vk::Queue&\s+GetGraphicsQueue\(',
        'virtual\s+vk::CommandPool&\s+GetCommandPool\(',
        'virtual\s+std::vector<vk::CommandBuffer>&\s+GetCommandBuffers\(',
        'virtual\s+vk::SwapchainKHR&\s+GetSwapchain\(',
        'virtual\s+std::vector<vk::Fence>&\s+GetTaskFinishedFences\(',
        'virtual\s+std::vector<vk::Semaphore>&\s+GetImageAcquiredSemaphores\(',
        'virtual\s+std::vector<vk::Semaphore>&\s+GetRenderFinishedSemaphores\(',
        'virtual\s+std::vector<vk::Fence>&\s+GetImagesInFlightFences\('
    ) `
    -RuleName "RHIDevice raw getter contract"

# RendererBackendVulkan owns the concrete RHIDeviceVulkan. Higher layers should
# ask the backend for intentional operations, not take a generic RHI reference
# and tunnel into low-level device state.
Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @("source/render/backend/rendererBackendVulkan.h") `
    -Patterns @(
        'GetRhiDevice\('
    ) `
    -RuleName "RendererBackend RHI escape hatch"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/main.cpp" `
    -Patterns @(
        '--reloadstress',
        '--reloadfail',
        '--reloadfail-material',
        '--lightstress',
        '--resizestress',
        '--graphreloadstress',
        '--framesmoke',
        '--exit-after-tests',
        'QueueRuntimeCommand\('
    ) `
    -RuleName "Automated runtime validation entry"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/engineLoop.cpp" `
    -Patterns @(
        'StartFrameSmokeTest',
        'UpdateFrameSmokeTest',
        'avgFrameMs',
        'avgFps',
        'Frame smoke test completed'
    ) `
    -RuleName "Frame smoke runtime validation"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/engineLoop.cpp" `
    -Patterns @(
        'RecreateRendererForWindowResize',
        'ReleaseSwapchainDependentResources',
        'RebuildSwapchainDependentResources',
        'Resize stress completed'
    ) `
    -RuleName "Swapchain resize runtime validation"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/engineLoop.cpp" `
    -Patterns @(
        'ReloadRenderGraphResources',
        'RenderGraphReleaseMode::Retire',
        'Render graph reload stress',
        'retired graph resources did not drain'
    ) `
    -RuleName "Render graph reload retire validation"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/renderGraph.cpp" `
    -Patterns @(
        'RetiredRenderGraphImageResource',
        'RetiredRenderGraphPassResource',
        'ResourceRetireQueue::GetInstance\(\)\.RetireShared',
        'RenderGraphReleaseMode::Retire'
    ) `
    -RuleName "Render graph resource retire ownership"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/runtimeTestHooks.cpp" `
    -Patterns @(
        'World reload failure rollback test',
        'CreateGeneratedMaterialFailureScene',
        'CreateGeneratedHighLightStressScene',
        'activeWorldBeforeCommand',
        'activeWorldAfterCommand',
        'SameRendererResourceFingerprint',
        'rendererResourcesBeforeLoad',
        'pass material bindings preserved'
    ) `
    -RuleName "World reload failure rollback validation"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/runtimeTestHooks.cpp" `
    -Patterns @(
        'RetireDrainFrameBudget',
        'maxPendingRetiredResources',
        'World reload stress waiting for retire queue drain',
        'retired world-local resources did not drain'
    ) `
    -RuleName "World reload retire queue validation"

Test-VulkanManagerBoundary -Failures $failures
Test-RendererBackendRawGetterBoundary -Failures $failures
Test-RendererSubmitBoundary -Failures $failures

if ($failures.Count -gt 0) {
    Write-Host "UE-Lite boundary audit failed:" -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host "UE-Lite boundary audit passed." -ForegroundColor Green
