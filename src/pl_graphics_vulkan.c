/*
   vulkan_pl_graphics.c
*/

/*
Index of this file:
// [SECTION] includes
// [SECTION] internal functions
// [SECTION] implementations
// [SECTION] internal implementations
*/

//-----------------------------------------------------------------------------
// [SECTION] includes
//-----------------------------------------------------------------------------

#include "pl_graphics_vulkan.h"
#include "pl_ds.h"
#include "pl_io.h"
#include <stdio.h>

#ifdef _WIN32
#pragma comment(lib, "vulkan-1.lib")
#endif

//-----------------------------------------------------------------------------
// [SECTION] internal functions
//-----------------------------------------------------------------------------

static VKAPI_ATTR VkBool32 VKAPI_CALL pl__debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT tMsgSeverity, VkDebugUtilsMessageTypeFlagsEXT tMsgType, const VkDebugUtilsMessengerCallbackDataEXT* ptCallbackData, void* pUserData);

// low level setup
static void pl__create_instance       (plVulkanGraphics* ptGraphics, uint32_t uVersion, bool bEnableValidation);
static void pl__create_instance_ex    (plVulkanGraphics* ptGraphics, uint32_t uVersion, uint32_t uLayerCount, const char** ppcEnabledLayers, uint32_t uExtensioncount, const char** ppcEnabledExtensions);
static void pl__create_frame_resources(plVulkanGraphics* ptGraphics, plVulkanDevice* ptDevice);
static void pl__create_device         (VkInstance tInstance, VkSurfaceKHR tSurface, plVulkanDevice* ptDeviceOut, bool bEnableValidation);

// low level swapchain ops
static void pl__create_swapchain   (plVulkanDevice* ptDevice, VkSurfaceKHR tSurface, uint32_t uWidth, uint32_t uHeight, plVulkanSwapchain* ptSwapchainOut);
static void pl__create_framebuffers(plVulkanDevice* ptDevice, VkRenderPass tRenderPass, plVulkanSwapchain* ptSwapchain);

// resource manager setup
static void pl__create_resource_manager  (plVulkanGraphics* ptGraphics, plVulkanDevice* ptDevice, plVulkanResourceManager* ptResourceManagerOut);
static void pl__cleanup_resource_manager (plVulkanResourceManager* ptResourceManager);

// misc
static uint32_t    pl__get_u32_max              (uint32_t a, uint32_t b) { return a > b ? a : b;}
static uint32_t    pl__get_u32_min              (uint32_t a, uint32_t b) { return a < b ? a : b;}
static int         pl__select_physical_device   (VkInstance tInstance, plVulkanDevice* ptDeviceOut);
static void        pl__staging_buffer_realloc   (plVulkanResourceManager* ptResourceManager, size_t szNewSize);
static inline void pl__staging_buffer_may_grow  (plVulkanResourceManager* ptResourceManager, size_t szSize);
static uint64_t    pl__get_free_buffer_index    (plVulkanResourceManager* ptResourceManager);
static size_t      pl__get_const_buffer_req_size(plVulkanDevice* ptDevice, size_t szSize);

//-----------------------------------------------------------------------------
// [SECTION] implementations
//-----------------------------------------------------------------------------

void
pl_setup_graphics(plVulkanGraphics* ptGraphics)
{
    // get io context
    plIOContext* ptIOCtx = pl_get_io_context();

    // create vulkan tInstance
    pl__create_instance(ptGraphics, VK_API_VERSION_1_2, true);

    // create tSurface
    #ifdef _WIN32
        VkWin32SurfaceCreateInfoKHR surfaceCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .pNext = NULL,
            .flags = 0,
            .hinstance = GetModuleHandle(NULL),
            .hwnd = *(HWND*)ptIOCtx->pBackendPlatformData
        };
        PL_VULKAN(vkCreateWin32SurfaceKHR(ptGraphics->tInstance, &surfaceCreateInfo, NULL, &ptGraphics->tSurface));
    #elif defined(__APPLE__)
        VkMetalSurfaceCreateInfoEXT tSurfaceCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
            .pLayer = (CAMetalLayer*)ptIOCtx->pBackendPlatformData
        };
        PL_VULKAN(vkCreateMetalSurfaceEXT(ptGraphics->tInstance, &tSurfaceCreateInfo, NULL, &ptGraphics->tSurface));
    #else // linux
        struct tPlatformData { xcb_connection_t* ptConnection; xcb_window_t tWindow;};
        struct tPlatformData* ptPlatformData = (struct tPlatformData*)ptIOCtx->pBackendPlatformData;
        VkXcbSurfaceCreateInfoKHR surfaceCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
            .pNext = NULL,
            .flags = 0,
            .window = ptPlatformData->tWindow,
            .connection = ptPlatformData->ptConnection
        };
        PL_VULKAN(vkCreateXcbSurfaceKHR(ptGraphics->tInstance, &surfaceCreateInfo, NULL, &ptGraphics->tSurface));
    #endif

    // create devices
    pl__create_device(ptGraphics->tInstance, ptGraphics->tSurface, &ptGraphics->tDevice, true);
    
    // create swapchain
    ptGraphics->tSwapchain.bVSync = true;
    pl__create_swapchain(&ptGraphics->tDevice, ptGraphics->tSurface, (uint32_t)ptIOCtx->afMainViewportSize[0], (uint32_t)ptIOCtx->afMainViewportSize[1], &ptGraphics->tSwapchain);

    // create render pass
    VkAttachmentDescription atAttachments[] = {

        // color attachment
        {
            .flags          = VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT,
            .format         = ptGraphics->tSwapchain.tFormat,
            .samples        = ptGraphics->tSwapchain.tMsaaSamples,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        },

        // depth attachment
        {
            .format         = pl_find_depth_format(&ptGraphics->tDevice),
            .samples        = ptGraphics->tSwapchain.tMsaaSamples,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },

        // color resolve attachment
        {
            .flags          = VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT,
            .format         = ptGraphics->tSwapchain.tFormat,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
        },
    };

    VkSubpassDependency tSubpassDependencies[] = {

        {
            .srcSubpass      = VK_SUBPASS_EXTERNAL,
            .dstSubpass      = 0,
            .srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask   = 0,
            .dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT
        },
        {
            .srcSubpass      = VK_SUBPASS_EXTERNAL,
            .dstSubpass      = 0,
            .srcStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            .srcAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            .dstAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT
        }
    };

    VkAttachmentReference atAttachmentReferences[] = {
        {
            .attachment = 0,
            .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        },
        {
            .attachment = 1,
            .layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL     
        },
        {
            .attachment = 2,
            .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL     
        }
    };

    VkSubpassDescription tSubpass = {
        .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount    = 1,
        .pColorAttachments       = &atAttachmentReferences[0],
        .pDepthStencilAttachment = &atAttachmentReferences[1],
        .pResolveAttachments     = &atAttachmentReferences[2]
    };

    VkRenderPassCreateInfo tRenderPassInfo = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 3,
        .pAttachments    = atAttachments,
        .subpassCount    = 1,
        .pSubpasses      = &tSubpass,
        .dependencyCount = 2,
        .pDependencies   = tSubpassDependencies
    };
    PL_VULKAN(vkCreateRenderPass(ptGraphics->tDevice.tLogicalDevice, &tRenderPassInfo, NULL, &ptGraphics->tRenderPass));

    // create frame buffers
    pl__create_framebuffers(&ptGraphics->tDevice, ptGraphics->tRenderPass, &ptGraphics->tSwapchain);
    
    // create per frame resources
    pl__create_frame_resources(ptGraphics, &ptGraphics->tDevice);

    VkCommandBuffer tCommandBuffer = pl_begin_command_buffer(ptGraphics, &ptGraphics->tDevice);
    const VkImageSubresourceRange tRange = {
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
        .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT
    };
    pl_transition_image_layout(tCommandBuffer, ptGraphics->tSwapchain.tDepthImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, tRange, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT , VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT  );
    pl_submit_command_buffer(ptGraphics, &ptGraphics->tDevice, tCommandBuffer);

    VkDescriptorPoolSize atPoolSizes[] =
    {
        { VK_DESCRIPTOR_TYPE_SAMPLER,                1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,   1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,   1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,       1000 }
    };
    VkDescriptorPoolCreateInfo tDescriptorPoolInfo = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = 1000 * 11,
        .poolSizeCount = 11u,
        .pPoolSizes    = atPoolSizes,
    };
    PL_VULKAN(vkCreateDescriptorPool(ptGraphics->tDevice.tLogicalDevice, &tDescriptorPoolInfo, NULL, &ptGraphics->tDescriptorPool));

    // setup resource manager
    pl__create_resource_manager(ptGraphics, &ptGraphics->tDevice, &ptGraphics->tResourceManager);
}

bool
pl_begin_frame(plVulkanGraphics* ptGraphics)
{
    plIOContext* ptIOCtx = pl_get_io_context();

    plVulkanFrameContext* ptCurrentFrame = pl_get_frame_resources(ptGraphics);

    PL_VULKAN(vkWaitForFences(ptGraphics->tDevice.tLogicalDevice, 1, &ptCurrentFrame->tInFlight, VK_TRUE, UINT64_MAX));
    VkResult err = vkAcquireNextImageKHR(ptGraphics->tDevice.tLogicalDevice, ptGraphics->tSwapchain.tSwapChain, UINT64_MAX, ptCurrentFrame->tImageAvailable, VK_NULL_HANDLE, &ptGraphics->tSwapchain.uCurrentImageIndex);
    if(err == VK_SUBOPTIMAL_KHR || err == VK_ERROR_OUT_OF_DATE_KHR)
    {
        if(err == VK_ERROR_OUT_OF_DATE_KHR)
        {
            pl__create_swapchain(&ptGraphics->tDevice, ptGraphics->tSurface, (uint32_t)ptIOCtx->afMainViewportSize[0], (uint32_t)ptIOCtx->afMainViewportSize[1], &ptGraphics->tSwapchain);
            pl__create_framebuffers(&ptGraphics->tDevice, ptGraphics->tRenderPass, &ptGraphics->tSwapchain);
            return false;
        }
    }
    else
    {
        PL_VULKAN(err);
    }

    if (ptCurrentFrame->tInFlight != VK_NULL_HANDLE)
        PL_VULKAN(vkWaitForFences(ptGraphics->tDevice.tLogicalDevice, 1, &ptCurrentFrame->tInFlight, VK_TRUE, UINT64_MAX));

    return true; 
}

void
pl_end_frame(plVulkanGraphics* ptGraphics)
{
    plVulkanFrameContext* ptCurrentFrame = pl_get_frame_resources(ptGraphics);
    plIOContext* ptIOCtx = pl_get_io_context();

    // submit
    const VkPipelineStageFlags atWaitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    const VkSubmitInfo tSubmitInfo = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = 1,
        .pWaitSemaphores      = &ptCurrentFrame->tImageAvailable,
        .pWaitDstStageMask    = atWaitStages,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &ptCurrentFrame->tCmdBuf,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = &ptCurrentFrame->tRenderFinish
    };
    PL_VULKAN(vkResetFences(ptGraphics->tDevice.tLogicalDevice, 1, &ptCurrentFrame->tInFlight));
    PL_VULKAN(vkQueueSubmit(ptGraphics->tDevice.tGraphicsQueue, 1, &tSubmitInfo, ptCurrentFrame->tInFlight));          
    
    // present                        
    const VkPresentInfoKHR tPresentInfo = {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &ptCurrentFrame->tRenderFinish,
        .swapchainCount     = 1,
        .pSwapchains        = &ptGraphics->tSwapchain.tSwapChain,
        .pImageIndices      = &ptGraphics->tSwapchain.uCurrentImageIndex,
    };
    const VkResult tResult = vkQueuePresentKHR(ptGraphics->tDevice.tPresentQueue, &tPresentInfo);
    if(tResult == VK_SUBOPTIMAL_KHR || tResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        pl__create_swapchain(&ptGraphics->tDevice, ptGraphics->tSurface, (uint32_t)ptIOCtx->afMainViewportSize[0], (uint32_t)ptIOCtx->afMainViewportSize[1], &ptGraphics->tSwapchain);
        pl__create_framebuffers(&ptGraphics->tDevice, ptGraphics->tRenderPass, &ptGraphics->tSwapchain);
    }
    else
    {
        PL_VULKAN(tResult);
    }

    ptGraphics->szCurrentFrameIndex = (ptGraphics->szCurrentFrameIndex + 1) % ptGraphics->uFramesInFlight;
}

void
pl_resize_graphics(plVulkanGraphics* ptGraphics)
{
    plIOContext* ptIOCtx = pl_get_io_context();

    pl__create_swapchain(&ptGraphics->tDevice, ptGraphics->tSurface, (uint32_t)ptIOCtx->afMainViewportSize[0], (uint32_t)ptIOCtx->afMainViewportSize[1], &ptGraphics->tSwapchain);
    pl__create_framebuffers(&ptGraphics->tDevice, ptGraphics->tRenderPass, &ptGraphics->tSwapchain);  

    VkCommandBuffer tCommandBuffer = pl_begin_command_buffer(ptGraphics, &ptGraphics->tDevice);
    const VkImageSubresourceRange tRange = {
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
        .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT
    };
    pl_transition_image_layout(tCommandBuffer, ptGraphics->tSwapchain.tDepthImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, tRange, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT , VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT  );
    pl_submit_command_buffer(ptGraphics, &ptGraphics->tDevice, tCommandBuffer);    
}

void
pl_begin_recording(plVulkanGraphics* ptGraphics)
{
    const plVulkanFrameContext* ptCurrentFrame = pl_get_frame_resources(ptGraphics);

    const VkCommandBufferBeginInfo tBeginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    vkResetCommandBuffer(ptCurrentFrame->tCmdBuf, 0);
    PL_VULKAN(vkBeginCommandBuffer(ptCurrentFrame->tCmdBuf, &tBeginInfo));    
}

void
pl_end_recording(plVulkanGraphics* ptGraphics)
{
    const plVulkanFrameContext* ptCurrentFrame = pl_get_frame_resources(ptGraphics);
    PL_VULKAN(vkEndCommandBuffer(ptCurrentFrame->tCmdBuf));
}

void
pl_begin_main_pass(plVulkanGraphics* ptGraphics)
{
    static const VkClearValue atClearValues[2] = 
    {
        {
            .color.float32[0] = 0.1f,
            .color.float32[1] = 0.0f,
            .color.float32[2] = 0.0f,
            .color.float32[3] = 1.0f
        },
        {
            .depthStencil.depth = 1.0f,
            .depthStencil.stencil = 0
        }    
    };

    const plVulkanFrameContext* ptCurrentFrame = pl_get_frame_resources(ptGraphics);

    const VkRenderPassBeginInfo tRenderPassBeginInfo = {
        .sType               = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass          = ptGraphics->tRenderPass,
        .framebuffer         = ptGraphics->tSwapchain.ptFrameBuffers[ptGraphics->tSwapchain.uCurrentImageIndex],
        .renderArea.offset.x = 0,
        .renderArea.offset.y = 0,
        .renderArea.extent   = ptGraphics->tSwapchain.tExtent,
        .clearValueCount     = 2,
        .pClearValues        = atClearValues
    };
    vkCmdBeginRenderPass(ptCurrentFrame->tCmdBuf, &tRenderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

    // set viewport
    const VkViewport tViewport = {
        .x        = 0.0f,
        .y        = 0.0f,
        .width    = (float)ptGraphics->tSwapchain.tExtent.width,
        .height   = (float)ptGraphics->tSwapchain.tExtent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    vkCmdSetViewport(ptCurrentFrame->tCmdBuf, 0, 1, &tViewport);

    // set scissor
    const VkRect2D tDynamicScissor = {
        .extent = ptGraphics->tSwapchain.tExtent
    };
    vkCmdSetScissor(ptCurrentFrame->tCmdBuf, 0, 1, &tDynamicScissor);
}

void
pl_end_main_pass(plVulkanGraphics* ptGraphics)
{
    const plVulkanFrameContext* ptCurrentFrame = pl_get_frame_resources(ptGraphics);
    vkCmdEndRenderPass(ptCurrentFrame->tCmdBuf);
}

VkCommandBuffer
pl_begin_command_buffer(plVulkanGraphics* ptGraphics, plVulkanDevice* ptDevice)
{
    VkCommandBuffer tCommandBuffer = {0};
    
    VkCommandBufferAllocateInfo tAllocInfo = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandPool        = ptGraphics->tCmdPool,
        .commandBufferCount = 1u,
    };
    vkAllocateCommandBuffers(ptDevice->tLogicalDevice, &tAllocInfo, &tCommandBuffer);

    VkCommandBufferBeginInfo tBeginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    vkBeginCommandBuffer(tCommandBuffer, &tBeginInfo);

    return tCommandBuffer;  
}

void
pl_submit_command_buffer(plVulkanGraphics* ptGraphics, plVulkanDevice* ptDevice, VkCommandBuffer tCmdBuffer)
{
    vkEndCommandBuffer(tCmdBuffer);
    VkSubmitInfo tSubmitInfo = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1u,
        .pCommandBuffers    = &tCmdBuffer,
    };

    vkQueueSubmit(ptDevice->tGraphicsQueue, 1, &tSubmitInfo, VK_NULL_HANDLE);
    vkDeviceWaitIdle(ptDevice->tLogicalDevice);
    vkFreeCommandBuffers(ptDevice->tLogicalDevice, ptGraphics->tCmdPool, 1, &tCmdBuffer);
}

void
pl_process_cleanup_queue(plVulkanResourceManager* ptResourceManager, uint32_t uFramesToProcess)
{

    const VkDevice tDevice = ptResourceManager->_ptDevice->tLogicalDevice;

    pl_sb_reset(ptResourceManager->_sbulTempQueue);

    bool bNeedUpdate = false;

    for(uint32_t i = 0; i < pl_sb_size(ptResourceManager->_sbulDeletionQueue); i++)
    {
        uint64_t ulBufferIndex = ptResourceManager->_sbulDeletionQueue[i];

        plVulkanBuffer* ptBuffer = &ptResourceManager->sbtBuffers[ulBufferIndex];

        // we are hiding the frame
        if(ptBuffer->szStride < uFramesToProcess)
            ptBuffer->szStride = 0;
        else
            ptBuffer->szStride -= uFramesToProcess;

        if(ptBuffer->szStride == 0)
        {
            if(ptBuffer->pucMapping)
                vkUnmapMemory(tDevice, ptBuffer->tBufferMemory);

            vkDestroyBuffer(tDevice, ptBuffer->tBuffer, NULL);
            vkFreeMemory(tDevice, ptBuffer->tBufferMemory, NULL);

            ptBuffer->pucMapping = NULL;
            ptBuffer->tBuffer = VK_NULL_HANDLE;
            ptBuffer->tBufferMemory = VK_NULL_HANDLE;
            ptBuffer->szSize = 0;
            ptBuffer->szRequestedSize = 0;
            ptBuffer->tUsage = PL_BUFFER_USAGE_UNSPECIFIED;

            // add to free indices
            pl_sb_push(ptResourceManager->_sbulFreeIndices, ulBufferIndex);

            bNeedUpdate = true;
        }
        else
        {
            pl_sb_push(ptResourceManager->_sbulTempQueue, ulBufferIndex);
        }
    }

    if(bNeedUpdate)
    {
        // copy temporary queue data over
        pl_sb_reset(ptResourceManager->_sbulDeletionQueue);
        pl_sb_resize(ptResourceManager->_sbulDeletionQueue, pl_sb_size(ptResourceManager->_sbulTempQueue));
        if(ptResourceManager->_sbulTempQueue)
            memcpy(ptResourceManager->_sbulDeletionQueue, ptResourceManager->_sbulTempQueue, pl_sb_size(ptResourceManager->_sbulTempQueue) * sizeof(uint64_t));
    }
}

void
pl_transfer_data_to_buffer(plVulkanResourceManager* ptResourceManager, VkBuffer tDest, size_t szSize, const void* pData)
{
    pl__staging_buffer_may_grow(ptResourceManager, szSize);

    // copy data
    memcpy(ptResourceManager->_pucMapping, pData, szSize);

    // flush memory (incase we are using non-coherent memory)
    const VkMappedMemoryRange tMemoryRange = {
        .sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = ptResourceManager->_tStagingBufferMemory,
        .size   = VK_WHOLE_SIZE
    };
    PL_VULKAN(vkFlushMappedMemoryRanges(ptResourceManager->_ptDevice->tLogicalDevice, 1, &tMemoryRange));

    // perform copy from staging buffer to destination buffer
    VkCommandBuffer tCommandBuffer = pl_begin_command_buffer(ptResourceManager->_ptGraphics, ptResourceManager->_ptDevice);

    const VkBufferCopy tCopyRegion = {
        .size = szSize
    };
    vkCmdCopyBuffer(tCommandBuffer, ptResourceManager->_tStagingBuffer, tDest, 1, &tCopyRegion);
    pl_submit_command_buffer(ptResourceManager->_ptGraphics, ptResourceManager->_ptDevice, tCommandBuffer);

}

uint64_t
pl_create_index_buffer(plVulkanResourceManager* ptResourceManager, size_t szSize, const void* pData)
{
    const VkDevice tDevice = ptResourceManager->_ptDevice->tLogicalDevice;

    plVulkanBuffer tBuffer = {
        .tUsage          = PL_BUFFER_USAGE_INDEX,
        .szRequestedSize = szSize
    };

    // create vulkan buffer
    const VkBufferCreateInfo tBufferCreateInfo = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = szSize,
        .usage       = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    PL_VULKAN(vkCreateBuffer(tDevice, &tBufferCreateInfo, NULL, &tBuffer.tBuffer));

    // get memory requirements
    VkMemoryRequirements tMemoryRequirements = {0};
    vkGetBufferMemoryRequirements(tDevice, tBuffer.tBuffer, &tMemoryRequirements);
    tBuffer.szSize = tMemoryRequirements.size;

    // allocate buffer
    const VkMemoryAllocateInfo tAllocInfo = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = tMemoryRequirements.size,
        .memoryTypeIndex = pl_find_memory_type(ptResourceManager->_ptDevice->tMemProps, tMemoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    PL_VULKAN(vkAllocateMemory(tDevice, &tAllocInfo, NULL, &tBuffer.tBufferMemory));
    PL_VULKAN(vkBindBufferMemory(tDevice, tBuffer.tBuffer, tBuffer.tBufferMemory, 0));

    // upload data if any is availble
    if(pData)
        pl_transfer_data_to_buffer(ptResourceManager, tBuffer.tBuffer, szSize, pData);

    // find free index
    const uint64_t ulBufferIndex = pl__get_free_buffer_index(ptResourceManager);
    ptResourceManager->sbtBuffers[ulBufferIndex] = tBuffer;
    return ulBufferIndex;
}

uint64_t
pl_create_vertex_buffer(plVulkanResourceManager* ptResourceManager, size_t szSize, size_t szStride, const void* pData)
{
    const VkDevice tDevice = ptResourceManager->_ptDevice->tLogicalDevice;

    plVulkanBuffer tBuffer = {
        .tUsage          = PL_BUFFER_USAGE_VERTEX,
        .szRequestedSize = szSize,
        .szStride        = szStride
    };

    // create vulkan buffer
    const VkBufferCreateInfo tBufferCreateInfo = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = szSize,
        .usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    PL_VULKAN(vkCreateBuffer(tDevice, &tBufferCreateInfo, NULL, &tBuffer.tBuffer));

    // get memory requirements
    VkMemoryRequirements tMemoryRequirements = {0};
    vkGetBufferMemoryRequirements(tDevice, tBuffer.tBuffer, &tMemoryRequirements);
    tBuffer.szSize = tMemoryRequirements.size;

    // allocate buffer
    const VkMemoryAllocateInfo tAllocInfo = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = tMemoryRequirements.size,
        .memoryTypeIndex = pl_find_memory_type(ptResourceManager->_ptDevice->tMemProps, tMemoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    PL_VULKAN(vkAllocateMemory(tDevice, &tAllocInfo, NULL, &tBuffer.tBufferMemory));
    PL_VULKAN(vkBindBufferMemory(tDevice, tBuffer.tBuffer, tBuffer.tBufferMemory, 0));

    // upload data if any is availble
    if(pData)
        pl_transfer_data_to_buffer(ptResourceManager, tBuffer.tBuffer, szSize, pData);

    // find free index
    const uint64_t ulBufferIndex = pl__get_free_buffer_index(ptResourceManager);
    ptResourceManager->sbtBuffers[ulBufferIndex] = tBuffer;
    return ulBufferIndex;  
}

uint64_t
pl_create_constant_buffer(plVulkanResourceManager* ptResourceManager, size_t szItemSize, size_t szItemCount)
{
    const VkDevice tDevice = ptResourceManager->_ptDevice->tLogicalDevice;

    plVulkanBuffer tBuffer = {
        .tUsage          = PL_BUFFER_USAGE_CONSTANT,
        .szRequestedSize = szItemSize * szItemCount
    };

    const size_t szRequiredSize = pl__get_const_buffer_req_size(ptResourceManager->_ptDevice, szItemSize * szItemCount);

    // create vulkan buffer
    const VkBufferCreateInfo tBufferCreateInfo = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = szRequiredSize,
        .usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    PL_VULKAN(vkCreateBuffer(tDevice, &tBufferCreateInfo, NULL, &tBuffer.tBuffer));

    // get memory requirements
    VkMemoryRequirements tMemoryRequirements = {0};
    vkGetBufferMemoryRequirements(tDevice, tBuffer.tBuffer, &tMemoryRequirements);
    tBuffer.szSize = tMemoryRequirements.size;
    tBuffer.szStride = szItemSize;

    // allocate buffer
    const VkMemoryAllocateInfo tAllocInfo = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = tMemoryRequirements.size,
        .memoryTypeIndex = pl_find_memory_type(ptResourceManager->_ptDevice->tMemProps, tMemoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
    PL_VULKAN(vkAllocateMemory(tDevice, &tAllocInfo, NULL, &tBuffer.tBufferMemory));
    PL_VULKAN(vkBindBufferMemory(tDevice, tBuffer.tBuffer, tBuffer.tBufferMemory, 0));
    PL_VULKAN(vkMapMemory(tDevice, tBuffer.tBufferMemory, 0, tMemoryRequirements.size, 0, (void**)&tBuffer.pucMapping));

    // find free index
    const uint64_t ulBufferIndex = pl__get_free_buffer_index(ptResourceManager);
    ptResourceManager->sbtBuffers[ulBufferIndex] = tBuffer;
    return ulBufferIndex;  
}

void
pl_submit_buffer_for_deletion(plVulkanResourceManager* ptResourceManager, uint64_t ulBufferIndex)
{
    PL_ASSERT(ulBufferIndex < pl_sb_size(ptResourceManager->sbtBuffers)); 
    pl_sb_push(ptResourceManager->_sbulDeletionQueue, ulBufferIndex);

    // using szStride member to store frame this buffer is ok to free
    ptResourceManager->sbtBuffers[ulBufferIndex].szStride = (size_t)ptResourceManager->_ptGraphics->uFramesInFlight;
}

void
pl_cleanup_graphics(plVulkanGraphics* ptGraphics)
{

    // ensure device is finished
    vkDeviceWaitIdle(ptGraphics->tDevice.tLogicalDevice);

    pl__cleanup_resource_manager(&ptGraphics->tResourceManager);

    if(ptGraphics->tSwapchain.tDepthImageView) vkDestroyImageView(ptGraphics->tDevice.tLogicalDevice, ptGraphics->tSwapchain.tDepthImageView, NULL);
    if(ptGraphics->tSwapchain.tDepthImage)     vkDestroyImage(ptGraphics->tDevice.tLogicalDevice, ptGraphics->tSwapchain.tDepthImage, NULL);
    if(ptGraphics->tSwapchain.tDepthMemory)    vkFreeMemory(ptGraphics->tDevice.tLogicalDevice, ptGraphics->tSwapchain.tDepthMemory, NULL);
    if(ptGraphics->tSwapchain.tColorImageView) vkDestroyImageView(ptGraphics->tDevice.tLogicalDevice, ptGraphics->tSwapchain.tColorImageView, NULL);
    if(ptGraphics->tSwapchain.tColorImage)     vkDestroyImage(ptGraphics->tDevice.tLogicalDevice, ptGraphics->tSwapchain.tColorImage, NULL);
    if(ptGraphics->tSwapchain.tColorMemory)    vkFreeMemory(ptGraphics->tDevice.tLogicalDevice, ptGraphics->tSwapchain.tColorMemory, NULL);

    // destroy swapchain
    for (uint32_t i = 0u; i < ptGraphics->tSwapchain.uImageCount; i++)
    {

        vkDestroyImageView(ptGraphics->tDevice.tLogicalDevice, ptGraphics->tSwapchain.ptImageViews[i], NULL);
        vkDestroyFramebuffer(ptGraphics->tDevice.tLogicalDevice, ptGraphics->tSwapchain.ptFrameBuffers[i], NULL);
    }

    vkDestroyDescriptorPool(ptGraphics->tDevice.tLogicalDevice, ptGraphics->tDescriptorPool, NULL);

    // destroy default render pass
    vkDestroyRenderPass(ptGraphics->tDevice.tLogicalDevice, ptGraphics->tRenderPass, NULL);
    vkDestroySwapchainKHR(ptGraphics->tDevice.tLogicalDevice, ptGraphics->tSwapchain.tSwapChain, NULL);

    for(uint32_t i = 0; i < ptGraphics->uFramesInFlight; i++)
    {
        // destroy command buffers
        vkFreeCommandBuffers(ptGraphics->tDevice.tLogicalDevice, ptGraphics->tCmdPool, 1u, &ptGraphics->sbFrames[i].tCmdBuf);

        // destroy sync primitives
        vkDestroySemaphore(ptGraphics->tDevice.tLogicalDevice, ptGraphics->sbFrames[i].tImageAvailable, NULL);
        vkDestroySemaphore(ptGraphics->tDevice.tLogicalDevice, ptGraphics->sbFrames[i].tRenderFinish, NULL);
        vkDestroyFence(ptGraphics->tDevice.tLogicalDevice, ptGraphics->sbFrames[i].tInFlight, NULL);
    }

    // destroy command pool
    vkDestroyCommandPool(ptGraphics->tDevice.tLogicalDevice, ptGraphics->tCmdPool, NULL);

    if(ptGraphics->tDbgMessenger)
    {
        PFN_vkDestroyDebugUtilsMessengerEXT tFunc = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(ptGraphics->tInstance, "vkDestroyDebugUtilsMessengerEXT");
        if (tFunc != NULL)
            tFunc(ptGraphics->tInstance, ptGraphics->tDbgMessenger, NULL);
    }

    // destroy tSurface
    vkDestroySurfaceKHR(ptGraphics->tInstance, ptGraphics->tSurface, NULL);

    // destroy device
    vkDestroyDevice(ptGraphics->tDevice.tLogicalDevice, NULL);

    // destroy tInstance
    vkDestroyInstance(ptGraphics->tInstance, NULL);
}


plVulkanFrameContext*
pl_get_frame_resources(plVulkanGraphics* ptGraphics)
{
    return &ptGraphics->sbFrames[ptGraphics->szCurrentFrameIndex];
}

uint32_t
pl_find_memory_type(VkPhysicalDeviceMemoryProperties tMemProps, uint32_t uTypeFilter, VkMemoryPropertyFlags tProperties)
{
    uint32_t uMemoryType = 0u;
    for (uint32_t i = 0; i < tMemProps.memoryTypeCount; i++) 
    {
        if ((uTypeFilter & (1 << i)) && (tMemProps.memoryTypes[i].propertyFlags & tProperties) == tProperties) 
        {
            uMemoryType = i;
            break;
        }
    }
    return uMemoryType;    
}

void 
pl_transition_image_layout(VkCommandBuffer tCommandBuffer, VkImage tImage, VkImageLayout tOldLayout, VkImageLayout tNewLayout, VkImageSubresourceRange tSubresourceRange, VkPipelineStageFlags tSrcStageMask, VkPipelineStageFlags tDstStageMask)
{
    //VkCommandBuffer commandBuffer = mvBeginSingleTimeCommands();
    VkImageMemoryBarrier tBarrier = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout           = tOldLayout,
        .newLayout           = tNewLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = tImage,
        .subresourceRange    = tSubresourceRange,
    };

    // Source layouts (old)
    // Source access mask controls actions that have to be finished on the old layout
    // before it will be transitioned to the new layout
    switch (tOldLayout)
    {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            // Image layout is undefined (or does not matter)
            // Only valid as initial layout
            // No flags required, listed only for completeness
            tBarrier.srcAccessMask = 0;
            break;

        case VK_IMAGE_LAYOUT_PREINITIALIZED:
            // Image is preinitialized
            // Only valid as initial layout for linear images, preserves memory contents
            // Make sure host writes have been finished
            tBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            break;

        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            // Image is a color attachment
            // Make sure any writes to the color buffer have been finished
            tBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            break;

        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            // Image is a depth/stencil attachment
            // Make sure any writes to the depth/stencil buffer have been finished
            tBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            break;

        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            // Image is a transfer source
            // Make sure any reads from the image have been finished
            tBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            break;

        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            // Image is a transfer destination
            // Make sure any writes to the image have been finished
            tBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;

        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            // Image is read by a shader
            // Make sure any shader reads from the image have been finished
            tBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            break;
        default:
            // Other source layouts aren't handled (yet)
            break;
        }

        // Target layouts (new)
        // Destination access mask controls the dependency for the new image layout
        switch (tNewLayout)
        {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            // Image will be used as a transfer destination
            // Make sure any writes to the image have been finished
            tBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;

        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            // Image will be used as a transfer source
            // Make sure any reads from the image have been finished
            tBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            break;

        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            // Image will be used as a color attachment
            // Make sure any writes to the color buffer have been finished
            tBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            break;

        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            // Image layout will be used as a depth/stencil attachment
            // Make sure any writes to depth/stencil buffer have been finished
            tBarrier.dstAccessMask = tBarrier.dstAccessMask | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            break;

        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            // Image will be read in a shader (sampler, input attachment)
            // Make sure any writes to the image have been finished
            if (tBarrier.srcAccessMask == 0)
                tBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            tBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            break;
        default:
            // Other source layouts aren't handled (yet)
            break;
    }
    vkCmdPipelineBarrier(tCommandBuffer, tSrcStageMask, tDstStageMask, 0, 0, NULL, 0, NULL, 1, &tBarrier);
}

VkFormat
pl_find_supported_format(plVulkanDevice* ptDevice, VkFormatFeatureFlags tFlags, const VkFormat* ptFormats, uint32_t uFormatCount)
{
    for(uint32_t i = 0u; i < uFormatCount; i++)
    {
        VkFormatProperties tProps = {0};
        vkGetPhysicalDeviceFormatProperties(ptDevice->tPhysicalDevice, ptFormats[i], &tProps);
        if(tProps.optimalTilingFeatures & tFlags)
            return ptFormats[i];
    }

    PL_ASSERT(false && "no supported format found");
    return VK_FORMAT_UNDEFINED;   
}

VkFormat
pl_find_depth_format(plVulkanDevice* ptDevice)
{
    const VkFormat atFormats[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT
    };
    return pl_find_supported_format(ptDevice, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, atFormats, 3);
}

bool
pl_format_has_stencil(VkFormat tFormat)
{
    switch(tFormat)
    {
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return true;

        case VK_FORMAT_D32_SFLOAT:
        default: return false;
    }
}

VkSampleCountFlagBits
pl_get_max_sample_count(plVulkanDevice* ptDevice)
{
    VkPhysicalDeviceProperties tPhysicalDeviceProperties = {0};
    vkGetPhysicalDeviceProperties(ptDevice->tPhysicalDevice, &tPhysicalDeviceProperties);

    VkSampleCountFlags tCounts = tPhysicalDeviceProperties.limits.framebufferColorSampleCounts & tPhysicalDeviceProperties.limits.framebufferDepthSampleCounts;
    if (tCounts & VK_SAMPLE_COUNT_64_BIT) { return VK_SAMPLE_COUNT_64_BIT; }
    if (tCounts & VK_SAMPLE_COUNT_32_BIT) { return VK_SAMPLE_COUNT_32_BIT; }
    if (tCounts & VK_SAMPLE_COUNT_16_BIT) { return VK_SAMPLE_COUNT_16_BIT; }
    if (tCounts & VK_SAMPLE_COUNT_8_BIT)  { return VK_SAMPLE_COUNT_8_BIT; }
    if (tCounts & VK_SAMPLE_COUNT_4_BIT)  { return VK_SAMPLE_COUNT_4_BIT; }
    if (tCounts & VK_SAMPLE_COUNT_2_BIT)  { return VK_SAMPLE_COUNT_2_BIT; }
    return VK_SAMPLE_COUNT_1_BIT;    
}

//-----------------------------------------------------------------------------
// [SECTION] internal implementations
//-----------------------------------------------------------------------------

static void
pl__create_instance(plVulkanGraphics* ptGraphics, uint32_t uVersion, bool bEnableValidation)
{
    static const char* pcKhronosValidationLayer = "VK_LAYER_KHRONOS_validation";

    const char** sbpcEnabledExtensions = NULL;
    pl_sb_push(sbpcEnabledExtensions, VK_KHR_SURFACE_EXTENSION_NAME);

    #ifdef _WIN32
        pl_sb_push(sbpcEnabledExtensions, VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    #elif defined(__APPLE__)
        pl_sb_push(sbpcEnabledExtensions, "VK_EXT_metal_surface");
        pl_sb_push(sbpcEnabledExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    #else // linux
        pl_sb_push(sbpcEnabledExtensions, VK_KHR_XCB_SURFACE_EXTENSION_NAME);
    #endif

    if(bEnableValidation)
        pl_sb_push(sbpcEnabledExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    pl__create_instance_ex(ptGraphics, uVersion, bEnableValidation ? 1 : 0, &pcKhronosValidationLayer, pl_sb_size(sbpcEnabledExtensions), sbpcEnabledExtensions);
}

static void
pl__create_instance_ex(plVulkanGraphics* ptGraphics, uint32_t uVersion, uint32_t uLayerCount, const char** ppcEnabledLayers, uint32_t uExtensioncount, const char** ppcEnabledExtensions)
{

    // check if validation should be activated
    bool bValidationEnabled = false;
    for(uint32_t i = 0; i < uLayerCount; i++)
    {
        if(strcmp("VK_LAYER_KHRONOS_validation", ppcEnabledLayers[i]) == 0)
        {
            bValidationEnabled = true;
            break;
        }
    }

    // retrieve supported layers
    uint32_t uInstanceLayersFound = 0u;
    VkLayerProperties* ptAvailableLayers = NULL;
    PL_VULKAN(vkEnumerateInstanceLayerProperties(&uInstanceLayersFound, NULL));
    if(uInstanceLayersFound > 0)
    {
        ptAvailableLayers = (VkLayerProperties*)malloc(sizeof(VkLayerProperties) * uInstanceLayersFound);
        PL_VULKAN(vkEnumerateInstanceLayerProperties(&uInstanceLayersFound, ptAvailableLayers));
    }

    // retrieve supported extensions
    uint32_t uInstanceExtensionsFound = 0u;
    VkExtensionProperties* ptAvailableExtensions = NULL;
    PL_VULKAN(vkEnumerateInstanceExtensionProperties(NULL, &uInstanceExtensionsFound, NULL));
    if(uInstanceExtensionsFound > 0)
    {
        ptAvailableExtensions = (VkExtensionProperties*)malloc(sizeof(VkExtensionProperties) * uInstanceExtensionsFound);
        PL_VULKAN(vkEnumerateInstanceExtensionProperties(NULL, &uInstanceExtensionsFound, ptAvailableExtensions));
    }

    // ensure extensions are supported
    const char** sbpcMissingExtensions = NULL;
    for(uint32_t i = 0; i < uExtensioncount; i++)
    {
        const char* requestedExtension = ppcEnabledExtensions[i];
        bool extensionFound = false;
        for(uint32_t j = 0; j < uInstanceExtensionsFound; j++)
        {
            if(strcmp(requestedExtension, ptAvailableExtensions[j].extensionName) == 0)
            {
                extensionFound = true;
                break;
            }
        }

        if(!extensionFound)
        {
            pl_sb_push(sbpcMissingExtensions, requestedExtension);
        }
    }

    // report if all requested extensions aren't found
    if(pl_sb_size(sbpcMissingExtensions) > 0)
    {
        printf("%d %s\n", pl_sb_size(sbpcMissingExtensions), "Missing Extensions:");
        for(uint32_t i = 0; i < pl_sb_size(sbpcMissingExtensions); i++)
        {
            printf("  * %s\n", sbpcMissingExtensions[i]);
        }
        PL_ASSERT(false && "Can't find all requested extensions");
    }

    // ensure layers are supported
    const char** sbpcMissingLayers = NULL;
    for(uint32_t i = 0; i < uLayerCount; i++)
    {
        const char* pcRequestedLayer = ppcEnabledLayers[i];
        bool bLayerFound = false;
        for(uint32_t j = 0; j < uInstanceLayersFound; j++)
        {
            if(strcmp(pcRequestedLayer, ptAvailableLayers[j].layerName) == 0)
            {
                bLayerFound = true;
                break;
            }
        }

        if(!bLayerFound)
        {
            pl_sb_push(sbpcMissingLayers, pcRequestedLayer);
        }
    }

    // report if all requested layers aren't found
    if(pl_sb_size(sbpcMissingLayers) > 0)
    {
        printf("%d %s\n", pl_sb_size(sbpcMissingLayers), "Missing Layers:");
        for(uint32_t i = 0; i < pl_sb_size(sbpcMissingLayers); i++)
        {
            printf("  * %s\n", sbpcMissingLayers[i]);
        }
        PL_ASSERT(false && "Can't find all requested layers");
    }

    // Setup debug messenger for vulkan tInstance
    VkDebugUtilsMessengerCreateInfoEXT tDebugCreateInfo = {
        .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = pl__debug_callback,
        .pNext           = VK_NULL_HANDLE
    };

    // create vulkan tInstance
    VkApplicationInfo tAppInfo = {
        .sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = uVersion
    };

    VkInstanceCreateInfo tCreateInfo = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo        = &tAppInfo,
        .pNext                   = bValidationEnabled ? (VkDebugUtilsMessengerCreateInfoEXT*)&tDebugCreateInfo : VK_NULL_HANDLE,
        .enabledExtensionCount   = uExtensioncount,
        .ppEnabledExtensionNames = ppcEnabledExtensions,
        .enabledLayerCount       = uLayerCount,
        .ppEnabledLayerNames     = ppcEnabledLayers,

        #ifdef __APPLE__
        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
        #endif
    };

    PL_VULKAN(vkCreateInstance(&tCreateInfo, NULL, &ptGraphics->tInstance));
    printf("%s\n", "created Vulkan tInstance.");

    // cleanup
    if(ptAvailableLayers)     free(ptAvailableLayers);
    if(ptAvailableExtensions) free(ptAvailableExtensions);
    pl_sb_free(sbpcMissingLayers);
    pl_sb_free(sbpcMissingExtensions);

    if(bValidationEnabled)
    {
        PFN_vkCreateDebugUtilsMessengerEXT func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(ptGraphics->tInstance, "vkCreateDebugUtilsMessengerEXT");
        PL_ASSERT(func != NULL && "failed to set up debug messenger!");
        PL_VULKAN(func(ptGraphics->tInstance, &tDebugCreateInfo, NULL, &ptGraphics->tDbgMessenger));
        printf("%s\n", "enabled Vulkan validation layers.");       
    }
}

static void
pl__create_device(VkInstance tInstance, VkSurfaceKHR tSurface, plVulkanDevice* ptDeviceOut, bool bEnableValidation)
{
    ptDeviceOut->iGraphicsQueueFamily = -1;
    ptDeviceOut->iPresentQueueFamily = -1;
    int iDeviceIndex = -1;
    ptDeviceOut->tMemProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    ptDeviceOut->tMemBudgetInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    ptDeviceOut->tMemProps2.pNext = &ptDeviceOut->tMemBudgetInfo;
    iDeviceIndex = pl__select_physical_device(tInstance, ptDeviceOut);
    ptDeviceOut->tMaxLocalMemSize = ptDeviceOut->tMemProps.memoryHeaps[iDeviceIndex].size;

    // find queue families
    uint32_t uQueueFamCnt = 0u;
    vkGetPhysicalDeviceQueueFamilyProperties(ptDeviceOut->tPhysicalDevice, &uQueueFamCnt, NULL);

    VkQueueFamilyProperties auQueueFamilies[64] = {0};
    vkGetPhysicalDeviceQueueFamilyProperties(ptDeviceOut->tPhysicalDevice, &uQueueFamCnt, auQueueFamilies);

    for(uint32_t i = 0; i < uQueueFamCnt; i++)
    {
        if (auQueueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) ptDeviceOut->iGraphicsQueueFamily = i;

        VkBool32 tPresentSupport = false;
        PL_VULKAN(vkGetPhysicalDeviceSurfaceSupportKHR(ptDeviceOut->tPhysicalDevice, i, tSurface, &tPresentSupport));

        if (tPresentSupport) ptDeviceOut->iPresentQueueFamily  = i;

        if (ptDeviceOut->iGraphicsQueueFamily > -1 && ptDeviceOut->iPresentQueueFamily > -1) // complete
            break;
        i++;
    }

    //-----------------------------------------------------------------------------
    // create logical device
    //-----------------------------------------------------------------------------

    VkPhysicalDeviceFeatures atDeviceFeatures = {0};
    const float fQueuePriority = 1.0f;
    VkDeviceQueueCreateInfo atQueueCreateInfos[] = {
        {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = ptDeviceOut->iGraphicsQueueFamily,
            .queueCount = 1,
            .pQueuePriorities = &fQueuePriority
        },
        {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = ptDeviceOut->iPresentQueueFamily,
            .queueCount = 1,
            .pQueuePriorities = &fQueuePriority   
        }
    };
    
    static const char* pcValidationLayers = "VK_LAYER_KHRONOS_validation";
    static const char* apcExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_portability_subset"};
    VkDeviceCreateInfo tCreateDeviceInfo = {
        .sType                    = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount     = atQueueCreateInfos[0].queueFamilyIndex == atQueueCreateInfos[1].queueFamilyIndex ? 1 : 2,
        .pQueueCreateInfos        = atQueueCreateInfos,
        .pEnabledFeatures         = &atDeviceFeatures,
        .ppEnabledExtensionNames  = apcExtensions,
        .enabledLayerCount        = bEnableValidation ? 1 : 0,
        .ppEnabledLayerNames      = bEnableValidation ? &pcValidationLayers : NULL,
        #ifdef __APPLE__
            .enabledExtensionCount = 2u,
        #else
            .enabledExtensionCount = 1u
        #endif
    };
    PL_VULKAN(vkCreateDevice(ptDeviceOut->tPhysicalDevice, &tCreateDeviceInfo, NULL, &ptDeviceOut->tLogicalDevice));

    // get device queues
    vkGetDeviceQueue(ptDeviceOut->tLogicalDevice, ptDeviceOut->iGraphicsQueueFamily, 0, &ptDeviceOut->tGraphicsQueue);
    vkGetDeviceQueue(ptDeviceOut->tLogicalDevice, ptDeviceOut->iPresentQueueFamily, 0, &ptDeviceOut->tPresentQueue);
}

static void
pl__create_frame_resources(plVulkanGraphics* ptGraphics, plVulkanDevice* ptDevice)
{
    // create command pool
    VkCommandPoolCreateInfo tCommandPoolInfo = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = ptDevice->iGraphicsQueueFamily,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
    };
    PL_VULKAN(vkCreateCommandPool(ptDevice->tLogicalDevice, &tCommandPoolInfo, NULL, &ptGraphics->tCmdPool));

    VkSemaphoreCreateInfo tSemaphoreInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };

    VkFenceCreateInfo tFenceInfo = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };

    VkCommandBufferAllocateInfo tAllocInfo = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = ptGraphics->tCmdPool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };

    ptGraphics->uFramesInFlight = PL_MAX_FRAMES_IN_FLIGHT;
    pl_sb_reserve(ptGraphics->sbFrames, ptGraphics->uFramesInFlight);
    for(uint32_t i = 0; i < ptGraphics->uFramesInFlight; i++)
    {
        plVulkanFrameContext tFrame = {0};
        PL_VULKAN(vkCreateSemaphore(ptDevice->tLogicalDevice, &tSemaphoreInfo, NULL, &tFrame.tImageAvailable));
        PL_VULKAN(vkCreateSemaphore(ptDevice->tLogicalDevice, &tSemaphoreInfo, NULL, &tFrame.tRenderFinish));
        PL_VULKAN(vkCreateFence(ptDevice->tLogicalDevice, &tFenceInfo, NULL, &tFrame.tInFlight));
        PL_VULKAN(vkAllocateCommandBuffers(ptDevice->tLogicalDevice, &tAllocInfo, &tFrame.tCmdBuf));  
        pl_sb_push(ptGraphics->sbFrames, tFrame);
    }
}

static void
pl__create_framebuffers(plVulkanDevice* ptDevice, VkRenderPass tRenderPass, plVulkanSwapchain* ptSwapchain)
{
    for(uint32_t i = 0; i < ptSwapchain->uImageCount; i++)
    {
        VkImageView atAttachments[] = {
            ptSwapchain->tColorImageView,
            ptSwapchain->tDepthImageView,
            ptSwapchain->ptImageViews[i]
        };
        VkFramebufferCreateInfo tFrameBufferInfo = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = tRenderPass,
            .attachmentCount = 3u,
            .pAttachments    = atAttachments,
            .width           = ptSwapchain->tExtent.width,
            .height          = ptSwapchain->tExtent.height,
            .layers          = 1u,
        };
        PL_VULKAN(vkCreateFramebuffer(ptDevice->tLogicalDevice, &tFrameBufferInfo, NULL, &ptSwapchain->ptFrameBuffers[i]));
    }
}

static void
pl__create_swapchain(plVulkanDevice* ptDevice, VkSurfaceKHR tSurface, uint32_t uWidth, uint32_t uHeight, plVulkanSwapchain* ptSwapchainOut)
{
    vkDeviceWaitIdle(ptDevice->tLogicalDevice);

    ptSwapchainOut->tMsaaSamples = pl_get_max_sample_count(ptDevice);

    //-----------------------------------------------------------------------------
    // query swapchain support
    //----------------------------------------------------------------------------- 

    VkSurfaceCapabilitiesKHR tCapabilities = {0};
    PL_VULKAN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ptDevice->tPhysicalDevice, tSurface, &tCapabilities));

    uint32_t uFormatCount = 0u;
    PL_VULKAN(vkGetPhysicalDeviceSurfaceFormatsKHR(ptDevice->tPhysicalDevice, tSurface, &uFormatCount, NULL));
    
    if(uFormatCount >ptSwapchainOut->uSurfaceFormatCapacity_)
    {
        if(ptSwapchainOut->ptSurfaceFormats_) free(ptSwapchainOut->ptSurfaceFormats_);

        ptSwapchainOut->ptSurfaceFormats_ = malloc(sizeof(VkSurfaceFormatKHR) * uFormatCount);
        ptSwapchainOut->uSurfaceFormatCapacity_ = uFormatCount;
    }

    VkBool32 tPresentSupport = false;
    PL_VULKAN(vkGetPhysicalDeviceSurfaceSupportKHR(ptDevice->tPhysicalDevice, 0, tSurface, &tPresentSupport));
    PL_ASSERT(uFormatCount > 0);
    PL_VULKAN(vkGetPhysicalDeviceSurfaceFormatsKHR(ptDevice->tPhysicalDevice, tSurface, &uFormatCount, ptSwapchainOut->ptSurfaceFormats_));

    uint32_t uPresentModeCount = 0u;
    PL_VULKAN(vkGetPhysicalDeviceSurfacePresentModesKHR(ptDevice->tPhysicalDevice, tSurface, &uPresentModeCount, NULL));
    PL_ASSERT(uPresentModeCount > 0 && uPresentModeCount < 16);

    VkPresentModeKHR atPresentModes[16] = {0};
    PL_VULKAN(vkGetPhysicalDeviceSurfacePresentModesKHR(ptDevice->tPhysicalDevice, tSurface, &uPresentModeCount, atPresentModes));

    // choose swap tSurface Format
    static VkFormat atSurfaceFormatPreference[4] = 
    {
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_B8G8R8A8_SRGB
    };

    bool bPreferenceFound = false;
    VkSurfaceFormatKHR tSurfaceFormat = ptSwapchainOut->ptSurfaceFormats_[0];

    for(uint32_t i = 0u; i < 4; i++)
    {
        if(bPreferenceFound) break;
        
        for(uint32_t j = 0u; j < uFormatCount; j++)
        {
            if(ptSwapchainOut->ptSurfaceFormats_[j].format == atSurfaceFormatPreference[i] && ptSwapchainOut->ptSurfaceFormats_[j].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                tSurfaceFormat = ptSwapchainOut->ptSurfaceFormats_[j];
                ptSwapchainOut->tFormat = tSurfaceFormat.format;
                bPreferenceFound = true;
                break;
            }
        }
    }
    PL_ASSERT(bPreferenceFound && "no preferred tSurface format found");

    // chose swap present mode
    VkPresentModeKHR tPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    if(!ptSwapchainOut->bVSync)
    {
        for(uint32_t i = 0 ; i < uPresentModeCount; i++)
        {
			if (atPresentModes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
			{
				tPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
				break;
			}
			if (atPresentModes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
				tPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
    }

    // chose swap extent 
    VkExtent2D tExtent = {0};
    if(tCapabilities.currentExtent.width != UINT32_MAX)
        tExtent = tCapabilities.currentExtent;
    else
    {
        tExtent.width = pl__get_u32_max(tCapabilities.minImageExtent.width, pl__get_u32_min(tCapabilities.maxImageExtent.width, uWidth));
        tExtent.height = pl__get_u32_max(tCapabilities.minImageExtent.height, pl__get_u32_min(tCapabilities.maxImageExtent.height, uHeight));
    }
    ptSwapchainOut->tExtent = tExtent;

    // decide image count
    const uint32_t uOldImageCount = ptSwapchainOut->uImageCount;
    uint32_t uMinImageCount = tCapabilities.minImageCount + 1;
    if(tCapabilities.maxImageCount > 0 && uMinImageCount > tCapabilities.maxImageCount) 
        uMinImageCount = tCapabilities.maxImageCount;

    VkSwapchainCreateInfoKHR tCreateSwapchainInfo = {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = tSurface,
        .minImageCount    = uMinImageCount,
        .imageFormat      = tSurfaceFormat.format,
        .imageColorSpace  = tSurfaceFormat.colorSpace,
        .imageExtent      = tExtent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform     = tCapabilities.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = tPresentMode,
        .clipped          = VK_TRUE,
        .oldSwapchain     = ptSwapchainOut->tSwapChain,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    uint32_t auQueueFamilyIndices[] = { (uint32_t)ptDevice->iGraphicsQueueFamily, (uint32_t)ptDevice->iPresentQueueFamily};
    if (ptDevice->iGraphicsQueueFamily != ptDevice->iPresentQueueFamily)
    {
        tCreateSwapchainInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        tCreateSwapchainInfo.queueFamilyIndexCount = 2;
        tCreateSwapchainInfo.pQueueFamilyIndices = auQueueFamilyIndices;
    }

    VkSwapchainKHR tOldSwapChain = ptSwapchainOut->tSwapChain;

    PL_VULKAN(vkCreateSwapchainKHR(ptDevice->tLogicalDevice, &tCreateSwapchainInfo, NULL, &ptSwapchainOut->tSwapChain));

    if(tOldSwapChain)
    {
        for (uint32_t i = 0u; i < uOldImageCount; i++)
        {
            vkDestroyImageView(ptDevice->tLogicalDevice, ptSwapchainOut->ptImageViews[i], NULL);
            vkDestroyFramebuffer(ptDevice->tLogicalDevice, ptSwapchainOut->ptFrameBuffers[i], NULL);
        }
        vkDestroySwapchainKHR(ptDevice->tLogicalDevice, tOldSwapChain, NULL);
    }

    vkGetSwapchainImagesKHR(ptDevice->tLogicalDevice, ptSwapchainOut->tSwapChain, &ptSwapchainOut->uImageCount, NULL);
    if(ptSwapchainOut->uImageCount > ptSwapchainOut->uImageCapacity)
    {
        ptSwapchainOut->uImageCapacity = ptSwapchainOut->uImageCount;
        if(ptSwapchainOut->ptImages)       free(ptSwapchainOut->ptImages);
        if(ptSwapchainOut->ptImageViews)   free(ptSwapchainOut->ptImageViews);
        if(ptSwapchainOut->ptFrameBuffers) free(ptSwapchainOut->ptFrameBuffers);
        ptSwapchainOut->ptImages         = malloc(sizeof(VkImage)*ptSwapchainOut->uImageCapacity);
        ptSwapchainOut->ptImageViews     = malloc(sizeof(VkImageView)*ptSwapchainOut->uImageCapacity);
        ptSwapchainOut->ptFrameBuffers   = malloc(sizeof(VkFramebuffer)*ptSwapchainOut->uImageCapacity);
    }
    vkGetSwapchainImagesKHR(ptDevice->tLogicalDevice, ptSwapchainOut->tSwapChain, &ptSwapchainOut->uImageCount, ptSwapchainOut->ptImages);

    for(uint32_t i = 0; i < ptSwapchainOut->uImageCount; i++)
    {

        VkImageViewCreateInfo tViewInfo = {
            .sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image                           = ptSwapchainOut->ptImages[i],
            .viewType                        = VK_IMAGE_VIEW_TYPE_2D,
            .format                          = ptSwapchainOut->tFormat,
            .subresourceRange.baseMipLevel   = 0,
            .subresourceRange.levelCount     = 1,
            .subresourceRange.baseArrayLayer = 0,
            .subresourceRange.layerCount     = 1,
            .subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        };

        PL_VULKAN(vkCreateImageView(ptDevice->tLogicalDevice, &tViewInfo, NULL, &ptSwapchainOut->ptImageViews[i]));   
    }  //-V1020

    // color & depth
    if(ptSwapchainOut->tColorImageView) vkDestroyImageView(ptDevice->tLogicalDevice, ptSwapchainOut->tColorImageView, NULL);
    if(ptSwapchainOut->tColorImage)     vkDestroyImage(ptDevice->tLogicalDevice, ptSwapchainOut->tColorImage, NULL);
    if(ptSwapchainOut->tColorMemory)    vkFreeMemory(ptDevice->tLogicalDevice, ptSwapchainOut->tColorMemory, NULL);
    ptSwapchainOut->tColorImageView = VK_NULL_HANDLE;
    ptSwapchainOut->tColorImage     = VK_NULL_HANDLE;
    ptSwapchainOut->tColorMemory    = VK_NULL_HANDLE;
    if(ptSwapchainOut->tDepthImageView) vkDestroyImageView(ptDevice->tLogicalDevice, ptSwapchainOut->tDepthImageView, NULL);
    if(ptSwapchainOut->tDepthImage)     vkDestroyImage(ptDevice->tLogicalDevice, ptSwapchainOut->tDepthImage, NULL);
    if(ptSwapchainOut->tDepthMemory)    vkFreeMemory(ptDevice->tLogicalDevice, ptSwapchainOut->tDepthMemory, NULL);
    ptSwapchainOut->tDepthImageView = VK_NULL_HANDLE;
    ptSwapchainOut->tDepthImage     = VK_NULL_HANDLE;
    ptSwapchainOut->tDepthMemory    = VK_NULL_HANDLE;

    VkImageCreateInfo tDepthImageInfo = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .extent.width  = ptSwapchainOut->tExtent.width,
        .extent.height = ptSwapchainOut->tExtent.height,
        .extent.depth  = 1,
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .format        = pl_find_depth_format(ptDevice),
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .samples       = ptSwapchainOut->tMsaaSamples,
        .flags         = 0
    };

    VkImageCreateInfo tColorImageInfo = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .extent.width  = ptSwapchainOut->tExtent.width,
        .extent.height = ptSwapchainOut->tExtent.height,
        .extent.depth  = 1,
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .format        = ptSwapchainOut->tFormat,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage         = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .samples       = ptSwapchainOut->tMsaaSamples,
        .flags         = 0
    };

    PL_VULKAN(vkCreateImage(ptDevice->tLogicalDevice, &tDepthImageInfo, NULL, &ptSwapchainOut->tDepthImage));
    PL_VULKAN(vkCreateImage(ptDevice->tLogicalDevice, &tColorImageInfo, NULL, &ptSwapchainOut->tColorImage));

    VkMemoryRequirements tDepthMemReqs = {0};
    VkMemoryRequirements tColorMemReqs = {0};
    vkGetImageMemoryRequirements(ptDevice->tLogicalDevice, ptSwapchainOut->tDepthImage, &tDepthMemReqs);
    vkGetImageMemoryRequirements(ptDevice->tLogicalDevice, ptSwapchainOut->tColorImage, &tColorMemReqs);

    VkMemoryAllocateInfo tDepthAllocInfo = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = tDepthMemReqs.size,
        .memoryTypeIndex = pl_find_memory_type(ptDevice->tMemProps, tDepthMemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };

    VkMemoryAllocateInfo tColorAllocInfo = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = tColorMemReqs.size,
        .memoryTypeIndex = pl_find_memory_type(ptDevice->tMemProps, tColorMemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };

    PL_VULKAN(vkAllocateMemory(ptDevice->tLogicalDevice, &tDepthAllocInfo, NULL, &ptSwapchainOut->tDepthMemory));
    PL_VULKAN(vkAllocateMemory(ptDevice->tLogicalDevice, &tColorAllocInfo, NULL, &ptSwapchainOut->tColorMemory));
    PL_VULKAN(vkBindImageMemory(ptDevice->tLogicalDevice, ptSwapchainOut->tDepthImage, ptSwapchainOut->tDepthMemory, 0));
    PL_VULKAN(vkBindImageMemory(ptDevice->tLogicalDevice, ptSwapchainOut->tColorImage, ptSwapchainOut->tColorMemory, 0));

    VkImageViewCreateInfo tDepthViewInfo = {
        .sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image                           = ptSwapchainOut->tDepthImage,
        .viewType                        = VK_IMAGE_VIEW_TYPE_2D,
        .format                          = tDepthImageInfo.format,
        .subresourceRange.baseMipLevel   = 0,
        .subresourceRange.levelCount     = 1,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount     = 1,
        .subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
    };

    if(pl_format_has_stencil(tDepthViewInfo.format))
        tDepthViewInfo.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;

    VkImageViewCreateInfo tColorViewInfo = {
        .sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image                           = ptSwapchainOut->tColorImage,
        .viewType                        = VK_IMAGE_VIEW_TYPE_2D,
        .format                          = tColorImageInfo.format,
        .subresourceRange.baseMipLevel   = 0,
        .subresourceRange.levelCount     = 1,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount     = 1,
        .subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
    };

    PL_VULKAN(vkCreateImageView(ptDevice->tLogicalDevice, &tDepthViewInfo, NULL, &ptSwapchainOut->tDepthImageView));
    PL_VULKAN(vkCreateImageView(ptDevice->tLogicalDevice, &tColorViewInfo, NULL, &ptSwapchainOut->tColorImageView));
}

static void
pl__create_resource_manager(plVulkanGraphics* ptGraphics, plVulkanDevice* ptDevice, plVulkanResourceManager* ptResourceManagerOut)
{
    ptResourceManagerOut->_ptGraphics = ptGraphics;
    ptResourceManagerOut->_ptDevice = ptDevice;
}

static void
pl__cleanup_resource_manager(plVulkanResourceManager* ptResourceManager)
{
    pl__staging_buffer_realloc(ptResourceManager, 0); // free staging buffer

    for(uint32_t i = 0; i < pl_sb_size(ptResourceManager->sbtBuffers); i++)
    {
        if(ptResourceManager->sbtBuffers[i].szSize > 0)
            pl_submit_buffer_for_deletion(ptResourceManager, (uint64_t)i);
    }
    pl_process_cleanup_queue(ptResourceManager, 100); // free deletion queued resources

    pl_sb_free(ptResourceManager->sbtBuffers);
    pl_sb_free(ptResourceManager->_sbulFreeIndices);
    pl_sb_free(ptResourceManager->_sbulDeletionQueue);
    pl_sb_free(ptResourceManager->_sbulTempQueue);

    ptResourceManager->_ptDevice = NULL;
    ptResourceManager->_ptGraphics = NULL;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
pl__debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT tMsgSeverity, VkDebugUtilsMessageTypeFlagsEXT tMsgType, const VkDebugUtilsMessengerCallbackDataEXT* ptCallbackData, void* pUserData) 
{
    if(tMsgSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        printf("error validation layer: %s\n", ptCallbackData->pMessage);
    }

    else if(tMsgSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        printf("warn validation layer: %s\n", ptCallbackData->pMessage);
    }

    // else if(tMsgSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
    // {
    //     printf("info validation layer: %s\n", ptCallbackData->pMessage);
    // }
    // else if(tMsgSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT)
    // {
    //     printf("trace validation layer: %s\n", ptCallbackData->pMessage);
    // }
    
    return VK_FALSE;
}

static int
pl__select_physical_device(VkInstance tInstance, plVulkanDevice* ptDeviceOut)
{
    uint32_t uDeviceCount = 0u;
    int iBestDvcIdx = 0;
    bool bDiscreteGPUFound = false;
    VkDeviceSize tMaxLocalMemorySize = 0u;

    PL_VULKAN(vkEnumeratePhysicalDevices(tInstance, &uDeviceCount, NULL));
    PL_ASSERT(uDeviceCount > 0 && "failed to find GPUs with Vulkan support!");

    // check if device is suitable
    VkPhysicalDevice atDevices[16] = {0};
    PL_VULKAN(vkEnumeratePhysicalDevices(tInstance, &uDeviceCount, atDevices));

    // prefer discrete, then memory size
    for(uint32_t i = 0; i < uDeviceCount; i++)
    {
        vkGetPhysicalDeviceProperties(atDevices[i], &ptDeviceOut->tDeviceProps);
        vkGetPhysicalDeviceMemoryProperties(atDevices[i], &ptDeviceOut->tMemProps);

        for(uint32_t j = 0; j < ptDeviceOut->tMemProps.memoryHeapCount; j++)
        {
            if(ptDeviceOut->tMemProps.memoryHeaps[j].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT && ptDeviceOut->tMemProps.memoryHeaps[j].size > tMaxLocalMemorySize && !bDiscreteGPUFound)
            {
                tMaxLocalMemorySize = ptDeviceOut->tMemProps.memoryHeaps[j].size;
                iBestDvcIdx = i;
            }
        }

        if(ptDeviceOut->tDeviceProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && !bDiscreteGPUFound)
        {
            iBestDvcIdx = i;
            bDiscreteGPUFound = true;
        }
    }

    ptDeviceOut->tPhysicalDevice = atDevices[iBestDvcIdx];
    PL_ASSERT(ptDeviceOut->tPhysicalDevice != VK_NULL_HANDLE && "failed to find a suitable GPU!");
    vkGetPhysicalDeviceProperties(atDevices[iBestDvcIdx], &ptDeviceOut->tDeviceProps);
    vkGetPhysicalDeviceMemoryProperties(atDevices[iBestDvcIdx], &ptDeviceOut->tMemProps);
    static const char* pacDeviceTypeName[] = {"Other", "Integrated", "Discrete", "Virtual", "CPU"};

    // print info on chosen device
    printf("Device ID: %u\n", ptDeviceOut->tDeviceProps.deviceID);
    printf("Vendor ID: %u\n", ptDeviceOut->tDeviceProps.vendorID);
    printf("API Version: %u\n", ptDeviceOut->tDeviceProps.apiVersion);
    printf("Driver Version: %u\n", ptDeviceOut->tDeviceProps.driverVersion);
    printf("Device Type: %s\n", pacDeviceTypeName[ptDeviceOut->tDeviceProps.deviceType]);
    printf("Device Name: %s\n", ptDeviceOut->tDeviceProps.deviceName);
    return iBestDvcIdx;
}

static inline void
pl__staging_buffer_may_grow(plVulkanResourceManager* ptResourceManager, size_t szSize)
{
    if(ptResourceManager->_szStagingBufferSize < szSize)
        pl__staging_buffer_realloc(ptResourceManager, szSize * 2);
}

static void
pl__staging_buffer_realloc(plVulkanResourceManager* ptResourceManager, size_t szNewSize)
{

    const VkDevice tDevice = ptResourceManager->_ptDevice->tLogicalDevice;

    // unmap host visible address
    if(ptResourceManager->_pucMapping && szNewSize != ptResourceManager->_szStagingBufferSize)
    {
        vkUnmapMemory(tDevice, ptResourceManager->_tStagingBufferMemory);
        ptResourceManager->_pucMapping = NULL;
    }

    if(szNewSize == 0) // free
    {
        if(ptResourceManager->_tStagingBuffer)       vkDestroyBuffer(tDevice, ptResourceManager->_tStagingBuffer, NULL);
        if(ptResourceManager->_tStagingBufferMemory) vkFreeMemory(tDevice, ptResourceManager->_tStagingBufferMemory, NULL);

        ptResourceManager->_tStagingBuffer       = VK_NULL_HANDLE;
        ptResourceManager->_tStagingBufferMemory = VK_NULL_HANDLE;
        ptResourceManager->_szStagingBufferSize  = 0;
    }
    else if(szNewSize != ptResourceManager->_szStagingBufferSize)
    {
        // free old buffer if needed
        if(ptResourceManager->_tStagingBuffer)       vkDestroyBuffer(tDevice, ptResourceManager->_tStagingBuffer, NULL);
        if(ptResourceManager->_tStagingBufferMemory) vkFreeMemory(tDevice, ptResourceManager->_tStagingBufferMemory, NULL);

        ptResourceManager->_tStagingBuffer       = VK_NULL_HANDLE;
        ptResourceManager->_tStagingBufferMemory = VK_NULL_HANDLE;

        // create buffer
        const VkBufferCreateInfo tBufferCreateInfo = {
            .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size        = szNewSize,
            .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        PL_VULKAN(vkCreateBuffer(tDevice, &tBufferCreateInfo, NULL, &ptResourceManager->_tStagingBuffer));

        // find memory requirements
        VkMemoryRequirements tMemoryRequirements = {0};
        vkGetBufferMemoryRequirements(tDevice, ptResourceManager->_tStagingBuffer, &tMemoryRequirements);
        ptResourceManager->_szStagingBufferSize = szNewSize;

        // allocate & bind buffer
        VkMemoryAllocateInfo tAllocInfo = {
            .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize  = tMemoryRequirements.size,
            .memoryTypeIndex = pl_find_memory_type(ptResourceManager->_ptDevice->tMemProps, tMemoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
        };
        PL_VULKAN(vkAllocateMemory(tDevice, &tAllocInfo, NULL, &ptResourceManager->_tStagingBufferMemory));
        PL_VULKAN(vkBindBufferMemory(tDevice, ptResourceManager->_tStagingBuffer, ptResourceManager->_tStagingBufferMemory, 0));   

        // map memory to host visible address
        PL_VULKAN(vkMapMemory(tDevice, ptResourceManager->_tStagingBufferMemory, 0, VK_WHOLE_SIZE, 0, (void**)&ptResourceManager->_pucMapping));

    }   
}

static uint64_t
pl__get_free_buffer_index(plVulkanResourceManager* ptResourceManager)
{
    // check if previous index is availble
    if(pl_sb_size(ptResourceManager->_sbulFreeIndices) > 0)
    {
        const uint64_t ulFreeIndex = pl_sb_pop(ptResourceManager->_sbulFreeIndices);
        return ulFreeIndex;
    }

    // no free buffer index available, create one
    const uint64_t ulFreeIndex = pl_sb_add_n(ptResourceManager->sbtBuffers, 1);
    return ulFreeIndex;
}

static size_t
pl__get_const_buffer_req_size(plVulkanDevice* ptDevice, size_t szSize)
{
    // Calculate required alignment based on minimum device offset alignment
    size_t minUboAlignment = ptDevice->tDeviceProps.limits.minUniformBufferOffsetAlignment;
    size_t alignedSize = szSize;
    if (minUboAlignment > 0) alignedSize = alignedSize + (minUboAlignment - alignedSize % minUboAlignment);
    return alignedSize; 
}