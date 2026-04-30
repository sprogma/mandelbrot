#include "render.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>


struct render
{
    struct render_config config;
    SDL_Window *window;
    VkInstance instance;
};


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
            SDL_WINDOW_VULKAN
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

    VkApplicationInfo appInfo = {0};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "ComputeApp";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "NoEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo createInfo = {0};
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

bool isComputeSupported(VkPhysicalDevice device) 
{
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, NULL);

    VkQueueFamilyProperties* queueFamilies = malloc(sizeof(VkQueueFamilyProperties) * queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies);

    for (uint32_t i = 0; i < queueFamilyCount; i++) 
    {
        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) 
        {
            free(queueFamilies);
            return true;
        }
    }
    free(queueFamilies);
    return false;
}

int init_device(struct render *r)
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

        if (isComputeSupported(devices[i]))
        {
            printf("    Computing supported\n");
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && !default_device_is_gpu)
            {
                default_device = i;
                default_device_is_gpu = true;
            }
            else
            {
                default_device = i;
            }
        }
    }

    if (r->config.device_id == -1)
    {
        printf("Selecting default device: %u\n", default_device);
    }
    else
    {
        /* check that gpu */
        if (r->config.device_id < 0 || (uint32_t)r->config.device_id >= deviceCount)
        {
            printf("Wrong device id: %d\n", r->config.device_id);
            SDL_DestroyWindow(r->window);
            SDL_Quit();
            free(r);
            return 1;
        }
        if (!isComputeSupported(devices[r->config.device_id]))
        {
            printf("Device %d doesn't support compute pipelines\n", r->config.device_id);
            SDL_DestroyWindow(r->window);
            SDL_Quit();
            free(r);
            return 1;
        }
    }

    return 0;
}

int init_queue(struct render *r)
{
//     uint32_t queueFamilyCount = 0;
//     vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, NULL);
// 
//     VkQueueFamilyProperties* queueFamilies = malloc(sizeof(VkQueueFamilyProperties) * queueFamilyCount);
//     vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies);
// 
//     bool computeSupported = false;
//     for (uint32_t i = 0; i < queueFamilyCount; i++) 
//     {
//         if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) 
//         {
//             computeSupported = true;
//             break;
//         }
//     }
//     free(queueFamilies);
// 
//     if (!computeSupported) 
//     {
//         printf("Compute shaders aren't supported by this gpu.\n");
//         return 1;
//     }

    return 0;
}

#define RETFROM(x) if (x) { return NULL; } 

struct render *init_render(const struct render_config *config)
{
    struct render *r = malloc(sizeof(*r));
    
    r->config = *config;

    RETFROM(init_window(r));
    RETFROM(init_instance(r));
    RETFROM(init_device(r));
    RETFROM(init_queue(r));

    return r;
}

void render_image(struct render *r)
{
    (void)r;
}

void render_deinit(struct render *r)
{
    vkDestroyInstance(r->instance, NULL);
    SDL_DestroyWindow(r->window);
    SDL_Quit();
}
