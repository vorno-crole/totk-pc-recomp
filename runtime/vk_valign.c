// Vertex streams Metal cannot read (macOS only). On this GPU family Metal wants vertex strides and attribute offsets in
// multiples of 4 bytes, and NVN does not: TotK's skinned meshes use strides of 6, 10 and 14 with attributes at +6, +9 and
// +10. MoltenVK reports a bad stride only when it is static, and here it is dynamic, so nothing warns: the attributes read
// garbage, bone indices above all, and vertices fly off into long streaks of stretched texture (docs/macos.md).
//
// A stream with such a layout gets a copy with every attribute at an aligned offset. The layout comes from the attribute
// state alone, not from the shader, so one copy serves every shader that draws from the stream. vk_pipeline.c
// vertex_input and vk_exec_draw.c bind_vertex_buffers both ask vk_valign_layout, so they cannot disagree.
//
// A copy is rebuilt when the pages under it change, checked once per vk_mem_gen. A stream the GPU writes reaches the
// copy only after the write-back at the end of its submission, a frame late; none is known.
#ifdef __APPLE__
#include <string.h>

#include "host.h"
#include "vk.h"

// Bytes of an attribute format, by VkFormat ranges: only vk_vertex_format's formats reach this.
static uint32_t fmt_bytes(VkFormat f) {
    if (f >= VK_FORMAT_R8_UNORM && f <= VK_FORMAT_R8_SRGB) return 1;
    if (f >= VK_FORMAT_R8G8_UNORM && f <= VK_FORMAT_R8G8_SRGB) return 2;
    if (f >= VK_FORMAT_R8G8B8_UNORM && f <= VK_FORMAT_B8G8R8_SRGB) return 3;
    if (f >= VK_FORMAT_R16_UNORM && f <= VK_FORMAT_R16_SFLOAT) return 2;
    if (f >= VK_FORMAT_R16G16B16_UNORM && f <= VK_FORMAT_R16G16B16_SFLOAT) return 6;
    if (f >= VK_FORMAT_R16G16B16A16_UNORM && f <= VK_FORMAT_R16G16B16A16_SFLOAT) return 8;
    if (f >= VK_FORMAT_R32G32_UINT && f <= VK_FORMAT_R32G32_SFLOAT) return 8;
    if (f >= VK_FORMAT_R32G32B32_UINT && f <= VK_FORMAT_R32G32B32_SFLOAT) return 12;
    if (f >= VK_FORMAT_R32G32B32A32_UINT && f <= VK_FORMAT_R32G32B32A32_SFLOAT) return 16;
    return 4;
}

// The fourth 16-bit channel an RGB16 attribute lacks: the 1 Maxwell reads there. vk_vertex_format maps RGB16 to RGBA16,
// which would otherwise take the next vertex's first bytes as w.
static uint16_t rgb16_w(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R16G16B16A16_UNORM: return 0xffff;
    case VK_FORMAT_R16G16B16A16_SNORM: return 0x7fff;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 0x3c00;
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT: return 1;
    default: return 0;
    }
}

int vk_valign_layout(const uint32_t vattr[16], int stream, uint32_t stride, VkValign *out) {
    memset(out, 0, sizeof *out);
    if (!stride) return 0;
    int bad = stride % 4 != 0;
    uint32_t at = 0;
    for (int i = 0; i < 16; i++) {
        va_st va;
        memcpy(&va, &vattr[i], sizeof va);
        VkFormat f = va.fmt ? vk_vertex_format((int)va.fmt, NULL) : VK_FORMAT_UNDEFINED;
        if (f == VK_FORMAT_UNDEFINED || (int)va.stream != stream) continue;
        uint32_t bytes = fmt_bytes(f), have = va.offset < stride ? stride - va.offset : bytes;
        if (va.offset % 4) bad = 1;
        VkValignAttr *a = &out->a[out->n++];
        a->attr = (uint8_t)i, a->src = va.offset, a->dst = at, a->copy = bytes < have ? bytes : have, a->bytes = bytes;
        a->w = bytes == 8 && a->copy == 6 ? rgb16_w(f) : 0;
        at += (bytes + 3) & ~3u;
    }
    if (!bad) return 0;
    out->stride_in = stride, out->stride = at ? at : (stride + 3) & ~3u;
    return 1;
}

uint32_t vk_valign_offset(const VkValign *v, int attr, uint32_t offset) {
    for (int i = 0; i < v->n; i++)
        if (v->a[i].attr == attr) return v->a[i].dst;
    return offset;
}

// ---- the copies: replay thread only

#define CACHE_SLOTS 8192u   // power of two; a full table starts over

typedef struct {
    uint64_t addr, size, hash;
    uint64_t checked;        // vk_mem_gen of the last look at the pages
    uint32_t since;          // page generation the contents are from
    VkBuffer buf;
    VmaAllocation alloc;
    VkDeviceSize bytes;
} Copy;
static Copy cache[CACHE_SLOTS];
static uint32_t ncache;
static uint64_t built, built_bytes;

static uint64_t layout_hash(const VkValign *v) {
    uint64_t h = 0xcbf29ce484222325ull ^ v->stride_in;
    for (const uint8_t *p = (const uint8_t *)v->a, *e = p + sizeof(VkValignAttr) * v->n; p < e; p++)
        h = (h ^ *p) * 0x100000001b3ull;
    return h * 0x100000001b3ull + v->stride;
}

static void fill(const VkValign *v, const uint8_t *src, uint64_t verts, uint8_t *dst) {
    for (uint64_t i = 0; i < verts; i++, src += v->stride_in, dst += v->stride)
        for (int k = 0; k < v->n; k++) {
            const VkValignAttr *a = &v->a[k];
            memcpy(dst + a->dst, src + a->src, a->copy);
            if (a->w) memcpy(dst + a->dst + 6, &a->w, 2);
        }
}

static void drop(Copy *c) {
    if (c->buf) vk_defer_buffer(c->buf, c->alloc);
    memset(c, 0, sizeof *c);
}

int vk_valign_buffer(uint64_t addr, uint64_t size, const VkValign *v, VkBuffer *buf, VkDeviceSize *bytes) {
    VkCtx *ctx = vk_ctx();
    uint64_t verts = size / v->stride_in, h = layout_hash(v);
    if (!verts) return 0;
    uint64_t slot = (addr * 0x9e3779b97f4a7c15ull ^ h ^ size) >> 19 & (CACHE_SLOTS - 1);
    Copy *c = NULL;
    for (uint32_t i = 0; i < CACHE_SLOTS; i++, slot = (slot + 1) & (CACHE_SLOTS - 1)) {
        c = &cache[slot];
        if (!c->buf || (c->addr == addr && c->size == size && c->hash == h)) break;
    }
    if (ncache >= CACHE_SLOTS / 2) {
        for (uint32_t i = 0; i < CACHE_SLOTS; i++) drop(&cache[i]);
        ncache = 0;
        rt_log("vk: vertex stream copies: table full, starting over");
    }
    uint64_t gen = vk_mem_gen();
    if (c->buf && c->addr == addr && c->size == size && c->hash == h) {
        if (c->checked == gen) goto hit;
        uint32_t now = c->since;
        int changed = vk_mem_changed(addr, size, c->since, &now);
        c->checked = gen;
        if (changed == 0) { c->since = now; goto hit; }
        drop(c);   // a new buffer: submissions still in flight read the old one
        ncache--;
    }
    uint32_t since = 0;
    vk_mem_changed(addr, size, 0, &since);   // the generation before the copy: a later write is seen next time
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = verts * v->stride,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO, .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                                          VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VkBuffer b;
    VmaAllocation al;
    if (vmaCreateBuffer(ctx->vma, &bi, &ai, &b, &al, &info) != VK_SUCCESS) return 0;
    fill(v, (const uint8_t *)(uintptr_t)addr, verts, info.pMappedData);
    vmaFlushAllocation(ctx->vma, al, 0, VK_WHOLE_SIZE);   // the managed memory type is not coherent
    *c = (Copy){addr, size, h, gen, since, b, al, bi.size};
    ncache++;
    built++, built_bytes += bi.size;
    if ((built & (built - 1)) == 0)
        rt_log("vk: vertex stream copies: %llu built, %.1f MiB, for Metal's 4-byte alignment", (unsigned long long)built,
               (double)built_bytes / 1048576.0);
hit:
    *buf = c->buf, *bytes = c->bytes;
    return 1;
}

void vk_valign_shutdown(void) {
    VkCtx *ctx = vk_ctx();
    for (uint32_t i = 0; ctx && i < CACHE_SLOTS; i++)
        if (cache[i].buf) vmaDestroyBuffer(ctx->vma, cache[i].buf, cache[i].alloc);
    memset(cache, 0, sizeof cache);
    ncache = 0;
}
#endif
