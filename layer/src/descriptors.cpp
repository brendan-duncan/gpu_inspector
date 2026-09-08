#include "descriptors.h"

#include "resources.h"
#include "vk_serialize.gen.h"

#include <algorithm>
#include <cstring>

namespace vkinsp {

DescriptorTracker& DescriptorTracker::Get() {
    static DescriptorTracker* instance = new DescriptorTracker();
    return *instance;
}

#define VKINSP_KEY(h) ((uint64_t)(uintptr_t)(h))

static bool IsImageType(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
           t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}

static bool IsBufferType(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static bool IsTexelType(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
}

static bool IsDynamicType(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

// ---------------------------------------------------------------------------------------------
// Layouts and sets

void DescriptorTracker::OnCreateLayout(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutCreateInfo* info) {
    if (!layout || !info) return;
    DescriptorSetContents c;
    c.layout = layout;
    for (uint32_t i = 0; i < info->bindingCount && info->pBindings; ++i) {
        const VkDescriptorSetLayoutBinding& b = info->pBindings[i];
        DescriptorBinding db;
        db.binding = b.binding;
        db.type = b.descriptorType;
        db.stages = b.stageFlags;
        db.immutableSamplers = b.pImmutableSamplers != nullptr &&
            (b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        // Inline uniform blocks count bytes, not descriptors; they are shown as one entry.
        uint32_t count = b.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ? 1 : b.descriptorCount;
        db.entries.resize(count);
        if (db.immutableSamplers) {
            for (uint32_t k = 0; k < count; ++k) {
                db.entries[k].sampler = b.pImmutableSamplers[k];
                if (b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) db.entries[k].written = true;
            }
        }
        c.bindings.push_back(std::move(db));
    }
    std::sort(c.bindings.begin(), c.bindings.end(), [](const DescriptorBinding& a, const DescriptorBinding& b) {
        return a.binding < b.binding;
    });
    std::unique_lock lock(_mutex);
    _layouts[VKINSP_KEY(layout)] = std::move(c);
}

void DescriptorTracker::OnAllocateSets(const VkDescriptorSetAllocateInfo* info, const VkDescriptorSet* sets) {
    if (!info || !sets) return;
    const VkDescriptorSetVariableDescriptorCountAllocateInfo* variable = nullptr;
    for (auto* n = static_cast<const VkBaseInStructure*>(info->pNext); n; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO)
            variable = reinterpret_cast<const VkDescriptorSetVariableDescriptorCountAllocateInfo*>(n);
    }
    std::unique_lock lock(_mutex);
    for (uint32_t i = 0; i < info->descriptorSetCount; ++i) {
        if (!sets[i]) continue;
        auto it = _layouts.find(VKINSP_KEY(info->pSetLayouts[i]));
        DescriptorSetContents c;
        if (it != _layouts.end()) c = it->second;
        c.layout = info->pSetLayouts[i];
        if (variable && i < variable->descriptorSetCount && !c.bindings.empty()) {
            // The variable-count binding is the one with the highest number.
            DescriptorBinding& last = c.bindings.back();
            last.entries.resize(variable->pDescriptorCounts[i]);
        }
        _sets[VKINSP_KEY(sets[i])] = std::move(c);
    }
}

// Descriptor writes that run past the end of a binding continue into the next binding (same
// type); this walks bindings in number order to place element `arrayElement + k`.
static DescriptorEntry* EntryAt(DescriptorSetContents& set, size_t& bindingIndex, uint32_t& arrayElement) {
    while (bindingIndex < set.bindings.size()) {
        DescriptorBinding& b = set.bindings[bindingIndex];
        if (arrayElement < b.entries.size()) return &b.entries[arrayElement];
        arrayElement -= (uint32_t)b.entries.size();
        bindingIndex++;
    }
    return nullptr;
}

static size_t FindBinding(const DescriptorSetContents& set, uint32_t binding) {
    for (size_t i = 0; i < set.bindings.size(); ++i)
        if (set.bindings[i].binding == binding) return i;
    return set.bindings.size();
}

void DescriptorTracker::ApplyWrite(DescriptorSetContents& set, uint32_t binding, uint32_t arrayElement,
                                   VkDescriptorType type, uint32_t count, const VkDescriptorImageInfo* images,
                                   const VkDescriptorBufferInfo* buffers, const VkBufferView* views, size_t stride) {
    size_t bi = FindBinding(set, binding);
    if (bi >= set.bindings.size()) return;
    if (type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) count = 1;
    uint32_t element = arrayElement;
    for (uint32_t k = 0; k < count; ++k) {
        DescriptorEntry* e = EntryAt(set, bi, element);
        if (!e) return;
        const uint8_t* base = nullptr;
        if (IsImageType(type) && images) base = reinterpret_cast<const uint8_t*>(images) + k * stride;
        else if (IsBufferType(type) && buffers) base = reinterpret_cast<const uint8_t*>(buffers) + k * stride;
        else if (IsTexelType(type) && views) base = reinterpret_cast<const uint8_t*>(views) + k * stride;
        if (base) {
            if (IsImageType(type)) {
                VkDescriptorImageInfo ii;
                memcpy(&ii, base, sizeof(ii));
                e->imageView = ii.imageView;
                e->imageLayout = ii.imageLayout;
                if (!set.bindings[bi].immutableSamplers) e->sampler = ii.sampler;
            } else if (IsBufferType(type)) {
                VkDescriptorBufferInfo bi2;
                memcpy(&bi2, base, sizeof(bi2));
                e->buffer = bi2.buffer;
                e->offset = bi2.offset;
                e->range = bi2.range;
            } else {
                VkBufferView bv;
                memcpy(&bv, base, sizeof(bv));
                e->bufferView = bv;
            }
        }
        e->written = true;
        element++;
    }
}

void DescriptorTracker::OnUpdateSets(uint32_t writeCount, const VkWriteDescriptorSet* writes, uint32_t copyCount,
                                     const VkCopyDescriptorSet* copies) {
    std::unique_lock lock(_mutex);
    for (uint32_t i = 0; writes && i < writeCount; ++i) {
        const VkWriteDescriptorSet& w = writes[i];
        auto it = _sets.find(VKINSP_KEY(w.dstSet));
        if (it == _sets.end()) continue;
        size_t stride = IsImageType(w.descriptorType) ? sizeof(VkDescriptorImageInfo)
                      : IsBufferType(w.descriptorType) ? sizeof(VkDescriptorBufferInfo) : sizeof(VkBufferView);
        ApplyWrite(it->second, w.dstBinding, w.dstArrayElement, w.descriptorType, w.descriptorCount, w.pImageInfo,
                   w.pBufferInfo, w.pTexelBufferView, stride);
    }
    for (uint32_t i = 0; copies && i < copyCount; ++i) {
        const VkCopyDescriptorSet& c = copies[i];
        auto src = _sets.find(VKINSP_KEY(c.srcSet));
        auto dst = _sets.find(VKINSP_KEY(c.dstSet));
        if (src == _sets.end() || dst == _sets.end()) continue;
        // Copy out first: src and dst may be the same set.
        std::vector<DescriptorEntry> tmp;
        size_t sb = FindBinding(src->second, c.srcBinding);
        uint32_t se = c.srcArrayElement;
        for (uint32_t k = 0; k < c.descriptorCount; ++k) {
            DescriptorEntry* e = EntryAt(src->second, sb, se);
            if (!e) break;
            tmp.push_back(*e);
            se++;
        }
        size_t db = FindBinding(dst->second, c.dstBinding);
        uint32_t de = c.dstArrayElement;
        for (auto& e : tmp) {
            DescriptorEntry* d = EntryAt(dst->second, db, de);
            if (!d) break;
            *d = e;
            de++;
        }
    }
}

void DescriptorTracker::OnCreateTemplate(VkDescriptorUpdateTemplate tmpl, const VkDescriptorUpdateTemplateCreateInfo* info) {
    if (!tmpl || !info) return;
    DescriptorTemplateInfo t;
    t.type = info->templateType;
    t.layout = info->descriptorSetLayout;
    if (info->pDescriptorUpdateEntries)
        t.entries.assign(info->pDescriptorUpdateEntries, info->pDescriptorUpdateEntries + info->descriptorUpdateEntryCount);
    std::unique_lock lock(_mutex);
    _templates[VKINSP_KEY(tmpl)] = std::move(t);
}

void DescriptorTracker::OnUpdateWithTemplate(VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl, const void* data) {
    if (!data) return;
    std::unique_lock lock(_mutex);
    auto tit = _templates.find(VKINSP_KEY(tmpl));
    auto sit = _sets.find(VKINSP_KEY(set));
    if (tit == _templates.end() || sit == _sets.end()) return;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (const VkDescriptorUpdateTemplateEntry& e : tit->second.entries) {
        const uint8_t* p = bytes + e.offset;
        ApplyWrite(sit->second, e.dstBinding, e.dstArrayElement, e.descriptorType, e.descriptorCount,
                   IsImageType(e.descriptorType) ? reinterpret_cast<const VkDescriptorImageInfo*>(p) : nullptr,
                   IsBufferType(e.descriptorType) ? reinterpret_cast<const VkDescriptorBufferInfo*>(p) : nullptr,
                   IsTexelType(e.descriptorType) ? reinterpret_cast<const VkBufferView*>(p) : nullptr, e.stride);
    }
}

void DescriptorTracker::OnDestroy(HandleType type, uint64_t handle) {
    std::unique_lock lock(_mutex);
    switch (type) {
        case HT_VkDescriptorSet: _sets.erase(handle); break;
        case HT_VkDescriptorSetLayout: _layouts.erase(handle); break;
        case HT_VkDescriptorUpdateTemplate: _templates.erase(handle); break;
        default: break;
    }
}

bool DescriptorTracker::GetSet(VkDescriptorSet set, DescriptorSetContents& out) const {
    std::shared_lock lock(_mutex);
    auto it = _sets.find(VKINSP_KEY(set));
    if (it == _sets.end()) return false;
    out = it->second;
    return true;
}

bool DescriptorTracker::GetLayout(VkDescriptorSetLayout layout, DescriptorSetContents& out) const {
    std::shared_lock lock(_mutex);
    auto it = _layouts.find(VKINSP_KEY(layout));
    if (it == _layouts.end()) return false;
    out = it->second;
    return true;
}

DescriptorSetContents DescriptorTracker::FromWrites(uint32_t writeCount, const VkWriteDescriptorSet* writes) {
    DescriptorSetContents c;
    for (uint32_t i = 0; writes && i < writeCount; ++i) {
        const VkWriteDescriptorSet& w = writes[i];
        DescriptorBinding b;
        b.binding = w.dstBinding;
        b.type = w.descriptorType;
        uint32_t count = w.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ? 1 : w.descriptorCount;
        b.entries.resize(w.dstArrayElement + count);
        for (uint32_t k = 0; k < count; ++k) {
            DescriptorEntry& e = b.entries[w.dstArrayElement + k];
            e.written = true;
            if (IsImageType(w.descriptorType) && w.pImageInfo) {
                e.imageView = w.pImageInfo[k].imageView;
                e.sampler = w.pImageInfo[k].sampler;
                e.imageLayout = w.pImageInfo[k].imageLayout;
            } else if (IsBufferType(w.descriptorType) && w.pBufferInfo) {
                e.buffer = w.pBufferInfo[k].buffer;
                e.offset = w.pBufferInfo[k].offset;
                e.range = w.pBufferInfo[k].range;
            } else if (IsTexelType(w.descriptorType) && w.pTexelBufferView) {
                e.bufferView = w.pTexelBufferView[k];
            }
        }
        c.bindings.push_back(std::move(b));
    }
    std::sort(c.bindings.begin(), c.bindings.end(), [](const DescriptorBinding& a, const DescriptorBinding& b) {
        return a.binding < b.binding;
    });
    return c;
}

// ---------------------------------------------------------------------------------------------
// JSON

bool IsBufferDescriptor(VkDescriptorType t) { return IsBufferType(t); }
bool IsDynamicDescriptor(VkDescriptorType t) { return IsDynamicType(t); }
bool IsImageDescriptor(VkDescriptorType t) { return IsImageType(t) && t != VK_DESCRIPTOR_TYPE_SAMPLER; }

VkDeviceSize DescriptorBufferRange(const DescriptorEntry& e) {
    if (e.range != VK_WHOLE_SIZE) return e.range;
    BufferInfo bi;
    if (!e.buffer || !ResourceRegistry::Get().GetBuffer(e.buffer, bi)) return 0;
    return bi.size > e.offset ? bi.size - e.offset : 0;
}

void WriteDescriptorSetJson(JsonWriter& w, uint32_t setIndex, VkDescriptorSet set, const DescriptorSetContents& contents,
                            const uint32_t* dynamicOffsets, uint32_t dynamicOffsetCount, uint32_t& dynamicIndex,
                            const std::vector<std::vector<uint32_t>>* dataIds) {
    w.BeginObject();
    w.Key("set"); w.Uint(setIndex);
    w.Key("descriptorSet"); w.Handle(HT_VkDescriptorSet, "VkDescriptorSet", (uint64_t)(uintptr_t)set);
    w.Key("layout"); w.Handle(HT_VkDescriptorSetLayout, "VkDescriptorSetLayout", (uint64_t)(uintptr_t)contents.layout);
    w.Key("bindings");
    WriteDescriptorBindingsJson(w, contents, dynamicOffsets, dynamicOffsetCount, dynamicIndex, dataIds);
    w.EndObject();
}

void WriteDescriptorBindingsJson(JsonWriter& w, const DescriptorSetContents& contents, const uint32_t* dynamicOffsets,
                                 uint32_t dynamicOffsetCount, uint32_t& dynamicIndex,
                                 const std::vector<std::vector<uint32_t>>* dataIds) {
    w.BeginArray();
    for (size_t bi = 0; bi < contents.bindings.size(); ++bi) {
        const DescriptorBinding& b = contents.bindings[bi];
        w.BeginObject();
        w.Key("binding"); w.Uint(b.binding);
        w.Key("type"); w.Enum(ToString_VkDescriptorType(b.type), (int64_t)b.type);
        if (b.stages) { w.Key("stages"); Flags_VkShaderStageFlags(w, b.stages); }
        w.Key("descriptors"); w.BeginArray();
        for (size_t k = 0; k < b.entries.size(); ++k) {
            const DescriptorEntry& e = b.entries[k];
            // Every element of a dynamic binding consumes a dynamic offset, written or not.
            uint32_t dynamicOffset = 0;
            if (IsDynamicType(b.type)) {
                dynamicOffset = dynamicOffsets && dynamicIndex < dynamicOffsetCount ? dynamicOffsets[dynamicIndex] : 0;
                dynamicIndex++;
            }
            if (!e.written) { w.Null(); continue; }
            w.BeginObject();
            if (IsBufferType(b.type)) {
                w.Key("buffer"); w.Handle(HT_VkBuffer, "VkBuffer", (uint64_t)(uintptr_t)e.buffer);
                w.Key("offset"); w.Uint(e.offset);
                w.Key("range"); w.Uint(DescriptorBufferRange(e));
                if (IsDynamicType(b.type)) { w.Key("dynamicOffset"); w.Uint(dynamicOffset); }
                if (dataIds && bi < dataIds->size() && k < (*dataIds)[bi].size() && (*dataIds)[bi][k]) {
                    w.Key("data"); w.Uint((*dataIds)[bi][k]);
                }
            } else if (IsImageType(b.type)) {
                if (b.type != VK_DESCRIPTOR_TYPE_SAMPLER) {
                    w.Key("imageView"); w.Handle(HT_VkImageView, "VkImageView", (uint64_t)(uintptr_t)e.imageView);
                    w.Key("imageLayout"); w.Enum(ToString_VkImageLayout(e.imageLayout), (int64_t)e.imageLayout);
                    // The texture capture id of the image's contents (see CaptureManager::QueueImageCapture).
                    if (dataIds && bi < dataIds->size() && k < (*dataIds)[bi].size() && (*dataIds)[bi][k]) {
                        w.Key("data"); w.Uint((*dataIds)[bi][k]);
                    }
                }
                if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER || b.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                    w.Key("sampler"); w.Handle(HT_VkSampler, "VkSampler", (uint64_t)(uintptr_t)e.sampler);
                    if (b.immutableSamplers) { w.Key("immutable"); w.Boolean(true); }
                }
            } else if (IsTexelType(b.type)) {
                w.Key("bufferView"); w.Handle(HT_VkBufferView, "VkBufferView", (uint64_t)(uintptr_t)e.bufferView);
            }
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
}

} // namespace vkinsp
