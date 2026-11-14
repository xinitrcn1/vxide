/* Vulkan backend. One render pass, one pipeline, one instanced draw. */
#define _POSIX_C_SOURCE 200809L
#include "gpu.h"

#include <stdio.h>
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "ui_vert.h"
#include "ui_frag.h"

#define FRAMES 2
#define MAX_QUADS (1u << 18)
#define MAX_IMAGES 8

typedef struct {
	VkCommandBuffer cmd;
	VkFence fence;
	VkSemaphore acquired;
	VkBuffer inst;
	VkDeviceMemory inst_mem;
	Quad *inst_map;
} Frame;

static struct {
	SDL_Window *win;
	bool headless;
	int w, h;

	VkInstance inst;
	VkSurfaceKHR surf;
	VkPhysicalDevice pdev;
	VkPhysicalDeviceMemoryProperties memp;
	char dev_name[256];
	VkDevice dev;
	u32 qfam;
	VkQueue q;
	VkCommandPool pool;

	VkFormat fmt;
	VkSwapchainKHR swap;
	u32 nimg;
	VkImage img[MAX_IMAGES];
	VkImageView view[MAX_IMAGES];
	VkFramebuffer fb[MAX_IMAGES];
	VkSemaphore rendered[MAX_IMAGES]; /* per swapchain image */
	VkDeviceMemory off_mem;           /* headless target */
	VkBuffer rb;                      /* headless readback */
	VkDeviceMemory rb_mem;
	void *rb_map;

	VkRenderPass rp;
	VkDescriptorSetLayout dsl;
	VkPipelineLayout pl;
	VkPipeline pipe;
	VkDescriptorPool dpool;
	VkDescriptorSet ds;
	VkSampler samp;

	VkImage atlas;
	VkDeviceMemory atlas_mem;
	VkImageView atlas_view;
	bool atlas_init;
	VkBuffer stage;
	VkDeviceMemory stage_mem;
	u8 *stage_map;
	const u8 *atlas_cpu;
	int dx0, dy0, dx1, dy1; /* dirty rect, empty when dx0 >= dx1 */

	Frame fr[FRAMES];
	u32 fi;
	bool need_recreate;
} G;

#define VK(x)                                                         \
	do {                                                              \
		VkResult r_ = (x);                                            \
		if (r_ != VK_SUCCESS)                                         \
			vx_die("%s:%d: %s = %d", __FILE__, __LINE__, #x, (int)r_); \
	} while (0)

static u32 mem_type(u32 bits, VkMemoryPropertyFlags want)
{
	for (u32 i = 0; i < G.memp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (G.memp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	vx_die("no memory type for flags %x", want);
}

static void make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *b,
                        VkDeviceMemory *m, void **map)
{
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = size;
	bi.usage = usage;
	VK(vkCreateBuffer(G.dev, &bi, NULL, b));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(G.dev, *b, &mr);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VK(vkAllocateMemory(G.dev, &ai, NULL, m));
	VK(vkBindBufferMemory(G.dev, *b, *m, 0));
	VK(vkMapMemory(G.dev, *m, 0, VK_WHOLE_SIZE, 0, map));
}

static void make_image(u32 w, u32 h, VkFormat f, VkImageUsageFlags usage, VkImage *img,
                       VkDeviceMemory *m, VkImageView *view)
{
	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = f;
	ii.extent = (VkExtent3D){w, h, 1};
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = usage;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK(vkCreateImage(G.dev, &ii, NULL, img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(G.dev, *img, &mr);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK(vkAllocateMemory(G.dev, &ai, NULL, m));
	VK(vkBindImageMemory(G.dev, *img, *m, 0));
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = *img;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = f;
	vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VK(vkCreateImageView(G.dev, &vi, NULL, view));
}

static VkImageView make_view(VkImage img, VkFormat f)
{
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = img;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = f;
	vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageView v;
	VK(vkCreateImageView(G.dev, &vi, NULL, &v));
	return v;
}

static void pick_device(void)
{
	u32 n = 0;
	VK(vkEnumeratePhysicalDevices(G.inst, &n, NULL));
	if (!n)
		vx_die("no Vulkan devices");
	VkPhysicalDevice *d = vx_alloc(n * sizeof *d);
	VK(vkEnumeratePhysicalDevices(G.inst, &n, d));
	int best = -1, best_score = -1;
	u32 best_q = 0;
	const char *want = getenv("VXIDE_GPU");
	for (u32 i = 0; i < n; i++) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(d[i], &p);
		u32 nq = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(d[i], &nq, NULL);
		VkQueueFamilyProperties qp[16];
		nq = VX_MIN(nq, 16u);
		vkGetPhysicalDeviceQueueFamilyProperties(d[i], &nq, qp);
		for (u32 k = 0; k < nq; k++) {
			if (!(qp[k].queueFlags & VK_QUEUE_GRAPHICS_BIT))
				continue;
			if (G.surf) {
				VkBool32 ok = 0;
				vkGetPhysicalDeviceSurfaceSupportKHR(d[i], k, G.surf, &ok);
				if (!ok)
					continue;
			}
			int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
			            : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
			                                                                     : 1;
			if (want && strstr(p.deviceName, want))
				score += 10;
			if (score > best_score) {
				best = (int)i;
				best_score = score;
				best_q = k;
			}
			break;
		}
	}
	if (best < 0)
		vx_die("no Vulkan device can draw here");
	G.pdev = d[best];
	G.qfam = best_q;
	free(d);
	VkPhysicalDeviceProperties p;
	vkGetPhysicalDeviceProperties(G.pdev, &p);
	snprintf(G.dev_name, sizeof G.dev_name, "%s", p.deviceName);
	vkGetPhysicalDeviceMemoryProperties(G.pdev, &G.memp);
}

static void make_device(void)
{
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qi.queueFamilyIndex = G.qfam;
	qi.queueCount = 1;
	qi.pQueuePriorities = &prio;
	const char *ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	di.queueCreateInfoCount = 1;
	di.pQueueCreateInfos = &qi;
	di.enabledExtensionCount = G.headless ? 0 : 1;
	di.ppEnabledExtensionNames = ext;
	VK(vkCreateDevice(G.pdev, &di, NULL, &G.dev));
	vkGetDeviceQueue(G.dev, G.qfam, 0, &G.q);
	VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pi.queueFamilyIndex = G.qfam;
	VK(vkCreateCommandPool(G.dev, &pi, NULL, &G.pool));
}

static void pick_format(void)
{
	if (G.headless) {
		G.fmt = VK_FORMAT_R8G8B8A8_UNORM;
		return;
	}
	u32 n = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(G.pdev, G.surf, &n, NULL);
	VkSurfaceFormatKHR f[64];
	n = VX_MIN(n, 64u);
	vkGetPhysicalDeviceSurfaceFormatsKHR(G.pdev, G.surf, &n, f);
	G.fmt = f[0].format;
	/* UNORM: blend in sRGB space, like the browser engine VSCodium uses. */
	for (u32 i = 0; i < n; i++)
		if (f[i].format == VK_FORMAT_B8G8R8A8_UNORM || f[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
			G.fmt = f[i].format;
			break;
		}
}

static void make_render_pass(void)
{
	VkAttachmentDescription a = {0};
	a.format = G.fmt;
	a.samples = VK_SAMPLE_COUNT_1_BIT;
	a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	a.finalLayout = G.headless ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sp = {0};
	sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sp.colorAttachmentCount = 1;
	sp.pColorAttachments = &ref;
	VkSubpassDependency dep = {0};
	dep.srcSubpass = VK_SUBPASS_EXTERNAL;
	dep.dstSubpass = 0;
	dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	VkRenderPassCreateInfo ri = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	ri.attachmentCount = 1;
	ri.pAttachments = &a;
	ri.subpassCount = 1;
	ri.pSubpasses = &sp;
	ri.dependencyCount = 1;
	ri.pDependencies = &dep;
	VK(vkCreateRenderPass(G.dev, &ri, NULL, &G.rp));
}

static VkShaderModule shader(const u32 *code, size_t size)
{
	VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	si.codeSize = size;
	si.pCode = code;
	VkShaderModule m;
	VK(vkCreateShaderModule(G.dev, &si, NULL, &m));
	return m;
}

static void make_pipeline(void)
{
	VkDescriptorSetLayoutBinding b = {0};
	b.binding = 0;
	b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	b.descriptorCount = 1;
	b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dli.bindingCount = 1;
	dli.pBindings = &b;
	VK(vkCreateDescriptorSetLayout(G.dev, &dli, NULL, &G.dsl));

	VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
	VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &G.dsl;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pcr;
	VK(vkCreatePipelineLayout(G.dev, &pli, NULL, &G.pl));

	VkShaderModule vs = shader(ui_vert_spv, sizeof ui_vert_spv);
	VkShaderModule fs = shader(ui_frag_spv, sizeof ui_frag_spv);
	VkPipelineShaderStageCreateInfo st[2] = {
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL},
	};
	VkVertexInputBindingDescription vb = {0, sizeof(Quad), VK_VERTEX_INPUT_RATE_INSTANCE};
	VkVertexInputAttributeDescription va[3] = {
	    {0, 0, VK_FORMAT_R16G16B16A16_SINT, offsetof(Quad, x)},
	    {1, 0, VK_FORMAT_R16G16_UINT, offsetof(Quad, u)},
	    {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Quad, rgba)},
	};
	VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &vb;
	vi.vertexAttributeDescriptionCount = 3;
	vi.pVertexAttributeDescriptions = va;
	VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vps.viewportCount = 1;
	vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState ba = {0};
	ba.blendEnable = VK_TRUE;
	ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	ba.colorBlendOp = VK_BLEND_OP_ADD;
	ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	ba.alphaBlendOp = VK_BLEND_OP_ADD;
	ba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &ba;
	VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gi.stageCount = 2;
	gi.pStages = st;
	gi.pVertexInputState = &vi;
	gi.pInputAssemblyState = &ia;
	gi.pViewportState = &vps;
	gi.pRasterizationState = &rs;
	gi.pMultisampleState = &ms;
	gi.pColorBlendState = &cb;
	gi.pDynamicState = &ds;
	gi.layout = G.pl;
	gi.renderPass = G.rp;
	VK(vkCreateGraphicsPipelines(G.dev, VK_NULL_HANDLE, 1, &gi, NULL, &G.pipe));
	vkDestroyShaderModule(G.dev, vs, NULL);
	vkDestroyShaderModule(G.dev, fs, NULL);

	VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK(vkCreateSampler(G.dev, &sci, NULL, &G.samp));

	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
	VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpi.maxSets = 1;
	dpi.poolSizeCount = 1;
	dpi.pPoolSizes = &ps;
	VK(vkCreateDescriptorPool(G.dev, &dpi, NULL, &G.dpool));
	VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = G.dpool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &G.dsl;
	VK(vkAllocateDescriptorSets(G.dev, &dai, &G.ds));
}

static void make_atlas(void)
{
	make_image(ATLAS_SIZE, ATLAS_SIZE, VK_FORMAT_R8_UNORM,
	           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &G.atlas, &G.atlas_mem,
	           &G.atlas_view);
	void *map;
	make_buffer((VkDeviceSize)ATLAS_SIZE * ATLAS_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &G.stage,
	            &G.stage_mem, &map);
	G.stage_map = map;
	VkDescriptorImageInfo ii = {G.samp, G.atlas_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = G.ds;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = &ii;
	vkUpdateDescriptorSets(G.dev, 1, &w, 0, NULL);
	G.dx0 = 0;
	G.dy0 = 0;
	G.dx1 = ATLAS_SIZE;
	G.dy1 = ATLAS_SIZE;
}

static void make_frames(void)
{
	VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = G.pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	for (int i = 0; i < FRAMES; i++) {
		Frame *f = &G.fr[i];
		VK(vkAllocateCommandBuffers(G.dev, &ai, &f->cmd));
		VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		VK(vkCreateFence(G.dev, &fi, NULL, &f->fence));
		VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VK(vkCreateSemaphore(G.dev, &si, NULL, &f->acquired));
		void *map;
		make_buffer((VkDeviceSize)MAX_QUADS * sizeof(Quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		            &f->inst, &f->inst_mem, &map);
		f->inst_map = map;
	}
}

static void destroy_target(void)
{
	for (u32 i = 0; i < G.nimg; i++) {
		vkDestroyFramebuffer(G.dev, G.fb[i], NULL);
		vkDestroyImageView(G.dev, G.view[i], NULL);
		vkDestroySemaphore(G.dev, G.rendered[i], NULL);
		if (G.headless)
			vkDestroyImage(G.dev, G.img[i], NULL);
	}
	if (G.headless) {
		vkFreeMemory(G.dev, G.off_mem, NULL);
		if (G.rb) {
			vkDestroyBuffer(G.dev, G.rb, NULL);
			vkFreeMemory(G.dev, G.rb_mem, NULL);
			G.rb = VK_NULL_HANDLE;
		}
	}
	G.nimg = 0;
}

static void make_target(void)
{
	if (G.headless) {
		G.nimg = 1;
		make_image((u32)G.w, (u32)G.h, G.fmt,
		           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &G.img[0],
		           &G.off_mem, &G.view[0]);
		make_buffer((VkDeviceSize)G.w * G.h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &G.rb, &G.rb_mem,
		            &G.rb_map);
	} else {
		VkSurfaceCapabilitiesKHR caps;
		VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(G.pdev, G.surf, &caps));
		VkExtent2D ext = caps.currentExtent;
		if (ext.width == 0xFFFFFFFFu) {
			ext.width = (u32)G.w;
			ext.height = (u32)G.h;
		}
		if (ext.width == 0 || ext.height == 0)
			return; /* minimized */
		G.w = (int)ext.width;
		G.h = (int)ext.height;
		u32 want = VX_MAX(caps.minImageCount + 1, 2u);
		if (caps.maxImageCount && want > caps.maxImageCount)
			want = caps.maxImageCount;
		VkSwapchainCreateInfoKHR si = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
		si.surface = G.surf;
		si.minImageCount = want;
		si.imageFormat = G.fmt;
		si.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		si.imageExtent = ext;
		si.imageArrayLayers = 1;
		si.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		si.preTransform = caps.currentTransform;
		si.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		si.presentMode = VK_PRESENT_MODE_FIFO_KHR;
		si.clipped = VK_TRUE;
		VkSwapchainKHR old = G.swap;
		si.oldSwapchain = old;
		VK(vkCreateSwapchainKHR(G.dev, &si, NULL, &G.swap));
		if (old)
			vkDestroySwapchainKHR(G.dev, old, NULL);
		G.nimg = MAX_IMAGES;
		VK(vkGetSwapchainImagesKHR(G.dev, G.swap, &G.nimg, G.img));
		for (u32 i = 0; i < G.nimg; i++)
			G.view[i] = make_view(G.img[i], G.fmt);
	}
	for (u32 i = 0; i < G.nimg; i++) {
		VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fi.renderPass = G.rp;
		fi.attachmentCount = 1;
		fi.pAttachments = &G.view[i];
		fi.width = (u32)G.w;
		fi.height = (u32)G.h;
		fi.layers = 1;
		VK(vkCreateFramebuffer(G.dev, &fi, NULL, &G.fb[i]));
		VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VK(vkCreateSemaphore(G.dev, &si, NULL, &G.rendered[i]));
	}
}

static void recreate(void)
{
	vkDeviceWaitIdle(G.dev);
	destroy_target();
	make_target();
	G.need_recreate = false;
}

bool gpu_init(SDL_Window *win, int w, int h)
{
	memset(&G, 0, sizeof G);
	G.win = win;
	G.headless = win == NULL;
	G.w = w;
	G.h = h;

	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "vxide";
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ii.pApplicationInfo = &app;
	Uint32 next = 0;
	const char *const *ext = NULL;
	if (win)
		ext = SDL_Vulkan_GetInstanceExtensions(&next);
	ii.enabledExtensionCount = next;
	ii.ppEnabledExtensionNames = ext;
	const char *layer = "VK_LAYER_KHRONOS_validation";
	if (getenv("VXIDE_VALIDATE")) {
		ii.enabledLayerCount = 1;
		ii.ppEnabledLayerNames = &layer;
	}
	if (vkCreateInstance(&ii, NULL, &G.inst) != VK_SUCCESS)
		return false;
	if (win && !SDL_Vulkan_CreateSurface(win, G.inst, NULL, &G.surf)) {
		fprintf(stderr, "vxide: SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
		return false;
	}
	pick_device();
	make_device();
	pick_format();
	make_render_pass();
	make_pipeline();
	make_atlas();
	make_frames();
	make_target();
	return true;
}

void gpu_shutdown(void)
{
	if (!G.dev)
		return;
	vkDeviceWaitIdle(G.dev);
	destroy_target();
	if (G.swap)
		vkDestroySwapchainKHR(G.dev, G.swap, NULL);
	for (int i = 0; i < FRAMES; i++) {
		Frame *f = &G.fr[i];
		vkDestroyBuffer(G.dev, f->inst, NULL);
		vkFreeMemory(G.dev, f->inst_mem, NULL);
		vkDestroyFence(G.dev, f->fence, NULL);
		vkDestroySemaphore(G.dev, f->acquired, NULL);
	}
	vkDestroyBuffer(G.dev, G.stage, NULL);
	vkFreeMemory(G.dev, G.stage_mem, NULL);
	vkDestroyImageView(G.dev, G.atlas_view, NULL);
	vkDestroyImage(G.dev, G.atlas, NULL);
	vkFreeMemory(G.dev, G.atlas_mem, NULL);
	vkDestroySampler(G.dev, G.samp, NULL);
	vkDestroyDescriptorPool(G.dev, G.dpool, NULL);
	vkDestroyPipeline(G.dev, G.pipe, NULL);
	vkDestroyPipelineLayout(G.dev, G.pl, NULL);
	vkDestroyDescriptorSetLayout(G.dev, G.dsl, NULL);
	vkDestroyRenderPass(G.dev, G.rp, NULL);
	vkDestroyCommandPool(G.dev, G.pool, NULL);
	vkDestroyDevice(G.dev, NULL);
	if (G.surf)
		vkDestroySurfaceKHR(G.inst, G.surf, NULL);
	vkDestroyInstance(G.inst, NULL);
	memset(&G, 0, sizeof G);
}

void gpu_resize(int w, int h)
{
	if (w == G.w && h == G.h && !G.need_recreate)
		return;
	G.w = w;
	G.h = h;
	recreate();
}

void gpu_set_atlas(const u8 *pixels) { G.atlas_cpu = pixels; }

void gpu_atlas_dirty(int x, int y, int w, int h)
{
	if (G.dx0 >= G.dx1) {
		G.dx0 = x;
		G.dy0 = y;
		G.dx1 = x + w;
		G.dy1 = y + h;
		return;
	}
	G.dx0 = VX_MIN(G.dx0, x);
	G.dy0 = VX_MIN(G.dy0, y);
	G.dx1 = VX_MAX(G.dx1, x + w);
	G.dy1 = VX_MAX(G.dy1, y + h);
}

static void barrier(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss,
                    VkPipelineStageFlags dst)
{
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = sa;
	b.dstAccessMask = da;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img;
	b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(c, ss, dst, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Upload the dirty atlas rect. The staging buffer is shared by both
 * frames in flight, so wait for the other frame first (rare: glyph adds). */
static void upload_atlas(VkCommandBuffer c)
{
	if (!G.atlas_cpu || G.dx0 >= G.dx1)
		return;
	Frame *other = &G.fr[(G.fi + 1) % FRAMES];
	vkWaitForFences(G.dev, 1, &other->fence, VK_TRUE, UINT64_MAX);
	int x0 = VX_MAX(G.dx0, 0), y0 = VX_MAX(G.dy0, 0);
	int x1 = VX_MIN(G.dx1, ATLAS_SIZE), y1 = VX_MIN(G.dy1, ATLAS_SIZE);
	int w = x1 - x0, h = y1 - y0;
	for (int y = 0; y < h; y++)
		memcpy(G.stage_map + (size_t)y * w, G.atlas_cpu + (size_t)(y0 + y) * ATLAS_SIZE + x0,
		       (size_t)w);
	barrier(c, G.atlas,
	        G.atlas_init ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
	        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	        VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkBufferImageCopy r = {0};
	r.bufferRowLength = (u32)w;
	r.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	r.imageOffset = (VkOffset3D){x0, y0, 0};
	r.imageExtent = (VkExtent3D){(u32)w, (u32)h, 1};
	if (!G.atlas_init) {
		/* First upload: whole atlas so no texel is undefined. */
		memcpy(G.stage_map, G.atlas_cpu, (size_t)ATLAS_SIZE * ATLAS_SIZE);
		r.bufferRowLength = ATLAS_SIZE;
		r.imageOffset = (VkOffset3D){0, 0, 0};
		r.imageExtent = (VkExtent3D){ATLAS_SIZE, ATLAS_SIZE, 1};
	}
	vkCmdCopyBufferToImage(c, G.stage, G.atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	barrier(c, G.atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
	        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	G.atlas_init = true;
	G.dx0 = G.dx1 = 0;
}

bool gpu_frame(const Quad *q, u32 n, u32 clear)
{
	if (G.need_recreate || G.nimg == 0) {
		recreate();
		if (G.nimg == 0)
			return false;
	}
	Frame *f = &G.fr[G.fi];
	VK(vkWaitForFences(G.dev, 1, &f->fence, VK_TRUE, UINT64_MAX));

	u32 idx = 0;
	if (!G.headless) {
		VkResult r = vkAcquireNextImageKHR(G.dev, G.swap, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &idx);
		if (r == VK_ERROR_OUT_OF_DATE_KHR) {
			G.need_recreate = true;
			return false;
		}
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
			vx_die("vkAcquireNextImageKHR = %d", (int)r);
	}
	VK(vkResetFences(G.dev, 1, &f->fence));

	if (n > MAX_QUADS)
		n = MAX_QUADS;
	memcpy(f->inst_map, q, (size_t)n * sizeof(Quad));

	VkCommandBuffer c = f->cmd;
	VK(vkResetCommandBuffer(c, 0));
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK(vkBeginCommandBuffer(c, &bi));
	upload_atlas(c);

	VkClearValue cv;
	cv.color.float32[0] = (float)((clear >> 24) & 255) / 255.0f;
	cv.color.float32[1] = (float)((clear >> 16) & 255) / 255.0f;
	cv.color.float32[2] = (float)((clear >> 8) & 255) / 255.0f;
	cv.color.float32[3] = 1.0f;
	VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	rb.renderPass = G.rp;
	rb.framebuffer = G.fb[idx];
	rb.renderArea.extent = (VkExtent2D){(u32)G.w, (u32)G.h};
	rb.clearValueCount = 1;
	rb.pClearValues = &cv;
	vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE);
	if (n && G.atlas_init) {
		VkViewport vp = {0, 0, (float)G.w, (float)G.h, 0, 1};
		VkRect2D sc = {{0, 0}, {(u32)G.w, (u32)G.h}};
		vkCmdSetViewport(c, 0, 1, &vp);
		vkCmdSetScissor(c, 0, 1, &sc);
		vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pipe);
		vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pl, 0, 1, &G.ds, 0, NULL);
		float pc[2] = {2.0f / (float)G.w, 2.0f / (float)G.h};
		vkCmdPushConstants(c, G.pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
		VkDeviceSize off = 0;
		vkCmdBindVertexBuffers(c, 0, 1, &f->inst, &off);
		vkCmdDraw(c, 6, n, 0, 0);
	}
	vkCmdEndRenderPass(c);
	if (G.headless) {
		VkBufferImageCopy r = {0};
		r.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		r.imageExtent = (VkExtent3D){(u32)G.w, (u32)G.h, 1};
		vkCmdCopyImageToBuffer(c, G.img[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, G.rb, 1, &r);
	}
	VK(vkEndCommandBuffer(c));

	VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &c;
	if (!G.headless) {
		si.waitSemaphoreCount = 1;
		si.pWaitSemaphores = &f->acquired;
		si.pWaitDstStageMask = &wait;
		si.signalSemaphoreCount = 1;
		si.pSignalSemaphores = &G.rendered[idx];
	}
	VK(vkQueueSubmit(G.q, 1, &si, f->fence));
	if (!G.headless) {
		VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &G.rendered[idx];
		pi.swapchainCount = 1;
		pi.pSwapchains = &G.swap;
		pi.pImageIndices = &idx;
		VkResult r = vkQueuePresentKHR(G.q, &pi);
		if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
			G.need_recreate = true;
		else if (r != VK_SUCCESS)
			vx_die("vkQueuePresentKHR = %d", (int)r);
	}
	G.fi = (G.fi + 1) % FRAMES;
	return true;
}

bool gpu_readback(u8 *rgba, int w, int h)
{
	if (!G.headless || w != G.w || h != G.h)
		return false;
	vkDeviceWaitIdle(G.dev);
	memcpy(rgba, G.rb_map, (size_t)w * h * 4);
	return true;
}

const char *gpu_device_name(void) { return G.dev_name; }
