#!/usr/bin/env python3
"""Check the cross-scale resolve shader against dense CPU pixel patterns.

No game assets or NVIDIA runtime required. Needs Python vulkan and glslangValidator.
Tests both copy paths using --fast-copy 0/1, integer scales 1..7,
8/16/32/64/128-bit pixels, six endian modes, and guards.
--original-control --baseline-cpp FILE runs a pre-fix SDK command_processor.cpp
and must detect failure of the 2x-to-3x RGBA transfer.
"""

from pathlib import Path
import argparse
import subprocess
import tempfile
import vulkan as v

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sdk", required=True, type=Path)
parser.add_argument("--glslang", default="glslangValidator")
parser.add_argument("--original-control", action="store_true")
parser.add_argument("--baseline-cpp", type=Path)
parser.add_argument("--fast-copy", type=int, choices=[0, 1], default=1)
args = parser.parse_args()
negative = args.original_control
temporary = tempfile.TemporaryDirectory()
temp = Path(temporary.name)
source = (args.sdk / "src/graphics/vulkan/command_processor.cpp").read_text()
a = source.index(
    'static constexpr char shader[] = R"glsl(',
    source.index("bool VulkanCommandProcessor::Fh1SynchronizeResolve"),
) + len('static constexpr char shader[] = R"glsl(')
b = source.index(')glsl";', a)
shader = source[a:b]
if negative:
    if args.baseline_cpp is None:
        parser.error("--original-control requires --baseline-cpp")
    original = args.baseline_cpp.read_text()
    a = original.index(
        'static constexpr char shader[] = R"glsl(',
        original.index("bool VulkanCommandProcessor::Fh1SynchronizeResolve"),
    ) + len('static constexpr char shader[] = R"glsl(')
    shader = original[a : original.index(')glsl";', a)]
(temp / "resolve.comp").write_text(shader)
subprocess.run(
    [args.glslang, "-V", str(temp / "resolve.comp"), "-o", str(temp / "resolve.spv")],
    check=True,
)
inst = v.vkCreateInstance(
    v.VkInstanceCreateInfo(
        pApplicationInfo=v.VkApplicationInfo(apiVersion=v.VK_MAKE_VERSION(1, 3, 0))
    ),
    None,
)
phy = next(
    p
    for p in v.vkEnumeratePhysicalDevices(inst)
    if any(
        f.queueFlags & v.VK_QUEUE_COMPUTE_BIT
        for f in v.vkGetPhysicalDeviceQueueFamilyProperties(p)
    )
)
family = next(
    i
    for i, p in enumerate(v.vkGetPhysicalDeviceQueueFamilyProperties(phy))
    if p.queueFlags & v.VK_QUEUE_COMPUTE_BIT
)
dev = v.vkCreateDevice(
    phy,
    v.VkDeviceCreateInfo(
        pEnabledFeatures=v.vkGetPhysicalDeviceFeatures(phy),
        queueCreateInfoCount=1,
        pQueueCreateInfos=[
            v.VkDeviceQueueCreateInfo(
                queueFamilyIndex=family, queueCount=1, pQueuePriorities=[1.0]
            )
        ],
    ),
    None,
)
q = v.vkGetDeviceQueue(dev, family, 0)
mem = v.vkGetPhysicalDeviceMemoryProperties(phy)
flags = v.VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | v.VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
print(
    "Memory types:",
    [(i, mem.memoryTypes[i].propertyFlags) for i in range(mem.memoryTypeCount)],
    flush=True,
)


def buffer(data):
    b = v.vkCreateBuffer(
        dev,
        v.VkBufferCreateInfo(
            size=len(data),
            usage=v.VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            sharingMode=v.VK_SHARING_MODE_EXCLUSIVE,
        ),
        None,
    )
    req = v.vkGetBufferMemoryRequirements(dev, b)
    ids = [
        i
        for i in range(mem.memoryTypeCount)
        if req.memoryTypeBits & (1 << i)
        and mem.memoryTypes[i].propertyFlags & flags == flags
    ]
    ids.sort(
        key=lambda i: not bool(
            mem.memoryTypes[i].propertyFlags & v.VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        )
    )
    a = v.vkAllocateMemory(
        dev,
        v.VkMemoryAllocateInfo(allocationSize=req.size, memoryTypeIndex=ids[0]),
        None,
    )
    v.vkBindBufferMemory(dev, b, a, 0)
    mapped = v.vkMapMemory(dev, a, 0, len(data), 0)
    mapped[:] = data
    return b, a, mapped


src = buffer(bytes(4096))
dst = buffer(bytes(4096))
layout = v.vkCreateDescriptorSetLayout(
    dev,
    v.VkDescriptorSetLayoutCreateInfo(
        bindingCount=2,
        pBindings=[
            v.VkDescriptorSetLayoutBinding(
                binding=i,
                descriptorType=v.VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                descriptorCount=1,
                stageFlags=v.VK_SHADER_STAGE_COMPUTE_BIT,
            )
            for i in range(2)
        ],
    ),
    None,
)
pl = v.vkCreatePipelineLayout(
    dev,
    v.VkPipelineLayoutCreateInfo(
        setLayoutCount=1,
        pSetLayouts=[layout],
        pushConstantRangeCount=1,
        pPushConstantRanges=[
            v.VkPushConstantRange(
                stageFlags=v.VK_SHADER_STAGE_COMPUTE_BIT, offset=0, size=32
            )
        ],
    ),
    None,
)
pool = v.vkCreateDescriptorPool(
    dev,
    v.VkDescriptorPoolCreateInfo(
        maxSets=1,
        poolSizeCount=1,
        pPoolSizes=[
            v.VkDescriptorPoolSize(
                type=v.VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descriptorCount=2
            )
        ],
    ),
    None,
)
sets = v.vkAllocateDescriptorSets(
    dev,
    v.VkDescriptorSetAllocateInfo(
        descriptorPool=pool, descriptorSetCount=1, pSetLayouts=[layout]
    ),
)
v.vkUpdateDescriptorSets(
    dev,
    2,
    [
        v.VkWriteDescriptorSet(
            dstSet=sets[0],
            dstBinding=i,
            descriptorCount=1,
            descriptorType=v.VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            pBufferInfo=[v.VkDescriptorBufferInfo(buffer=b[0], offset=0, range=4096)],
        )
        for i, b in enumerate([src, dst])
    ],
    0,
    None,
)
cp = v.vkCreateCommandPool(
    dev,
    v.VkCommandPoolCreateInfo(
        queueFamilyIndex=family, flags=v.VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
    ),
    None,
)
cmd = v.vkAllocateCommandBuffers(
    dev,
    v.VkCommandBufferAllocateInfo(
        commandPool=cp, level=v.VK_COMMAND_BUFFER_LEVEL_PRIMARY, commandBufferCount=1
    ),
)[0]
code = (temp / "resolve.spv").read_bytes()
module = v.vkCreateShaderModule(
    dev, v.VkShaderModuleCreateInfo(codeSize=len(code), pCode=code), None
)
stage = v.VkPipelineShaderStageCreateInfo(
    stage=v.VK_SHADER_STAGE_COMPUTE_BIT, module=module, pName="main"
)
p = v.vkCreateComputePipelines(
    dev,
    v.VK_NULL_HANDLE,
    1,
    [v.VkComputePipelineCreateInfo(stage=stage, layout=pl, basePipelineIndex=-1)],
    None,
)[0]


def pack(image, scale, pb, mask):
    # Encode a dense image into the SDK's 16-byte, column-major quad storage.
    result = bytearray()
    for tile in image:
        for column in range(scale):
            for row in range(scale):
                quad = bytearray(16)
                pixels = tile[row][column * (16 // pb) : (column + 1) * (16 // pb)]
                for i, value in enumerate(b"".join(pixels)):
                    quad[i ^ mask] = value
                result.extend(quad)
    return result


passed = 0
for ss in range(1, 8):
    for ds in range(1, 8):
        for pb in [1, 2, 4, 8, 16]:
            image = [
                [
                    [
                        bytes(
                            (q * 79 + y * 13 + x * 37 + c * 101) % 256
                            for c in range(pb)
                        )
                        for x in range(16 // pb * ss)
                    ]
                    for y in range(ss)
                ]
                for q in range(4)
            ]
            expected_image = [
                [
                    [
                        tile[int((y + 0.5) * ss / ds)][int((x + 0.5) * ss / ds)]
                        for x in range(16 // pb * ds)
                    ]
                    for y in range(ds)
                ]
                for tile in image
            ]
            for mask in [0, 1, 3, 2, 7, 15]:
                if negative and (ss, ds, pb, mask) != (2, 3, 4, 0):
                    continue
                source = pack(image, ss, pb, mask)
                expected = pack(expected_image, ds, pb, mask)
                src[2][:] = b"\xab" * 32 + source + b"\xab" * (4096 - 32 - len(source))
                dst[2][:] = b"\xcd" * 4096
                v.vkResetCommandBuffer(cmd, 0)
                v.vkBeginCommandBuffer(cmd, v.VkCommandBufferBeginInfo())
                v.vkCmdBindPipeline(cmd, v.VK_PIPELINE_BIND_POINT_COMPUTE, p)
                v.vkCmdBindDescriptorSets(
                    cmd, v.VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, sets, 0, None
                )
                push = v.ffi.new(
                    "uint32_t[]",
                    [ss, ds, pb, len(expected), 32, 64, args.fast_copy, mask],
                )
                v.vkCmdPushConstants(
                    cmd, pl, v.VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, push
                )
                v.vkCmdDispatch(cmd, (len(expected) + 1023) // 1024, 1, 1)
                barrier = v.VkMemoryBarrier(
                    srcAccessMask=v.VK_ACCESS_SHADER_WRITE_BIT,
                    dstAccessMask=v.VK_ACCESS_HOST_READ_BIT,
                )
                v.vkCmdPipelineBarrier(
                    cmd,
                    v.VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    v.VK_PIPELINE_STAGE_HOST_BIT,
                    0,
                    1,
                    [barrier],
                    0,
                    None,
                    0,
                    None,
                )
                v.vkEndCommandBuffer(cmd)
                v.vkQueueSubmit(
                    q,
                    1,
                    [v.VkSubmitInfo(commandBufferCount=1, pCommandBuffers=[cmd])],
                    v.VK_NULL_HANDLE,
                )
                v.vkQueueWaitIdle(q)
                actual = bytes(dst[2][:])
                if negative:
                    assert actual != b"\xcd" * 64 + expected + b"\xcd" * (
                        4096 - 64 - len(expected)
                    ), "Original unexpectedly matches"
                    print(
                        "Original shader fails the 2x to 3x RGBA pattern as expected",
                        flush=True,
                    )
                else:
                    assert actual == b"\xcd" * 64 + expected + b"\xcd" * (
                        4096 - 64 - len(expected)
                    ), (ss, ds, pb, mask)
                passed += 1
    print("PASS source scale", ss, passed, "cases", flush=True)
print("ALL", passed, "GPU CASES PASSED")
v.vkDeviceWaitIdle(dev)
v.vkDestroyPipeline(dev, p, None)
v.vkDestroyShaderModule(dev, module, None)
v.vkDestroyCommandPool(dev, cp, None)
v.vkDestroyDescriptorPool(dev, pool, None)
v.vkDestroyPipelineLayout(dev, pl, None)
v.vkDestroyDescriptorSetLayout(dev, layout, None)
for buf, allocation, mapped in [src, dst]:
    v.vkUnmapMemory(dev, allocation)
    v.vkDestroyBuffer(dev, buf, None)
    v.vkFreeMemory(dev, allocation, None)
v.vkDestroyDevice(dev, None)
v.vkDestroyInstance(inst, None)
temporary.cleanup()
