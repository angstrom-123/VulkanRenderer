#include "VkBootstrap.h"

#include "vk_engine.h"
#include "vk_initialiser.h"
#include "vk_pipeline.h"

#include <GLFW/glfw3.h>

// #ifdef PLAT_LINUX_WAYLAND
//     #define GLFW_EXPOSE_NATIVE_WAYLAND
// #elifdef PLAT_LINUX_X11
//     #define GLFW_EXPOSE_NATIVE_X11
// #elifdef PLAT_WINDOWS
//     #define GLFW_EXPOSE_NATIVE_WIN32
// #endif
// #include <GLFW/glfw3native.h>

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vulkan/vulkan_core.h>

#define SHADER_DIR "src/Shader/"
#define SHADER_FILE_EXTENSION ".spirv"
#define LOAD_SHADER_MODULE(shaderFile, module) LoadShaderModule(SHADER_DIR shaderFile SHADER_FILE_EXTENSION, module)

#ifdef DEBUG 
    #define USE_VALIDATION_LAYERS VK_TRUE 
    #define VK_CHECK(x)\
        do {\
            VkResult __err = x;\
            if (__err) {\
                fprintf(stderr, "[%s]: Detected Vulkan Error (%d)\n    at %s:%s:%d\n",\
                    __TIME__, __err, __FILE__, __func__, __LINE__);\
                abort();\
            }\
        } while (0);
#elifdef RELEASE
    #define USE_VALIDATION_LAYERS VK_FALSE 
    #define VK_CHECK(x) (void) x
#else 
    #error "DEBUG or RELEASE must be specified"
#endif

void GLFWErrorCb(int error, const char *desc) 
{
    (void) error;
    std::cerr << desc << std::endl;
}

void GLFWResizeCb(GLFWwindow *window, int width, int height)
{
    VulkanEngine *engine = static_cast<VulkanEngine *>(glfwGetWindowUserPointer(window));
    if (engine && width > 0 && height > 0) {
        engine->m_DidResize = true;
    }
}

void GLFWKeyCb(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    (void) scancode; (void) mods;
    VulkanEngine *engine = static_cast<VulkanEngine *>(glfwGetWindowUserPointer(window));
    if (key == GLFW_KEY_SPACE && action == GLFW_PRESS) {
        engine->m_SelectedShader++;
        if (engine->m_SelectedShader > 1) {
            engine->m_SelectedShader = 0;
        }
    }
}

void VulkanEngine::Init(Config *config)
{
    glfwInit();

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    m_Window = glfwCreateWindow(m_Extent.width, m_Extent.height, "Renderer", nullptr, nullptr);

    glfwSetErrorCallback(GLFWErrorCb);
    glfwSetWindowUserPointer(m_Window, this);
    glfwSetFramebufferSizeCallback(m_Window, GLFWResizeCb);
    glfwSetWindowSizeCallback(m_Window, GLFWResizeCb);
    glfwSetKeyCallback(m_Window, GLFWKeyCb);

    switch (config->syncStrategy) {
        case SYNC_STRATEGY_UNCAPPED: m_PresentMode = VK_PRESENT_MODE_MAILBOX_KHR; break;
        case SYNC_STRATEGY_VSYNC: m_PresentMode = VK_PRESENT_MODE_FIFO_KHR; break; // TODO: Fix fullscreen flickering
        default: std::cerr << "Error: " << __func__ << " : Invalid sync strategy" << std::endl;
    };
    
    InitVulkan();
    InitSwapchain();
    InitCommands();
    InitDefaultRenderPass();
    InitFramebuffers();
    InitSyncStructures();
    InitPipelines();

    m_DidResize = true;
    m_IsInitialized = true;
}

void VulkanEngine::Run()
{
    double lastTime = glfwGetTime();
    double fpsCounter = 0.0;

    while (!glfwWindowShouldClose(m_Window)) {
        glfwPollEvents();

        if (m_DidResize) {
            Resize();
            m_DidResize = false;
        }

        double currTime = glfwGetTime();
        double deltaTime = currTime - lastTime;
        fpsCounter++;

        if (deltaTime >= 1.0) {
            std::cout << "FPS: " << 1.0 / (deltaTime / fpsCounter) << std::endl;
            fpsCounter = 0;
            lastTime = currTime;
        }

        Draw();
    }
}

void VulkanEngine::Draw()
{
    FrameData &frame = m_Frames[m_FrameNum % FIF];

    VK_CHECK(vkWaitForFences(m_Device, 1, &frame.renderFence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(m_Device, 1, &frame.renderFence));

    uint32_t imageIndex;
    VkResult acquireErr = vkAcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX, frame.acquireSemaphore, nullptr, &imageIndex);
    if (acquireErr == VK_ERROR_OUT_OF_DATE_KHR || acquireErr == VK_SUBOPTIMAL_KHR) {
        m_DidResize = true;
        return;
    } else if (acquireErr == VK_NOT_READY) {
        return;
    } 
    VK_CHECK(acquireErr);

    ImageData &image = m_Images[imageIndex];

    VK_CHECK(vkResetCommandBuffer(frame.commandBuffer, 0));

    if (image.flightFence != VK_NULL_HANDLE && image.flightFence != frame.renderFence) {
        VK_CHECK(vkWaitForFences(m_Device, 1, &image.flightFence, VK_TRUE, UINT64_MAX));
    }
    image.flightFence = frame.renderFence;

    VkCommandBufferBeginInfo beginInfo = vkinit::CommandBufferBeginInfo(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    VK_CHECK(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo));

    // Debug visual: Flashing blue with 120*pi frame period
    // VkClearValue clear_val = { .color = {{ 0.0, 0.0, static_cast<float>(std::abs(sin(m_FrameNum / 1200.0))), 1.0 }} };
    VkClearValue clear_val = { .color = {{ 0.0, 0.0, static_cast<float>(std::abs(sin(m_FrameNum / 120.0))), 1.0 }} };

    VkRenderPassBeginInfo passInfo = vkinit::RenderPassBeginInfo(m_RenderPass, m_Framebuffers[imageIndex], &clear_val, m_Extent);
    vkCmdBeginRenderPass(frame.commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &m_Viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &m_Scissor);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipelines[m_SelectedShader]);
    vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);

    vkCmdEndRenderPass(frame.commandBuffer);

    VK_CHECK(vkEndCommandBuffer(frame.commandBuffer));

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo = vkinit::SubmitInfo(&frame, &image, &waitStage);
    VK_CHECK(vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, frame.renderFence));

    VkPresentInfoKHR presentInfo = vkinit::PresentInfo(&image, &m_Swapchain, &imageIndex);
    VkResult presentErr = vkQueuePresentKHR(m_GraphicsQueue, &presentInfo);
    if (presentErr == VK_ERROR_OUT_OF_DATE_KHR || presentErr == VK_SUBOPTIMAL_KHR) {
        m_DidResize = true;
    } else {
        VK_CHECK(presentErr);
    }

    m_FrameNum++;
}

void VulkanEngine::Resize()
{
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR getCapabilities = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR) glfwGetInstanceProcAddress(m_Instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkSurfaceCapabilitiesKHR capabilities;
    VK_CHECK(getCapabilities(m_PhysicalDevice, m_Surface, &capabilities));

    if (capabilities.currentExtent.width == 0 
            || capabilities.currentExtent.height == 0 
            || capabilities.currentExtent.width == UINT32_MAX 
            || capabilities.currentExtent.height == UINT32_MAX) {
        return;
    }

    if (capabilities.currentExtent.width == m_Extent.width 
            && capabilities.currentExtent.height == m_Extent.height) {
        return;
    }

    std::cout << "Resizing" << std::endl;

    VK_CHECK(vkDeviceWaitIdle(m_Device));

    for (FrameData &frame : m_Frames) {
        VK_CHECK(vkResetCommandBuffer(frame.commandBuffer, 0));
    }

    DestroySwapchain();

    for (ImageData &image : m_Images) {
        vkDestroySemaphore(m_Device, image.renderSemaphore, nullptr);
        image.flightFence = VK_NULL_HANDLE;
    }
    for (FrameData &frame : m_Frames) {
        vkDestroyFence(m_Device, frame.renderFence, nullptr);
        vkDestroySemaphore(m_Device, frame.acquireSemaphore, nullptr);
    }

    m_Extent = capabilities.currentExtent;
    m_Viewport.width = static_cast<float>(m_Extent.width);
    m_Viewport.height = static_cast<float>(m_Extent.height);
    m_Scissor.extent = m_Extent;

    InitSwapchain();
    InitFramebuffers();
    InitSyncStructures();

    m_DidResize = false;
}

void VulkanEngine::ToggleFullscreen()
{
    // TODO: Stop the flickering :(
}

void VulkanEngine::DestroySwapchain() 
{
    vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);

    for (size_t i = 0; i < m_SwapchainViews.size(); i++) {
        vkDestroyImageView(m_Device, m_SwapchainViews[i], nullptr);
        vkDestroyFramebuffer(m_Device, m_Framebuffers[i], nullptr);
    }
}

void VulkanEngine::Cleanup() 
{
    if (m_IsInitialized) {
        VK_CHECK(vkDeviceWaitIdle(m_Device));

        for (FrameData &frame : m_Frames) {
            vkDestroyCommandPool(m_Device, frame.commandPool, nullptr);

            vkDestroyFence(m_Device, frame.renderFence, nullptr);
            vkDestroySemaphore(m_Device, frame.acquireSemaphore, nullptr);
        }

        for (ImageData &image : m_Images) {
            vkDestroySemaphore(m_Device, image.renderSemaphore, nullptr);
        }

        for (size_t i = 0; i < m_ShaderModules.size(); i++) {
            vkDestroyShaderModule(m_Device, m_ShaderModules[i], nullptr);
        }

        vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
        for (VkPipeline &pipeline : m_Pipelines) {
            vkDestroyPipeline(m_Device, pipeline, nullptr);
        }

        DestroySwapchain();

        vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
        vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);

        vkDestroyDevice(m_Device, nullptr);
        vkb::destroy_debug_utils_messenger(m_Instance, m_DebugMessenger);
        vkDestroyInstance(m_Instance, nullptr);

        glfwDestroyWindow(m_Window);
        glfwTerminate();
    }
}

void VulkanEngine::InitVulkan()
{
    vkb::InstanceBuilder builder;
    auto builtInstance = builder.set_app_name("Renderer")
                                .request_validation_layers(USE_VALIDATION_LAYERS)
                                .require_api_version(1, 1, 0)
                                .use_default_debug_messenger()
                                .build();

    vkb::Instance vkbInstance = builtInstance.value();

    m_Instance = vkbInstance.instance;
    m_DebugMessenger = vkbInstance.debug_messenger;

    VK_CHECK(glfwCreateWindowSurface(m_Instance, m_Window, nullptr, &m_Surface));

    vkb::PhysicalDeviceSelector selector { vkbInstance };
    std::vector<std::string> devices = selector.set_surface(m_Surface)
                                               .select_device_names()
                                               .value();
    if (devices.size() == 0) {
        std::cout << "Error: " 
                  << __func__ 
                  << ": Failed to find physical device" 
                  << std::endl;
        abort();
    }
    if (devices.size() == 1) {
        std::cout << "Warning: " 
                  << __func__ 
                  << ": Only one physical device found (expected at least 2)" 
                  << std::endl
                  << "Defaulted to: "
                  << devices.front()
                  << std::endl;
    }
    vkb::PhysicalDevice physicalDevice = selector.set_minimum_version(1, 1)
                                                 .set_surface(m_Surface)
                                                 .select()
                                                 .value();
    std::cout << "Info: " << __func__ << ": Selected physical device: " << physicalDevice.name << std::endl;
    vkb::DeviceBuilder deviceBuilder { physicalDevice };
    vkb::Device vkbDevice = deviceBuilder.build().value();

    m_Device = vkbDevice.device;
    m_PhysicalDevice = vkbDevice.physical_device;

    m_GraphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    m_GraphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();
}

void VulkanEngine::InitSwapchain()
{
    vkb::SwapchainBuilder builder { m_PhysicalDevice, m_Device, m_Surface };
    vkb::Swapchain vkbSwapchain = builder.use_default_format_selection()
                                         .set_desired_present_mode(m_PresentMode)
                                         .set_desired_min_image_count(3)
                                         .set_desired_extent(m_Extent.width, m_Extent.height)
                                         .build()
                                         .value();

    if (vkbSwapchain.present_mode != m_PresentMode) {
        std::cerr << "Warning: " 
                  << __func__ 
                  << ": Requested present mode unavailable (" 
                  << m_PresentMode 
                  << "). Fell back to " 
                  << vkbSwapchain.present_mode
                  << std::endl;
    }

    m_Swapchain = vkbSwapchain.swapchain;
    m_SwapchainImages = vkbSwapchain.get_images().value();
    m_SwapchainViews = vkbSwapchain.get_image_views().value();
    m_SwapchainFormat = vkbSwapchain.image_format;
}

void VulkanEngine::InitCommands()
{
    VkCommandPoolCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = m_GraphicsQueueFamily,
    };

    for (FrameData &frame : m_Frames) {
        VK_CHECK(vkCreateCommandPool(m_Device, &createInfo, nullptr, &frame.commandPool));
        VkCommandBufferAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = frame.commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };

        VK_CHECK(vkAllocateCommandBuffers(m_Device, &alloc_info, &frame.commandBuffer));
    }
}

void VulkanEngine::InitDefaultRenderPass()
{
    VkAttachmentDescription colorAttachment = {
        .format = m_SwapchainFormat,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };

    VkAttachmentReference ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ref
    };

    VkRenderPassCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &colorAttachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
    };

    VK_CHECK(vkCreateRenderPass(m_Device, &info, nullptr, &m_RenderPass));
}

void VulkanEngine::InitFramebuffers()
{
    VkFramebufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = nullptr,
        .renderPass = m_RenderPass,
        .attachmentCount = 1,
        .width = m_Extent.width,
        .height = m_Extent.height,
        .layers = 1,
    };

    m_Framebuffers = std::vector<VkFramebuffer>(m_SwapchainViews.size());
    for (size_t i = 0; i < m_SwapchainViews.size(); i++) {
        info.pAttachments = &m_SwapchainViews[i];
        VK_CHECK(vkCreateFramebuffer(m_Device, &info, nullptr, &m_Framebuffers[i]));
    }
}

void VulkanEngine::InitSyncStructures()
{
    VkFenceCreateInfo fenceInfo = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    VkSemaphoreCreateInfo semaphoreInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };

    for (FrameData &frame : m_Frames) {
        VK_CHECK(vkCreateFence(m_Device, &fenceInfo, nullptr, &frame.renderFence));
        VK_CHECK(vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &frame.acquireSemaphore));
    }

    m_Images = std::vector<ImageData>(m_SwapchainImages.size());
    for (ImageData &image : m_Images) {
        image.flightFence = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &image.renderSemaphore));
    }
}

void VulkanEngine::InitPipelines()
{
    // Load shaders
    VkShaderModule triangleVert;
    VkShaderModule triangleFrag;
    LOAD_SHADER_MODULE("triangle.vert", &triangleVert);
    LOAD_SHADER_MODULE("triangle.frag", &triangleFrag);
    m_ShaderModules.push_back(triangleVert);
    m_ShaderModules.push_back(triangleFrag);

    VkShaderModule triangleAltVert;
    VkShaderModule triangleAltFrag;
    LOAD_SHADER_MODULE("triangleAlt.vert", &triangleAltVert);
    LOAD_SHADER_MODULE("triangleAlt.frag", &triangleAltFrag);
    m_ShaderModules.push_back(triangleAltVert);
    m_ShaderModules.push_back(triangleAltFrag);

    // Init pipelines
    VkPipelineLayoutCreateInfo layoutInfo = vkinit::LayoutCreateInfo();
    VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutInfo, nullptr, &m_PipelineLayout)); 

    // Build viewport and scissor
    m_Viewport.x = 0.0;
    m_Viewport.y = 0.0;
    m_Viewport.width = m_Extent.width;
    m_Viewport.height = m_Extent.height;
    m_Viewport.minDepth = 0.0;
    m_Viewport.maxDepth = 1.0;

    m_Scissor.offset = { 0, 0 };
    m_Scissor.extent = m_Extent;

    // Build pipelines
    PipelineBuilder builder;
    // How verts are read from buffers (currently unused)
    builder.m_VertexInputInfo = vkinit::VertexInputStateCreateInfo();
    builder.m_InputAssemblyInfo = vkinit::InputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

    builder.m_RasterizerInfo = vkinit::RasterizationStateCreateInfo(VK_POLYGON_MODE_FILL);
    builder.m_MultisamplingInfo = vkinit::MultisampleStateCreateInfo();
    builder.m_ColorBlendAttachment = vkinit::ColorBlendAttachmentState();
    builder.m_PipelineLayout = m_PipelineLayout;

    // Build Vulkan Pipeline Objects
    builder.m_ShaderStages.push_back(vkinit::ShaderStageCreateInfo(VK_SHADER_STAGE_VERTEX_BIT, triangleVert));
    builder.m_ShaderStages.push_back(vkinit::ShaderStageCreateInfo(VK_SHADER_STAGE_FRAGMENT_BIT, triangleFrag));
    m_Pipelines.push_back(builder.BuildPipeline(m_Device, m_RenderPass, m_Viewport, m_Scissor));

    builder.m_ShaderStages.clear();

    builder.m_ShaderStages.push_back(vkinit::ShaderStageCreateInfo(VK_SHADER_STAGE_VERTEX_BIT, triangleAltVert));
    builder.m_ShaderStages.push_back(vkinit::ShaderStageCreateInfo(VK_SHADER_STAGE_FRAGMENT_BIT, triangleAltFrag));
    m_Pipelines.push_back(builder.BuildPipeline(m_Device, m_RenderPass, m_Viewport, m_Scissor));
}

void VulkanEngine::LoadShaderModule(const char *path, VkShaderModule *module)
{
    // 'ate' flag puts the cursor at the end
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: load_shader_module: Failed to open shader file `" << path << "`" << std::endl;
        abort();
    }

    // Get cursor pos (initialised at end)
    size_t fileSize = file.tellg();
    uint32_t *buf = new uint32_t[fileSize];

    file.seekg(0);
    file.read((char *) buf, fileSize);
    file.close();

    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .codeSize = fileSize,
        .pCode = buf,
    };

    VkShaderModule shaderModule;
    VkResult err = vkCreateShaderModule(m_Device, &info, nullptr, &shaderModule);
    if (err) {
        std::cerr << "Error: " << __func__ << " : Failed to create shader module for `" << path << "`" << std::endl;
        abort();
    }

    delete[] buf;

    *module = shaderModule;
}
