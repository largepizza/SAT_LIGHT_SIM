#include "SatMeshRenderer.h"
#include "../VulkanContext.h"
#include "../Log.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace
{
// Host-visible, coherent buffer filled from `data` (may be null for zero-filled) — the mesh data is
// small (a model is ~1e4 vertices), so staging to device-local memory isn't worth it yet.
void makeHostBuffer(VulkanContext &ctx, VkDeviceSize size, VkBufferUsageFlags usage, const void *data,
                    VkBuffer &buf, VkDeviceMemory &mem, void **keepMapped = nullptr)
{
    size = std::max<VkDeviceSize>(size, 16);
    ctx.createBuffer(size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buf,
                     mem);
    void *p = nullptr;
    vkMapMemory(ctx.device, mem, 0, size, 0, &p);
    if (data)
        memcpy(p, data, (size_t)size);
    else
        memset(p, 0, (size_t)size);
    if (keepMapped)
        *keepMapped = p;
    else
        vkUnmapMemory(ctx.device, mem);
}

void makeImage(VulkanContext &ctx, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
               VkSampleCountFlagBits samples, VkImageAspectFlags aspect, VkImage &img, VkDeviceMemory &mem,
               VkImageView &view)
{
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx.device, &ci, nullptr, &img) != VK_SUCCESS)
        throw std::runtime_error("SatMeshRenderer: vkCreateImage failed");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx.device, img, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = ctx.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(ctx.device, &ai, nullptr, &mem) != VK_SUCCESS)
        throw std::runtime_error("SatMeshRenderer: vkAllocateMemory failed");
    vkBindImageMemory(ctx.device, img, mem, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCreateImageView(ctx.device, &vci, nullptr, &view);
}

template <typename T> void destroy(VkDevice d, T &h, void (*fn)(VkDevice, T, const VkAllocationCallbacks *))
{
    if (h != VK_NULL_HANDLE)
        fn(d, h, nullptr);
    h = VK_NULL_HANDLE;
}
} // namespace

void setMeshInstanceEarth(GpuMeshInstance &inst, const SatEarthLight &L, const glm::dmat3 &rot)
{
    inst.earthshine = glm::vec4(glm::vec3(rot * L.dir), (float)L.E);
    inst.earthX = glm::vec4(glm::vec3(rot * L.ex), (float)L.sh[0]);
    inst.earthZ = glm::vec4(glm::vec3(rot * L.ez), (float)L.sh[1]);
    inst.earthShA = glm::vec4((float)L.sh[2], (float)L.sh[3], (float)L.sh[4], (float)L.sh[5]);
    inst.earthShB = glm::vec4((float)L.sh[6], (float)L.sh[7], (float)L.sh[8], (float)L.sh[9]);
    inst.earthShC = glm::vec4((float)L.sh[10], 0.0f, 0.0f, 0.0f);
}

// ─── init / cleanup ───────────────────────────────────────────────────────────────────────────────
void SatMeshRenderer::init(VulkanContext &ctx, const EarthTextures &earth, const ProbeBindings &probes)
{
    device_ = ctx.device;
    earth_ = earth;
    probes_ = probes;
    colorFormat = ctx.swapFormat; // the viewer's output goes through the UI unchanged (UIImage)

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(ctx.physicalDevice, &props);
    const VkSampleCountFlags both =
        props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    samples = (both & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;

    makeHostBuffer(ctx, sizeof(GpuMeshFrame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr, frameBuf, frameMem,
                   &frameMapped);
    makeHostBuffer(ctx, sizeof(GpuMeshFrame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr, checkFrameBuf,
                   checkFrameMem, &checkFrameMapped);
    makeHostBuffer(ctx, sizeof(GpuMeshFrame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr, sceneFrameBuf,
                   sceneFrameMem, &sceneFrameMapped);
    makeHostBuffer(ctx, sizeof(GpuMeshInstance) * kMaxInstances, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nullptr,
                   instanceBuf, instanceMem, &instanceMapped);
    // Placeholders until setTypeModels() so the descriptor set is always complete.
    makeHostBuffer(ctx, sizeof(GpuSatMeshMaterial), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nullptr, materialBuf,
                   materialMem);
    makeHostBuffer(ctx, sizeof(GpuSatMeshOccluder), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nullptr, occluderBuf,
                   occluderMem);
    makeHostBuffer(ctx, sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nullptr, componentBuf, componentMem);

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(ctx.device, &sci, nullptr, &sampler);

    createDescriptors(ctx);
    createViewerPass(ctx);
    createCheckPass(ctx);
    createScenePass(ctx);
    createPipelines(ctx);
    Log::line(std::string("mesh renderer: viewer MSAA ") + (samples == VK_SAMPLE_COUNT_4_BIT ? "4x" : "off"));
}

void SatMeshRenderer::cleanup(VkDevice d)
{
    destroyViewerTarget();
    destroyGeometry();
    destroy(d, viewerMeshPipe, vkDestroyPipeline);
    destroy(d, checkMeshPipe, vkDestroyPipeline);
    destroy(d, checkFb, vkDestroyFramebuffer);
    destroy(d, checkColorView, vkDestroyImageView);
    destroy(d, checkDepthView, vkDestroyImageView);
    destroy(d, checkColor, vkDestroyImage);
    destroy(d, checkDepth, vkDestroyImage);
    destroy(d, checkColorMem, vkFreeMemory);
    destroy(d, checkDepthMem, vkFreeMemory);
    destroy(d, checkReadBuf, vkDestroyBuffer);
    destroy(d, checkReadMem, vkFreeMemory);
    destroy(d, checkPass, vkDestroyRenderPass);
    destroy(d, checkFrameBuf, vkDestroyBuffer);
    destroy(d, checkFrameMem, vkFreeMemory);
    checkMapped = checkFrameMapped = nullptr;
    destroySceneTarget();
    destroy(d, reflPipe, vkDestroyPipeline);
    destroy(d, reflPass, vkDestroyRenderPass);
    destroy(d, reflAddPipe, vkDestroyPipeline);
    destroy(d, reflAddPipeLayout, vkDestroyPipelineLayout);
    destroy(d, reflAddPool, vkDestroyDescriptorPool);
    destroy(d, reflAddLayout, vkDestroyDescriptorSetLayout);
    destroy(d, sceneMeshPipe, vkDestroyPipeline);
    destroy(d, sceneDepthPipe, vkDestroyPipeline);
    destroy(d, scenePass, vkDestroyRenderPass);
    destroy(d, sceneFrameBuf, vkDestroyBuffer);
    destroy(d, sceneFrameMem, vkFreeMemory);
    sceneFrameMapped = nullptr;
    destroy(d, bloomPipe, vkDestroyPipeline);
    destroy(d, bloomPipeLayout, vkDestroyPipelineLayout);
    destroy(d, bloomDescPool, vkDestroyDescriptorPool);
    destroy(d, bloomDescLayout, vkDestroyDescriptorSetLayout);
    destroy(d, viewerBgPipe, vkDestroyPipeline);
    destroy(d, viewerMarkerPipe, vkDestroyPipeline);
    destroy(d, pipeLayout, vkDestroyPipelineLayout);
    destroy(d, viewerPass, vkDestroyRenderPass);
    destroy(d, descPool, vkDestroyDescriptorPool);
    destroy(d, descLayout, vkDestroyDescriptorSetLayout);
    destroy(d, sampler, vkDestroySampler);
    destroy(d, frameBuf, vkDestroyBuffer);
    destroy(d, frameMem, vkFreeMemory);
    destroy(d, instanceBuf, vkDestroyBuffer);
    destroy(d, instanceMem, vkFreeMemory);
    destroy(d, materialBuf, vkDestroyBuffer);
    destroy(d, materialMem, vkFreeMemory);
    destroy(d, occluderBuf, vkDestroyBuffer);
    destroy(d, occluderMem, vkFreeMemory);
    destroy(d, componentBuf, vkDestroyBuffer);
    destroy(d, componentMem, vkFreeMemory);
    frameMapped = instanceMapped = nullptr;
}

void SatMeshRenderer::destroyGeometry()
{
    destroy(device_, vertexBuf, vkDestroyBuffer);
    destroy(device_, vertexMem, vkFreeMemory);
    destroy(device_, indexBuf, vkDestroyBuffer);
    destroy(device_, indexMem, vkFreeMemory);
}

// ─── Descriptors ──────────────────────────────────────────────────────────────────────────────────
void SatMeshRenderer::createDescriptors(VulkanContext &ctx)
{
    const VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkShaderStageFlags f = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[10] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, vf, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, vf, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, f, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, f, nullptr},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, f, nullptr},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, f, nullptr},
        {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, f, nullptr},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, vf, nullptr}, // per-component pivots (Phase 4f)
        {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, f, nullptr},  // every env probe's SH irradiance
        {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, f, nullptr}, // viewer background (HDR)
    };
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 10;
    li.pBindings = b;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &descLayout);

    // Two sets of the same layout: the viewer's and the photometric check's (own frame UBO — both
    // can be recorded in one frame, and a shared host-written UBO would hold only the last write).
    // Three sets of the same layout — viewer, photometric check, scene — each with its own frame UBO
    // (all can be recorded in one frame, and a shared host-written UBO would hold only the last write).
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 15},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 3;
    pi.pPoolSizes = ps;
    pi.maxSets = 3;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &descPool);

    VkDescriptorSetLayout layouts[3] = {descLayout, descLayout, descLayout};
    VkDescriptorSet sets[3] = {};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descPool;
    ai.descriptorSetCount = 3;
    ai.pSetLayouts = layouts;
    vkAllocateDescriptorSets(ctx.device, &ai, sets);
    descSet = sets[0];
    descSetCheck = sets[1];
    descSetScene = sets[2];
    const VkBuffer frameBufs[3] = {frameBuf, checkFrameBuf, sceneFrameBuf};
    for (int si = 0; si < 3; ++si)
    {
    const VkDescriptorSet set = sets[si];
    VkDescriptorBufferInfo frameInfo{frameBufs[si], 0, sizeof(GpuMeshFrame)};
    VkDescriptorBufferInfo instInfo{instanceBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo img[3] = {
        {earth_.daySampler, earth_.day, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {earth_.nightSampler, earth_.night, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {earth_.cloudsSampler, earth_.clouds, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkDescriptorBufferInfo shInfo{probes_.shBuffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[6] = {};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            nullptr, &frameInfo, nullptr};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            nullptr, &instInfo, nullptr};
    for (int i = 0; i < 3; ++i)
        w[2 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, (uint32_t)(4 + i), 0, 1,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &img[i], nullptr, nullptr};
    w[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 8, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            nullptr, &shInfo, nullptr};
    vkUpdateDescriptorSets(ctx.device, 6, w, 0, nullptr);
    }
    writeGeometryDescriptors();
}

// Binding 9 of the viewer's set only: the pipelines of the other two never read it.
void SatMeshRenderer::setViewerBackground(VkImageView view, VkSampler smp)
{
    if (!descSet || !view)
        return;
    VkDescriptorImageInfo ii{smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descSet, 9, 0, 1,
                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, nullptr, nullptr};
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
}

void SatMeshRenderer::writeGeometryDescriptors()
{
    VkDescriptorBufferInfo matInfo{materialBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo occInfo{occluderBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo compInfo{componentBuf, 0, VK_WHOLE_SIZE};
    for (VkDescriptorSet set : {descSet, descSetCheck, descSetScene})
    {
        VkWriteDescriptorSet w[3] = {};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                nullptr, &matInfo, nullptr};
        w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                nullptr, &occInfo, nullptr};
        w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 7, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                nullptr, &compInfo, nullptr};
        vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
    }
}

// ─── Geometry ─────────────────────────────────────────────────────────────────────────────────────
void SatMeshRenderer::setTypeModels(VulkanContext &ctx, const std::vector<TypeModel> &types)
{
    vkDeviceWaitIdle(ctx.device);
    destroyGeometry();
    destroy(ctx.device, materialBuf, vkDestroyBuffer);
    destroy(ctx.device, materialMem, vkFreeMemory);
    destroy(ctx.device, occluderBuf, vkDestroyBuffer);
    destroy(ctx.device, occluderMem, vkFreeMemory);
    destroy(ctx.device, componentBuf, vkDestroyBuffer);
    destroy(ctx.device, componentMem, vkFreeMemory);

    std::vector<SatMeshVertex> verts;
    std::vector<glm::vec4> comps;
    std::vector<uint32_t> idx;
    std::vector<GpuSatMeshMaterial> mats;
    std::vector<GpuSatMeshOccluder> occs;
    typeMeshes.assign(types.size(), TypeMesh{});
    for (size_t ti = 0; ti < types.size(); ++ti)
    {
        const TypeModel &tm = types[ti];
        if (!tm.model)
            continue;
        SatRenderMesh mesh = buildSatRenderMesh(*tm.model);
        TypeMesh &out = typeMeshes[ti];
        out.valid = !mesh.indices.empty();
        out.firstIndex = (uint32_t)idx.size();
        out.indexCount = (uint32_t)mesh.indices.size();
        out.vertexOffset = (int32_t)verts.size();
        out.firstMaterial = (uint32_t)mats.size();
        out.boundsCenter = mesh.boundsCenter;
        out.boundsRadius = mesh.boundsRadius;
        out.triangles = (int)(mesh.indices.size() / 3);
        verts.insert(verts.end(), mesh.vertices.begin(), mesh.vertices.end());
        idx.insert(idx.end(), mesh.indices.begin(), mesh.indices.end());
        for (const SatMaterial &m : tm.model->materials)
        {
            mats.push_back(packSatMeshMaterial(m));
            out.hasSharp |= m.roughness < 0.02f; // sat_mesh.frag: sharp share = 1 - smoothstep(0.004, 0.02, α)
        }
        out.firstComponent = (uint32_t)comps.size();
        for (const SatComponent &c : tm.model->components)
            comps.push_back(glm::vec4(c.pivot, (float)tm.model->groups[c.group].parent));
        out.firstOccluder = (uint32_t)occs.size();
        if (tm.occlusion)
        {
            std::vector<GpuSatMeshOccluder> o = packSatMeshOccluders(*tm.occlusion);
            out.occluderCount = (uint32_t)o.size();
            occs.insert(occs.end(), o.begin(), o.end());
        }
        Log::line("mesh renderer: model '" + tm.model->id + "' " + std::to_string(out.triangles) +
                  " triangles, " + std::to_string(out.occluderCount) + " occluders");
    }
    makeHostBuffer(ctx, verts.size() * sizeof(SatMeshVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, verts.data(),
                   vertexBuf, vertexMem);
    makeHostBuffer(ctx, idx.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, idx.data(), indexBuf,
                   indexMem);
    makeHostBuffer(ctx, mats.size() * sizeof(GpuSatMeshMaterial), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   mats.empty() ? nullptr : mats.data(), materialBuf, materialMem);
    makeHostBuffer(ctx, occs.size() * sizeof(GpuSatMeshOccluder), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   occs.empty() ? nullptr : occs.data(), occluderBuf, occluderMem);
    makeHostBuffer(ctx, comps.size() * sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   comps.empty() ? nullptr : comps.data(), componentBuf, componentMem);
    writeGeometryDescriptors();
}

// ─── Viewer pass + pipelines ──────────────────────────────────────────────────────────────────────
void SatMeshRenderer::createViewerPass(VulkanContext &ctx)
{
    const bool ms = samples != VK_SAMPLE_COUNT_1_BIT;
    VkAttachmentDescription att[3] = {};
    // 0: colour (multisampled, resolved into 2; or the output itself without MSAA)
    att[0].format = colorFormat;
    att[0].samples = samples;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = ms ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = ms ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // 1: depth
    att[1].format = ctx.depthFormat;
    att[1].samples = samples;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    // 2: resolve target (MSAA only) — what the UI samples
    att[2].format = colorFormat;
    att[2].samples = VK_SAMPLE_COUNT_1_BIT;
    att[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolveRef{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    sub.pResolveAttachments = ms ? &resolveRef : nullptr;
    sub.pDepthStencilAttachment = &depthRef;

    // In: last frame's UI read of the output (and anything else) before we overwrite it.
    // Out: the colour writes are visible to the UI pass's fragment shader this frame.
    VkSubpassDependency dep[2] = {};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = 0;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = ms ? 3 : 2;
    rpci.pAttachments = att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies = dep;
    if (vkCreateRenderPass(ctx.device, &rpci, nullptr, &viewerPass) != VK_SUCCESS)
        throw std::runtime_error("SatMeshRenderer: viewer render pass");
}

void SatMeshRenderer::createPipelines(VulkanContext &ctx)
{
    // Set 0: the renderer's; set 1: one environment probe's cube (SatEnvProbes), bound per draw.
    const VkDescriptorSetLayout setLayouts[2] = {descLayout, probes_.setLayout};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2;
    li.pSetLayouts = setLayouts;
    vkCreatePipelineLayout(ctx.device, &li, nullptr, &pipeLayout);

    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dys{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dys.dynamicStateCount = 2;
    dys.pDynamicStates = dyn;
    VkPipelineColorBlendAttachmentState cba[3] = {};
    for (auto &a : cba)
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = cba;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    auto build = [&](const char *vs, const char *fs, bool mesh, VkRenderPass pass, VkSampleCountFlagBits ns,
                     VkPipeline &out, uint32_t colorCount = 1, VkCompareOp depthOp = VK_COMPARE_OP_LESS,
                     bool depthWrite = true, bool colorWrite = true, bool overlay = false) {
        cb.attachmentCount = colorCount;
        const VkColorComponentFlags rgba = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        for (auto &a : cba)
        {
            a.colorWriteMask = colorWrite ? rgba : 0;
            // overlay: alpha-blended over what is there (the viewer's markers)
            a.blendEnable = overlay ? VK_TRUE : VK_FALSE;
            a.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            a.colorBlendOp = VK_BLEND_OP_ADD;
            a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            a.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineMultisampleStateCreateInfo msci{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        msci.rasterizationSamples = ns;
        VkShaderModule v = ctx.loadShader(vs), f = ctx.loadShader(fs);
        VkPipelineShaderStageCreateInfo st[2] = {};
        st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, v,
                 "main", nullptr};
        st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, f,
                 "main", nullptr};

        VkVertexInputBindingDescription bind{0, sizeof(SatMeshVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attr[4] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(SatMeshVertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(SatMeshVertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(SatMeshVertex, uv)},
            {3, 0, VK_FORMAT_R32_UINT, (uint32_t)offsetof(SatMeshVertex, packed)},
        };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (mesh)
        {
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &bind;
            vi.vertexAttributeDescriptionCount = 4;
            vi.pVertexAttributeDescriptions = attr;
        }

        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        // Back faces culled: a two-sided plane is two coplanar quads (front + back material), which
        // would z-fight otherwise. Triangles wind CCW seen from outside; the projection's Y flip keeps
        // that CCW in framebuffer space (Scene3D's convention).
        rs.cullMode = mesh ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = (mesh || overlay) ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = (mesh && depthWrite) ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = overlay ? VK_COMPARE_OP_LESS_OR_EQUAL : depthOp;

        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount = 2;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vps;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &msci;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dys;
        ci.layout = pipeLayout;
        ci.renderPass = pass;
        if (vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &out) != VK_SUCCESS)
            throw std::runtime_error(std::string("SatMeshRenderer: pipeline ") + fs);
        vkDestroyShaderModule(ctx.device, v, nullptr);
        vkDestroyShaderModule(ctx.device, f, nullptr);
    };
    build("shaders/sat_mesh_bg.vert.spv", "shaders/sat_mesh_bg.frag.spv", false, viewerPass, samples, viewerBgPipe);
    build("shaders/sat_mesh.vert.spv", "shaders/sat_mesh.frag.spv", true, viewerPass, samples, viewerMeshPipe);
    // Markers over the mesh: the lines take their own depth (the model hides what is behind it), the
    // dots depth 0 (sat_mesh_marker.frag).
    build("shaders/sat_mesh_bg.vert.spv", "shaders/sat_mesh_marker.frag.spv", false, viewerPass, samples,
          viewerMarkerPipe, 1, VK_COMPARE_OP_LESS_OR_EQUAL, false, true, true);
    build("shaders/sat_mesh.vert.spv", "shaders/sat_mesh.frag.spv", true, checkPass, VK_SAMPLE_COUNT_1_BIT,
          checkMeshPipe);
    // Scene: infinite reverse-Z (SatelliteSim::recordMeshScene builds depth = near / distance), so
    // GREATER and a clear to 0 — meshes from centimetres to a thousand km apart all keep precision.
    // Two passes (recordScene): depth first — only the open-lattice cut-outs run — then shading with
    // depth EQUAL and forced early fragment tests (sat_mesh.frag's MESH_*_PASS variants), so each
    // covered pixel is shaded once however many layers of a station overlap there.
    build("shaders/sat_mesh.vert.spv", "shaders/sat_mesh_depth.frag.spv", true, scenePass, VK_SAMPLE_COUNT_1_BIT,
          sceneDepthPipe, 3, VK_COMPARE_OP_GREATER, true, false);
    build("shaders/sat_mesh.vert.spv", "shaders/sat_mesh_scene.frag.spv", true, scenePass, VK_SAMPLE_COUNT_1_BIT,
          sceneMeshPipe, 3, VK_COMPARE_OP_EQUAL, false, true);
}

// ─── Viewer target ────────────────────────────────────────────────────────────────────────────────
void SatMeshRenderer::destroyViewerTarget()
{
    destroy(device_, viewerFb, vkDestroyFramebuffer);
    destroy(device_, viewerColorMsView, vkDestroyImageView);
    destroy(device_, viewerDepthMsView, vkDestroyImageView);
    destroy(device_, viewerResolveView, vkDestroyImageView);
    destroy(device_, viewerColorMs, vkDestroyImage);
    destroy(device_, viewerDepthMs, vkDestroyImage);
    destroy(device_, viewerResolve, vkDestroyImage);
    destroy(device_, viewerColorMsMem, vkFreeMemory);
    destroy(device_, viewerDepthMsMem, vkFreeMemory);
    destroy(device_, viewerResolveMem, vkFreeMemory);
    viewerW = viewerH = 0;
    viewerHasContent = false;
}

bool SatMeshRenderer::ensureViewerTarget(VulkanContext &ctx, uint32_t w, uint32_t h)
{
    w = std::max(w, 16u);
    h = std::max(h, 16u);
    if (w == viewerW && h == viewerH)
        return false;
    vkDeviceWaitIdle(ctx.device); // the UI may still reference the old image in a pending frame
    destroyViewerTarget();
    const bool ms = samples != VK_SAMPLE_COUNT_1_BIT;
    makeImage(ctx, w, h, colorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, viewerResolve, viewerResolveMem, viewerResolveView);
    if (ms)
        makeImage(ctx, w, h, colorFormat,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, samples,
                  VK_IMAGE_ASPECT_COLOR_BIT, viewerColorMs, viewerColorMsMem, viewerColorMsView);
    makeImage(ctx, w, h, ctx.depthFormat,
              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, samples,
              VK_IMAGE_ASPECT_DEPTH_BIT, viewerDepthMs, viewerDepthMsMem, viewerDepthMsView);

    VkImageView views[3] = {ms ? viewerColorMsView : viewerResolveView, viewerDepthMsView, viewerResolveView};
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = viewerPass;
    fci.attachmentCount = ms ? 3 : 2;
    fci.pAttachments = views;
    fci.width = w;
    fci.height = h;
    fci.layers = 1;
    if (vkCreateFramebuffer(ctx.device, &fci, nullptr, &viewerFb) != VK_SUCCESS)
        throw std::runtime_error("SatMeshRenderer: viewer framebuffer");
    viewerW = w;
    viewerH = h;

    // The UI may sample it before the first recordViewer(): give it a defined, readable layout.
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    ctx.imageBarrier(cmd, viewerResolve, 0, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ctx.endOneTimeCommands(cmd);
    return true;
}

void SatMeshRenderer::recordViewer(VkCommandBuffer cmd, const GpuMeshFrame &frame, const GpuMeshInstance &inst,
                                   int typeIdx, VkDescriptorSet probeSet)
{
    if (!viewerFb)
        return;
    memcpy(frameMapped, &frame, sizeof(frame));
    memcpy(instanceMapped, &inst, sizeof(inst)); // viewer = instance slot 0

    VkClearValue clears[3] = {};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = viewerPass;
    rbi.framebuffer = viewerFb;
    rbi.renderArea = {{0, 0}, {viewerW, viewerH}};
    rbi.clearValueCount = 3;
    rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0.0f, 0.0f, (float)viewerW, (float)viewerH, 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {viewerW, viewerH}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    const VkDescriptorSet sets[2] = {descSet, probeSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 2, sets, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, viewerBgPipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    const TypeMesh *tm = typeMesh(typeIdx);
    if (tm && vertexBuf)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, viewerMeshPipe);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuf, &off);
        vkCmdBindIndexBuffer(cmd, indexBuf, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, tm->indexCount, 1, tm->firstIndex, tm->vertexOffset, 0);
    }
    if (viewerMarkerPipe && (frame.marker0.w > 0.5f || frame.marker1.w > 0.5f))
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, viewerMarkerPipe);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
    viewerHasContent = true;
}

// ─── Photometric check ────────────────────────────────────────────────────────────────────────────
// A sun-only, single-sampled R32F render (sat_mesh.frag's check mode writes L·d² per pixel) copied to
// host memory, so the CPU can integrate the rendered radiant intensity and compare it with the lobe
// model (SatelliteSim::recordModelViewer / buildInfoWindow / buildViewPopoutWindow).
void SatMeshRenderer::createCheckPass(VulkanContext &ctx)
{
    VkAttachmentDescription att[2] = {};
    att[0].format = VK_FORMAT_R32_SFLOAT;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    att[1].format = ctx.depthFormat;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;
    VkSubpassDependency dep[2] = {};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 2;
    rpci.pAttachments = att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies = dep;
    if (vkCreateRenderPass(ctx.device, &rpci, nullptr, &checkPass) != VK_SUCCESS)
        throw std::runtime_error("SatMeshRenderer: check render pass");

    makeImage(ctx, kCheckSize, kCheckSize, VK_FORMAT_R32_SFLOAT,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT,
              VK_IMAGE_ASPECT_COLOR_BIT, checkColor, checkColorMem, checkColorView);
    makeImage(ctx, kCheckSize, kCheckSize, ctx.depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, checkDepth, checkDepthMem, checkDepthView);
    VkImageView views[2] = {checkColorView, checkDepthView};
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = checkPass;
    fci.attachmentCount = 2;
    fci.pAttachments = views;
    fci.width = kCheckSize;
    fci.height = kCheckSize;
    fci.layers = 1;
    if (vkCreateFramebuffer(ctx.device, &fci, nullptr, &checkFb) != VK_SUCCESS)
        throw std::runtime_error("SatMeshRenderer: check framebuffer");
    makeHostBuffer(ctx, (VkDeviceSize)kCheckSize * kCheckSize * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   nullptr, checkReadBuf, checkReadMem, &checkMapped);
}

void SatMeshRenderer::recordCheck(VkCommandBuffer cmd, const GpuMeshFrame &frame, const GpuMeshInstance &inst,
                                  int typeIdx, VkDescriptorSet probeSet)
{
    const TypeMesh *tm = typeMesh(typeIdx);
    if (!checkFb || !tm || !vertexBuf)
        return;
    memcpy(checkFrameMapped, &frame, sizeof(frame));
    memcpy(static_cast<char *>(instanceMapped) + sizeof(GpuMeshInstance), &inst, sizeof(inst)); // slot 1

    VkClearValue clears[2] = {};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = checkPass;
    rbi.framebuffer = checkFb;
    rbi.renderArea = {{0, 0}, {kCheckSize, kCheckSize}};
    rbi.clearValueCount = 2;
    rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0.0f, 0.0f, (float)kCheckSize, (float)kCheckSize, 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {kCheckSize, kCheckSize}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    const VkDescriptorSet sets[2] = {descSetCheck, probeSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 2, sets, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, checkMeshPipe);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuf, &off);
    vkCmdBindIndexBuffer(cmd, indexBuf, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, tm->indexCount, 1, tm->firstIndex, tm->vertexOffset, 1); // instance slot 1
    vkCmdEndRenderPass(cmd);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kCheckSize, kCheckSize, 1};
    vkCmdCopyImageToBuffer(cmd, checkColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, checkReadBuf, 1, &region);
    VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = checkReadBuf;
    bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0,
                         nullptr);
}

// ─── Scene pass (4c) ──────────────────────────────────────────────────────────────────────────────
void SatMeshRenderer::createScenePass(VulkanContext &ctx)
{
    // 0 radiance, 1 distance, 2 the sharp-reflection G-buffer, 3 depth.
    VkAttachmentDescription att[4] = {};
    const VkFormat fmts[3] = {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT};
    for (int i = 0; i < 3; ++i)
    {
        att[i].format = fmts[i];
        att[i].samples = VK_SAMPLE_COUNT_1_BIT;
        att[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL; // read with imageLoad by compute and fragment passes
    }
    att[3].format = ctx.depthFormat;
    att[3].samples = VK_SAMPLE_COUNT_1_BIT;
    att[3].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[3].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[3].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[3].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorRefs[3] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                          {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                          {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference depthRef{3, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 3;
    sub.pColorAttachments = colorRefs;
    sub.pDepthStencilAttachment = &depthRef;
    // In: last frame's compute/fragment reads before we overwrite. Out: this frame's scene_depth.comp,
    // sat_sky.frag and the bloom draw read the results.
    VkSubpassDependency dep[2] = {};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = 0;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT; // + mesh_refl_add.comp
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 4;
    rpci.pAttachments = att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies = dep;
    if (vkCreateRenderPass(ctx.device, &rpci, nullptr, &scenePass) != VK_SUCCESS)
        throw std::runtime_error("SatMeshRenderer: scene render pass");

    // The sharp-reflection pass: SKY_REFL's radiance, cleared, then read by mesh_refl_add.comp.
    VkAttachmentDescription ra{};
    ra.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ra.samples = VK_SAMPLE_COUNT_1_BIT;
    ra.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ra.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ra.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ra.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ra.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ra.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference rref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription rsub{};
    rsub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    rsub.colorAttachmentCount = 1;
    rsub.pColorAttachments = &rref;
    VkSubpassDependency rdep[2] = {};
    rdep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    rdep[0].dstSubpass = 0;
    rdep[0].srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    rdep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    rdep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; // the G-buffer the fragment reads
    rdep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    rdep[1].srcSubpass = 0;
    rdep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    rdep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    rdep[1].dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    rdep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    rdep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rci.attachmentCount = 1;
    rci.pAttachments = &ra;
    rci.subpassCount = 1;
    rci.pSubpasses = &rsub;
    rci.dependencyCount = 2;
    rci.pDependencies = rdep;
    if (vkCreateRenderPass(ctx.device, &rci, nullptr, &reflPass) != VK_SUCCESS)
        throw std::runtime_error("SatMeshRenderer: reflection render pass");
}

void SatMeshRenderer::destroySceneTarget()
{
    destroy(device_, sceneFb, vkDestroyFramebuffer);
    destroy(device_, reflFb, vkDestroyFramebuffer);
    destroy(device_, sceneReflGViewH, vkDestroyImageView);
    destroy(device_, sceneReflG, vkDestroyImage);
    destroy(device_, sceneReflGMem, vkFreeMemory);
    destroy(device_, reflRadView, vkDestroyImageView);
    destroy(device_, reflRad, vkDestroyImage);
    destroy(device_, reflRadMem, vkFreeMemory);
    destroy(device_, sceneColorViewH, vkDestroyImageView);
    destroy(device_, sceneDistViewH, vkDestroyImageView);
    destroy(device_, sceneDepthView, vkDestroyImageView);
    destroy(device_, sceneColor, vkDestroyImage);
    destroy(device_, sceneDist, vkDestroyImage);
    destroy(device_, sceneDepth, vkDestroyImage);
    destroy(device_, sceneColorMem, vkFreeMemory);
    destroy(device_, sceneDistMem, vkFreeMemory);
    destroy(device_, sceneDepthMem, vkFreeMemory);
    sceneW = sceneH = 0;
}

bool SatMeshRenderer::ensureSceneTarget(VulkanContext &ctx, uint32_t w, uint32_t h)
{
    w = std::max(w, 1u);
    h = std::max(h, 1u);
    if (w == sceneW && h == sceneH)
        return false;
    vkDeviceWaitIdle(ctx.device);
    destroySceneTarget();
    makeImage(ctx, w, h, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, sceneColor, sceneColorMem, sceneColorViewH);
    makeImage(ctx, w, h, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, sceneDist, sceneDistMem, sceneDistViewH);
    makeImage(ctx, w, h, VK_FORMAT_R32G32B32A32_UINT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, sceneReflG, sceneReflGMem, sceneReflGViewH);
    makeImage(ctx, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, reflRad, reflRadMem, reflRadView);
    makeImage(ctx, w, h, ctx.depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_SAMPLE_COUNT_1_BIT,
              VK_IMAGE_ASPECT_DEPTH_BIT, sceneDepth, sceneDepthMem, sceneDepthView);
    VkImageView views[4] = {sceneColorViewH, sceneDistViewH, sceneReflGViewH, sceneDepthView};
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = scenePass;
    fci.attachmentCount = 4;
    fci.pAttachments = views;
    fci.width = w;
    fci.height = h;
    fci.layers = 1;
    if (vkCreateFramebuffer(ctx.device, &fci, nullptr, &sceneFb) != VK_SUCCESS)
        throw std::runtime_error("SatMeshRenderer: scene framebuffer");
    VkFramebufferCreateInfo rfci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    rfci.renderPass = reflPass;
    rfci.attachmentCount = 1;
    rfci.pAttachments = &reflRadView;
    rfci.width = w;
    rfci.height = h;
    rfci.layers = 1;
    if (vkCreateFramebuffer(ctx.device, &rfci, nullptr, &reflFb) != VK_SUCCESS)
        throw std::runtime_error("SatMeshRenderer: reflection framebuffer");
    sceneW = w;
    sceneH = h;
    // Defined contents + GENERAL layout before the first frame's readers (the pass also clears).
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    for (VkImage img : {sceneColor, sceneDist, sceneReflG, reflRad})
    {
        ctx.imageBarrier(cmd, img, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue zero{};
        VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rng);
    }
    ctx.endOneTimeCommands(cmd);
    if (bloomDescSet)
    {
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, sceneColorViewH, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w0{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bloomDescSet, 0, 0, 1,
                                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii, nullptr, nullptr};
        vkUpdateDescriptorSets(device_, 1, &w0, 0, nullptr);
    }
    writeReflAddDescriptors();
    return true;
}

void SatMeshRenderer::recordScene(VkCommandBuffer cmd, const GpuMeshFrame &frame,
                                  const std::vector<GpuMeshInstance> &insts, const std::vector<int> &types,
                                  const std::vector<VkDescriptorSet> &probeSets)
{
    if (!sceneFb)
        return;
    memcpy(sceneFrameMapped, &frame, sizeof(frame));
    const size_t n = std::min(insts.size(), (size_t)kMaxInstances - 2);
    if (n)
        memcpy(static_cast<char *>(instanceMapped) + 2 * sizeof(GpuMeshInstance), insts.data(),
               n * sizeof(GpuMeshInstance)); // slots 2.. (0 = viewer, 1 = check)

    VkClearValue clears[4] = {};
    clears[3].depthStencil = {0.0f, 0}; // reverse-Z
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = scenePass;
    rbi.framebuffer = sceneFb;
    rbi.renderArea = {{0, 0}, {sceneW, sceneH}};
    rbi.clearValueCount = 4;
    rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (n && vertexBuf)
    {
        VkViewport vp{0.0f, 0.0f, (float)sceneW, (float)sceneH, 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {sceneW, sceneH}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 1, &descSetScene, 0, nullptr);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuf, &off);
        vkCmdBindIndexBuffer(cmd, indexBuf, 0, VK_INDEX_TYPE_UINT32);
        // Depth pre-pass, then shading with depth EQUAL (see createPipelines).
        for (VkPipeline pipe : {sceneDepthPipe, sceneMeshPipe})
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            VkDescriptorSet bound = VK_NULL_HANDLE;
            for (size_t i = 0; i < n; ++i)
                if (const TypeMesh *tm = typeMesh(types[i]))
                {
                    if (i < probeSets.size() && probeSets[i] != bound) // each instance's own probe (set 1)
                    {
                        bound = probeSets[i];
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 1, 1, &bound, 0,
                                                nullptr);
                    }
                    vkCmdDrawIndexed(cmd, tm->indexCount, 1, tm->firstIndex, tm->vertexOffset, (uint32_t)(2 + i));
                }
        }
    }
    vkCmdEndRenderPass(cmd);
}

// ─── Sharp reflections (2026-09-25) ───────────────────────────────────────────────────────────────
struct ReflAddPC
{
    int32_t x, y, w, h;
};

void SatMeshRenderer::createReflPipeline(VulkanContext &ctx, VkPipelineLayout skyLayout)
{
    skyLayout_ = skyLayout;
    // SKY_REFL: the sky's own vertex shader (its ray is unused) and layout, one attachment, no depth.
    {
        VkShaderModule vs = ctx.loadShader("shaders/sat_sky.vert.spv");
        VkShaderModule fs = ctx.loadShader("shaders/sat_sky_refl.frag.spv");
        VkPipelineShaderStageCreateInfo st[2] = {};
        st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs,
                 "main", nullptr};
        st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs,
                 "main", nullptr};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dys{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dys.dynamicStateCount = 2;
        dys.pDynamicStates = dyn;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                             VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount = 2;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vps;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dys;
        ci.layout = skyLayout;
        ci.renderPass = reflPass;
        if (vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &reflPipe) != VK_SUCCESS)
            throw std::runtime_error("SatMeshRenderer: sharp reflection pipeline");
        vkDestroyShaderModule(ctx.device, vs, nullptr);
        vkDestroyShaderModule(ctx.device, fs, nullptr);
    }
    // mesh_refl_add.comp: the mesh radiance (read-write) and the SKY_REFL output.
    {
        VkDescriptorSetLayoutBinding b[2] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 2;
        li.pBindings = b;
        vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &reflAddLayout);
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        pi.maxSets = 1;
        vkCreateDescriptorPool(ctx.device, &pi, nullptr, &reflAddPool);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = reflAddPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &reflAddLayout;
        vkAllocateDescriptorSets(ctx.device, &ai, &reflAddSet);
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ReflAddPC)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &reflAddLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(ctx.device, &pli, nullptr, &reflAddPipeLayout);
        VkShaderModule cs = ctx.loadShader("shaders/mesh_refl_add.comp.spv");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs,
                    "main", nullptr};
        ci.layout = reflAddPipeLayout;
        if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &reflAddPipe) != VK_SUCCESS)
            throw std::runtime_error("SatMeshRenderer: reflection add pipeline");
        vkDestroyShaderModule(ctx.device, cs, nullptr);
    }
    writeReflAddDescriptors();
}

void SatMeshRenderer::writeReflAddDescriptors()
{
    if (!reflAddSet || !sceneColorViewH || !reflRadView)
        return;
    VkDescriptorImageInfo ii[2] = {{VK_NULL_HANDLE, sceneColorViewH, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, reflRadView, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[2] = {};
    for (int i = 0; i < 2; ++i)
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, reflAddSet, (uint32_t)i, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[i], nullptr, nullptr};
    vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
}

void SatMeshRenderer::recordReflections(VkCommandBuffer cmd, VkDescriptorSet skyDescSet,
                                        const std::vector<ReflDraw> &draws, uint32_t pcSize)
{
    if (draws.empty() || !reflPipe || !reflFb || !reflAddPipe)
        return;
    VkClearValue clear{};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = reflPass;
    rbi.framebuffer = reflFb;
    rbi.renderArea = {{0, 0}, {sceneW, sceneH}};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{0.0f, 0.0f, (float)sceneW, (float)sceneH, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, reflPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyLayout_, 0, 1, &skyDescSet, 0, nullptr);
    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = 0, y1 = 0;
    for (const ReflDraw &d : draws)
    {
        vkCmdSetScissor(cmd, 0, 1, &d.rect);
        vkCmdPushConstants(cmd, skyLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcSize, d.pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        x0 = std::min(x0, d.rect.offset.x);
        y0 = std::min(y0, d.rect.offset.y);
        x1 = std::max(x1, d.rect.offset.x + (int32_t)d.rect.extent.width);
        y1 = std::max(y1, d.rect.offset.y + (int32_t)d.rect.extent.height);
    }
    vkCmdEndRenderPass(cmd);

    // Into the mesh radiance, over the union of the rectangles.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, reflAddPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, reflAddPipeLayout, 0, 1, &reflAddSet, 0, nullptr);
    const ReflAddPC pc{x0, y0, x1 - x0, y1 - y0};
    vkCmdPushConstants(cmd, reflAddPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (uint32_t)(pc.w + 15) / 16, (uint32_t)(pc.h + 15) / 16, 1);
    // Visible to everything that reads the mesh radiance after this (scene_depth / sat_sky / bloom).
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}

// ─── Bloom source (4c) ────────────────────────────────────────────────────────────────────────────
struct MeshBloomPC
{
    float exposure, gain, scale, pad;
};

void SatMeshRenderer::createBloomPipeline(VulkanContext &ctx, VkRenderPass flareSourcePass)
{
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = b;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &bloomDescLayout);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &bloomDescPool);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = bloomDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &bloomDescLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &bloomDescSet);
    {
        VkDescriptorBufferInfo bi{instanceBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w1{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bloomDescSet, 1, 0, 1,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi, nullptr};
        vkUpdateDescriptorSets(ctx.device, 1, &w1, 0, nullptr);
    }
    if (sceneColorViewH)
    {
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, sceneColorViewH, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w0{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bloomDescSet, 0, 0, 1,
                                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii, nullptr, nullptr};
        vkUpdateDescriptorSets(ctx.device, 1, &w0, 0, nullptr);
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshBloomPC)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &bloomDescLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(ctx.device, &pli, nullptr, &bloomPipeLayout);

    VkShaderModule v = ctx.loadShader("shaders/sat_mesh_bg.vert.spv"), f = ctx.loadShader("shaders/mesh_bloom.frag.spv");
    VkPipelineShaderStageCreateInfo st[2] = {};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, v, "main",
             nullptr};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, f, "main",
             nullptr};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dys{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dys.dynamicStateCount = 2;
    dys.pDynamicStates = dyn;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    // Additive, like flareSourcePipeline (the target is summed, then blurred).
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vps;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dys;
    ci.layout = bloomPipeLayout;
    ci.renderPass = flareSourcePass; // later recreations of that pass stay compatible (same format)
    if (vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &bloomPipe) != VK_SUCCESS)
        throw std::runtime_error("SatMeshRenderer: bloom pipeline");
    vkDestroyShaderModule(ctx.device, v, nullptr);
    vkDestroyShaderModule(ctx.device, f, nullptr);
}

void SatMeshRenderer::recordBloom(VkCommandBuffer cmd, uint32_t targetW, uint32_t targetH, float exposure, float gain)
{
    if (!bloomPipe || !sceneColorViewH || targetW == 0 || targetH == 0)
        return;
    VkViewport vp{0.0f, 0.0f, (float)targetW, (float)targetH, 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {targetW, targetH}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomPipeLayout, 0, 1, &bloomDescSet, 0, nullptr);
    MeshBloomPC pc{exposure, gain, (float)sceneW / (float)targetW, 0.0f};
    vkCmdPushConstants(cmd, bloomPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}
