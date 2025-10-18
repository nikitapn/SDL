#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_ANDROID

#include "SDL_androidvideo.h"
#include "SDL_androidwindow.h"
#include "../SDL_sysvideo.h"
#include "../../render/SDL_sysrender.h"
#include "../../render/software/SDL_render_sw_c.h"
#include "SDL3/SDL_surface.h"
#include "SDL3/SDL_hints.h"
#include <android/native_window.h>
#include "SDL_androidsoftnoegl_simd.h"

// RGBA_8888 format constant (compatible with older Android APIs)
#ifndef WINDOW_FORMAT_RGBA_8888
#    define WINDOW_FORMAT_RGBA_8888 1
#endif

// Forward declarations
extern ANativeWindow* Android_JNI_GetNativeWindow(void);
bool SW_CreateRendererForSurface(SDL_Renderer *renderer, SDL_Surface *surface, SDL_PropertiesID create_props);

static bool AndroidSoft_CreateRenderer(SDL_Renderer *renderer, SDL_Window *window, SDL_PropertiesID props);
static void AndroidSoft_DestroyRenderer(SDL_Renderer *renderer);
static bool AndroidSoft_RenderPresent(SDL_Renderer *renderer);

typedef struct {
    // First two members must match SW_RenderData
    SDL_Surface *renderSurface;
    ANativeWindow *nativeWindow;
    // SW renderer's original driverdata (SW_RenderData*)
    void *sw_driverdata; 
    // SW renderer's original functions
    bool (*sw_RunCommandQueue)(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize);
    bool (*sw_RenderPresent)(SDL_Renderer *renderer);
    void (*sw_DestroyRenderer)(SDL_Renderer *renderer);
} AndroidSoftData;

SDL_RenderDriver ANDROID_SW_NO_EGL_RenderDriver = {
    .CreateRenderer = AndroidSoft_CreateRenderer,
    .name = "android_soft",
};

// Wrapper for RunCommandQueue - restores SW driverdata before calling SW renderer
static bool AndroidSoft_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    AndroidSoftData *data = (AndroidSoftData *)renderer->internal;
    void *saved_driverdata = renderer->internal;
    bool result;

    // Temporarily restore SW renderer's driverdata
    renderer->internal = data->sw_driverdata;

    // Call the SW renderer's RunCommandQueue
    result = data->sw_RunCommandQueue(renderer, cmd, vertices, vertsize);
    
    // Restore our driverdata
    renderer->internal = saved_driverdata;
    
    return result;
}

static bool AndroidSoft_CreateRenderer(SDL_Renderer *renderer, SDL_Window *window, SDL_PropertiesID props)
{
    // SDL_Renderer *renderer = NULL;
    AndroidSoftData *data = NULL;
    SDL_Surface *render_surface = NULL;
    ANativeWindow *native_window = NULL;
    int window_width, window_height;
    int window_format;

    SDL_Log("AndroidSoft: Creating pure software renderer with ANativeWindow present");

    // Get the native window first
    native_window = Android_JNI_GetNativeWindow();
    if (!native_window) {
        SDL_SetError("No native window available");
        return false;
    }

    // Get the window size
    SDL_GetWindowSize(window, &window_width, &window_height);
    
    // Get the native window format
    window_format = ANativeWindow_getFormat(native_window);
    
    SDL_Log("AndroidSoft: Native window: %dx%d, format %d",
            window_width, window_height, window_format);

    // Create a rendering surface matching the window
    // SDL's software renderer internally uses ARGB8888 (which is BGRA in memory on little-endian)
    render_surface = SDL_CreateSurface(window_width, window_height, SDL_PIXELFORMAT_ARGB8888);
    if (!render_surface) {
        ANativeWindow_release(native_window);
        SDL_SetError("Failed to create rendering surface");
        return false;
    }

    SDL_Log("AndroidSoft: Render surface created: %dx%d, format %s",
            render_surface->w, render_surface->h,
            SDL_GetPixelFormatName(render_surface->format));

    if (!SW_CreateRendererForSurface(renderer, render_surface, 0)) {
        SDL_DestroySurface(render_surface);
        ANativeWindow_release(native_window);
        SDL_SetError("Failed to create software renderer");
        return false;
    }
    
    // Set the window association
    renderer->window = window;

    // Allocate our Android-specific data
    data = (AndroidSoftData *)SDL_calloc(1, sizeof(*data));

    // Store references
    data->sw_driverdata = renderer->internal;  // Save SW renderer's driverdata
    data->nativeWindow = native_window;
    data->renderSurface = render_surface;

    // Save the SW renderer's original functions
    data->sw_RunCommandQueue = renderer->RunCommandQueue;
    data->sw_RenderPresent = renderer->RenderPresent;
    data->sw_DestroyRenderer = renderer->DestroyRenderer;

    // Replace driverdata with our wrapper that contains both
    renderer->internal = data;

    // Replace the functions we need to wrap
    renderer->RunCommandQueue = AndroidSoft_RunCommandQueue;
    renderer->RenderPresent = AndroidSoft_RenderPresent;
    renderer->DestroyRenderer = AndroidSoft_DestroyRenderer;

    SDL_Log("AndroidSoft: Successfully created renderer");
    return true;
}

static void AndroidSoft_DestroyRenderer(SDL_Renderer *renderer)
{
    AndroidSoftData *data = (AndroidSoftData *)renderer->internal;
    if (data) {
        // Restore the original SW renderer data before destroying
        if (data->sw_DestroyRenderer) {
            renderer->internal = data->sw_driverdata;
            data->sw_DestroyRenderer(renderer);
        }

        // Free the render surface
        if (data->renderSurface) {
            SDL_DestroySurface(data->renderSurface);
        }

        // Release the native window
        if (data->nativeWindow) {
            ANativeWindow_release(data->nativeWindow);
        }

        // Free our data
        SDL_free(data);
    }
}

static bool AndroidSoft_RenderPresent(SDL_Renderer *renderer)
{
    AndroidSoftData *data = (AndroidSoftData *)renderer->internal;
    SDL_Surface *surface = NULL;
    ANativeWindow_Buffer buffer;
    int w, h;
    Uint8 *dst, *src;
    int y;

    if (!data || !data->nativeWindow || !data->renderSurface) {
        return SDL_SetError("Invalid renderer data");
    }

    // Use our render surface
    surface = data->renderSurface;

    // Set buffer geometry to match our surface dimensions and format
    // This is required before locking on Android
    // Use WINDOW_FORMAT_RGBA_8888 (format 1) which matches our ARGB8888 surface
    if (ANativeWindow_setBuffersGeometry(data->nativeWindow, 
                                         surface->w, 
                                         surface->h,
                                         WINDOW_FORMAT_RGBA_8888) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "ANativeWindow_setBuffersGeometry failed");
        return SDL_SetError("ANativeWindow_setBuffersGeometry failed");
    }

    // Lock the native window buffer
    if (ANativeWindow_lock(data->nativeWindow, &buffer, NULL) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "ANativeWindow_lock failed");
        return SDL_SetError("ANativeWindow_lock failed");
    }

    // Calculate dimensions
    w = SDL_min(buffer.width, surface->w);
    h = SDL_min(buffer.height, surface->h);

    dst = (Uint8 *)buffer.bits;
    src = (Uint8 *)surface->pixels;

    for (y = 0; y < h; ++y) {
        Uint32 *src_row = (Uint32 *)(src + y * surface->pitch);
        Uint32 *dst_row = (Uint32 *)(dst + y * buffer.stride * 4);

        // Use SIMD-optimized conversion (NEON on ARM, SSE2 on x86, scalar fallback)
        ConvertARGBtoRGBA_Scanline(src_row, dst_row, w);
    }

    // Unlock and present
    ANativeWindow_unlockAndPost(data->nativeWindow);

    return true;
}

#endif /* SDL_VIDEO_DRIVER_ANDROID */