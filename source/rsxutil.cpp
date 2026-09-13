#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>

#include <sys/event_queue.h>
#include <sysutil/video.h>

#include "rsxutil.h"

// NOTE: clangd (CLion's editor diagnostics) will show false "no matching
// function" errors throughout this file, e.g. on rsxSetClearColor/
// rsxClearSurface/gGcmContext. Cause: ppu-types.h's ATTRIBUTE_PRXPTR is
// __attribute__((mode(SI))) applied to a pointer, a GCC-only extension
// Clang doesn't support on pointer types -- Clang's error recovery then
// misresolves gGcmContext's type as unsigned int instead of
// gcmContextData*. This only affects clangd's editor-side parsing; the
// real ppu-g++ build (GCC) handles it correctly and the code runs fine.
// Safe to ignore these specific squiggles here and in rsx/*.h.

videoResolution vResolution; // resolved video mode (width/height/etc.)

u32 curr_fb = 0; // ring-buffer index we're currently rendering into

u32 display_width;
u32 display_height;

u32 depth_pitch;
u32 depth_offset; // RSX offset of the shared depth/stencil buffer
u32 *depth_buffer;

u32 color_pitch;
u32 color_offset[FRAME_BUFFER_COUNT]; // RSX offset of each color buffer
u32 *color_buffer[FRAME_BUFFER_COUNT];

f32 aspect_ratio;

u32 fbOnDisplay = 0; // buffer index currently being scanned out to the TV
u32 fbFlipped = 0;   // buffer index of the most recently requested flip
bool fbOnFlip = false; // true while a flip is in flight (requested but not yet shown)
sys_event_queue_t flipEventQueue;
sys_event_port_t flipEventPort;

gcmSurface surface; // render-target description, shared by initRenderTarget()/setRenderTarget()

static u32 sLabelVal = 1; // next value used by waitRSXFinish()'s PPU/RSX handshake

// Candidate video resolutions, tried highest-to-lowest in
// initVideoConfiguration() until one is supported.
static u32 sResolutionIds[] = {
    VIDEO_RESOLUTION_1600x1080,
    VIDEO_RESOLUTION_1440x1080,
    VIDEO_RESOLUTION_1280x1080,
    VIDEO_RESOLUTION_960x1080,
    VIDEO_RESOLUTION_720,
    VIDEO_RESOLUTION_480,
    VIDEO_RESOLUTION_576
};
static size_t RESOLUTION_ID_COUNT = sizeof(sResolutionIds)/sizeof(u32);

extern "C" {
// Called by the RSX driver once a requested buffer flip has actually
// happened on screen. Marks the newly-visible buffer(s) idle again so
// the PPU is allowed to start rendering into them.
static void flipHandler(const u32 head)
{
    (void)head; // unused: we only ever drive a single display head

    u32 v = fbFlipped; // buffer index that just became visible

    // Everything between the old "on display" buffer and the new one is
    // now safe to render into again -- mark each as idle.
    for (u32 i = fbOnDisplay; i != v; i=(i + 1)%FRAME_BUFFER_COUNT) {
        *((vu32*) gcmGetLabelAddress(GCM_BUFFER_STATUS_INDEX + i)) = BUFFER_IDLE;
    }
    fbOnDisplay = v;
    fbOnFlip = false; // no flip in flight anymore

    // Wake up syncPPUGPU()'s event-queue wait, in case the PPU is blocked
    // there waiting for room to queue another frame.
    sysEventPortSend(flipEventPort, 0, 0, 0);
}

// Called on every vertical blank (i.e. once per TV refresh). Checks
// whether a buffer flip was requested and, if the display isn't already
// mid-flip, tells the RSX to actually swap to it now.
static void vblankHandler(const u32 head)
{
    (void)head; // unused: single display head only

    u32 data;
    u32 bufferToFlip;
    u32 indexToFlip;

    // GCM_PREPARED_BUFFER_INDEX label was packed by flip(): high bits =
    // which ring-buffer index was requested, low 3 bits = the GCM queue
    // id needed to actually perform the flip.
    data = *((vu32*) gcmGetLabelAddress(GCM_PREPARED_BUFFER_INDEX));
    bufferToFlip = (data >> 8);
    indexToFlip = (data & 0x07);

    if (!fbOnFlip) { // don't stack a second flip on top of one in progress
        if (bufferToFlip != fbOnDisplay) { // only flip if something changed
            s32 ret = gcmSetFlipImmediate(indexToFlip);
            if (ret != 0) {
                printf("flip immediate failed\n");
                return;
            }
            fbFlipped = bufferToFlip;
            fbOnFlip = true; // flipHandler() will clear this once it lands
        }
    }
}
}

// Blocks the PPU (CPU) if it's gotten too far ahead of the RSX (GPU) --
// i.e. it has queued more frames than MAX_BUFFER_QUEUE_SIZE that the RSX
// hasn't displayed yet. Prevents runaway memory/buffer usage from the
// CPU rendering much faster than the screen can refresh.
static void syncPPUGPU()
{
    vu32 *label = (vu32*) gcmGetLabelAddress(GCM_PREPARED_BUFFER_INDEX);

    // How many buffers "ahead" curr_fb is of the last one the RSX was
    // told to flip to. If that's more than we're allowed to queue up,
    // block until flipHandler() (triggered by an actual flip) wakes us.
    while(((curr_fb + FRAME_BUFFER_COUNT - ((*label)>>8))%FRAME_BUFFER_COUNT) > MAX_BUFFER_QUEUE_SIZE) {
        sys_event_t event;

        sysEventQueueReceive(flipEventQueue, &event, 0); // block for the wake-up
        sysEventQueueDrain(flipEventQueue); // discard any other queued events
    }
}

// Pushes a "write this label value" command into the RSX command buffer,
// flushes it, then busy-waits until the RSX has actually processed it.
// This is how the PPU knows the GPU has caught up to a specific point.
static void waitRSXFinish()
{
	// Queue a command telling the RSX to write sLabelVal into a known
	// label once it reaches this point in the command stream.
	rsxSetWriteBackendLabel(gGcmContext,GCM_WAIT_LABEL_INDEX,sLabelVal);

	rsxFlushBuffer(gGcmContext); // actually submit the queued commands

	// Poll until that label shows up, i.e. the RSX has processed
	// everything queued before it.
	while(*(vu32*)gcmGetLabelAddress(GCM_WAIT_LABEL_INDEX)!=sLabelVal)
		usleep(30);

	++sLabelVal; // next call needs a different value to wait for
}

// Waits for the RSX to fully catch up and go idle (no pending commands).
// Used once during startup, before any frame buffers/render target are
// touched, to make sure the GPU is in a known, quiet state first.
static void waitRSXIdle()
{
	// Queue a "write label" command followed immediately by a "wait for
	// that same label" command -- makes the RSX itself stall until it's
	// caught up to this point before processing anything further.
	rsxSetWriteBackendLabel(gGcmContext,GCM_WAIT_LABEL_INDEX,sLabelVal);
	rsxSetWaitLabel(gGcmContext,GCM_WAIT_LABEL_INDEX,sLabelVal);

	++sLabelVal;

	waitRSXFinish(); // and have the PPU wait for it too
}

// Picks the best video resolution the connected display/RPCS3 supports
// (from a fixed list of candidates, highest first), configures the video
// output to use it, and records its width/height/aspect ratio for later
// use (e.g. setting up the RSX viewport).
void initVideoConfiguration()
{
    s32 rval = 0;
    s32 resId = 0;

    // sResolutionIds is ordered highest-to-lowest -- try each until we
    // find one the display actually supports.
    for (size_t i=0;i < RESOLUTION_ID_COUNT;i++) {
        rval = videoGetResolutionAvailability(VIDEO_PRIMARY, sResolutionIds[i], VIDEO_ASPECT_AUTO, 0);
        if (rval != 1) continue; // not supported, try the next one

        resId = sResolutionIds[i];
        rval = videoGetResolution(resId, &vResolution); // fills in width/height
        if(!rval) break; // success
    }

    if(rval) {
        // Ran out of candidates without finding a usable resolution.
        printf("Error: videoGetResolutionAvailability failed. No usable resolution.\n");
        exit(1);
    }

    // Ask the video subsystem to actually switch to the resolution we
    // picked, using a plain 32-bit XRGB framebuffer format.
    videoConfiguration config = {
        (u8)resId,
        VIDEO_BUFFER_FORMAT_XRGB,
        VIDEO_ASPECT_AUTO,
        {0,0,0,0,0,0,0,0,0}, // reserved/unused fields
        (u32)vResolution.width*4 // pitch: 4 bytes per pixel
    };

    rval = videoConfigure(VIDEO_PRIMARY, &config, NULL, 0);
    if(rval) {
        printf("Error: videoConfigure failed.\n");
        exit(1);
    }

    // Read back the actual display mode to find out its aspect ratio
    // (needed later for perspective/viewport math).
    videoState state;

    rval = videoGetState(VIDEO_PRIMARY, 0, &state);
    switch(state.displayMode.aspect) {
        case VIDEO_ASPECT_4_3:
            aspect_ratio = 4.0f/3.0f;
            break;
        case VIDEO_ASPECT_16_9:
            aspect_ratio = 16.0f/9.0f;
            break;
        default:
            printf("unknown aspect ratio %x\n", state.displayMode.aspect);
            aspect_ratio = 16.0f/9.0f; // reasonable fallback
            break;
    }

    display_height = vResolution.height;
    display_width = vResolution.width;
}

// Sets up the event queue/port used to notify the PPU when a flip has
// completed, and registers flipHandler/vblankHandler as the callbacks
// the RSX driver invokes for flip and vblank events.
void initFlipEvent()
{
    sys_event_queue_attr_t queueAttr = { SYS_EVENT_QUEUE_PRIO, SYS_EVENT_QUEUE_PPU, "\0" };

    // The PPU-side wait queue that syncPPUGPU() blocks on, and the port
    // flipHandler() sends to in order to wake it up.
    sysEventQueueCreate(&flipEventQueue, &queueAttr, SYS_EVENT_QUEUE_KEY_LOCAL, 32);
    sysEventPortCreate(&flipEventPort, SYS_EVENT_PORT_LOCAL, SYS_EVENT_PORT_NO_NAME);
    sysEventPortConnectLocal(flipEventPort, flipEventQueue);

    // Register our two RSX-driver callbacks (defined above).
    gcmSetFlipHandler(flipHandler);
    gcmSetVBlankHandler(vblankHandler);
}

// Fills in the gcmSurface struct describing where the RSX should render
// to: which color buffer, which depth buffer, and their formats/sizes.
// setRenderTarget() later just swaps which color buffer this points at.
void initRenderTarget()
{
    memset(&surface, 0, sizeof(gcmSurface));

	// Main (target 0) color output: current framebuffer, in RSX-local
	// video memory.
	surface.colorFormat		= GCM_SURFACE_X8R8G8B8;
	surface.colorTarget		= GCM_SURFACE_TARGET_0;
	surface.colorLocation[0]	= GCM_LOCATION_RSX;
	surface.colorOffset[0]	= color_offset[curr_fb];
	surface.colorPitch[0]	= color_pitch;

    // We only actually render to target 0; the remaining multi-render-
    // target (MRT) slots are left unused (offset 0), matching known-working
    // PSL1GHT samples -- aliasing them onto color_offset[curr_fb] with a
    // mismatched pitch (as this used to) is placeholder-only for CPU pixel
    // writes, which never touch these slots, but is a bogus surface
    // description the moment real GPU 3D rendering is involved.
    for(u32 i=1; i< GCM_MAX_MRT_COUNT;i++) {
        surface.colorLocation[i]	= GCM_LOCATION_RSX;
        surface.colorOffset[i]		= 0;
        surface.colorPitch[i]		= 64;
    }

	// Shared depth/stencil buffer (same one reused across all color buffers).
	// Z24S8, not Z16: matches the locally-verified working PSL1GHT rsxtest
	// sample (shader-triangle-test/rsxtest-bisect), which also clears depth
	// with 0xffffff00 (24-bit Z + 8-bit stencil). The depth buffer is
	// already allocated at 4 bytes/pixel (zs_depth = 4), so it fits.
	surface.depthFormat		= GCM_SURFACE_ZETA_Z24S8;
	surface.depthLocation	= GCM_LOCATION_RSX;
	surface.depthOffset		= depth_offset;
	surface.depthPitch		= depth_pitch;

	surface.type				= GCM_SURFACE_TYPE_LINEAR;
	surface.antiAlias		= GCM_SURFACE_CENTER_1; // no multisampling

	// Full-screen surface, no offset.
	surface.width			= display_width;
	surface.height			= display_height;
	surface.x				= 0;
	surface.y				= 0;

}

// Points the render target at a different color buffer (by index) and
// tells the RSX to use it. Called after each flip so the next frame is
// drawn into the buffer that's no longer being displayed.
void setRenderTarget(u32 index)
{
	surface.colorOffset[0]	= color_offset[index]; // just swap the color buffer...
	rsxSetSurface(gGcmContext,&surface);            // ...and re-submit the surface to the RSX
}

// One-time RSX/display bring-up, called once at program start. Initializes
// the RSX command buffer, picks a video mode, allocates the color buffers
// (one per FRAME_BUFFER_COUNT) and a shared depth buffer, and sets up the
// flip-tracking labels/events so flip() and the vblank handler know which
// buffer is on screen. Must run before any other RSX drawing call.
void initScreen()
{
    u32 zs_depth = 4;   // bytes per pixel in the depth buffer
    u32 color_depth = 4; // bytes per pixel in the color buffer (XRGB)
    u32 bufferSize = rsxAlign(HOST_ADDR_ALIGNMENT, (DEFAULT_CB_SIZE + HOST_SIZE));

    gcmInitDefaultFifoMode(GCM_DEFAULT_FIFO_MODE_CONDITIONAL);

    // Host-side (main RAM) memory backing the RSX command buffer + heap,
    // then hand it to the RSX driver to actually bring RSX up.
    void *hostAddr = memalign(HOST_ADDR_ALIGNMENT, bufferSize);
    rsxInit(nullptr, DEFAULT_CB_SIZE, bufferSize, hostAddr);

    initVideoConfiguration(); // sets display_width/display_height

	color_pitch = display_width*color_depth;
    depth_pitch = display_width*zs_depth;

    waitRSXIdle(); // make sure RSX is quiet before we start allocating

    gcmSetFlipMode(GCM_FLIP_VSYNC); // flip in sync with the display's vsync -- matches the known-working rsxtest sample

    // Allocate one color buffer per ring-buffer slot in RSX-local video
    // memory, convert each to an RSX offset, and register it with GCM as
    // a possible display buffer. The raw pointer is also kept around
    // (color_buffer[]) so the CPU can write pixels into it directly --
    // this is PSL1GHT's documented approach (see rsx.h's "Write the
    // pixel data to the buffer which is not being displayed").
    void *buffer;
    for (u32 i=0;i < FRAME_BUFFER_COUNT;i++) {
        buffer = rsxMemalign(64,(display_height*color_pitch));
        color_buffer[i] = (u32*) buffer;
        rsxAddressToOffset(buffer,&color_offset[i]);
        gcmSetDisplayBuffer(i,color_offset[i],color_pitch,display_width,display_height);
    }

    // Single depth buffer shared by all color buffers (double-sized, as
    // is customary for the Z16 format used here).
    buffer = rsxMemalign(64,(display_height*depth_pitch)*2);
    rsxAddressToOffset(buffer, &depth_offset);

    // All buffers start idle/available except the one currently on screen.
    for (u32 i=0;i < FRAME_BUFFER_COUNT;i++) {
        *((vu32*) gcmGetLabelAddress(GCM_BUFFER_STATUS_INDEX + i)) = BUFFER_IDLE;
    }
    *((vu32*) gcmGetLabelAddress(GCM_PREPARED_BUFFER_INDEX)) = (fbOnDisplay << 8);
    *((vu32*) gcmGetLabelAddress(GCM_BUFFER_STATUS_INDEX + fbOnDisplay)) = BUFFER_BUSY;

    curr_fb = (fbOnDisplay + 1)%FRAME_BUFFER_COUNT; // next buffer we'll render into

    initFlipEvent();    // wire up flip/vblank callbacks
    initRenderTarget(); // point the RSX at curr_fb to start drawing

    rsxSetWriteCommandLabel(gGcmContext, GCM_BUFFER_STATUS_INDEX + curr_fb, BUFFER_BUSY);
}

// Presents the buffer that was just rendered to (queues it for display on
// the next vblank), waits if the PPU has gotten too far ahead of the RSX,
// then advances to the next buffer in the ring and points the render
// target at it so the caller can start drawing the following frame.
void flip()
{
    // Ask GCM for a flip "queue id" for curr_fb; retry if none is free yet.
    s32 qid = gcmSetPrepareFlip(gGcmContext, curr_fb);
    while (qid < 0) {
        usleep(100);
        qid = gcmSetPrepareFlip(gGcmContext, curr_fb);
    }

    // Record which buffer + queue id was requested (vblankHandler reads
    // this back), then push the flip command out to the RSX.
    rsxSetWriteBackendLabel(gGcmContext, GCM_PREPARED_BUFFER_INDEX, ((curr_fb << 8) | qid));
    rsxFlushBuffer(gGcmContext);

    syncPPUGPU(); // don't let the CPU get too far ahead of the GPU

    curr_fb = (curr_fb + 1)%FRAME_BUFFER_COUNT; // move to the next ring slot

    // Make the RSX itself wait until that next buffer is actually idle
    // (i.e. no longer being scanned out) before it's marked busy again.
    rsxSetWaitLabel(gGcmContext, GCM_BUFFER_STATUS_INDEX + curr_fb, BUFFER_IDLE);
    rsxSetWriteCommandLabel(gGcmContext, GCM_BUFFER_STATUS_INDEX + curr_fb, BUFFER_BUSY);

    setRenderTarget(curr_fb); // caller can now draw the next frame into it
}

// Cleanly shuts down rendering: flushes any remaining RSX commands and
// waits until the last-queued buffer has actually been displayed, so the
// program doesn't exit while the RSX is still mid-frame. Called from the
// exit callback in main.cpp.
void finish()
{
	rsxFinish(gGcmContext,1); // flush + wait for the RSX to drain its queue

    // Wait until the last buffer we asked to flip to has actually become
    // the one on display, so we don't tear down RSX mid-flip.
    u32 data = *((vu32*) gcmGetLabelAddress(GCM_PREPARED_BUFFER_INDEX));
    u32 lastBuffer = (data >> 8);
    while (lastBuffer != fbOnDisplay)
        usleep(100);
}
