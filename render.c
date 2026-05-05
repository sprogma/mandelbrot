#include "render.h"
#include "mandelbrot.h"

#include "assert.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#include "common_defines.h"

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <sys/stat.h>

#ifdef _WIN32
#define STAT _stat
#else
#define STAT stat
#endif

int file_exists(const char *path) 
{
    struct STAT buffer;
    return (STAT(path, &buffer) == 0);
}


struct push_constant_parameter
{
    float zoom_m;
    int zoom_e;
    float time;
    uint32_t anchor_points;
    uint32_t path_length;
    float center[2];
};

#define MAX_FRAMES_IN_FLIGHT 3
#define ENCODE_QUEUE_SIZE 3

struct encode_job 
{
    AVFrame *frame;
    int64_t pts;
};

int encoder_worker(void *ptr);



struct render
{
    struct render_config config;

/* sdl info */
    SDL_Window *window;

/* devices */
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    int compute_family_index;

/* swapchains and buffers */
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat swapchain_format;
    VkExtent2D swapchain_extent;
    uint32_t image_count;
    VkImage *swapchain_images;
    VkImageView *swapchain_image_views;

    VkImage intermediate_image;
    VkDeviceMemory intermediate_memory;
    VkImageView intermediate_view;

    bool use_intermediate;
    
    VkDeviceMemory *render_images_memory;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet *descriptor_sets;

/* buffers */
    VkBuffer download_buffers[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory download_memories[MAX_FRAMES_IN_FLIGHT];
    void *download_memory_mappings[MAX_FRAMES_IN_FLIGHT];
    VkCommandBuffer cmd_buffers[MAX_FRAMES_IN_FLIGHT];
    VkFence in_flight_fences[MAX_FRAMES_IN_FLIGHT];
    uint32_t current_frame;
    
    
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;

    VkBuffer device_buffer;
    VkDeviceMemory device_memory;

/* render pipeline */
    VkCommandPool command_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkPipelineLayout pipeline_layout;
    VkPipeline compute_pipeline;

/* syncronization */
    VkFence render_fence;
    VkSemaphore image_available_sem;
    VkSemaphore render_finished_sem;

/* video encoding */
    AVCodecContext *enc_ctx;
    AVFormatContext *fmt_ctx;
    AVStream *video_st;
    AVFrame *frame;
    AVPacket *pkt;
    struct SwsContext *sws_ctx;
    int64_t frame_count;

    AVBufferRef *hw_device_ctx;
    AVBufferRef *hw_frames_ctx;
    VkSemaphore render_sem;

    struct encode_job encode_queue[ENCODE_QUEUE_SIZE];
    int encode_queue_read;
    int encode_queue_write;
    SDL_Mutex *encode_mutex;
    SDL_Condition *encode_cond;
    SDL_Thread *encode_thread;
    bool encode_running;
};


const char* deviceExtensionsForWindow[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

const char* deviceExtensionsForRender[] = {
};

#define length(x) (sizeof(x)/sizeof(*x))


int init_window(struct render *r)
{
    if (SDL_Init(SDL_INIT_VIDEO) == 0) 
    {
        printf("SDL initialization failed: %s\n", SDL_GetError());
        free(r);
        return 1;
    }

    if (!r->config.output_filename) 
    {
        r->window = SDL_CreateWindow(
            "Brandelbrot",
            r->config.w, r->config.h,
            SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY
        );
        if (!r->window) 
        {
            printf("Window creation failed: %s\n", SDL_GetError());
            free(r);
            return 1;
        }
    }   

    return 0;
}

int init_instance(struct render *r)
{
    uint32_t count_instance_extensions;
    const char * const *instance_extensions = SDL_Vulkan_GetInstanceExtensions(&count_instance_extensions);

    if (instance_extensions == NULL) 
    {
        printf("Can't get extensions from sdl: %s\n", SDL_GetError());
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }

    uint32_t count_extensions = count_instance_extensions + 1;
    const char **extensions = malloc(count_extensions * sizeof(const char *));
    extensions[0] = VK_EXT_DEBUG_REPORT_EXTENSION_NAME;
    memcpy(&extensions[1], instance_extensions, count_instance_extensions * sizeof(const char*)); 
    

    if (extensions == NULL)
    {
        printf("Can't get extensions from sdl: %s\n", SDL_GetError());
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }

    printf("SDL need %u extensions:\n", count_extensions);
    for (uint32_t i = 0; i < count_extensions; i++) 
    {
        printf(" - %s\n", extensions[i]);
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "ComputeApp";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "NoEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    
    createInfo.enabledExtensionCount = count_extensions;
    createInfo.ppEnabledExtensionNames = extensions;

    createInfo.enabledLayerCount = 0;
    createInfo.ppEnabledLayerNames = NULL;

    VkResult result = vkCreateInstance(&createInfo, NULL, &r->instance);

    free((void *)extensions);

    if (result != VK_SUCCESS) 
    {
        printf("Can't create instance: %d\n", result);
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }

    printf("Instance created\n");
    return 0;
}


uint32_t findComputeQueueFamily(struct render *r, VkPhysicalDevice physDevice) 
{
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, NULL);

    VkQueueFamilyProperties* queueFamilies = malloc(sizeof(VkQueueFamilyProperties) * queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, queueFamilies);

    for (uint32_t i = 0; i < queueFamilyCount; i++) 
    {
        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
        {
            if (r->config.output_filename)
            {
                free(queueFamilies);
                return i;
            }
            else if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                VkBool32 presentSupport = false;
                vkGetPhysicalDeviceSurfaceSupportKHR(physDevice, i, r->surface, &presentSupport);
                if (presentSupport) {
                    free(queueFamilies);
                    return i;
                }
            }
        }
    }

    free(queueFamilies);
    return UINT32_MAX;
}

const char *checkDevice(struct render *r, VkPhysicalDevice device)
{
    VkPhysicalDeviceFeatures supportedFeatures;
    vkGetPhysicalDeviceFeatures(device, &supportedFeatures);

    if (r->config.use_float64)
    {
        if (!supportedFeatures.shaderFloat64) 
        {
            return "float64 not supported";
        }
    }
            
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(device, NULL, &extensionCount, NULL);
    VkExtensionProperties* availableExtensions = malloc(sizeof(VkExtensionProperties) * extensionCount);
    vkEnumerateDeviceExtensionProperties(device, NULL, &extensionCount, availableExtensions);

    const char **deviceExtensions = (r->config.output_filename ? deviceExtensionsForRender : deviceExtensionsForWindow);
    uint32_t extLength = (r->config.output_filename ? length(deviceExtensionsForRender) : length(deviceExtensionsForWindow));
    
    for (uint32_t e = 0; e < extLength; ++e)
    {
        bool externsionSupported = false;
        for (uint32_t j = 0; j < extensionCount; j++) 
        {
            if (strcmp(deviceExtensions[e], availableExtensions[j].extensionName) == 0) 
            {
                externsionSupported = true;
                break;
            }
        }
        if (!externsionSupported) 
        {
            free(availableExtensions);
            return deviceExtensions[e];
        }
    }
    
    free(availableExtensions);
    return NULL;
}

int find_device(struct render *r)
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(r->instance, &deviceCount, NULL);

    if (deviceCount == 0) 
    {
        printf("No processing devices supporting vulkan found.\n");
        return 1;
    }

    VkPhysicalDevice* devices = malloc(sizeof(VkPhysicalDevice) * deviceCount);
    vkEnumeratePhysicalDevices(r->instance, &deviceCount, devices);

    uint32_t default_device;
    bool default_device_is_gpu = false;

    for (uint32_t i = 0; i < deviceCount; i++) 
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);

        printf("[%u] Device: %s\n", i, props.deviceName);
        
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) 
        {
            printf("    Discrete gpu\n");
        } 
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) 
        {
            printf("    Intergated gpu\n");
        }

        const char *notSupport = checkDevice(r, devices[i]);
        if (notSupport != NULL)
        {
            printf("    Device doesn't support extension %s.\n", notSupport);
            continue;
        }

        if (findComputeQueueFamily(r, devices[i]) != UINT32_MAX)
        {
            printf("    Processing supported.\n");
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && !default_device_is_gpu)
            {
                default_device = i;
                default_device_is_gpu = true;
            }
            else if (!default_device_is_gpu)
            {
                default_device = i;
            }
        }
        else
        {
            printf("    Device doesn't support needed queue family.\n");
        }
    }

    uint32_t selected;

    if (r->config.device_id == -1)
    {
        printf("Selecting default device: %u\n", default_device);
        selected = default_device;
    }
    else
    {
        printf("Trying to use selected device: %d\n", r->config.device_id);
        /* check that gpu */
        if (r->config.device_id < 0 || (uint32_t)r->config.device_id >= deviceCount)
        {
            printf("Wrong device id: %d\n", r->config.device_id);
            SDL_DestroyWindow(r->window);
            SDL_Quit();
            free(r);
            return 1;
        }


        const char *notSupport = checkDevice(r, devices[r->config.device_id]);
        if (notSupport != NULL)
        {
            printf("Error: Device %d doesn't support extension %s.\n", r->config.device_id, notSupport);
            SDL_DestroyWindow(r->window);
            SDL_Quit();
            free(r);
            return 1;
        }

        
        if (findComputeQueueFamily(r, devices[r->config.device_id]) == UINT32_MAX)
        {
            printf("Error: Device %d doesn't support compute or rendering queues\n", r->config.device_id);
            SDL_DestroyWindow(r->window);
            SDL_Quit();
            free(r);
            return 1;
        }
        
        selected = r->config.device_id;
    }

    r->physical_device = devices[selected];

    return 0;
}


int init_device_and_queue(struct render *r)
{
    uint32_t computeFamilyIndex = findComputeQueueFamily(r, r->physical_device);
    if (computeFamilyIndex == UINT32_MAX)
    {
        printf("Can't find compute family on that device.\n");
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }

    float queuePriority = 1.0f;

    VkDeviceQueueCreateInfo queueCreateInfo = {};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = computeFamilyIndex;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;


    VkDeviceCreateInfo deviceCreateInfo = {};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;

    if (r->config.output_filename)
    {
        deviceCreateInfo.enabledExtensionCount = length(deviceExtensionsForRender);
        deviceCreateInfo.ppEnabledExtensionNames = deviceExtensionsForRender;
    }
    else
    {
        deviceCreateInfo.enabledExtensionCount = length(deviceExtensionsForWindow);
        deviceCreateInfo.ppEnabledExtensionNames = deviceExtensionsForWindow;
    }
    
    VkPhysicalDeviceFeatures deviceFeatures = {};
    if (r->config.use_float64)
    {
        deviceFeatures.shaderFloat64 = VK_TRUE;
    }
    deviceCreateInfo.pEnabledFeatures = &deviceFeatures;

    VkResult result = vkCreateDevice(r->physical_device, &deviceCreateInfo, NULL, &r->device);
    if (result != VK_SUCCESS) 
    {
        printf("Can't create device from physical device. Error %d\n", result);
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }

    vkGetDeviceQueue(r->device, computeFamilyIndex, 0, &r->queue);

    r->compute_family_index = computeFamilyIndex;

    printf("Device and Queue created.\n");
    return 0;
}

int init_surface(struct render *r)
{
    if (r->config.output_filename) return 0;

    if (!SDL_Vulkan_CreateSurface(r->window, r->instance, NULL, &r->surface)) 
    {
        printf("Failed to create surface: %s\n", SDL_GetError());
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }
    return 0;
}

uint32_t find_memory_type(struct render *r, uint32_t typeFilter, VkMemoryPropertyFlags properties) 
{
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(r->physical_device, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) 
    {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) 
        {
            return i;
        }
    }
    return UINT32_MAX;
}

int init_swapchain(struct render *r) 
{
    if (r->config.output_filename)
    {
        r->image_count = 0;        
        r->use_intermediate = true;
        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = r->config.w;
        imageInfo.extent.height = r->config.h;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        
        imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM; 
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        if (vkCreateImage(r->device, &imageInfo, NULL, &r->intermediate_image) != VK_SUCCESS) {
            printf("Failed to create intermediate image\n");
            return 1;
        }

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(r->device, r->intermediate_image, &memReqs);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        
        allocInfo.memoryTypeIndex = find_memory_type(r, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(r->device, &allocInfo, NULL, &r->intermediate_memory) != VK_SUCCESS) {
            printf("Failed to allocate image memory\n");
            return 1;
        }

        vkBindImageMemory(r->device, r->intermediate_image, r->intermediate_memory, 0);

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = r->intermediate_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageInfo.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(r->device, &viewInfo, NULL, &r->intermediate_view) != VK_SUCCESS) {
            printf("Failed to create image view\n");
            return 1;
        }

        return 0;
    }

    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->physical_device, r->surface, &capabilities);

    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical_device, r->surface, &formatCount, NULL);
    VkSurfaceFormatKHR *formats = malloc(sizeof(VkSurfaceFormatKHR) * formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical_device, r->surface, &formatCount, formats);

    VkSurfaceFormatKHR selectedFormat = formats[0];
    for (uint32_t i = 0; i < formatCount; i++) 
    {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) 
        {
            selectedFormat = formats[i];
            break;
        }
    }
    r->swapchain_format = selectedFormat.format;
    free(formats);


    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(r->physical_device, r->swapchain_format, &props);
    
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) 
    {
        printf("Device supports writing to swapchain.\n");
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    } 
    else 
    {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        printf("Waring: Device doesn't supports direct writing to swapchain. [performace won't be optimal]\n");

        r->use_intermediate = true;
    }


    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = r->surface;
    createInfo.minImageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && createInfo.minImageCount > capabilities.maxImageCount)
    {
        createInfo.minImageCount = capabilities.maxImageCount;
    }

    createInfo.imageFormat = r->swapchain_format;
    createInfo.imageColorSpace = selectedFormat.colorSpace;
    createInfo.imageExtent = capabilities.currentExtent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = usage; 

    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; 
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.clipped = VK_TRUE;

    VkResult result;
    if ((result = vkCreateSwapchainKHR(r->device, &createInfo, NULL, &r->swapchain)) != VK_SUCCESS) 
    {
        printf("Can't create swapchain: %d\n", result);
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }

    r->swapchain_extent = capabilities.currentExtent;

    vkGetSwapchainImagesKHR(r->device, r->swapchain, &r->image_count, NULL);
    r->swapchain_images = malloc(sizeof(VkImage) * r->image_count);
    vkGetSwapchainImagesKHR(r->device, r->swapchain, &r->image_count, r->swapchain_images);

    r->swapchain_image_views = malloc(sizeof(VkImageView) * r->image_count);
    for (uint32_t i = 0; i < r->image_count; i++) 
    {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = r->swapchain_images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = r->swapchain_format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        result = vkCreateImageView(r->device, &viewInfo, NULL, &r->swapchain_image_views[i]);
        if (result != VK_SUCCESS)
        {
            printf("Can't create image view: %d\n", result);
            SDL_DestroyWindow(r->window);
            SDL_Quit();
            free(r);
            return 1;
        }
    }

    printf("Swapchain created.\n");
    return 0;
}

int init_intermediate_image(struct render *r) 
{
    if (r->config.output_filename || !r->use_intermediate) return 0;
    
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = r->config.w;
    imageInfo.extent.height = r->config.h;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = r->swapchain_format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    vkCreateImage(r->device, &imageInfo, NULL, &r->intermediate_image);

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(r->device, r->intermediate_image, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = find_memory_type(r, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (allocInfo.memoryTypeIndex == UINT32_MAX || 
        vkAllocateMemory(r->device, &allocInfo, NULL, &r->intermediate_memory) != VK_SUCCESS)
    {
        printf("Can't allocate %llu bytes for image.\n", memReqs.size);
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }
    vkBindImageMemory(r->device, r->intermediate_image, r->intermediate_memory, 0);



    VkImageViewCreateInfo viewInfo = {0};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = r->intermediate_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = imageInfo.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(r->device, &viewInfo, NULL, &r->intermediate_view);
    
    return 0;
}

int init_commands(struct render *r) 
{
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = r->compute_family_index;

    if (vkCreateCommandPool(r->device, &poolInfo, NULL, &r->command_pool) != VK_SUCCESS) 
    {
        printf("Error: Failed to create command pool.\n");
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }
    
    printf("Command pool created.\n");
    return 0;
}


int init_render_targets(struct render *r) 
{
    if (!r->config.output_filename) return 0; 

    r->image_count = 1;
    r->current_frame = 0;
    r->swapchain_images = malloc(sizeof(VkImage) * r->image_count);
    r->swapchain_image_views = malloc(sizeof(VkImageView) * r->image_count);
    r->render_images_memory = malloc(sizeof(VkDeviceMemory) * r->image_count);

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = r->config.w;
    imageInfo.extent.height = r->config.h;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(r->device, &imageInfo, NULL, &r->swapchain_images[0]) != VK_SUCCESS)
    {
        printf("Failed to create new image.\n");
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(r->device, r->swapchain_images[0], &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = find_memory_type(r, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (allocInfo.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(r->device, &allocInfo, NULL, &r->render_images_memory[0]) != VK_SUCCESS)
    {
        printf("Can't allocate image of size %llu.\n", allocInfo.allocationSize);
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }
    vkBindImageMemory(r->device, r->swapchain_images[0], r->render_images_memory[0], 0);

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = r->swapchain_images[0];
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = imageInfo.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(r->device, &viewInfo, NULL, &r->swapchain_image_views[0]);

    return 0;
}



int create_buffers(struct render *r, VkDeviceSize size)
{
    VkBufferCreateInfo staging_info = {};
    staging_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_info.size = size;
    staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    staging_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(r->device, &staging_info, NULL, &r->staging_buffer) != VK_SUCCESS) 
    {
        printf("Failed to create staging buffer\n");
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }

    VkMemoryRequirements staging_reqs;
    vkGetBufferMemoryRequirements(r->device, r->staging_buffer, &staging_reqs);

    VkMemoryAllocateInfo staging_alloc = {};
    staging_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    staging_alloc.allocationSize = staging_reqs.size;
    staging_alloc.memoryTypeIndex = find_memory_type(r, staging_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (staging_alloc.memoryTypeIndex == UINT32_MAX || 
        vkAllocateMemory(r->device, &staging_alloc, NULL, &r->staging_memory) != VK_SUCCESS) 
    {
        printf("Failed to allocate %llu bytes of memory at host.\n", staging_reqs.size);
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }
    vkBindBufferMemory(r->device, r->staging_buffer, r->staging_memory, 0);



    VkBufferCreateInfo device_info = {};
    device_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    device_info.size = size;
    device_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    device_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(r->device, &device_info, NULL, &r->device_buffer) != VK_SUCCESS) 
    {
        printf("Failed to create device buffer\n");
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }

    VkMemoryRequirements device_reqs;
    vkGetBufferMemoryRequirements(r->device, r->device_buffer, &device_reqs);

    VkMemoryAllocateInfo device_alloc = {};
    device_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    device_alloc.allocationSize = device_reqs.size;
    device_alloc.memoryTypeIndex = find_memory_type(r, device_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (device_alloc.memoryTypeIndex == UINT32_MAX || 
        vkAllocateMemory(r->device, &device_alloc, NULL, &r->device_memory) != VK_SUCCESS) 
    {
        printf("Failed to allocate %llu bytes of memory at device.\n", device_reqs.size);
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }
    vkBindBufferMemory(r->device, r->device_buffer, r->device_memory, 0);

    printf("Buffers of size %llu bytes initializated\n", (uint64_t)size);
    return 0;
}

int init_pipeline_layout(struct render *r) 
{
    VkDescriptorSetLayoutBinding bindings[2] = {};

    // destination image
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // source buffer
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 2;
    layout_info.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(r->device, &layout_info, NULL, &r->descriptor_set_layout) != VK_SUCCESS) 
    {
        printf("Failed to create descriptor set layout.\n");
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }

    VkPushConstantRange push_constant = {};
    push_constant.offset = 0;
    push_constant.size = sizeof(struct push_constant_parameter);
    push_constant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkPipelineLayoutCreateInfo pipeline_layout_info = {};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &r->descriptor_set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_constant;

    if (vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL, &r->pipeline_layout) != VK_SUCCESS) 
    {
        printf("Failed to create pipeline layout.\n");
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }

    return 0;
}

static char* read_file(const char* filename, size_t* length) 
{
#ifdef _WIN32
    FILE *f;
    if (fopen_s(&f, filename, "rb") != 0) return NULL;
#else
    FILE* f = fopen(filename, "rb");
    if (!f) return NULL;
#endif
    fseek(f, 0, SEEK_END);
    *length = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buffer = malloc(*length);
    fread(buffer, 1, *length, f);
    fclose(f);
    return buffer;
}

VkShaderModule create_shader_module(VkDevice device, const char* filename) 
{
    size_t size;
    char* code = read_file(filename, &size);
    if (!code) return VK_NULL_HANDLE;

    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = size;
    createInfo.pCode = (uint32_t*)code;

    VkShaderModule shaderModule;
    vkCreateShaderModule(device, &createInfo, NULL, &shaderModule);
    free(code);
    return shaderModule;
}

int init_compute_pipeline(struct render *r, const char* shader_path) 
{
    VkShaderModule compute_module = create_shader_module(r->device, shader_path);
    if (compute_module == VK_NULL_HANDLE) 
    {
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }

    VkPipelineShaderStageCreateInfo stage_info = {};
    stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage_info.module = compute_module;
    stage_info.pName = "main";

    VkComputePipelineCreateInfo pipeline_info = {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage_info;
    pipeline_info.layout = r->pipeline_layout;

    if (vkCreateComputePipelines(r->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &r->compute_pipeline) != VK_SUCCESS) 
    {        
        SDL_DestroyWindow(r->window);
        SDL_Quit();
        free(r);
        return 1;
    }

    vkDestroyShaderModule(r->device, compute_module, NULL);
    return 0;
}


int init_download_buffers(struct render *r) 
{
    VkDeviceSize size = r->config.w * r->config.h * 4;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) 
    {
        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        vkCreateBuffer(r->device, &bufferInfo, NULL, &r->download_buffers[i]);

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(r->device, r->download_buffers[i], &memReqs);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = find_memory_type(r, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        vkAllocateMemory(r->device, &allocInfo, NULL, &r->download_memories[i]);
        vkBindBufferMemory(r->device, r->download_buffers[i], r->download_memories[i], 0);
        vkMapMemory(r->device, r->download_memories[i], 0, size, 0, &r->download_memory_mappings[i]);
    }
    return 0;
}


int init_descriptor_sets(struct render *r) 
{
    if (r->use_intermediate)
    {
        VkDescriptorPoolSize pool_sizes[2] = {};
        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pool_sizes[0].descriptorCount = 1;
        pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_sizes[1].descriptorCount = 1;

        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.poolSizeCount = 2;
        pool_info.pPoolSizes = pool_sizes;
        pool_info.maxSets = 1;

        vkCreateDescriptorPool(r->device, &pool_info, NULL, &r->descriptor_pool);

        r->descriptor_sets = malloc(sizeof(VkDescriptorSet) * 1);
        VkDescriptorSetLayout *layouts = malloc(sizeof(VkDescriptorSetLayout) * 1);
        for(uint32_t i=0; i<r->image_count; i++) layouts[i] = r->descriptor_set_layout;

        VkDescriptorSetAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = r->descriptor_pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = layouts;

        vkAllocateDescriptorSets(r->device, &alloc_info, r->descriptor_sets);
        free(layouts);


        VkDescriptorImageInfo image_info = {};
        image_info.imageView = r->intermediate_view;
        image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo buffer_info = {};
        buffer_info.buffer = r->device_buffer;
        buffer_info.offset = 0;
        buffer_info.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writes[2] = {};
        
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = r->descriptor_sets[0];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &image_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = r->descriptor_sets[0];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &buffer_info;

        vkUpdateDescriptorSets(r->device, 2, writes, 0, NULL);
    }
    else
    {
        VkDescriptorPoolSize pool_sizes[2] = {};
        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pool_sizes[0].descriptorCount = r->image_count;
        pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_sizes[1].descriptorCount = r->image_count;

        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.poolSizeCount = 2;
        pool_info.pPoolSizes = pool_sizes;
        pool_info.maxSets = r->image_count;

        vkCreateDescriptorPool(r->device, &pool_info, NULL, &r->descriptor_pool);

        r->descriptor_sets = malloc(sizeof(VkDescriptorSet) * r->image_count);
        VkDescriptorSetLayout *layouts = malloc(sizeof(VkDescriptorSetLayout) * r->image_count);
        for(uint32_t i=0; i<r->image_count; i++) layouts[i] = r->descriptor_set_layout;

        VkDescriptorSetAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = r->descriptor_pool;
        alloc_info.descriptorSetCount = r->image_count;
        alloc_info.pSetLayouts = layouts;

        vkAllocateDescriptorSets(r->device, &alloc_info, r->descriptor_sets);
        free(layouts);

        for (uint32_t i = 0; i < r->image_count; i++) 
        {
            VkDescriptorImageInfo image_info = {};
            image_info.imageView = r->swapchain_image_views[i];
            image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkDescriptorBufferInfo buffer_info = {};
            buffer_info.buffer = r->device_buffer;
            buffer_info.offset = 0;
            buffer_info.range = VK_WHOLE_SIZE;

            VkWriteDescriptorSet writes[2] = {};
            
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = r->descriptor_sets[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[0].descriptorCount = 1;
            writes[0].pImageInfo = &image_info;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = r->descriptor_sets[i];
            writes[1].dstBinding = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[1].descriptorCount = 1;
            writes[1].pBufferInfo = &buffer_info;

            vkUpdateDescriptorSets(r->device, 2, writes, 0, NULL);
        }
    }

    printf("Descriptors to write to images created\n");
    return 0;
}

int init_command_buffer(struct render *r)
{
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = r->command_pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    vkAllocateCommandBuffers(r->device, &allocInfo, r->cmd_buffers);
    return 0;
}

int init_syncs(struct render *r)
{
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(r->device, &fenceInfo, NULL, &r->render_fence);

    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) 
    {
        vkCreateFence(r->device, &fenceInfo, NULL, &r->in_flight_fences[i]);
    }

    VkSemaphoreCreateInfo semaphoreInfo = {};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(r->device, &semaphoreInfo, NULL, &r->image_available_sem);
    vkCreateSemaphore(r->device, &semaphoreInfo, NULL, &r->render_finished_sem);
    return 0;
}

int init_video(struct render *r)
{
    if (!r->config.output_filename) return 0;

    if (r->config.use_accelerated_encoding)
    {
        printf("Not supported.\n");
        return 1;
    }

    r->frame_count = 0;

    if (avformat_alloc_output_context2(&r->fmt_ctx, NULL, NULL, r->config.output_filename) < 0) 
    {
        printf("Can't create av context\n");
        return 1;
    }

    const AVCodec *codec = avcodec_find_encoder_by_name("libx264rgb");
    if (!codec)
    {
        printf("Can't find codec libx264rgb. Install it to your ffmpeg.\n");
        return 1;
    }

    r->video_st = avformat_new_stream(r->fmt_ctx, NULL);
    if (!r->video_st) return 1;

    r->enc_ctx = avcodec_alloc_context3(codec);
    if (!r->enc_ctx) return 1;

    r->enc_ctx->width = r->config.w;
    r->enc_ctx->height = r->config.h;
    r->enc_ctx->time_base = (AVRational){1, r->config.fps};
    r->enc_ctx->framerate = (AVRational){r->config.fps, 1};
    r->enc_ctx->max_b_frames = 0;
    
    r->enc_ctx->pix_fmt = AV_PIX_FMT_BGR0; 
    
    // r->enc_ctx->bit_rate = 15000000;
    av_opt_set(r->enc_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(r->enc_ctx->priv_data, "crf", "17", 0);
    r->enc_ctx->global_quality = 17; 
    av_opt_set(r->enc_ctx->priv_data, "global_quality", "17", 0);
    av_opt_set(r->enc_ctx->priv_data, "tune", "zerolatency", 0);
    r->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(r->enc_ctx, codec, NULL) < 0) 
    {
        printf("Can't open codec\n");
        return 1;
    }

    avcodec_parameters_from_context(r->video_st->codecpar, r->enc_ctx);
    
    r->video_st->time_base = r->enc_ctx->time_base;
    r->video_st->avg_frame_rate = r->enc_ctx->framerate;

    r->frame = av_frame_alloc();
    r->frame->format = r->enc_ctx->pix_fmt;
    r->frame->width  = r->enc_ctx->width;
    r->frame->height = r->enc_ctx->height;
    if (av_frame_get_buffer(r->frame, 0) < 0) return 1;

    r->pkt = av_packet_alloc();

    if (!(r->fmt_ctx->oformat->flags & AVFMT_NOFILE)) 
    {
        if (avio_open(&r->fmt_ctx->pb, r->config.output_filename, AVIO_FLAG_WRITE) < 0) 
        {
            printf("Can't open file\n");
            return 1;
        }
    }

    if (avformat_write_header(r->fmt_ctx, NULL) < 0) return 1;

    r->encode_queue_read = r->encode_queue_write = 0;
    r->encode_mutex = SDL_CreateMutex();
    r->encode_cond = SDL_CreateCondition();
    r->encode_running = true;
    r->encode_thread = SDL_CreateThread(encoder_worker, "encode", r);

    return 0;
}

#define RETFROM(x) if (x) { return NULL; } 

struct render *init_render(const struct render_config *config)
{
    struct render *r = calloc(1, sizeof(*r));
    
    r->config = *config;

    RETFROM(init_window(r));
    RETFROM(init_instance(r));
    RETFROM(init_surface(r));
    RETFROM(find_device(r));
    RETFROM(init_device_and_queue(r));
    RETFROM(init_swapchain(r));
    RETFROM(init_intermediate_image(r));
    RETFROM(init_commands(r));
    RETFROM(create_buffers(r, MAX_POINTS_COUNT * MAX_PATH_LENGTH * sizeof(float) * 2));
    RETFROM(init_download_buffers(r));
    RETFROM(init_render_targets(r));
    RETFROM(init_pipeline_layout(r));
    if (r->config.use_float64)
    {
        printf("Error: For now, float64 aren't supported.\n");
        return NULL;
    }
    RETFROM(init_compute_pipeline(r, (r->config.use_float64 ? "./kernel64_opt.spv" : "./kernel_opt.spv")));
    RETFROM(init_descriptor_sets(r));
    RETFROM(init_command_buffer(r));
    RETFROM(init_syncs(r));
    RETFROM(init_video(r));

    printf("Initialzation finished.\n");

    return r;
}


int encoder_worker(void *ptr) 
{
    struct render *r = (struct render*)ptr;
    while (1) 
    {
        SDL_LockMutex(r->encode_mutex);
        while (r->encode_queue_read == r->encode_queue_write && r->encode_running)
        {
            SDL_WaitCondition(r->encode_cond, r->encode_mutex);
        }

        if (r->encode_queue_read == r->encode_queue_write) 
        {
            SDL_UnlockMutex(r->encode_mutex);
            break;
        }

        struct encode_job job = r->encode_queue[r->encode_queue_read];
        r->encode_queue_read = (r->encode_queue_read + 1) % ENCODE_QUEUE_SIZE;
        SDL_UnlockMutex(r->encode_mutex);

        int ret = avcodec_send_frame(r->enc_ctx, job.frame);
        if (ret < 0) 
        {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "Send frame error: %s\n", errbuf);
            av_frame_free(&job.frame);
            continue;
        }
        while (ret >= 0) {
            ret = avcodec_receive_packet(r->enc_ctx, r->pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;

            r->pkt->stream_index = r->video_st->index;
            av_packet_rescale_ts(r->pkt, r->enc_ctx->time_base, r->video_st->time_base);
            int ret_write = av_interleaved_write_frame(r->fmt_ctx, r->pkt);
            if (ret_write < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret_write, errbuf, sizeof(errbuf));
                fprintf(stderr, "Write error: %s\n", errbuf);
                av_packet_unref(r->pkt);
                break;
            }

            av_packet_unref(r->pkt);
        }
        av_frame_free(&job.frame);
    }
    return 0;
}




int save_to_file(struct render *r, int buf_index)
{
    AVFrame *frame = av_frame_alloc();
    frame->format = r->enc_ctx->pix_fmt;
    frame->width  = r->enc_ctx->width;
    frame->height = r->enc_ctx->height;
    av_frame_get_buffer(frame, 0);

    void *src_data = r->download_memory_mappings[buf_index];

    if (r->sws_ctx == NULL) 
    {
        int pixel_bytes = 4;
        if (frame->linesize[0] == r->config.w * pixel_bytes) 
        {
            memcpy(frame->data[0], src_data, r->config.w * r->config.h * pixel_bytes);
        } 
        else 
        {
            uint8_t* src = (uint8_t*)src_data;
            uint8_t* dst = frame->data[0];
            for (int y = 0; y < r->config.h; y++) 
            {
                memcpy(dst, src, r->config.w * pixel_bytes);
                src += r->config.w * pixel_bytes;
                dst += frame->linesize[0];
            }
        }
    } 
    else 
    {
        const uint8_t* src_slices[] = { (uint8_t*)src_data };
        int src_strides[] = { r->config.w * 4 };
        sws_scale(r->sws_ctx, src_slices, src_strides, 0, r->config.h,
                  frame->data, frame->linesize);
    }

    frame->pts = r->frame_count++;

    SDL_LockMutex(r->encode_mutex);
    int next = (r->encode_queue_write + 1) % ENCODE_QUEUE_SIZE;
    while (next == r->encode_queue_read) 
    {
        SDL_WaitCondition(r->encode_cond, r->encode_mutex);
        next = (r->encode_queue_write + 1) % ENCODE_QUEUE_SIZE;
    }
    r->encode_queue[r->encode_queue_write] = (struct encode_job){frame, frame->pts};
    r->encode_queue_write = next;
    SDL_SignalCondition(r->encode_cond);
    SDL_UnlockMutex(r->encode_mutex);

    return 0;
}



void render_image(struct render *r, struct path_data *path)
{
    VkDeviceSize buffer_size = MAX_POINTS_COUNT * MAX_PATH_LENGTH * sizeof(float) * 2;
    vkMapMemory(r->device, r->staging_memory, 0, buffer_size, 0, (void **)&path->data);


    int64_t time_ms = SDL_GetPerformanceCounter(), counter = 0;
    int64_t ifreq = SDL_GetPerformanceFrequency();
    int64_t ffreq = SDL_GetPerformanceFrequency();

    double dz = 1.0;
    double dx = 0.0;
    double dy = 0.0;

    #define SPEEDS_PROBES 4
    double speeds[SPEEDS_PROBES] = {}, prev_zoom_e = 0.0;
    int64_t speeds_pos = 0, speeds_cnt = 0;
    
    while (1)
    {
        
        int64_t new_time = SDL_GetPerformanceCounter();
        counter++;
        if (new_time - time_ms > ifreq)
        {
            double fps = counter / ((double)(new_time - time_ms)) * ffreq;
            time_ms = SDL_GetPerformanceCounter();
            counter = 0;

            speeds[speeds_pos++] = fps;
            speeds_cnt++;
            if (speeds_pos == SPEEDS_PROBES) { speeds_pos = 0; }
            if (speeds_cnt > SPEEDS_PROBES) { speeds_cnt = SPEEDS_PROBES; }

            double avg = 0.0;
            for (int64_t i = 0; i < SPEEDS_PROBES; ++i) { avg += speeds[i]; }
            avg /= speeds_cnt;

            double remain = (path->total_images - path->current_image) / avg;
            double rendered_time = path->current_image / (double)r->config.fps;
            printf("Measured %8.2f fps. | zoom=2^%7.1f | depth: %6.2f%% | skip %6.2f%% | remain %4lld m. %5.1f s. | rendered: %.1f seconds. \n", 
            fps, 
            -prev_zoom_e, 
            path->current_depth * 100.0 / MAX_PATH_LENGTH, 
            path->skip_steps * 100.0 / path->current_depth, 
            ((int64_t)remain)/60, fmod(remain, 60.0),
            rendered_time);

            if (file_exists("./stop_now"))
            {
                printf("Found ./stop_now file, removing it and breaking rendering.\n");
                if (remove("./stop_now") != 0)
                {
                    printf("Waring: Can't delete file ./stop_now.\n");
                }
                break;
            }
        }
        
        if (r->config.output_filename)
        {
            uint32_t prev_i = (r->current_frame+MAX_FRAMES_IN_FLIGHT-1) % MAX_FRAMES_IN_FLIGHT;
            uint32_t i = r->current_frame % MAX_FRAMES_IN_FLIGHT;

            vkWaitForFences(r->device, 1, &r->in_flight_fences[prev_i], VK_TRUE, UINT64_MAX);
            vkResetFences(r->device, 1, &r->in_flight_fences[prev_i]);

            if (r->current_frame > 0) 
            {
                save_to_file(r, prev_i);
            } 
            update_zoom(path, path->zoom_step, 0.0, 0.0);
            
            struct push_constant_parameter params;
            params.time = path->time;
            params.anchor_points = path->points_count;
            params.path_length = path->path_length;
            if (!calculate_path(path, &params.zoom_m, &params.zoom_e,
                                &params.center[0], &params.center[1]))
            {    
                return;
            }


            prev_zoom_e = params.zoom_e + log2(params.zoom_m);

            VkCommandBufferBeginInfo begin_info = {};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(r->cmd_buffers[i], &begin_info);

            if (path->moditified) 
            {             
                VkBufferCopy copyRegion = {0, 0, buffer_size};
                vkCmdCopyBuffer(r->cmd_buffers[i], r->staging_buffer, r->device_buffer, 1, &copyRegion);        

                VkBufferMemoryBarrier buffer_barrier = {};
                buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                buffer_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                buffer_barrier.buffer = r->device_buffer;
                buffer_barrier.size = VK_WHOLE_SIZE;

                vkCmdPipelineBarrier(
                    r->cmd_buffers[i],
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 0, NULL, 1, &buffer_barrier, 0, NULL
                );
            }

    
            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = r->intermediate_image;

            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

            vkCmdPipelineBarrier(
                r->cmd_buffers[i],
                VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, NULL, 0, NULL, 1, &barrier
            );

            vkCmdBindPipeline(r->cmd_buffers[i], VK_PIPELINE_BIND_POINT_COMPUTE, r->compute_pipeline);
            VkDescriptorSet current_set = r->descriptor_sets[0];
            vkCmdBindDescriptorSets(r->cmd_buffers[i], VK_PIPELINE_BIND_POINT_COMPUTE, 
                                    r->pipeline_layout, 0, 1, &current_set, 0, NULL);
                                                
            vkCmdPushConstants(r->cmd_buffers[i], r->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct push_constant_parameter), &params);
            uint32_t gx = (r->config.w + WORK_GROUP_SIZE_X - 1) / WORK_GROUP_SIZE_X;
            uint32_t gy = (r->config.h + WORK_GROUP_SIZE_Y - 1) / WORK_GROUP_SIZE_Y;
            vkCmdDispatch(r->cmd_buffers[i], gx, gy, 1);

            VkImageMemoryBarrier copy_barrier = {};
            copy_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            
            copy_barrier.image = r->intermediate_image;
            copy_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL; 
            copy_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            copy_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            copy_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            copy_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy_barrier.subresourceRange.levelCount = 1;
            copy_barrier.subresourceRange.layerCount = 1;

            vkCmdPipelineBarrier(r->cmd_buffers[i], 
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 
                VK_PIPELINE_STAGE_TRANSFER_BIT, 
                0, 0, NULL, 0, NULL, 1, &copy_barrier);


            VkBufferImageCopy region = {};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = (VkExtent3D){ r->config.w, r->config.h, 1 };
            vkCmdCopyImageToBuffer(r->cmd_buffers[i], r->intermediate_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   r->download_buffers[i], 1, &region);

            VkBufferMemoryBarrier mem_barrier = {};
            mem_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            mem_barrier.buffer = r->download_buffers[i];
            mem_barrier.size = VK_WHOLE_SIZE;
            mem_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mem_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(r->cmd_buffers[i], VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &mem_barrier, 0, NULL);

            vkEndCommandBuffer(r->cmd_buffers[i]);

            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &r->cmd_buffers[i];

            vkQueueSubmit(r->queue, 1, &submitInfo, r->in_flight_fences[i]);

            r->current_frame++;
        }
        else
        {
            
            vkWaitForFences(r->device, 1, &r->in_flight_fences[0], VK_TRUE, UINT64_MAX);
            vkResetFences(r->device, 1, &r->in_flight_fences[0]);

            struct push_constant_parameter params;
            params.time = path->time;
            params.anchor_points = path->points_count;
            params.path_length = path->path_length;
            if (!calculate_path(path, &params.zoom_m, &params.zoom_e, &params.center[0], &params.center[1]))
            {
                return;
            }
            
            uint32_t image_index = 0;

            vkAcquireNextImageKHR(r->device, r->swapchain, UINT64_MAX, 
                                  r->image_available_sem, VK_NULL_HANDLE, &image_index);

            VkCommandBufferBeginInfo begin_info = {};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(r->cmd_buffers[0], &begin_info);

            if (path->moditified)
            {
                VkBufferCopy copyRegion = {0, 0, buffer_size};
                vkCmdCopyBuffer(r->cmd_buffers[0], r->staging_buffer, r->device_buffer, 1, &copyRegion);        

                VkBufferMemoryBarrier buffer_barrier = {};
                buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                buffer_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                buffer_barrier.buffer = r->device_buffer;
                buffer_barrier.size = VK_WHOLE_SIZE;

                vkCmdPipelineBarrier(
                    r->cmd_buffers[0],
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 0, NULL, 1, &buffer_barrier, 0, NULL
                );
            }
            
            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = r->swapchain_images[image_index];
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

            vkCmdPipelineBarrier(
                r->cmd_buffers[0],
                VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, NULL, 0, NULL, 1, &barrier
            );

            vkCmdBindPipeline(r->cmd_buffers[0], VK_PIPELINE_BIND_POINT_COMPUTE, r->compute_pipeline);

            VkDescriptorSet current_set;
            if (r->use_intermediate) 
            {
                current_set = r->descriptor_sets[0];
            } 
            else 
            {
                current_set = r->descriptor_sets[image_index];
            }

            vkCmdBindDescriptorSets(r->cmd_buffers[0], VK_PIPELINE_BIND_POINT_COMPUTE, 
                                    r->pipeline_layout, 0, 1, &current_set, 0, NULL);


            vkCmdPushConstants(r->cmd_buffers[0], r->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct push_constant_parameter), &params);

            uint32_t gx = (r->config.w + WORK_GROUP_SIZE_X - 1) / WORK_GROUP_SIZE_X;
            uint32_t gy = (r->config.h + WORK_GROUP_SIZE_Y - 1) / WORK_GROUP_SIZE_Y;
            if (!r->config.output_filename)
            {
                assert(r->config.w == r->swapchain_extent.width);
                assert(r->config.h == r->swapchain_extent.height);
            }
            vkCmdDispatch(r->cmd_buffers[0], gx, gy, 1);

            if (r->use_intermediate) 
            {
                
                VkImageCopy copyRegion = {};
                copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                copyRegion.srcSubresource.layerCount = 1;
                copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                copyRegion.dstSubresource.layerCount = 1;
                copyRegion.extent.width = r->config.w;
                copyRegion.extent.height = r->config.h;
                copyRegion.extent.depth = 1;

                vkCmdCopyImage(
                    r->cmd_buffers[0],
                    r->intermediate_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    r->swapchain_images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &copyRegion
                );
            }

            VkImageMemoryBarrier present_barrier = {};
            present_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            present_barrier.oldLayout = r->use_intermediate ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
            present_barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            present_barrier.image = r->swapchain_images[image_index];
            present_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            present_barrier.subresourceRange.levelCount = 1;
            present_barrier.subresourceRange.layerCount = 1;
            present_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            present_barrier.dstAccessMask = 0;

            vkCmdPipelineBarrier(
                r->cmd_buffers[0],
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0, 0, NULL, 0, NULL, 1, &present_barrier
            );

            vkEndCommandBuffer(r->cmd_buffers[0]);

            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &r->cmd_buffers[0];

            static VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT };
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = &r->image_available_sem;
            submitInfo.pWaitDstStageMask = waitStages;
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = &r->render_finished_sem;
            
            vkQueueSubmit(r->queue, 1, &submitInfo, r->in_flight_fences[0]);

            VkPresentInfoKHR presentInfo = {};
            presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            presentInfo.waitSemaphoreCount = 1;
            presentInfo.pWaitSemaphores = &r->render_finished_sem;
            presentInfo.swapchainCount = 1;
            presentInfo.pSwapchains = &r->swapchain;
            presentInfo.pImageIndices = &image_index;

            vkQueuePresentKHR(r->queue, &presentInfo);

            SDL_Event e;
            while (SDL_PollEvent(&e))
            {
                if (e.type == SDL_EVENT_QUIT)
                {
                    return;
                }
                else if (e.type == SDL_EVENT_MOUSE_WHEEL) 
                {   
                    dz -= e.wheel.y * 0.01;
                }
            }

            int numkeys;
            const bool *state = SDL_GetKeyboardState(&numkeys);
            dx = dy = 0.0;
            if (state[SDL_SCANCODE_UP])  { dy = -5.0; }
            if (state[SDL_SCANCODE_DOWN])  { dy = +5.0; }
            if (state[SDL_SCANCODE_LEFT])  { dx = -5.0; }
            if (state[SDL_SCANCODE_RIGHT])  { dx = +5.0; }


            dz = (0.05 + dz * 0.95);

            update_zoom(path, dz, dx, dy);
        }
    }
}

void close_video(struct render *r) 
{
    if (!r->fmt_ctx) return;

    r->encode_running = false;
    SDL_LockMutex(r->encode_mutex);
    SDL_SignalCondition(r->encode_cond);
    SDL_UnlockMutex(r->encode_mutex);
    SDL_WaitThread(r->encode_thread, NULL);

    SDL_DestroyMutex(r->encode_mutex);
    SDL_DestroyCondition(r->encode_cond);


    avcodec_send_frame(r->enc_ctx, NULL); 
    while (avcodec_receive_packet(r->enc_ctx, r->pkt) >= 0) 
    {
        r->pkt->stream_index = r->video_st->index;
        av_packet_rescale_ts(r->pkt, r->enc_ctx->time_base, r->video_st->time_base);
        av_interleaved_write_frame(r->fmt_ctx, r->pkt);
        av_packet_unref(r->pkt);
    }

    av_interleaved_write_frame(r->fmt_ctx, NULL);

    av_write_trailer(r->fmt_ctx);

    if (!(r->fmt_ctx->oformat->flags & AVFMT_NOFILE)) 
    {
        avio_closep(&r->fmt_ctx->pb);
    }
    avcodec_free_context(&r->enc_ctx);
    av_frame_free(&r->frame);
    av_packet_free(&r->pkt);
    avformat_free_context(r->fmt_ctx);
    sws_freeContext(r->sws_ctx);
    
    r->fmt_ctx = NULL;
}


void render_deinit(struct render *r)
{
    close_video(r);

    if (r->device) 
    {
        vkDeviceWaitIdle(r->device);
    }

    vkDestroySemaphore(r->device, r->image_available_sem, NULL);
    vkDestroySemaphore(r->device, r->render_finished_sem, NULL);
    if (r->render_fence) vkDestroyFence(r->device, r->render_fence, NULL);

    vkDestroyPipeline(r->device, r->compute_pipeline, NULL);
    vkDestroyPipelineLayout(r->device, r->pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(r->device, r->descriptor_set_layout, NULL);
    vkDestroyDescriptorPool(r->device, r->descriptor_pool, NULL);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) 
    {
        if (r->download_buffers[i]) {
            vkUnmapMemory(r->device, r->download_memories[i]);
            vkDestroyBuffer(r->device, r->download_buffers[i], NULL);
            vkFreeMemory(r->device, r->download_memories[i], NULL);
        }
        if (r->in_flight_fences[i]) 
        {
            vkDestroyFence(r->device, r->in_flight_fences[i], NULL);
        }
    }

    if (r->device_buffer) 
    {
        vkDestroyBuffer(r->device, r->device_buffer, NULL);
        vkFreeMemory(r->device, r->device_memory, NULL);
    }
    if (r->staging_buffer) 
    {
        vkDestroyBuffer(r->device, r->staging_buffer, NULL);
        vkFreeMemory(r->device, r->staging_memory, NULL);
    }

    if (r->swapchain) 
    {
        for (uint32_t i = 0; i < r->image_count; i++) 
        {
            vkDestroyImageView(r->device, r->swapchain_image_views[i], NULL);
        }
        vkDestroySwapchainKHR(r->device, r->swapchain, NULL);
        free(r->swapchain_images);
        free(r->swapchain_image_views);
    }

    if (r->surface) vkDestroySurfaceKHR(r->instance, r->surface, NULL);
    if (r->device) vkDestroyDevice(r->device, NULL);
    if (r->instance) vkDestroyInstance(r->instance, NULL);

    if (!r->config.output_filename) SDL_DestroyWindow(r->window);
    SDL_Quit();

    free(r);
    printf("Render resources cleaned up gracefully.\n");

}
