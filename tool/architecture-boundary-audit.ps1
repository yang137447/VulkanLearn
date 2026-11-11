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

function Test-DirectoryPatternsAbsent {
    param(
        [System.Collections.Generic.List[string]]$Failures,
        [string]$RelativeDirectory,
        [string[]]$Patterns,
        [string]$RuleName
    )

    $directory = Resolve-RepoPath $RelativeDirectory
    if (!(Test-Path -LiteralPath $directory)) {
        Add-Failure $Failures "${RuleName}: missing expected directory '$RelativeDirectory'."
        return
    }

    $files = Get-ChildItem -LiteralPath $directory -Recurse -File -Include *.h,*.cpp
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

function Test-RuntimeValidationAdapterBoundary {
    param([System.Collections.Generic.List[string]]$Failures)

    $adapterPath = (
        Resolve-RepoPath "source/engine/testing/runtimeValidationServices.cpp"
    ).ToLowerInvariant()
    if (!(Test-Path -LiteralPath $adapterPath)) {
        Add-Failure $Failures "Runtime validation adapter boundary: missing explicit adapter 'source/engine/testing/runtimeValidationServices.cpp'."
        return
    }

    $files = @()
    $files += Get-Item -LiteralPath (
        Resolve-RepoPath "source/engine/runtimeTestHooks.h"
    )
    $files += Get-Item -LiteralPath (
        Resolve-RepoPath "source/engine/runtimeTestHooks.cpp"
    )
    $files += Get-ChildItem `
        -LiteralPath (Resolve-RepoPath "source/engine/testing") `
        -Recurse `
        -File `
        -Include *.h,*.cpp

    $patterns = @(
        '#include\s+"renderSystem\.h"',
        '#include\s+"renderGraph\.h"',
        '\bRenderSystem::',
        '\bRenderGraph::',
        '\bRenderSystem\s*[*&]',
        '\bRenderGraph\s*[*&]'
    )
    foreach ($file in $files) {
        if ($file.FullName.ToLowerInvariant() -eq $adapterPath) {
            continue
        }

        foreach ($pattern in $patterns) {
            $matches = Select-String -LiteralPath $file.FullName -Pattern $pattern
            foreach ($match in $matches) {
                $relative = Resolve-Path -LiteralPath $file.FullName -Relative
                Add-Failure $Failures "Runtime validation adapter boundary: '${relative}:$($match.LineNumber)' bypasses RuntimeValidationServices with '$pattern'."
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
            Add-Failure $Failures "${RuleName}: removed path '$relativePath' still exists."
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

function Test-AsyncRenderLoopNoImmediateIdleWait {
    param(
        [System.Collections.Generic.List[string]]$Failures,
        [string]$RuleName = "Bounded async render loop no immediate idle wait"
    )

    $relativePath = "source/engine/engineLoop.cpp"
    $path = Resolve-RepoPath $relativePath
    if (!(Test-Path -LiteralPath $path)) {
        Add-Failure $Failures "${RuleName}: missing expected file '$relativePath'."
        return
    }

    # The async branch publishes and returns to Tick. A per-frame
    # WaitForRenderThreadIdle() directly after SubmitFrame() recreates the
    # synchronous pipeline under a different name, so forbid the sequence while
    # leaving the explicit mutation safe points in the same file untouched.
    $text = Get-Content -LiteralPath $path -Raw
    $pattern = 'renderThread->SubmitFrame\(\)\s*;\s*(?:\r?\n[^\r\n]*){0,3}\r?\n[^\r\n]*WaitForRenderThreadIdle\(\)'
    $match = [regex]::Match($text, $pattern)
    if ($match.Success) {
        $line = ($text.Substring(0, $match.Index) -split "`n").Count
        Add-Failure $Failures "${RuleName}: '${relativePath}:${line}' waits for the render thread immediately after SubmitFrame(); use WaitForRenderFrameSlot() before publishing instead."
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
                Add-Failure $Failures "VulkanManager boundary: '${relative}:$($match.LineNumber)' bypasses the Vulkan device boundary."
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

function Test-RHIDeviceVulkanPublicRawGetterBoundary {
    param([System.Collections.Generic.List[string]]$Failures)

    $relativePath = "source/render/rhi/vulkan/rhiDeviceVulkan.h"
    $path = Resolve-RepoPath $relativePath
    if (!(Test-Path -LiteralPath $path)) {
        Add-Failure $Failures "Vulkan device raw getter privacy: missing expected file '$relativePath'."
        return
    }

    $content = Get-Content -LiteralPath $path -Raw
    $classIndex = $content.IndexOf("class RHIDeviceVulkan")
    if ($classIndex -lt 0) {
        Add-Failure $Failures "Vulkan device raw getter privacy: '$relativePath' is missing class RHIDeviceVulkan."
        return
    }

    $classContent = $content.Substring($classIndex)
    $publicIndex = $classContent.IndexOf("public:")
    $privateIndex = $classContent.IndexOf("private:")
    if ($publicIndex -lt 0 -or $privateIndex -lt 0 -or $privateIndex -le $publicIndex) {
        Add-Failure $Failures "Vulkan device raw getter privacy: '$relativePath' has an unexpected public/private layout."
        return
    }

    $publicSection = $classContent.Substring($publicIndex, $privateIndex - $publicIndex)
    $forbiddenPatterns = @(
        'vk::Device&\s+GetDevice\(',
        'vk::PhysicalDevice&\s+GetPhysicalDevice\(',
        'vk::PhysicalDeviceMemoryProperties&\s+GetGpuMemoryProperties\(',
        'vk::CommandPool&\s+GetCommandPool\(',
        'vk::Queue&\s+GetGraphicsQueue\(',
        'vk::SwapchainKHR&\s+GetSwapchain\(',
        'std::vector<vk::CommandBuffer>&\s+GetCommandBuffers\(',
        'std::vector<vk::Fence>&\s+GetTaskFinishedFences\(',
        'std::vector<vk::Semaphore>&\s+GetImageAcquiredSemaphores\(',
        'std::vector<vk::Semaphore>&\s+GetRenderFinishedSemaphores\(',
        'std::vector<vk::Fence>&\s+GetImagesInFlightFences\('
    )
    foreach ($pattern in $forbiddenPatterns) {
        if ($publicSection -match $pattern) {
            Add-Failure $Failures "Vulkan device raw getter privacy: '$relativePath' public contract exposes forbidden pattern '$pattern'."
        }
    }
}

function Test-VulkanManagerPublicBoundary {
    param([System.Collections.Generic.List[string]]$Failures)

    $relativePath = "source/VulkanManager.h"
    $path = Resolve-RepoPath $relativePath
    if (!(Test-Path -LiteralPath $path)) {
        Add-Failure $Failures "VulkanManager public boundary: missing expected file '$relativePath'."
        return
    }

    $content = Get-Content -LiteralPath $path -Raw
    if ($content -notmatch 'friend\s+class\s+VL::RHIDeviceVulkan;') {
        Add-Failure $Failures "VulkanManager public boundary: '$relativePath' must expose low-level Vulkan state only to RHIDeviceVulkan."
    }

    $classIndex = $content.IndexOf("class VulkanManager")
    if ($classIndex -lt 0) {
        Add-Failure $Failures "VulkanManager public boundary: '$relativePath' is missing class VulkanManager."
        return
    }

    $classContent = $content.Substring($classIndex)
    $publicIndex = $classContent.IndexOf("public:")
    $privateIndex = $classContent.IndexOf("private:")
    if ($publicIndex -lt 0 -or $privateIndex -lt 0 -or $privateIndex -le $publicIndex) {
        Add-Failure $Failures "VulkanManager public boundary: '$relativePath' has an unexpected public/private layout."
        return
    }

    $publicSection = $classContent.Substring($publicIndex, $privateIndex - $publicIndex)
    $forbiddenPatterns = @(
        '~VulkanManager\s*\(',
        '\bInit\s*\(',
        '\bReCreateSwapChain\s*\(',
        '\bGetDevice\s*\(',
        '\bGetPhysicalDevice\s*\(',
        '\bGetGpuMemoryProperties\s*\(',
        '\bGetCommandPool\s*\(',
        '\bGetCommandBuffers\s*\(',
        '\bGetGraphicQueue\s*\(',
        '\bGetGraphicsQueue\s*\(',
        '\bGetTaskFinishedFences\s*\(',
        '\bGetSwapChain\s*\(',
        '\bGetImageAcquiredSemaphores\s*\(',
        '\bGetRenderFinishedSemaphores\s*\(',
        '\bGetImagesInFlightFences\s*\(',
        '\bGetSwapChainImageCount\s*\(',
        '\bGetSurfaceFormat\s*\(',
        '\bGetSwapChainImageViews\s*\(',
        '\bGetSwapChainExtent\s*\('
    )
    foreach ($pattern in $forbiddenPatterns) {
        if ($publicSection -match $pattern) {
            Add-Failure $Failures "VulkanManager public boundary: '$relativePath' public contract exposes forbidden pattern '$pattern'."
        }
    }
}

$failures = [System.Collections.Generic.List[string]]::new()

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "documents/README.md",
        "documents/contracts/vulkanlearn-architecture.html",
        "documents/contracts/descriptor-imageinfo-management.md",
        "documents/plan/architecture/ue-lite-completion-plan.md"
    ) `
    -Patterns @(
        '(?-i)(?<![A-Za-z0-9_])RHI(?![A-Za-z0-9_])'
    ) `
    -RuleName "Vulkan-native architecture wording"

# '\bUE-Lite\b' / '\bUE_LITE\b' are banned in source/** by "Source comment clarity",
# but that rule never looks at documentation, so the legacy route name could be
# reintroduced into living docs unchallenged. Two documents are deliberately excluded
# because they must be able to name the thing they describe:
#   - the architecture HTML *defines* the deprecation ("UE-Lite is only the earlier
#     architecture route name; new C++ engine code uses the VL namespace");
#   - ue-lite-completion-plan.md is the historical record of that route and mentions
#     it 29 times as its subject matter.
# Excluding them keeps this rule from failing permanently, because a gate that is
# always red is a gate people learn to ignore -- which is worse than the coverage gap.
Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "documents/README.md",
        "documents/contracts/descriptor-imageinfo-management.md"
    ) `
    -Patterns @(
        '\bUE-Lite\b',
        '\bUE_LITE\b'
    ) `
    -RuleName "VL namespace naming in documents"

Test-PathsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/sceneLoader.h",
        "source/sceneLoader.cpp",
        "source/lightManager.h",
        "source/lightManager.cpp"
    ) `
    -RuleName "Removed scene/light singleton cutover"

Test-SourcePatternsAbsent `
    -Failures $failures `
    -Patterns @(
        '#include\s+"sceneLoader\.h"',
        '#include\s+"lightManager\.h"',
        '\bSceneLoader\b',
        '\bLightManager\b'
    ) `
    -RuleName "Removed scene/light singleton references"

Test-PathsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/render.h",
        "source/render.cpp",
        "source/renderProcess.h",
        "source/renderProcess.cpp",
        "source/swapchain.h",
        "source/swapchain.cpp",
        "source/shaderModule.h",
        "source/shaderModule.cpp",
        "source/triangleData.h",
        "source/triangleData.cpp",
        "source/TriangleData.h",
        "source/TriangleData.cpp"
    ) `
    -RuleName "Removed tutorial render path cutover"

Test-SourcePatternsAbsent `
    -Failures $failures `
    -Patterns @(
        '#include\s+"render\.h"',
        '#include\s+"renderProcess\.h"',
        '#include\s+"swapchain\.h"',
        '#include\s+"shaderModule\.h"',
        '#include\s+"triangleData\.h"',
        '\bRenderProcess\b',
        '\bTriangleData\b',
        '(?-i)\bQueueFamilyIndices\b',
        '\bclass\s+Render\b',
        '\bclass\s+Swapchain\b',
        '\bclass\s+ShaderModule\b'
    ) `
    -RuleName "Removed tutorial render path references"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/VulkanManager.h",
        "source/vulkanManager.cpp",
        "source/pipeline/graphicsPipeline.h"
    ) `
    -Patterns @(
        '\bDrawableObject\b',
        '\bGetCommandBufferBeginInfo\s*\(',
        '\bcommandBufferBeginInfo\b',
        # A by-reference submit parameter is not a cached submit path; the old
        # VulkanManager member (a stored vk::SubmitInfo) is what this forbids.
        '\bvk::SubmitInfo\s+\w+\s*;',
        '\bpiplineStageFlags\b'
    ) `
    -RuleName "Removed VulkanManager cached submit path"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @("source/VulkanManager.h") `
    -Patterns @(
        '<SDL3/SDL\.h>',
        '\bGetSampleCount\s*\(',
        '\bvk::SampleCountFlagBits\s+sampleCount\b',
        '\bInitInstance\s*\(',
        '\buint32_t\s+gpuCount\b',
        '\buint32_t\s+queueFamilyCount\b',
        '\bqueueFamilyProperties\b',
        '\bvk::Queue\s+presentQueue\b',
        '\bsurfaceFormats\b',
        '\bsurfaceCapabilities\b',
        '\bpresentModes\b',
        '\bswapChainImages\b',
        '\bGPUIndex\b',
        '\bGetGraphicQueue\s*\(',
        '\bgraphicQueueFamilyIndex\b',
        '\bvk::Queue\s+graphicQueue\b',
        '\bgraohics\b',
        '\bqueque\b'
    ) `
    -RuleName "VulkanManager private platform dependency boundary"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/VulkanManager.h",
        "source/vulkanManager.cpp",
        "source/render/rhi/vulkan/rhiDeviceVulkan.cpp"
    ) `
    -Patterns @(
        '\bGPUIndex\b',
        '\bGetGraphicQueue\s*\(',
        '\bgraphicQueueFamilyIndex\b',
        '\bvk::Queue\s+graphicQueue\b',
        '\bgraohics\b',
        '\bqueque\b'
    ) `
    -RuleName "VulkanManager historical naming cleanup"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @("source/commonFunction.h") `
    -Patterns @(
        '\bCreateDepthImage\s*\(',
        '\bCreateDepthImageView\s*\(',
        '\bGetInitScene\s*\(',
        '\bParserRenderResourceSize\s*\(',
        '\bGetDeltaTime\s*\(',
        '\bFindSupportedFormat\s*\(',
        '\bFindDepthFormat\s*\('
    ) `
    -RuleName "CommonFunction depth helper cleanup"

Test-SourcePatternsAbsent `
    -Failures $failures `
    -Patterns @(
        'TODO',
        'FIXME',
        '\bUE-Lite\b',
        '\bUE_LITE\b',
        '\bmigration\b',
        '\bLegacy\b',
        '\blegacy\b',
        '\bmaybe\b',
        '\u8FD9\u91CC\u6709\u95EE\u9898',
        '\u770B\u540E\u7EED',
        '\u53EF\u80FD\u9700\u8981',
        '\u9700\u8981\u5904\u7406\s+sdl',
        '\u5E94\u8BE5\u5148\u68C0\u6D4B',
        '\u76EE\u524D\u6682\u65F6',
        # 裸词 '\u4E34\u65F6'（临时）会误伤正常的技术描述，例如
        # "临时转移焦点"、"临时视口标记"、"临时记忆"、"临时场景" —— 2026-10-11 实测有 4 处
        # 这样的合法注释被拦成门禁失败。收紧为 TODO 式搭配，保留原意（注释里不许留待办）
        # 而不拦截普通用词。
        '\u4E34\u65F6\u65B9\u6848',
        '\u4E34\u65F6\u4EE3\u7801',
        '\u4E34\u65F6\u5904\u7406',
        '\u4E34\u65F6\u7ED5\u8FC7'
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

Test-DirectoryPatternsAbsent `
    -Failures $failures `
    -RelativeDirectory "source/render/pass" `
    -Patterns @(
        '#include\s+"sceneObject\.h"',
        '#include\s+"world/',
        '\bSceneObject\s*[*&>]',
        '\bWorld\s*[*&>]',
        'GetSceneObject\(',
        'GetSceneObjects\('
    ) `
    -RuleName "Render pass gameplay boundary"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/render/backend/rendererDescriptorContext.h",
        "source/render/backend/rendererDescriptorWriter.h",
        "source/render/backend/rendererDescriptorWriter.cpp",
        "source/render/backend/rendererDescriptorPlan.h",
        "source/render/backend/rendererDescriptorPlan.cpp"
    ) `
    -Patterns @(
        '#include\s+"renderSystem\.h"',
        '#include\s+"sceneObject\.h"',
        '#include\s+"world/',
        '\bRenderSystem\s*[*&>]',
        'RenderSystem::',
        '\bSceneObject\s*[*&>]',
        '\bWorld\s*[*&>]'
    ) `
    -RuleName "Descriptor writer explicit context boundary"

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

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/render/resource/rendererResourceCache.h",
        "source/render/resource/rendererResourceCache.cpp",
        "source/render/resource/rendererMeshLoader.h",
        "source/render/resource/rendererMeshLoader.cpp",
        "source/world/world.h",
        "source/world/world.cpp",
        "source/world/loading/worldBuilder.cpp",
        "source/world/worldSnapshotBuilder.cpp"
    ) `
    -Patterns @(
        '#include\s+"sceneObject\.h"',
        '\bSceneObject\s*[*&>]',
        'std::shared_ptr<SceneObject>',
        'sceneObjects',
        'BindSceneObject\(',
        'GetSceneObject\(',
        'GetSceneObjects\(',
        'AddSceneObject\('
    ) `
    -RuleName "World mesh object cutover"

Test-PathsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/sceneObject.h",
        "source/sceneObject.cpp"
    ) `
    -RuleName "SceneObject wrapper file cutover"

Test-SourcePatternsAbsent `
    -Failures $failures `
    -Patterns @(
        '\bSceneObjectBuildPlan\b',
        '\bRendererMeshObjectBinding\b',
        '\bmeshObjectBindings\b',
        'BindMeshObjectBinding\(',
        'GetMeshObjectBinding\(',
        'GetMeshObjectBindings\('
    ) `
    -RuleName "Renderer cache mesh binding cutover"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/scene/sceneAssetTypes.h" `
    -Patterns @(
        'struct MeshObjectBuildPlan'
    ) `
    -RuleName "Mesh object build plan contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/world/worldBuildPlan.h" `
    -Patterns @(
        'struct WorldBuildPlan',
        'WorldGameplayPlans gameplay;',
        'struct WorldAssemblyAssets',
        'std::unordered_map<std::string, ResolvedMaterialBinding> materialBindings;'
    ) `
    -RuleName "World build plan pure data contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/world/loading/worldGameplayPlanBuilder.h" `
    -Patterns @(
        'class WorldGameplayPlanBuilder',
        'RuntimeResult<WorldGameplayPlans> Build\('
    ) `
    -RuleName "World gameplay plan builder contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/render/resource/rendererResourceLoadCoordinator.h" `
    -Patterns @(
        'using RendererWorldResourceLoadResult = WorldAssemblyAssets',
        'LoadRendererResources\(',
        'const WorldBuildPlan& worldBuildPlan'
    ) `
    -RuleName "Renderer resource load result contract"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/render/resource/rendererResourceLoadCoordinator.cpp"
    ) `
    -Patterns @(
        '\bsoftBodyObjectPlans\b',
        '\bglassObjectPlans\b',
        '\bprojectileSettings\b',
        '\bSoftBodyObjectBuildPlan\b',
        '\bGlassObjectBuildPlan\b',
        '#include\s+"world/loading/worldLoader\.h"'
    ) `
    -RuleName "Renderer gameplay plan boundary"

Test-DirectoryPatternsAbsent `
    -Failures $failures `
    -RelativeDirectory "source/render" `
    -Patterns @(
        '\bsoftBodyObjectPlans\b',
        '\bglassObjectPlans\b',
        '\bprojectileSettings\b',
        '#include\s+"world/loading/worldLoader\.h"'
    ) `
    -RuleName "Renderer world loader include boundary"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/worldCandidatePreparer.h" `
    -Patterns @(
        'class WorldCandidatePreparer',
        'RuntimeResult<PreparedWorldTransition> Prepare\('
    ) `
    -RuleName "World transaction engine ownership contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/preparedWorldTransition.h" `
    -Patterns @(
        'struct PreparedWorldTransition'
    ) `
    -RuleName "Prepared world transition contract"

Test-PathsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/world/loading/worldTransitionCoordinator.h",
        "source/world/loading/worldTransitionCoordinator.cpp",
        "source/world/loading/worldGraphTransactionCoordinator.h",
        "source/world/loading/worldGraphTransactionCoordinator.cpp"
    ) `
    -RuleName "World transaction ownership cutover"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/world/loading/worldBuilder.h" `
    -Patterns @(
        'const WorldBuildPlan& worldBuildPlan',
        'WorldAssemblyAssets assemblyAssets'
    ) `
    -RuleName "WorldBuilder assembly assets contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/world/loading/worldBuilder.cpp" `
    -Patterns @(
        'worldBuildPlan\.gameplay',
        'assemblyAssets\.meshObjectPlans',
        'WorldBuilder\.MaterialBindingMissing',
        'world->AddMeshObject\('
    ) `
    -RuleName "WorldBuilder mesh object plan consumption"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/engineLoop.cpp" `
    -Patterns @(
        'WaitForRenderFrameSlot\(\)',
        'SetRendererMutationSafePoint\('
    ) `
    -RuleName "Bounded async render loop contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/frame/engineFramePhases.cpp" `
    -Patterns @(
        'renderThread->SubmitFrame\(\)'
    ) `
    -RuleName "Bounded async render loop submit contract"

Test-AsyncRenderLoopNoImmediateIdleWait `
    -Failures $failures `
    -RuleName "Bounded async render loop no immediate idle wait"

# Runtime validation hooks and feature tests must apply pressure through
# CommandBus or the one explicit adapter. Only RuntimeValidationServices.cpp
# may touch RenderSystem/RenderGraph; allowing the testing directory as a whole
# would recreate the owner leak that this boundary closes.
Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/engine/runtimeTestHooks.h",
        "source/engine/runtimeTestHooks.cpp"
    ) `
    -Patterns @(
        '#include\s+"engine/worldCandidatePreparer\.h"',
        '#include\s+"renderSystem\.h"',
        '#include\s+"renderGraph\.h"',
        'RequestWorldLoad\(',
        'RenderSystem::',
        'RenderGraph::'
    ) `
    -RuleName "Runtime test hook command boundary"

Test-RuntimeValidationAdapterBoundary -Failures $failures

# Hair 的 4 条 test-oracle / authoring-metadata 规则随 Hair 旧实现（2026-09-12 清理）删除：
# 它们断言的对象（source/render/hair/hairReference.*、hairGBufferCodec.*、hairAssets.cpp、
# hairLutBaker.cpp、tool/hair-tests/CMakeLists.txt）已不存在，其中 "Hair test oracle support
# boundary" 会在文件缺失时直接失败。重建 Hair 时如需恢复边界断言，按新实现重新写规则。

# RHIDeviceVulkan is the concrete Vulkan device-facing boundary. The old abstract
# RHIDevice interface was removed because Vulkan is the only graphics API and a
# single-implementation virtual layer was redundant historical architecture.
Test-PathsAbsent `
    -Failures $failures `
    -RelativePaths @("source/render/rhi/rhiDevice.h") `
    -RuleName "Redundant Vulkan device abstraction cutover"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @("source/render/rhi/vulkan/rhiDeviceVulkan.h") `
    -Patterns @(
        'class\s+RHIDevice\b',
        ':\s*public\s+RHIDevice',
        '\bvirtual\b',
        '\boverride\b'
    ) `
    -RuleName "Vulkan device boundary concrete-only contract"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @("source/render/rhi/vulkan/rhiDeviceVulkan.h") `
    -Patterns @(
        'std::pair<vk::Buffer,\s*vk::DeviceMemory>\s+CreateBuffer\(',
        'MapMemory\(vk::DeviceMemory',
        'UnmapMemory\(vk::DeviceMemory',
        'DestroyBuffer\(vk::Buffer',
        'CopyBufferToBuffer\(vk::Buffer'
    ) `
    -RuleName "Vulkan device boundary buffer lifecycle handle contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/render/rhi/vulkan/rhiDeviceVulkan.h" `
    -Patterns @(
        'RHIBufferHandle CreateBuffer\(',
        'MapBufferMemory\(RHIBufferHandle',
        'UnmapBufferMemory\(RHIBufferHandle',
        'DestroyBuffer\(RHIBufferHandle',
        'RHIBufferHandle source'
    ) `
    -RuleName "Vulkan device boundary buffer lifecycle handle adoption"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/BaseStructs.h" `
    -Patterns @(
        'struct Buffer',
        'std::vector<VL::RHIBufferHandle> bufferHandles',
        'bool HasResources\(\) const'
    ) `
    -RuleName "Buffer resource package lifecycle handle adoption"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/texture.h" `
    -Patterns @(
        'VL::RHIImageHandle imageHandle',
        'VL::RHIImageViewHandle imageViewHandle',
        'VL::RHISamplerHandle samplerHandle',
        'GetImageHandle\(\) const',
        'GetImageViewHandle\(\) const',
        'GetSamplerHandle\(\) const'
    ) `
    -RuleName "Texture resource package lifecycle handle adoption"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/resource/device/deviceTextureFactory.h" `
    -Patterns @(
        'struct DeviceTextureResource',
        'VL::RHIImageHandle imageHandle',
        'CreateResourceFromHostImage\('
    ) `
    -RuleName "Device texture factory lifecycle handle result"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/render/backend/rendererBackendVulkan.h" `
    -Patterns @(
        'RHIImageHandle GetImageHandle\(',
        'RHIImageViewHandle GetImageViewHandle\(',
        'RHISamplerHandle GetSamplerHandle\(',
        'RHIImageHandle& imageHandle',
        'RHIDescriptorSetLayoutHandle GetDescriptorSetLayoutHandle\(',
        'RHIDescriptorPoolHandle GetDescriptorPoolHandle\(',
        'RHIDescriptorSetHandle GetDescriptorSetHandle\(',
        'RHIRenderPassHandle GetRenderPassHandle\(',
        'RHIFramebufferHandle GetFramebufferHandle\('
    ) `
    -RuleName "Backend texture handle bridge contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/renderGraph.h" `
    -Patterns @(
        'VL::RHIImageHandle imageHandle',
        'VL::RHIImageViewHandle imageViewHandle',
        'VL::RHISamplerHandle samplerHandle',
        'VL::RHIRenderPassHandle renderPassHandle',
        'std::vector<VL::RHIFramebufferHandle> framebufferHandles',
        'VL::RHIDescriptorPoolHandle descriptorPoolHandle',
        'VL::RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle',
        'std::vector<std::vector<VL::RHIDescriptorSetHandle>> descriptorSetHandles'
    ) `
    -RuleName "RenderGraph resource package lifecycle handle adoption"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/render/backend/rendererObjectGpuResources.h" `
    -Patterns @(
        'RHIDescriptorPoolHandle descriptorPoolHandle',
        'std::vector<std::vector<RHIDescriptorSetHandle>> descriptorSetHandles',
        'RHIDescriptorPoolHandle shadowDescriptorPoolHandle',
        'std::vector<std::vector<RHIDescriptorSetHandle>> shadowDescriptorSetHandles'
    ) `
    -RuleName "Object GPU resource descriptor handle adoption"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/render/rhi/rhiResourceHandles.h" `
    -Patterns @(
        'struct RHIBufferHandle',
        'struct RHIImageHandle',
        'struct RHIImageViewHandle',
        'struct RHISamplerHandle',
        'struct RHIDescriptorSetLayoutHandle',
        'struct RHIDescriptorPoolHandle',
        'struct RHIDescriptorSetHandle',
        'struct RHIRenderPassHandle',
        'struct RHIFramebufferHandle',
        'uint64_t id'
    ) `
    -RuleName "lifecycle handle definitions"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @("source/render/rhi/vulkan/rhiDeviceVulkan.h") `
    -Patterns @(
        'std::pair<vk::Image,\s*vk::DeviceMemory>\s+CreateImage\(',
        'TransitionImageLayout\(vk::Image',
        'Create2DImageView\(vk::Image',
        'CreateImageView\(vk::Image',
        'CreateCubeImageView\(vk::Image',
        'CreateCubeStorageImageView\(vk::Image',
        'CopyBufferToImage\(\s*vk::Buffer',
        'CopyImageToBuffer\(\s*vk::Image',
        'GenerateMipmaps\(vk::Image',
        'DestroyImageResource\(\s*vk::Image&'
    ) `
    -RuleName "Vulkan device boundary image lifecycle handle contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/render/rhi/vulkan/rhiDeviceVulkan.h" `
    -Patterns @(
        'RHIImageHandle CreateImage\(',
        'TransitionImageLayout\(',
        'RHIImageHandle imageHandle',
        'CopyBufferToImage\(',
        'RHIBufferHandle bufferHandle',
        'DestroyImageResource\('
    ) `
    -RuleName "Vulkan device boundary image lifecycle handle adoption"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @("source/render/rhi/vulkan/rhiDeviceVulkan.h") `
    -Patterns @(
        'virtual\s+vk::ImageView\s+Create2DImageView\(',
        'virtual\s+vk::ImageView\s+CreateImageView\(',
        'virtual\s+vk::ImageView\s+CreateCubeImageView\(',
        'virtual\s+vk::ImageView\s+CreateCubeStorageImageView\(',
        'DestroyImageView\(vk::ImageView&',
        'virtual\s+vk::Sampler\s+Create2DSampler\(',
        'virtual\s+vk::Sampler\s+CreateSampler\(',
        'virtual\s+vk::Sampler\s+CreateCubeSampler\(',
        'DestroySampler\(vk::Sampler&',
        'virtual\s+vk::Sampler\s+CreateDepthSampler\(',
        'virtual\s+vk::Sampler\s+CreateDepthCompareSampler\('
    ) `
    -RuleName "Vulkan device boundary image view sampler lifecycle handle contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/render/rhi/vulkan/rhiDeviceVulkan.h" `
    -Patterns @(
        'RHIImageViewHandle Create2DImageView\(',
        'RHIImageViewHandle CreateImageView\(',
        'RHIImageViewHandle CreateCubeImageView\(',
        'RHIImageViewHandle CreateCubeStorageImageView\(',
        'DestroyImageView\(RHIImageViewHandle',
        'RHISamplerHandle Create2DSampler\(',
        'RHISamplerHandle CreateSampler\(',
        'DestroySampler\(RHISamplerHandle'
    ) `
    -RuleName "Vulkan device boundary image view sampler lifecycle handle adoption"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @("source/render/rhi/vulkan/rhiDeviceVulkan.h") `
    -Patterns @(
        'virtual\s+vk::DescriptorSetLayout\s+CreateDescriptorSetLayout\(',
        'DestroyDescriptorSetLayout\(vk::DescriptorSetLayout&',
        'virtual\s+vk::DescriptorPool\s+CreateDescriptorPool\(',
        'DestroyDescriptorPool\(vk::DescriptorPool&',
        'AllocateDescriptorSets\(\s*const vk::DescriptorSetAllocateInfo',
        'FreeDescriptorSet\(vk::DescriptorPool',
        'UpdateDescriptorSets\(const std::vector<vk::WriteDescriptorSet>&',
        'SetDescriptorSetDebugName\(vk::DescriptorSet'
    ) `
    -RuleName "Vulkan device boundary descriptor lifecycle handle contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/render/rhi/rhiResourceHandles.h" `
    -Patterns @(
        'struct RHIDescriptorWrite',
        'RHIDescriptorSetHandle destinationSet',
        'vk::DescriptorType descriptorType',
        'std::vector<vk::DescriptorImageInfo> imageInfos',
        'std::vector<vk::DescriptorBufferInfo> bufferInfos'
    ) `
    -RuleName "Vulkan-native descriptor write type"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/render/rhi/vulkan/rhiDeviceVulkan.h" `
    -Patterns @(
        'RHIDescriptorSetLayoutHandle CreateDescriptorSetLayout\(',
        'DestroyDescriptorSetLayout\(RHIDescriptorSetLayoutHandle',
        'RHIDescriptorPoolHandle CreateDescriptorPool\(',
        'DestroyDescriptorPool\(RHIDescriptorPoolHandle',
        'std::vector<RHIDescriptorSetHandle> AllocateDescriptorSets\(',
        'RHIDescriptorPoolHandle descriptorPoolHandle',
        'FreeDescriptorSet\(',
        'RHIDescriptorSetHandle descriptorSetHandle',
        'UpdateDescriptorSets\(const std::vector<RHIDescriptorWrite>&',
        'SetDescriptorSetDebugName\('
    ) `
    -RuleName "Vulkan device boundary descriptor lifecycle handle adoption"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/render/rhi/rhiResourceHandles.h",
        "source/render/backend/rendererBackendVulkan.cpp",
        "source/render/rhi/vulkan/rhiDeviceVulkan.h",
        "source/render/rhi/vulkan/rhiDeviceVulkan.cpp"
    ) `
    -Patterns @(
        'RHIDescriptorType',
        'RHIImageLayout',
        'RHIDescriptorImageInfo',
        'RHIDescriptorBufferInfo'
    ) `
    -RuleName "Vulkan-native descriptor write contract"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @("source/render/rhi/vulkan/rhiDeviceVulkan.h") `
    -Patterns @(
        'virtual\s+vk::RenderPass\s+CreateRenderPass\(',
        'DestroyRenderPass\(vk::RenderPass&',
        'virtual\s+vk::Framebuffer\s+CreateFramebuffer\(',
        'DestroyFramebuffers\(std::vector<vk::Framebuffer>&'
    ) `
    -RuleName "Vulkan device boundary render pass framebuffer lifecycle handle contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/render/rhi/vulkan/rhiDeviceVulkan.h" `
    -Patterns @(
        'RHIRenderPassHandle CreateRenderPass\(',
        'DestroyRenderPass\(RHIRenderPassHandle',
        'RHIFramebufferHandle CreateFramebuffer\(',
        'RHIRenderPassHandle renderPassHandle',
        'DestroyFramebuffer\(RHIFramebufferHandle'
    ) `
    -RuleName "Vulkan device boundary render pass framebuffer lifecycle handle adoption"

# RendererBackendVulkan owns the concrete RHIDeviceVulkan. Higher layers should
# ask the backend for intentional operations, not take a generic device-boundary reference
# and tunnel into low-level device state.
Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @("source/render/backend/rendererBackendVulkan.h") `
    -Patterns @(
        'GetRhiDevice\('
    ) `
    -RuleName "RendererBackend device boundary escape hatch"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @("source/render/backend/rendererBackendVulkan.h") `
    -Patterns @(
        '#include\s+"render/rhi/vulkan/rhiDeviceVulkan\.h"',
        'RHIDeviceVulkan\s+rhiDevice'
    ) `
    -RuleName "RendererBackend hides concrete device boundary"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/render/backend/rendererBackendVulkan.h" `
    -Patterns @(
        'class RHIDeviceVulkan;',
        'std::unique_ptr<RHIDeviceVulkan> rhiDevice'
    ) `
    -RuleName "RendererBackend device boundary forward declaration"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/launchOptions.cpp" `
    -Patterns @(
        '--shader-reload-test',
        '--shader-compute-reload-test',
        '--world-graph-transaction-test',
        '--exit-after-tests',
        'QueueRuntimeCommand\('
    ) `
    -RuleName "Automated runtime validation entry"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @("source/engine/launchOptions.cpp") `
    -Patterns @(
        'engineLoop\.StartResizeStress\(',
        'engineLoop\.StartRenderGraphReloadStress\(',
        'engineLoop\.StartFrameSmokeTest\('
    ) `
    -RuleName "Launch runtime tests use CommandBus"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @("source/engine/engineLoop.cpp", "source/engine/engineLoop.h") `
    -Patterns @(
        'StartResizeStress',
        'UpdateResizeStress',
        'resizeStressActive',
        'StartRenderGraphReloadStress',
        'UpdateRenderGraphReloadStress',
        'graphReloadStressActive',
        'StartFrameSmokeTest',
        'UpdateFrameSmokeTest',
        'frameSmokeActive',
        'StartEnvironmentUpdateStress',
        'UpdateEnvironmentUpdateStress',
        'environmentUpdateStressPhase',
        'friend\s+class\s+RuntimeTestHooks'
    ) `
    -RuleName "Runtime test state stays out of EngineLoop"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/engineLoop.cpp" `
    -Patterns @(
        'RecreateRendererForWindowResize'
    ) `
    -RuleName "Swapchain resize production lifecycle"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/window/engineWindowLifecycle.cpp" `
    -Patterns @(
        'ReleaseSwapchainDependentResources',
        'RebuildSwapchainDependentResources'
    ) `
    -RuleName "Swapchain resize production lifecycle resources"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/engineLoop.cpp" `
    -Patterns @(
        'ReloadRenderGraphResources',
        'RenderGraphReleaseMode::Retire'
    ) `
    -RuleName "Render graph reload production lifecycle"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/renderGraphPassResources.cpp" `
    -Patterns @(
        'RetiredRenderGraphImageResource',
        'RetiredRenderGraphPassResource',
        'ResourceRetireQueue::GetInstance\(\)\.RetireShared',
        'RenderGraphReleaseMode::Retire'
    ) `
    -RuleName "Render graph resource retire ownership"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/testing/worldTransactionRuntimeTests.cpp" `
    -Patterns @(
        'BeginWorldGraphTransactionTest',
        'ValidateWorldGraphTransactionFailure',
        'CreateWorldGraphTransactionHighLightScene',
        'World/graph transaction runtime test completed'
    ) `
    -RuleName "World reload failure state machine validation"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/testing/runtimeTestFixtures.cpp" `
    -Patterns @(
        'CreateShaderReloadTestScene',
        'CreateWorldGraphTransactionHighLightScene',
        'CleanupGeneratedRuntimeFixture',
        'SameRendererResourceFingerprint'
    ) `
    -RuleName "World reload failure fixture validation"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/runtimeTestHooks.cpp" `
    -Patterns @(
        'waitingForShaderReloadTestWorld',
        'waitingForShaderComputeReloadTestWorld',
        'waitingForWorldGraphTransactionTestWorld'
    ) `
    -RuleName "World reload failure command result validation"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/runtimeCommandExecutor.cpp" `
    -Patterns @(
        'Scene.ResolvePathFailed',
        'loadWorldError'
    ) `
    -RuleName "LoadWorld path resolve runtime error contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/world/loading/worldLoader.cpp" `
    -Patterns @(
        'ClassifyWorldLoaderError',
        'Scene.LoadFailed',
        'Mesh.LoadFailed'
    ) `
    -RuleName "World loader runtime error code contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/worldCandidatePreparer.cpp" `
    -Patterns @(
        'ClassifyRendererResourceLoadError',
        'Material.LoadFailed',
        'Texture.LoadFailed',
        'Mesh.LoadFailed',
        'Hair.LoadFailed',
        'Shader.LoadFailed',
        'WorldTransition.LoadFailed'
    ) `
    -RuleName "Renderer resource load runtime error code contract"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/pipeline/computePipeline.cpp",
        "source/pipeline/graphicsPipeline.cpp",
        "source/pipeline/graphicsPipelineBuilder.cpp",
        "source/pipeline/pipelineLayoutBuilder.cpp"
    ) `
    -Patterns @(
        'assert\(result == vk::Result::eSuccess\)'
    ) `
    -RuleName "Pipeline Vulkan creation diagnostics"

Test-DirectoryPatternsAbsent `
    -Failures $failures `
    -RelativeDirectory "source/particles" `
    -Patterns @(
        '#include\s+"editor/'
    ) `
    -RuleName "Particle runtime excludes editor"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/pipeline/vulkanPipelineDiagnostics.h" `
    -Patterns @(
        'RequireVulkanPipelineSuccess',
        'vk::to_string\(result\)',
        'pipelineKind'
    ) `
    -RuleName "Pipeline Vulkan creation diagnostic context"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/testing/worldTransactionRuntimeTests.cpp" `
    -Patterns @(
        'RetireDrainFrameBudget',
        'worldGraphTransactionTestMaxPendingRetiredResources',
        'retire queue drained while an old World/resource/graph owner remained live'
    ) `
    -RuleName "World graph retire queue validation"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "tool/architecture-final-validation.ps1" `
    -Patterns @(
        'ctest',
        '--shader-reload-test',
        '--shader-compute-reload-test',
        '--world-graph-transaction-test',
        'artifacts/architecture-validation'
    ) `
    -RuleName "Architecture final validation script contract"

# Runtime/editor target ownership is declared once in CMake and enforced twice:
# the runtime source glob filters editor paths, and a configure-time read rejects
# editor-only includes before any target is linked.
Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "CMakeLists.txt" `
    -Patterns @(
        'option\(VULKANLEARN_ENABLE_EDITOR',
        'option\(VULKANLEARN_BUILD_GAME',
        'option\(VULKANLEARN_BUILD_LEGACY_MAIN',
        'if\(NOT VULKANLEARN_ENABLE_EDITOR AND NOT VULKANLEARN_BUILD_GAME\)',
        'VULKANLEARN_ENABLE_EDITOR and VULKANLEARN_BUILD_GAME cannot both be OFF'
    ) `
    -RuleName "Runtime editor target ownership"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/CMakeLists.txt" `
    -Patterns @(
        'list\(FILTER sources EXCLUDE REGEX "/source/editor/"\)',
        'list\(FILTER sources EXCLUDE REGEX "/source/render/editor/"\)',
        'editorThumbnailRenderSession',
        '/source/main\\\\.cpp',
        '/source/main_editor',
        '/source/main_game',
        'file\(READ "\$\{runtime_source\}" runtime_source_text\)',
        'message\(FATAL_ERROR',
        'vl_runtime source',
        'add_library\(vl_runtime STATIC \$\{sources\}\)',
        'add_library\(vl_editor_asset_core STATIC \$\{editor_asset_core_sources\}\)',
        'add_library\(vl_editor STATIC \$\{editor_sources\}\)',
        'list\(FILTER editor_sources EXCLUDE REGEX "/source/editor/\(command|service|persistence\)/"\)',
        'add_executable\(main_editor main_editor\.cpp\)',
        'add_executable\(main_game main_game\.cpp\)'
    ) `
    -RuleName "Runtime editor target ownership"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/main_game.cpp" `
    -Patterns @(
        '#include "engine/nullEditorHost\.h"',
        'std::make_unique<VL::NullEditorHost>\(\)'
    ) `
    -RuleName "Game executable link boundary"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/main_game.cpp",
        "source/engine/nullEditorHost.h",
        "source/engine/nullEditorHost.cpp"
    ) `
    -Patterns @(
        '#include "(editor|render/editor)/',
        '\bEditorUi::',
        '\bEditor::'
    ) `
    -RuleName "Game executable link boundary"

foreach ($linkFile in @("source/CMakeLists.txt")) {
    $linkPath = Resolve-RepoPath $linkFile
    if (!(Test-Path -LiteralPath $linkPath)) {
        Add-Failure $failures "Game executable link boundary: missing expected file '$linkFile'."
        continue
    }
    $linkText = Get-Content -LiteralPath $linkPath -Raw
    $mainGameStart = $linkText.IndexOf('add_executable(main_game main_game.cpp)')
    if ($mainGameStart -lt 0) {
        Add-Failure $failures "Game executable link boundary: main_game target block is missing."
        continue
    }
    $mainGameBody = $linkText.Substring($mainGameStart)
    if ($mainGameBody -notmatch 'target_link_libraries\(\s*main_game\s+PRIVATE\s+vl_runtime\s*\)') {
        Add-Failure $failures "Game executable link boundary: main_game must link vl_runtime as its only library target."
    }
    if ($mainGameBody -match 'vl_editor') {
        Add-Failure $failures "Game executable link boundary: main_game block references an editor target."
    }

    $mainEditorStart = $linkText.IndexOf('add_executable(main_editor main_editor.cpp)')
    $mainEditorEnd = $linkText.IndexOf('if(VULKANLEARN_BUILD_GAME)')
    if ($mainEditorStart -lt 0 -or $mainEditorEnd -le $mainEditorStart) {
        Add-Failure $failures "Game executable link boundary: main_editor target block is missing."
    }
    else {
        $mainEditorBody = $linkText.Substring($mainEditorStart, $mainEditorEnd - $mainEditorStart)
        if ($mainEditorBody -notmatch 'target_link_libraries\(\s*main_editor\s+PRIVATE\s+vl_editor\s*\)') {
            Add-Failure $failures "Game executable link boundary: main_editor must link vl_editor."
        }
    }

    $runtimeLink = [regex]::Match(
        $linkText,
        'target_link_libraries\(\s*vl_runtime\s+PUBLIC(?<body>[^\)]*)\)')
    if (!$runtimeLink.Success) {
        Add-Failure $failures "Game executable link boundary: vl_runtime PUBLIC link block is missing."
    }
    elseif ($runtimeLink.Groups["body"].Value -match 'vl_editor') {
        Add-Failure $failures "Game executable link boundary: vl_runtime link interface references an editor target."
    }
}

# The runtime UI may hold only the editor shell interface. Editor documents,
# panels and textures are attached by the editor module through this seam.
Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/ui/iEditorUiShell.h" `
    -Patterns @(
        'class IEditorUiShell',
        'class IEditorUiRuntimeBridge',
        'virtual void Shutdown\(\) = 0;',
        'ClearEditorUiShell'
    ) `
    -RuleName "Runtime UI editor include boundary"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePaths @(
        "source/ui/uiSubsystem.h",
        "source/ui/uiSubsystem.cpp",
        "source/ui/uiSubsystemDocuments.cpp",
        "source/ui/uiSubsystemInput.cpp",
        "source/ui/uiSubsystemActions.cpp",
        "source/ui/uiSubsystemInternal.h",
        "source/ui/iEditorUiShell.h",
        "source/ui/rmlUiInterfaces.h",
        "source/ui/rmlUiInterfaces.cpp",
        "source/ui/uiAction.h",
        "source/ui/viewModel/runtimeUiSections.h",
        "source/ui/uiExternalTexture.h",
        "source/ui/uiRenderSnapshot.h"
    ) `
    -Patterns @(
        '#include "(editor|render/editor)/',
        '#include "ui/widgets/(editor|workspace)/',
        '\bEditorUi::',
        '\bEditor::'
    ) `
    -RuleName "Runtime UI editor include boundary"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/ui/uiSubsystem.h" `
    -Patterns @(
        'IEditorUiShell\* editorUiShell',
        'SetEditorUiShell\(IEditorUiShell\* shell\)'
    ) `
    -RuleName "Runtime UI editor include boundary"

# Game packaging is a closure, not a directory copy. The exporter must resolve
# the runtime UI manifest, strip the editor graph, require the game flavor
# marker, and ship gameplay default materials that no scene JSON names.
Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "ui/runtime-ui.manifest.json" `
    -Patterns @(
        '"format": 1',
        '"runtime-hud\.rml"',
        '"localization\.json"'
    ) `
    -RuleName "Game packaging manifest closure"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePath "ui/runtime-ui.manifest.json" `
    -Patterns @(
        '"editor-',
        '"widgets/'
    ) `
    -RuleName "Game packaging manifest closure"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "tool/game-export/gameExporter.cpp" `
    -Patterns @(
        'kRuntimeUiManifest',
        'kEditorUiManifest',
        'CopyUiClosure',
        'ValidateUiManifest',
        'StripEditorGraphForGame',
        'kGameBuildMarker',
        'editorEnabled',
        'MI_glass_crack\.json'
    ) `
    -RuleName "Game packaging manifest closure"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "tool/validate-game-package.ps1" `
    -Patterns @(
        'runtime-ui\.manifest\.json',
        'editor-ui\.manifest\.json',
        'game-export-manifest\.json',
        'vl_build_flavor\.json',
        '--exit-after-frames',
        'misses=0'
    ) `
    -RuleName "Game packaging manifest closure"

# Shader packaging follows cache artifact identity, not guessed file kinds.
# Every output role is copied, including debug SPV, because the cache/ABI
# reflection path requires them.
Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "tool/game-export/gameExporter.cpp" `
    -Patterns @(
        'shader/spv/shader-build-cache\.json',
        'compilePolicy',
        'configuration=Release;',
        'MaterialGraphicsPair',
        'materialSource',
        'normalizedKey',
        'shaderEvaluation',
        'generatedAtStartup',
        'for \(const auto& \[role, output\] : artifact\.at\("outputs"\)\.items\(\)\)'
    ) `
    -RuleName "Game shader cache closure"

Test-FilePatternsAbsent `
    -Failures $failures `
    -RelativePath "tool/game-export/gameExporter.cpp" `
    -Patterns @(
        'continue;[^\r\n]*debug(Vertex|Fragment|Compute)',
        'if \(role == "debug'
    ) `
    -RuleName "Game shader cache closure"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "tool/validate-game-package.ps1" `
    -Patterns @(
        'runtime-shader\.manifest\.json',
        'retained shader output is missing',
        '--verify-package',
        'Shader cache miss'
    ) `
    -RuleName "Game shader cache closure"

Test-VulkanManagerBoundary -Failures $failures
Test-RendererBackendRawGetterBoundary -Failures $failures
Test-RendererSubmitBoundary -Failures $failures
Test-RHIDeviceVulkanPublicRawGetterBoundary -Failures $failures
Test-VulkanManagerPublicBoundary -Failures $failures

if ($failures.Count -gt 0) {
    Write-Host "Architecture boundary audit failed:" -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host "Architecture boundary audit passed." -ForegroundColor Green
