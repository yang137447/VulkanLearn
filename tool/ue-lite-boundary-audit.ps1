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
        "documents/architecture/vulkanlearn-architecture.html",
        "documents/rendering/descriptor-imageinfo-management.md",
        "documents/plan/architecture/ue-lite-completion-plan.md"
    ) `
    -Patterns @(
        '(?-i)(?<![A-Za-z0-9_])RHI(?![A-Za-z0-9_])'
    ) `
    -RuleName "Vulkan-native architecture wording"

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
        '\bsubmitInfo\b',
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
    -RelativePath "source/render/resource/rendererResourceLoadCoordinator.h" `
    -Patterns @(
        'struct RendererWorldResourceLoadResult',
        'std::vector<MeshObjectBuildPlan> meshObjectPlans'
    ) `
    -RuleName "Renderer resource load result contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/world/loading/worldLoader.h" `
    -Patterns @(
        'std::vector<MeshObjectBuildPlan> meshObjectPlans'
    ) `
    -RuleName "World build plan mesh object contract"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/world/loading/worldBuilder.cpp" `
    -Patterns @(
        'worldBuildPlan\.meshObjectPlans',
        'world->AddMeshObject\('
    ) `
    -RuleName "WorldBuilder mesh object plan consumption"

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
        '--reloadstress',
        '--reloadfail',
        '--reloadfail-material',
        '--reloadfail-mesh',
        '--reloadfail-texture',
        '--lightstress',
        '--resizestress',
        '--graphreloadstress',
        '--framesmoke',
        '--environmentstress',
        '--exit-after-tests',
        'RuntimeCommandType::RunResizeStress',
        'RuntimeCommandType::RunRenderGraphReloadStress',
        'RuntimeCommandType::RunFrameSmokeTest',
        'RuntimeCommandType::RunEnvironmentUpdateStress',
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

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/runtimeTestHooks.cpp" `
    -Patterns @(
        'BeginResizeStress',
        'UpdateResizeStress',
        'BeginRenderGraphReloadStress',
        'UpdateRenderGraphReloadStress',
        'BeginFrameSmokeTest',
        'RecordFrameTime',
        'BeginEnvironmentUpdateStress',
        'UpdateEnvironmentUpdateStress',
        'RuntimeCommandType::SetProceduralSkyParameters',
        'Environment update stress completed'
    ) `
    -RuleName "Runtime validation state owned by test subsystem"

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
        'environmentUpdateStressPhase'
    ) `
    -RuleName "Runtime test state stays out of EngineLoop"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/runtimeTestHooks.cpp" `
    -Patterns @(
        'BeginFrameSmokeTest',
        'RecordFrameTime',
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
        'RebuildSwapchainDependentResources'
    ) `
    -RuleName "Swapchain resize production lifecycle"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "source/engine/runtimeTestHooks.cpp" `
    -Patterns @(
        'BeginResizeStress',
        'UpdateResizeStress',
        'Resize stress completed'
    ) `
    -RuleName "Swapchain resize runtime validation"

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
    -RelativePath "source/engine/runtimeTestHooks.cpp" `
    -Patterns @(
        'BeginRenderGraphReloadStress',
        'UpdateRenderGraphReloadStress',
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
        'CreateGeneratedMeshFailureScene',
        'CreateGeneratedTextureFailureScene',
        'Material.LoadFailed',
        'Mesh.LoadFailed',
        'Texture.LoadFailed',
        'CreateGeneratedHighLightStressScene',
        'activeWorldBeforeCommand',
        'activeWorldAfterCommand',
        'loadWorldError',
        'SameRendererResourceFingerprint',
        'rendererResourcesBeforeLoad',
        'pass material bindings preserved'
    ) `
    -RuleName "World reload failure rollback validation"

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
    -RelativePath "source/world/loading/worldTransitionCoordinator.cpp" `
    -Patterns @(
        'ClassifyRendererResourceLoadError',
        'Material.LoadFailed',
        'Texture.LoadFailed',
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
    -RelativePath "source/engine/runtimeTestHooks.cpp" `
    -Patterns @(
        'RetireDrainFrameBudget',
        'maxPendingRetiredResources',
        'World reload stress waiting for retire queue drain',
        'retired world-local resources did not drain'
    ) `
    -RuleName "World reload retire queue validation"

Test-FilePatternsPresent `
    -Failures $failures `
    -RelativePath "tool/ue-lite-final-validation.ps1" `
    -Patterns @(
        '--framesmoke',
        '--reloadstress',
        '--reloadfail',
        '--reloadfail-material',
        '--reloadfail-mesh',
        '--reloadfail-texture',
        '--lightstress',
        '--resizestress',
        '--graphreloadstress',
        'artifacts/ue-lite-validation'
    ) `
    -RuleName "UE-Lite final validation script contract"

Test-VulkanManagerBoundary -Failures $failures
Test-RendererBackendRawGetterBoundary -Failures $failures
Test-RendererSubmitBoundary -Failures $failures
Test-RHIDeviceVulkanPublicRawGetterBoundary -Failures $failures
Test-VulkanManagerPublicBoundary -Failures $failures

if ($failures.Count -gt 0) {
    Write-Host "UE-Lite boundary audit failed:" -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host "UE-Lite boundary audit passed." -ForegroundColor Green
