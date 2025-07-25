// Copyright 2020 Tencent
// SPDX-License-Identifier: BSD-3-Clause

#include "pipelinecache.h"

#include "gpu.h"

namespace ncnn {

#if NCNN_VULKAN
// https://en.wikipedia.org/wiki/MurmurHash
static uint32_t murmur3_32(const uint32_t* data, int size)
{
    uint32_t h = 0;

    for (int i = 0; i < size; i++)
    {
        uint32_t k = *data++;

        k *= 0xcc9e2d51;
        k = (k << 15) | (k >> (32 - 15));
        k *= 0x1b873593;

        h ^= k;
        h = (h << 13) | (h >> (32 - 13));
        h = (h * 5) + 0xe6546b64;
    }

    h ^= size * 4;

    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;

    return h;
}

// https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function#FNV-1a_hash
static uint32_t fnv1a_32(const uint8_t* data, int size)
{
    uint32_t h = 0x811c9dc5;

    for (int i = 0; i < size; i++)
    {
        h ^= (uint32_t)*data++;
        h *= 0x01000193;
    }

    return h;
}

class PipelineCachePrivate
{
public:
    // digest -> artifact
    struct pipeline_cache_digest
    {
        pipeline_cache_digest(const uint32_t* spv_data, size_t spv_data_size, const std::vector<vk_specialization_type>& specializations,
                              uint32_t local_size_x, uint32_t local_size_y, uint32_t local_size_z, uint32_t subgroup_size);
        pipeline_cache_digest(int shader_type_index, const Option& opt, const std::vector<vk_specialization_type>& specializations,
                              uint32_t local_size_x, uint32_t local_size_y, uint32_t local_size_z, uint32_t subgroup_size);

        bool operator==(const pipeline_cache_digest& rhs) const
        {
            return d0 == rhs.d0 && d1 == rhs.d1 && d2 == rhs.d2 && d3 == rhs.d3;
        }

        bool operator!=(const pipeline_cache_digest& rhs) const
        {
            return d0 != rhs.d0 || d1 != rhs.d1 || d2 != rhs.d2 || d3 != rhs.d3;
        }

        union
        {
            struct
            {
                union
                {
                    uint32_t spv_data_murmur3;
                    int shader_type_index;
                };
                uint32_t opt_bits;
                uint32_t local_size_x;
                uint32_t local_size_y;
                uint32_t local_size_z;
                uint32_t subgroup_size;
                uint32_t specializations_murmur3;
                uint32_t specializations_fnv1a;
            };

            struct
            {
                uint64_t d0;
                uint64_t d1;
                uint64_t d2;
                uint64_t d3;
            };
        };
    };

    struct pipeline_cache_artifact
    {
        VkShaderModule shader_module;
        VkDescriptorSetLayout descriptorset_layout;
        VkPipelineLayout pipeline_layout;
        VkPipeline pipeline;
        VkDescriptorUpdateTemplateKHR descriptor_update_template;
        ShaderInfo shader_info; // TODO use pointer ?
    };

    mutable std::vector<pipeline_cache_digest> cache_digests;
    mutable std::vector<pipeline_cache_artifact> cache_artifacts;
    mutable Mutex cache_lock;
};

PipelineCachePrivate::pipeline_cache_digest::pipeline_cache_digest(const uint32_t* spv_data, size_t spv_data_size, const std::vector<vk_specialization_type>& specializations,
        uint32_t _local_size_x, uint32_t _local_size_y, uint32_t _local_size_z, uint32_t _subgroup_size)
{
    spv_data_murmur3 = murmur3_32(spv_data, spv_data_size / 4);

    opt_bits = 0;

    local_size_x = _local_size_x;
    local_size_y = _local_size_y;
    local_size_z = _local_size_z;
    subgroup_size = _subgroup_size;

    // encode specializations
    const int specialization_count = specializations.size();
    specializations_murmur3 = murmur3_32((const uint32_t*)specializations.data(), specialization_count);
    specializations_fnv1a = fnv1a_32((const uint8_t*)specializations.data(), specialization_count * sizeof(vk_specialization_type));
}

PipelineCachePrivate::pipeline_cache_digest::pipeline_cache_digest(int _shader_type_index, const Option& opt, const std::vector<vk_specialization_type>& specializations,
        uint32_t _local_size_x, uint32_t _local_size_y, uint32_t _local_size_z, uint32_t _subgroup_size)
{
    shader_type_index = _shader_type_index;

    // encode opt
    opt_bits = 0 << 7
               | opt.use_fp16_packed << 6
               | opt.use_fp16_storage << 5
               | opt.use_fp16_arithmetic << 4
               | opt.use_int8_storage << 3
               | opt.use_int8_arithmetic << 2;

    local_size_x = _local_size_x;
    local_size_y = _local_size_y;
    local_size_z = _local_size_z;
    subgroup_size = _subgroup_size;

    // encode specializations
    const int specialization_count = specializations.size();
    specializations_murmur3 = murmur3_32((const uint32_t*)specializations.data(), specialization_count);
    specializations_fnv1a = fnv1a_32((const uint8_t*)specializations.data(), specialization_count * sizeof(vk_specialization_type));
}

PipelineCache::PipelineCache(const VulkanDevice* _vkdev)
    : vkdev(_vkdev), d(new PipelineCachePrivate)
{
}

PipelineCache::~PipelineCache()
{
    clear();

    delete d;
}

PipelineCache::PipelineCache(const PipelineCache&)
    : d(0)
{
}

PipelineCache& PipelineCache::operator=(const PipelineCache&)
{
    return *this;
}

void PipelineCache::clear()
{
    MutexLockGuard lock(d->cache_lock);

    for (size_t i = 0; i < d->cache_artifacts.size(); i++)
    {
        const PipelineCachePrivate::pipeline_cache_artifact& cc = d->cache_artifacts[i];

        if (vkdev->info.support_VK_KHR_descriptor_update_template())
        {
            if (cc.descriptor_update_template)
            {
                vkdev->vkDestroyDescriptorUpdateTemplateKHR(vkdev->vkdevice(), cc.descriptor_update_template, 0);
            }
        }

        if (cc.pipeline)
        {
            vkDestroyPipeline(vkdev->vkdevice(), cc.pipeline, 0);
        }

        if (cc.pipeline_layout)
        {
            vkDestroyPipelineLayout(vkdev->vkdevice(), cc.pipeline_layout, 0);
        }

        if (cc.descriptorset_layout)
        {
            vkDestroyDescriptorSetLayout(vkdev->vkdevice(), cc.descriptorset_layout, 0);
        }

        if (cc.shader_module)
        {
            vkDestroyShaderModule(vkdev->vkdevice(), cc.shader_module, 0);
        }
    }

    d->cache_digests.clear();
    d->cache_artifacts.clear();
}

int PipelineCache::get_pipeline(const uint32_t* spv_data, size_t spv_data_size, const std::vector<vk_specialization_type>& specializations,
                                uint32_t local_size_x, uint32_t local_size_y, uint32_t local_size_z, uint32_t subgroup_size,
                                VkShaderModule* _shader_module,
                                VkDescriptorSetLayout* descriptorset_layout,
                                VkPipelineLayout* pipeline_layout,
                                VkPipeline* pipeline,
                                VkDescriptorUpdateTemplateKHR* descriptor_update_template,
                                ShaderInfo& shader_info) const
{
    MutexLockGuard lock(d->cache_lock);

    PipelineCachePrivate::pipeline_cache_digest key(spv_data, spv_data_size, specializations, local_size_x, local_size_y, local_size_z, subgroup_size);

    if (!vkdev->info.bug_corrupted_online_pipeline_cache())
    {
        // find cache
        for (size_t i = 0; i < d->cache_digests.size(); i++)
        {
            if (d->cache_digests[i] != key)
                continue;

            // hit cache
            const PipelineCachePrivate::pipeline_cache_artifact& cc = d->cache_artifacts[i];

            *_shader_module = cc.shader_module;
            *descriptorset_layout = cc.descriptorset_layout;
            *pipeline_layout = cc.pipeline_layout;
            *pipeline = cc.pipeline;
            *descriptor_update_template = cc.descriptor_update_template;
            shader_info = cc.shader_info;

            // NCNN_LOGE("get_pipeline hit %d", last_digest_index);

            return 0;
        }
    }

    int ret = 0;

    ret = resolve_shader_info(spv_data, spv_data_size, shader_info);
    if (ret != 0)
    {
        NCNN_LOGE("resolve_shader_info failed %d", ret);
        return -1;
    }

    VkShaderModule shader_module = vkdev->compile_shader_module(spv_data, spv_data_size, local_size_x, local_size_y, local_size_z);
    if (!shader_module)
    {
        NCNN_LOGE("create_shader_module failed");
        return -1;
    }

    ret = new_pipeline(shader_module, shader_info, specializations, subgroup_size, descriptorset_layout, pipeline_layout, pipeline, descriptor_update_template);
    if (ret != 0)
    {
        NCNN_LOGE("new_pipeline failed");
        vkDestroyShaderModule(vkdev->vkdevice(), shader_module, 0);
        return -1;
    }

    *_shader_module = shader_module;

    // save to cache
    {
        PipelineCachePrivate::pipeline_cache_artifact cc;

        cc.shader_module = *_shader_module;
        cc.descriptorset_layout = *descriptorset_layout;
        cc.pipeline_layout = *pipeline_layout;
        cc.pipeline = *pipeline;
        cc.descriptor_update_template = *descriptor_update_template;
        cc.shader_info = shader_info;

        d->cache_digests.push_back(key);
        d->cache_artifacts.push_back(cc);
    }

    // NCNN_LOGE("new_pipeline %d", last_digest_index);

    return 0;
}

int PipelineCache::get_pipeline(int shader_type_index, const Option& opt, const std::vector<vk_specialization_type>& specializations,
                                uint32_t local_size_x, uint32_t local_size_y, uint32_t local_size_z, uint32_t subgroup_size,
                                VkShaderModule* _shader_module,
                                VkDescriptorSetLayout* descriptorset_layout,
                                VkPipelineLayout* pipeline_layout,
                                VkPipeline* pipeline,
                                VkDescriptorUpdateTemplateKHR* descriptor_update_template,
                                ShaderInfo& shader_info) const
{
    MutexLockGuard lock(d->cache_lock);

    PipelineCachePrivate::pipeline_cache_digest key(shader_type_index, opt, specializations, local_size_x, local_size_y, local_size_z, subgroup_size);

    if (!vkdev->info.bug_corrupted_online_pipeline_cache())
    {
        // find cache
        for (size_t i = 0; i < d->cache_digests.size(); i++)
        {
            if (d->cache_digests[i] != key)
                continue;

            // hit cache
            const PipelineCachePrivate::pipeline_cache_artifact& cc = d->cache_artifacts[i];

            *_shader_module = cc.shader_module;
            *descriptorset_layout = cc.descriptorset_layout;
            *pipeline_layout = cc.pipeline_layout;
            *pipeline = cc.pipeline;
            *descriptor_update_template = cc.descriptor_update_template;
            shader_info = cc.shader_info;

            // NCNN_LOGE("get_pipeline hit %d", last_digest_index);

            return 0;
        }
    }

    int ret = 0;

    // create new pipeline
    VkShaderModule shader_module = 0;
    ret = create_shader_module(shader_type_index, opt, local_size_x, local_size_y, local_size_z, &shader_module, shader_info);
    if (ret != 0)
    {
        NCNN_LOGE("create_shader_module failed");
        return -1;
    }

    ret = new_pipeline(shader_module, shader_info, specializations, subgroup_size, descriptorset_layout, pipeline_layout, pipeline, descriptor_update_template);
    if (ret != 0)
    {
        NCNN_LOGE("new_pipeline failed");
        vkDestroyShaderModule(vkdev->vkdevice(), shader_module, 0);
        return -1;
    }

    *_shader_module = shader_module;

    // save to cache
    {
        PipelineCachePrivate::pipeline_cache_artifact cc;

        cc.shader_module = *_shader_module;
        cc.descriptorset_layout = *descriptorset_layout;
        cc.pipeline_layout = *pipeline_layout;
        cc.pipeline = *pipeline;
        cc.descriptor_update_template = *descriptor_update_template;
        cc.shader_info = shader_info;

        d->cache_digests.push_back(key);
        d->cache_artifacts.push_back(cc);
    }

    // NCNN_LOGE("new_pipeline %d", last_digest_index);

    return 0;
}

int PipelineCache::create_shader_module(int shader_type_index, const Option& opt,
                                        uint32_t local_size_x, uint32_t local_size_y, uint32_t local_size_z,
                                        VkShaderModule* _shader_module, ShaderInfo& si) const
{
    std::vector<uint32_t> spirv;
    int retc = compile_spirv_module(shader_type_index, opt, spirv);
    if (retc != 0)
    {
        NCNN_LOGE("compile_spirv_module failed %d", retc);
        return -1;
    }

    const uint32_t* spv_data = spirv.data();
    size_t spv_data_size = spirv.size() * 4;

    int ret = resolve_shader_info(spv_data, spv_data_size, si);
    if (ret != 0)
    {
        NCNN_LOGE("resolve_shader_info failed %d", ret);
        return -1;
    }

    VkShaderModule shader_module = vkdev->compile_shader_module(spv_data, spv_data_size, local_size_x, local_size_y, local_size_z);

    if (!shader_module)
    {
        NCNN_LOGE("create_shader_module failed");
        return -1;
    }

    *_shader_module = shader_module;

    return 0;
}

int PipelineCache::new_pipeline(VkShaderModule shader_module, const ShaderInfo& shader_info,
                                const std::vector<vk_specialization_type>& specializations, uint32_t subgroup_size,
                                VkDescriptorSetLayout* _descriptorset_layout,
                                VkPipelineLayout* _pipeline_layout,
                                VkPipeline* _pipeline,
                                VkDescriptorUpdateTemplateKHR* _descriptor_update_template) const
{
    int ret = 0;

    VkDescriptorSetLayout descriptorset_layout = 0;
    VkPipelineLayout pipeline_layout = 0;
    VkPipeline pipeline = 0;
    VkDescriptorUpdateTemplateKHR descriptor_update_template = 0;

    // create new pipeline
    if ((int)specializations.size() != shader_info.specialization_count)
    {
        NCNN_LOGE("pipeline specialization count mismatch, expect %d but got %d", shader_info.specialization_count, (int)specializations.size());
        goto ERROR_PipelineCache;
    }

    ret = vkdev->create_descriptorset_layout(shader_info.binding_count, shader_info.binding_types, &descriptorset_layout);
    if (ret != 0)
        goto ERROR_PipelineCache;

    ret = vkdev->create_pipeline_layout(shader_info.push_constant_count, descriptorset_layout, &pipeline_layout);
    if (ret != 0)
        goto ERROR_PipelineCache;

    ret = vkdev->create_pipeline(shader_module, pipeline_layout, specializations, subgroup_size, &pipeline);
    if (ret != 0)
        goto ERROR_PipelineCache;

    if (vkdev->info.support_VK_KHR_descriptor_update_template())
    {
        ret = vkdev->create_descriptor_update_template(shader_info.binding_count, shader_info.binding_types, descriptorset_layout, pipeline_layout, &descriptor_update_template);
        if (ret != 0)
            goto ERROR_PipelineCache;
    }

    *_descriptorset_layout = descriptorset_layout;
    *_pipeline_layout = pipeline_layout;
    *_pipeline = pipeline;
    *_descriptor_update_template = descriptor_update_template;

    return 0;

ERROR_PipelineCache:

    if (vkdev->info.support_VK_KHR_descriptor_update_template())
    {
        if (descriptor_update_template)
        {
            vkdev->vkDestroyDescriptorUpdateTemplateKHR(vkdev->vkdevice(), descriptor_update_template, 0);
        }
    }

    if (pipeline)
    {
        vkDestroyPipeline(vkdev->vkdevice(), pipeline, 0);
    }

    if (pipeline_layout)
    {
        vkDestroyPipelineLayout(vkdev->vkdevice(), pipeline_layout, 0);
    }

    if (descriptorset_layout)
    {
        vkDestroyDescriptorSetLayout(vkdev->vkdevice(), descriptorset_layout, 0);
    }

    return -1;
}

#endif // NCNN_VULKAN

} // namespace ncnn
