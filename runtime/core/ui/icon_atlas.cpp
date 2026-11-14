// sr-recomp: the menu's icon textures (icon_atlas.h).
//
// Two RGBA textures of square cells, each with its mip chain: the small one (128-pixel cells) for the icons next to the
// row values, the large one (512 pixels, the game's own size) for the described row's icon in the description pane. A
// worker thread decodes the queued pictures (game_art_icon: the RomFS file, ASTC, scaled to the cell) into a copy of the
// texture in memory; icons_frame copies the cells that changed into the texture through a staging buffer and makes
// their smaller mips from them (vkCmdBlitImage, linear), in order on the queue with the frames that draw it (pipeline
// barriers, no wait for the GPU). When a texture is full the icon asked for longest ago gives up its cell. Each texture
// is an ImGui texture of its own (ImGui_ImplVulkan_AddTexture), with a trilinear sampler: drawn smaller than its cell,
// an icon is filtered instead of skipping pixels.
#include <stdlib.h>
#include <string.h>

#include "backends/imgui_impl_vulkan.h"
#include "ui/icon_atlas.h"

extern "C" {
#include "game_art.h"
#include "platform/platform.h"
#include "vk.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

enum { QUEUED, READY, SHOWN, FAILED };
enum { MAX_SLOTS = 256, MIPS = 4 };   // the smallest mip is an eighth of the cell

struct Icon { char path[128]; int state; unsigned gen; uint64_t used; };

struct Atlas {
    const char *name;
    int cell, cells, side, slots;
    Icon icons[MAX_SLOTS];
    int count;
    bool dirty[MAX_SLOTS];   // decoded, not copied into the texture yet
    uint8_t *pixels;         // the texture's level 0 in memory, side x side RGBA
    struct {
        VkImage image;
        VkDeviceMemory image_mem, staging_mem;
        VkBuffer staging;
        uint8_t *staging_map;
        VkImageView view;
        VkSampler sampler;
        VkCommandPool pool;
        VkCommandBuffer cb;
        VkFence fence;
        VkDescriptorSet set;   // the ImTextureID
        bool ok, failed, fresh;   // made; making it failed for good; still to be cleared and laid out
    } vk;
};

static Atlas g_small = {"small", 128, 16, 128 * 16, 16 * 16};
static Atlas g_large = {"large", 512, 2, 512 * 2, 2 * 2};
static PlatMutex g_lock;   // the atlases' icons, count, dirty and pixels
static VkDevice g_device;
static uint64_t g_tick;    // frames laid out; an icon's last use
static int g_worker;

// The next queued icon, the large texture's first (the described row's icon is the one looked at).
static bool next_job(Atlas **a, int *at, char *path, unsigned *gen) {
    Atlas *order[2] = {&g_large, &g_small};
    for (Atlas *t : order)
        for (int i = 0; i < t->count; i++)
            if (t->icons[i].state == QUEUED && t->pixels) {
                *a = t, *at = i, *gen = t->icons[i].gen;
                memcpy(path, t->icons[i].path, sizeof t->icons[i].path);
                return true;
            }
    return false;
}

static void worker(void *arg) {
    (void)arg;
    for (;;) {
        char path[128];
        Atlas *a;
        int at;
        unsigned gen;
        plat_mutex_lock(&g_lock);
        bool job = next_job(&a, &at, path, &gen);
        plat_mutex_unlock(&g_lock);
        if (!job) {
            plat_sleep_ms(30);
            continue;
        }
        uint8_t *px = game_art_icon(path, a->cell);
        plat_mutex_lock(&g_lock);
        if (a->icons[at].gen == gen) {   // the cell was not given to another icon meanwhile
            if (px) {
                int cx = at % a->cells, cy = at / a->cells, c = a->cell;
                for (int y = 0; y < c; y++)
                    memcpy(a->pixels + ((size_t)(cy * c + y) * a->side + cx * c) * 4, px + (size_t)y * c * 4, (size_t)c * 4);
                a->dirty[at] = true;
            }
            a->icons[at].state = px ? READY : FAILED;
        }
        plat_mutex_unlock(&g_lock);
        free(px);
    }
}

static bool get(Atlas *a, const char *path, ImTextureID *tex, ImVec2 *uv0, ImVec2 *uv1) {
    if (!path || !*path || !a->vk.ok || strlen(path) >= sizeof a->icons[0].path) return false;
    plat_mutex_lock(&g_lock);
    int at = -1;
    for (int i = 0; i < a->count && at < 0; i++)
        if (!strcmp(a->icons[i].path, path)) at = i;
    if (at < 0) {
        if (a->count < a->slots) at = a->count++;
        else   // full: the cell of the icon asked for longest ago, not one asked for this frame
            for (int i = 0; i < a->count; i++)
                if (a->icons[i].used < g_tick && (at < 0 || a->icons[i].used < a->icons[at].used)) at = i;
        if (at >= 0) {
            Icon &e = a->icons[at];
            strcpy(e.path, path);
            e.state = QUEUED;
            e.gen++;
            a->dirty[at] = false;
        }
    }
    bool shown = false;
    if (at >= 0) a->icons[at].used = g_tick, shown = a->icons[at].state == SHOWN;
    plat_mutex_unlock(&g_lock);
    if (!shown) return false;
    int cx = at % a->cells, cy = at / a->cells;
    float s = (float)a->side;
    *tex = (ImTextureID)a->vk.set;
    *uv0 = ImVec2((float)(cx * a->cell) / s, (float)(cy * a->cell) / s);
    *uv1 = ImVec2((float)((cx + 1) * a->cell) / s, (float)((cy + 1) * a->cell) / s);
    return true;
}

bool icon_get(const char *path, ImTextureID *tex, ImVec2 *uv0, ImVec2 *uv1) { return get(&g_small, path, tex, uv0, uv1); }
bool icon_get_large(const char *path, ImTextureID *tex, ImVec2 *uv0, ImVec2 *uv1) {
    return get(&g_large, path, tex, uv0, uv1);
}

static int memory_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

static bool make(Atlas *a, const MenuTarget *t) {
    VkDevice d = t->device;
    auto &v = a->vk;
    VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = VK_FORMAT_R8G8B8A8_UNORM;
    ic.extent = {(uint32_t)a->side, (uint32_t)a->side, 1};
    ic.mipLevels = MIPS;
    ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d, &ic, nullptr, &v.image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(d, v.image, &mr);
    int type = memory_type(t->phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (type < 0 || vkAllocateMemory(d, &ai, nullptr, &v.image_mem) != VK_SUCCESS) return false;
    vkBindImageMemory(d, v.image, v.image_mem, 0);
    VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = (VkDeviceSize)a->side * a->side * 4;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(d, &bc, nullptr, &v.staging) != VK_SUCCESS) return false;
    vkGetBufferMemoryRequirements(d, v.staging, &mr);
    type = memory_type(t->phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (type < 0 || vkAllocateMemory(d, &ai, nullptr, &v.staging_mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(d, v.staging, v.staging_mem, 0);
    if (vkMapMemory(d, v.staging_mem, 0, VK_WHOLE_SIZE, 0, (void **)&v.staging_map) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = v.image;
    vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vc.format = ic.format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, MIPS, 0, 1};
    if (vkCreateImageView(d, &vc, nullptr, &v.view) != VK_SUCCESS) return false;
    VkSamplerCreateInfo sc = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.maxLod = (float)(MIPS - 1);
    if (vkCreateSampler(d, &sc, nullptr, &v.sampler) != VK_SUCCESS) return false;
    VkCommandPoolCreateInfo pc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pc.queueFamilyIndex = t->queue_family;
    if (vkCreateCommandPool(d, &pc, nullptr, &v.pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cba = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cba.commandPool = v.pool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(d, &cba, &v.cb) != VK_SUCCESS) return false;
    VkFenceCreateInfo fc = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(d, &fc, nullptr, &v.fence) != VK_SUCCESS) return false;
    v.set = ImGui_ImplVulkan_AddTexture(v.sampler, v.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return v.set != VK_NULL_HANDLE;
}

static void barrier(VkCommandBuffer cb, VkImage image, uint32_t level, uint32_t levels, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags src, VkAccessFlags src_access, VkPipelineStageFlags dst, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from, b.newLayout = to;
    b.srcAccessMask = src_access, b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, levels, 0, 1};
    vkCmdPipelineBarrier(cb, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static void release(Atlas *a) {
    auto &v = a->vk;
    VkDevice d = g_device;
    if (!d) return;
    if (v.fence) vkWaitForFences(d, 1, &v.fence, VK_TRUE, UINT64_MAX);
    if (v.set) ImGui_ImplVulkan_RemoveTexture(v.set);
    if (v.fence) vkDestroyFence(d, v.fence, nullptr);
    if (v.pool) vkDestroyCommandPool(d, v.pool, nullptr);
    if (v.sampler) vkDestroySampler(d, v.sampler, nullptr);
    if (v.view) vkDestroyImageView(d, v.view, nullptr);
    if (v.image) vkDestroyImage(d, v.image, nullptr);
    if (v.image_mem) vkFreeMemory(d, v.image_mem, nullptr);
    if (v.staging) vkDestroyBuffer(d, v.staging, nullptr);
    if (v.staging_mem) vkFreeMemory(d, v.staging_mem, nullptr);
    bool failed = v.failed;
    memset(&v, 0, sizeof v);
    v.failed = failed;
    plat_mutex_lock(&g_lock);   // what the texture held is still in memory: it goes into the next one
    for (int i = 0; i < a->count; i++)
        if (a->icons[i].state == SHOWN) a->icons[i].state = READY, a->dirty[i] = true;
    plat_mutex_unlock(&g_lock);
}

static int frame(Atlas *a, const MenuTarget *t) {
    auto &v = a->vk;
    if (v.failed) return 0;
    if (!v.ok) {
        plat_mutex_lock(&g_lock);
        if (!a->pixels) a->pixels = (uint8_t *)calloc((size_t)a->side * a->side, 4);
        plat_mutex_unlock(&g_lock);
        if (!a->pixels) return 0;
        g_device = t->device;
        if (!make(a, t)) {
            rt_log("menu: icon texture (%s): Vulkan refused it; the pages show no icons there", a->name);
            release(a);
            v.failed = true;
            return 0;
        }
        v.ok = v.fresh = true;
        if (!g_worker) {
            plat_thread_spawn(worker, nullptr, "menu icons", 0);
            g_worker = 1;
        }
    }
    if (vkGetFenceStatus(g_device, v.fence) != VK_SUCCESS) return 1;   // the last copy is still in flight
    VkBufferImageCopy regions[MAX_SLOTS];
    int cell_of[MAX_SLOTS], nr = 0;
    unsigned gen_of[MAX_SLOTS];
    const int c = a->cell;
    plat_mutex_lock(&g_lock);
    for (int i = 0; i < a->count; i++) {
        if (!a->dirty[i]) continue;
        int cx = i % a->cells, cy = i / a->cells;
        for (int y = 0; y < c; y++) {   // the cell into the staging buffer, at its place in the texture
            size_t o = ((size_t)(cy * c + y) * a->side + cx * c) * 4;
            memcpy(v.staging_map + o, a->pixels + o, (size_t)c * 4);
        }
        VkBufferImageCopy &r = regions[nr];
        r = {};
        r.bufferOffset = ((VkDeviceSize)cy * c * a->side + (VkDeviceSize)cx * c) * 4;
        r.bufferRowLength = (uint32_t)a->side;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageOffset = {cx * c, cy * c, 0};
        r.imageExtent = {(uint32_t)c, (uint32_t)c, 1};
        a->dirty[i] = false;
        cell_of[nr] = i, gen_of[nr] = a->icons[i].gen;
        nr++;
    }
    plat_mutex_unlock(&g_lock);
    if (!nr && !v.fresh) return 1;
    vkResetFences(g_device, 1, &v.fence);
    vkResetCommandBuffer(v.cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(v.cb, &bi);
    const VkImageLayout DST = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, SRC = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        READ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkPipelineStageFlags XFER = VK_PIPELINE_STAGE_TRANSFER_BIT, FRAG = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (v.fresh) {   // new: cleared to nothing, so the cells not filled yet draw as nothing
        barrier(v.cb, v.image, 0, MIPS, VK_IMAGE_LAYOUT_UNDEFINED, DST, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, XFER,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue clear = {};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, MIPS, 0, 1};
        vkCmdClearColorImage(v.cb, v.image, DST, &clear, 1, &all);
        barrier(v.cb, v.image, 0, MIPS, DST, DST, XFER, VK_ACCESS_TRANSFER_WRITE_BIT, XFER, VK_ACCESS_TRANSFER_WRITE_BIT);
    } else {
        barrier(v.cb, v.image, 0, MIPS, READ, DST, FRAG, VK_ACCESS_SHADER_READ_BIT, XFER, VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    if (nr) {
        vkCmdCopyBufferToImage(v.cb, v.staging, v.image, DST, (uint32_t)nr, regions);
        for (uint32_t l = 1; l < MIPS; l++) {   // each mip of the new cells from the one above it
            barrier(v.cb, v.image, l - 1, 1, DST, SRC, XFER, VK_ACCESS_TRANSFER_WRITE_BIT, XFER, VK_ACCESS_TRANSFER_READ_BIT);
            int big = c >> (l - 1), small = c >> l;
            for (int k = 0; k < nr; k++) {
                int cx = cell_of[k] % a->cells, cy = cell_of[k] / a->cells;
                VkImageBlit b = {};
                b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1};
                b.srcOffsets[0] = {cx * big, cy * big, 0};
                b.srcOffsets[1] = {(cx + 1) * big, (cy + 1) * big, 1};
                b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
                b.dstOffsets[0] = {cx * small, cy * small, 0};
                b.dstOffsets[1] = {(cx + 1) * small, (cy + 1) * small, 1};
                vkCmdBlitImage(v.cb, v.image, SRC, v.image, DST, 1, &b, VK_FILTER_LINEAR);
            }
        }
        barrier(v.cb, v.image, 0, MIPS - 1, SRC, READ, XFER, VK_ACCESS_TRANSFER_READ_BIT, FRAG, VK_ACCESS_SHADER_READ_BIT);
        barrier(v.cb, v.image, MIPS - 1, 1, DST, READ, XFER, VK_ACCESS_TRANSFER_WRITE_BIT, FRAG, VK_ACCESS_SHADER_READ_BIT);
    } else {
        barrier(v.cb, v.image, 0, MIPS, DST, READ, XFER, VK_ACCESS_TRANSFER_WRITE_BIT, FRAG, VK_ACCESS_SHADER_READ_BIT);
    }
    vkEndCommandBuffer(v.cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &v.cb;
    vk_queue_lock();
    VkResult r = vkQueueSubmit(t->queue, 1, &si, v.fence);
    vk_queue_unlock();
    if (r != VK_SUCCESS) {
        rt_log("menu: icon texture (%s): a copy failed (%d)", a->name, (int)r);
        return 0;
    }
    v.fresh = false;
    plat_mutex_lock(&g_lock);   // drawn from the next frame on: after the copy on the queue
    for (int k = 0; k < nr; k++)
        if (a->icons[cell_of[k]].gen == gen_of[k] && a->icons[cell_of[k]].state == READY) a->icons[cell_of[k]].state = SHOWN;
    plat_mutex_unlock(&g_lock);
    return 1;
}

int icons_frame(const MenuTarget *t) {
    g_tick++;
    int small = frame(&g_small, t), large = frame(&g_large, t);
    return small || large;
}

void icons_release(void) {
    release(&g_small);
    release(&g_large);
}
