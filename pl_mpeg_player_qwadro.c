/*
PL_MPEG Example - Video player using SDL2/OpenGL for rendering
SPDX-License-Identifier: MIT

Dominic Szablewski - https://phoboslab.org


-- Usage

pl_mpeg_player_gl <video-file.mpg>

Use the arrow keys to seek forward/backward by 3 seconds. Click anywhere on the
window to seek to seek through the whole file.


-- About

This program demonstrates a simple video/audio player using plmpeg for decoding
and SDL2 with OpenGL for rendering and sound output. It was tested on Windows
using Microsoft Visual Studio 2015 and on macOS using XCode 10.2

This program can be configured to either convert the raw YCrCb data to RGB on
the GPU (default), or to do it on CPU. Just pass APP_TEXTURE_MODE_RGB to
app_create() to switch to do the conversion on the CPU.

YCrCb->RGB conversion on the CPU is a very costly operation and should be
avoided if possible. It easily takes as much time as all other mpeg1 decoding
steps combined.

*/

#include <stdlib.h>
#include <stdio.h>

#include "qwadro/afxQwadro.h"

afxDrawSystem dsys;
afxMixSystem msys;
afxEnvironment env;

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"


#define APP_SHADER_SOURCE(...) #__VA_ARGS__

afxString const APP_VERTEX_SHADER = AFX_STATIC_STRING_R(
    PUSH(pushable)
    {
        vec2 texture_crop_size;
    }; 
	OUT(0, vec2, tex_coord);

    const vec4 gsTriPos[3] = vec4[](vec4(-1, -1, 0, 1), vec4(3, -1, 0, 1), vec4(-1, 3, 0, 1));
    const vec2 gsTriUv[3] = vec2[](vec2(0, 0), vec2(2, 0), vec2(0, 2));

    void main()
    {
        // draw a full coverage triangle (3 indices). AfxDraw(3, 1, 0, 0)
        gl_Position = gsTriPos[gl_VertexID];
        tex_coord = 0.5 * vec2(gl_Position.x * texture_crop_size.x, gl_Position.y * texture_crop_size.y * -1.0) + vec2(0.5);
    }
#if 0
    IN(0, vec2, vertex);

	void main() {
		tex_coord = vertex * texture_crop_size;
		gl_Position = vec4((vertex * 2.0 - 1.0) * vec2(1, -1), 0.0, 1.0);
	}
#endif
);

afxString const APP_FRAGMENT_SHADER_YCRCB = AFX_STATIC_STRING_R(
    TEXTURE(0, 0, sampler2D, texture_y); 
    TEXTURE(0, 1, sampler2D, texture_cb); 
    TEXTURE(0, 2, sampler2D, texture_cr); 
	IN(0, vec2, tex_coord); 
    OUT(0, vec4, rgbaOut); 

	mat4 rec601 = mat4(
		1.16438,  0.00000,  1.59603, -0.87079,
		1.16438, -0.39176, -0.81297,  0.52959,
		1.16438,  2.01723,  0.00000, -1.08139,
		0, 0, 0, 1
	);
	  
	void main() {
		float y = texture(texture_y, tex_coord).r;
		float cb = texture(texture_cb, tex_coord).r;
		float cr = texture(texture_cr, tex_coord).r;

        rgbaOut = vec4(y, cb, cr, 1.0) * rec601;
	}
);

afxString const APP_FRAGMENT_SHADER_RGB = AFX_STATIC_STRING_R(
	TEXTURE(0, 0, sampler2D, texture_rgb); 
	IN(0, vec2, tex_coord); 
    OUT(0, vec4, rgbaOut); 

	void main() {
        rgbaOut = vec4(texture(texture_rgb, tex_coord).rgb, 1.0);
	}
);

#undef APP_SHADER_SOURCE

#define APP_TEXTURE_MODE_YCRCB 1
#define APP_TEXTURE_MODE_RGB 2

typedef struct {
	plm_t *plm;
    afxClock lastClock;
	double last_time;
	int wants_to_quit;
	
	afxWindow window;
    afxSurface dout;
	afxSink audio_device;
	
    avxPipeline yuv2rgbPip;
    avxSampler sampler;
	
	int texture_mode;
	avxRaster texture_y;
    avxRaster texture_cb;
    avxRaster texture_cr;
	//GLuint texture_crop_size;
	
    avxRaster texture_rgb;
	uint8_t *rgb_data;

    afxDrawContext contexts[3];

    afxSize stageBufSiz;
    afxSize rasUnpakOff[3];
    afxSize rasUnpakSiz[3];

    afxReal lastFrameCw;
    afxReal lastFrameCh;
} app_t;

app_t * app_create(const char *filename, int texture_mode);
void app_update(app_t *self);
void app_destroy(app_t *self);

void app_on_video(plm_t *player, plm_frame_t *frame, void *user);
void app_on_audio(plm_t *player, plm_samples_t *samples, void *user);



app_t * app_create(const char *filename, int texture_mode) {

    afxError err = { 0 };

	app_t *self = (app_t *)malloc(sizeof(app_t));
	memset(self, 0, sizeof(app_t));
	
	self->texture_mode = texture_mode;
	
	// Initialize plmpeg, load the video file, install decode callbacks
	self->plm = plm_create_with_filename(filename);
	if (!self->plm) {
        AfxReportComment("Couldn't open %s", filename);
		exit(1);
	}

	if (!plm_probe(self->plm, 5000 * 1024)) {
        AfxReportComment("No MPEG video or audio streams found in %s", filename);
		exit(1);
	}

	int samplerate = plm_get_samplerate(self->plm);

	AfxReportComment(
		"Opened %s - framerate: %f, samplerate: %d, duration: %f",
		filename, 
		plm_get_framerate(self->plm),
		plm_get_samplerate(self->plm),
		plm_get_duration(self->plm)
	);
	
	plm_set_video_decode_callback(self->plm, app_on_video, self);
	plm_set_audio_decode_callback(self->plm, app_on_audio, self);
	
	plm_set_loop(self->plm, TRUE);
	plm_set_audio_enabled(self->plm, TRUE);
	plm_set_audio_stream(self->plm, 0);

	if (plm_get_num_audio_streams(self->plm) > 0) {
		// Initialize SDL Audio
#if 0
		SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
		SDL_AudioSpec audio_spec;
		SDL_memset(&audio_spec, 0, sizeof(audio_spec));
		audio_spec.freq = samplerate;
		audio_spec.format = AUDIO_F32;
		audio_spec.channels = 2;
		audio_spec.samples = 4096;

        self->audio_device = SDL_OpenAudioDevice(NULL, 0, &audio_spec, NULL, 0);
		if (self->audio_device == 0) {
            AfxReportComment("Failed to open audio device: %s", SDL_GetError());
		}
		SDL_PauseAudioDevice(self->audio_device, 0);
        // Adjust the audio lead time according to the audio_spec buffer size
        plm_set_audio_lead_time(self->plm, (double)audio_spec.samples / (double)samplerate);
#endif

        afxSink sink;
        afxSinkConfig sinkCfg = { 0 };
        sinkCfg.latency = 3;
        sinkCfg.samplesPerFrame = 4096;
        sinkCfg.freq = samplerate;
        sinkCfg.fmt = amxFormat_S32f;
        sinkCfg.chanCnt = 2;
        AfxOpenAudioSink(msys, &sinkCfg, &sink);

        self->audio_device = sink;

        // Adjust the audio lead time according to the audio_spec buffer size
        plm_set_audio_lead_time(self->plm, (double)sinkCfg.samplesPerFrame / (double)samplerate);
    }
	
	// Create MMUX Window

    afxWindow wnd;
    afxWindowConfig wcfg = { 0 };
    wcfg.dout.dsys = dsys;
    wcfg.dout.ccfg.whd.w = plm_get_width(self->plm);
    wcfg.dout.ccfg.whd.h = plm_get_height(self->plm);
    wcfg.dout.ccfg.bins[0].fmt = avxFormat_BGRA8v;
    wcfg.dout.latency = 3;
    AfxConfigureWindow(env, &wcfg, NIL, NIL);
    AfxAcquireWindow(env, &wcfg, &wnd);
    AFX_ASSERT_OBJECTS(afxFcc_WND, 1, &wnd);
    AfxAdjustWindow(wnd, NIL, 0, NIL, &AFX_RECT(0, 0, wcfg.dout.ccfg.whd.w, wcfg.dout.ccfg.whd.h));

    afxSurface dout;
    AfxGetWindowSurface(wnd, &dout);
    AFX_ASSERT_OBJECTS(afxFcc_DOUT, 1, &dout);

    self->window = wnd;
    self->dout = dout;
	
	// Setup OpenGL shaders and textures
	const afxString * fsh = self->texture_mode == APP_TEXTURE_MODE_YCRCB
		? &APP_FRAGMENT_SHADER_YCRCB
		: &APP_FRAGMENT_SHADER_RGB;
	
    avxShader codb;
    AvxAcquireShaders(dsys, 1, NIL, &codb);
    AFX_ASSERT_OBJECTS(afxFcc_SHD, 1, &codb);

    avxShaderSpecialization specs[2] = { 0 };
    specs[0].stage = avxShaderType_VERTEX;
    specs[0].prog = AFX_STRING("vsh");
    specs[1].stage = avxShaderType_FRAGMENT;
    specs[1].prog = AFX_STRING("fsh");
    AvxCompileShader(codb, &specs[0].prog, &APP_VERTEX_SHADER);
    AvxCompileShader(codb, &specs[1].prog, fsh);

    avxPipeline pip;
    avxPipelineConfig pipb = { 0 };
    pipb.codb = codb;
    pipb.progCnt = 2;
    pipb.progSpecs = specs;
    pipb.cullMode = avxCullMode_BACK;
    pipb.primTop = avxTopology_TRI_LIST;
    pipb.fillMode = avxFillMode_FACE;
    AvxAssembleGfxPipelines(dsys, 1, &pipb, &pip);
    AFX_ASSERT_OBJECTS(afxFcc_PIP, 1, &pip);

    AfxDisposeObjects(1, &codb);

    self->yuv2rgbPip = pip;


    avxSamplerConfig smpSpec = { 0 };
    smpSpec.magnify = (smpSpec.minify = avxTexelFilter_LINEAR);
    smpSpec.mipFlt = avxTexelFilter_LINEAR;
    smpSpec.uvw[0] = (smpSpec.uvw[1] = (smpSpec.uvw[2] = avxTexelWrap_EDGE));
    //smpSpec.uvw[1] = avxTexelAddress_REPEAT;
    //smpSpec.uvw[2] = avxTexelAddress_REPEAT;

    AvxAcquireSamplers(dsys, 1, &smpSpec, &self->sampler);
    AFX_ASSERT_OBJECTS(afxFcc_SAMP, 1, &self->sampler);

    // Create textures for YCrCb or RGB rendering
    if (self->texture_mode == APP_TEXTURE_MODE_YCRCB) {
        //self->texture_y = app_create_texture(self, 0, "texture_y");
        //self->texture_cb = app_create_texture(self, 1, "texture_cb");
        //self->texture_cr = app_create_texture(self, 2, "texture_cr");


        avxFormatDescription pfd;
        AvxDescribeFormats(1, (avxFormat[]) { avxFormat_R8un }, &pfd);
        avxRasterInfo texi[3] = { 0 };
        texi[0].whd.w = plm_get_width(self->plm);
        texi[1].whd.w = plm_get_width(self->plm) / 2;
        texi[2].whd.w = plm_get_width(self->plm) / 2;
        texi[0].whd.h = plm_get_height(self->plm);
        texi[1].whd.h = plm_get_height(self->plm) / 2;
        texi[2].whd.h = plm_get_height(self->plm) / 2;
        texi[0].whd.d = 1;
        texi[1].whd.d = 1;
        texi[2].whd.d = 1;
        texi[0].fmt = avxFormat_R8un;
        texi[1].fmt = avxFormat_R8un;
        texi[2].fmt = avxFormat_R8un;
        texi[0].usage = avxRasterUsage_TEXTURE;
        texi[1].usage = avxRasterUsage_TEXTURE;
        texi[2].usage = avxRasterUsage_TEXTURE;

        afxUnit rasCnt = 3;

        for (afxUnit i = 0; i < 1; ++i)
        {
            if (AfxFailed(AvxAcquireRasters(dsys, rasCnt, texi, &self->texture_y)))
                AfxThrowError();
        }
    }
    else {
        //self->texture_rgb = app_create_texture(self, 0, "texture_rgb");
        int num_pixels = plm_get_width(self->plm) * plm_get_height(self->plm);
        self->rgb_data = (uint8_t*)malloc(num_pixels * 3);

        avxRasterInfo texi = { 0 };
        texi.whd.w = plm_get_width(self->plm);
        texi.whd.h = plm_get_height(self->plm);
        texi.whd.d = 1;
        texi.fmt = avxFormat_RGBA8un;
        texi.usage = avxRasterUsage_TEXTURE;

        afxUnit rasCnt = 1;

        for (afxUnit i = 0; i < 1; ++i)
        {
            if (AfxFailed(AvxAcquireRasters(dsys, rasCnt, &texi, &self->texture_y)))
                AfxThrowError();
        }
    }
    //self->texture_crop_size = glGetUniformLocation(self->shader_program, "texture_crop_size");

    avxContextConfig dctxi = { 0 };
    dctxi.caps = avxAptitude_GFX;
    if (AfxFailed(AvxAcquireDrawContexts(dsys, NIL, &dctxi, ARRAY_SIZE(self->contexts), self->contexts)))
    {
        AfxThrowError();
    }

    AfxGetClock(&self->lastClock);

	return self;
}

void app_destroy(app_t *self) {
	plm_destroy(self->plm);
	
	if (self->texture_mode == APP_TEXTURE_MODE_RGB) {
		free(self->rgb_data);
	}

	if (self->audio_device) {
		//SDL_CloseAudioDevice(self->audio_device);
        AfxDisposeObjects(1, &self->audio_device);
	}
	
    AfxDisposeObjects(1, &self->yuv2rgbPip);
    AfxDisposeObjects(1, &self->window);
	
	free(self);
}

afxError DoVideo(afxSurface dout, afxUnit outBufIdx, afxDrawContext dctx, app_t *self)
{
    afxError err = AFX_ERR_NONE;

    afxUnit queIdx = 0;
    afxUnit portId = 0;

    if (AvxPrepareDrawCommands(dctx, TRUE, avxCmdFlag_ONCE))
    {
        AfxThrowError();
        return err;
    }

    avxCanvas canv;
    afxLayeredRect crc;
    AvxGetSurfaceCanvas(dout, outBufIdx, &canv, &crc);
    AFX_ASSERT_OBJECTS(afxFcc_CANV, 1, &canv);

    afxBool readjust = TRUE;
    afxBool upscale = FALSE;
    afxRect crop = AFX_RECT(crc.area.x, crc.area.y, plm_get_width(self->plm), plm_get_height(self->plm));

    if (readjust)
    {
        crop.w = crc.area.w;
        crop.h = crc.area.h;
    }

    if (!upscale)
    {
        crop.w = AFX_CLAMP(crop.w, 1, plm_get_width(self->plm));
        crop.h = AFX_CLAMP(crop.h, 1, plm_get_height(self->plm));
    }

    avxDrawScope dps = { 0 };
    dps.canv = canv;
    dps.bounds.area = crop;
    dps.bounds.layerCnt = 1;
    dps.targetCnt = 1;
    dps.targets[0].clearVal.rgba[0] = 0.3;
    dps.targets[0].clearVal.rgba[1] = 0.1;
    dps.targets[0].clearVal.rgba[2] = 0.3;
    dps.targets[0].clearVal.rgba[3] = 1;
    dps.targets[0].loadOp = avxLoadOp_CLEAR;
    dps.targets[0].storeOp = avxStoreOp_STORE;
    if (AfxSucceded(AvxCmdCommenceDrawScope(dctx, &dps)))
    {
        avxViewport vp = AVX_VIEWPORT(crop.x, crop.y, crop.w, crop.h, 0, 1);
        //AvxFlipViewport(&vp, &vp, FALSE);
        AvxCmdAdjustViewports(dctx, 0, 1, &vp);

        //AvxCmdChangeCullMode(dctx, avxCullMode_BACK);
        //AvxCmdChangeFillModeEXT(dctx, avxFillMode_SOLID);
        //AvxCmdSwitchFrontFace(dctx, FALSE);

        // turn off Z buffering, culling, and projection (since we are drawing orthographically)
        //AvxCmdSwitchDepthTesting(dctx, FALSE);

        AvxCmdBindPipeline(dctx, self->yuv2rgbPip, NIL, NIL);

        if (self->texture_mode == APP_TEXTURE_MODE_YCRCB) {

            AvxCmdBindRasters(dctx, avxBus_GFX, 0, 0, 3, (avxRaster[]) { self->texture_y, self->texture_cr, self->texture_cb });
            AvxCmdBindSamplers(dctx, avxBus_GFX, 0, 0, 3, (avxSampler[]) { self->sampler, self->sampler, self->sampler });

            //glUniform2f(self->texture_crop_size, cw, ch);
            AvxCmdPushConstants(dctx, 0, sizeof(afxV2d), AFX_V2D(self->lastFrameCw, self->lastFrameCh));
        }
        else {
            //glBindTexture(GL_TEXTURE_2D, self->texture_rgb);
            AvxCmdBindRasters(dctx, avxBus_GFX, 0, 0, 1, &self->texture_rgb);
            AvxCmdBindSamplers(dctx, avxBus_GFX, 0, 0, 1, (avxSampler[]) { self->sampler });

            //glUniform2f(self->texture_crop_size, 1.0, 1.0);
            AvxCmdPushConstants(dctx, 0, sizeof(afxV2d), AFX_V2D(self->lastFrameCw, self->lastFrameCw));
        }

        AvxCmdDraw(dctx, 3, 1, 0, 0); // fullscreen triangle in shader

        AvxCmdConcludeDrawScope(dctx);
    }

    if (AfxFailed(AvxCompileDrawCommands(dctx)))
    {
        AfxThrowError();
        return err;
    }

    avxSubmission subm = { 0 };
    avxFence dscrCompleteSem = NIL;
    subm.signal = dscrCompleteSem;
    subm.dctx = dctx;

    if (AfxFailed(AvxExecuteDrawCommands(dsys, 1, &subm, NIL)))
    {
        AfxThrowError();
        return err;
    }

    //AfxWaitForDrawQueue(dsys, subm.exuIdx, subm.baseQueIdx, 0);
    AvxWaitForDrawBridges(dsys, AFX_TIMEOUT_INFINITE, subm.exuMask);

    avxPresentation pres = { 0 };
    pres.dout = dout;
    pres.bufIdx = outBufIdx;
    if (AfxFailed(AvxPresentSurfaces(dsys, 1, &pres, NIL)))
    {
        AfxThrowError();
        return err;
    }

    return err;
}

void app_update(app_t *self) {
	double seek_to = -1;

    AfxDoUx(NIL, AFX_TIMEOUT_IGNORED);
#if 0
	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {
		if (
			ev.type == SDL_QUIT || 
			(ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_ESCAPE)
		) {
			self->wants_to_quit = TRUE;
		}
		
		if (
			ev.type == SDL_WINDOWEVENT &&
			ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED
		) {
			glViewport(0, 0, ev.window.data1, ev.window.data2);
		}

		// Seek 3sec forward/backward using arrow keys
		if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_RIGHT) {
			seek_to = plm_get_time(self->plm) + 3;
		}
		else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_LEFT) {
			seek_to = plm_get_time(self->plm) - 3;
		}
	}
#endif

	// Compute the delta time since the last app_update(), limit max step to 
	// 1/30th of a second
    
    afxClock currClock;
    AfxGetClock(&currClock);

	double current_time = (double)AfxGetSecondsElapsed(&self->lastClock, &currClock);
    self->lastClock = currClock;
    double elapsed_time = current_time;// -self->last_time;
	if (elapsed_time > 1.0 / 30.0) {
		elapsed_time = 1.0 / 30.0;
	}
	self->last_time = current_time;

	// Seek using mouse position
	afxRect cursor;
    AfxGetCursorPlacement(0, &cursor, NIL, NIL, NIL);
	if (AfxIsMousePressed(0, AFX_LMB)) {
		afxRect sr;
        AfxGetWindowRect(self->window, NIL, &sr);
		seek_to = plm_get_duration(self->plm) * ((float)cursor.x / (float)sr.w);
	}
	
	// Seek or advance decode
	if (seek_to != -1) {
		//SDL_ClearQueuedAudio(self->audio_device);
		plm_seek(self->plm, seek_to, FALSE);
	}
	else {
		plm_decode(self->plm, elapsed_time);
	}

	if (plm_has_ended(self->plm)) {
		self->wants_to_quit = TRUE;
	}
	
    afxUnit outBufIdx = 0;
    if (!AvxLockSurfaceBuffer(self->dout, AFX_TIMEOUT_IGNORED, NIL, NIL, &outBufIdx))
    {
        if (DoVideo(self->dout, outBufIdx, self->contexts[outBufIdx], self))
        {
            AvxUnlockSurfaceBuffer(self->dout, outBufIdx);
        }
    }
	//glClear(GL_COLOR_BUFFER_BIT);
	//glRectf(0.0, 0.0, 1.0, 1.0);
	//SDL_GL_SwapWindow(self->window);
}

void app_update_texture(app_t *self, afxUnit unit, avxRaster texture, plm_plane_t *plane) {
	//glActiveTexture(GL_TEXTURE0 + unit);
	//glBindTexture(GL_TEXTURE_2D, texture);
	
    //glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, plane->width, plane->height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, plane->data);

    avxRasterIo iop = { 0 };
    iop.rgn.whd.w = plane->width;
    iop.rgn.whd.h = plane->height;
    AvxUpdateRaster(texture, 1, &iop, plane->data, NIL, NIL);
}

void app_on_video(plm_t *mpeg, plm_frame_t *frame, void *user) {
	app_t *self = (app_t *)user;
	
	// Hand the decoded data over to OpenGL. For the RGB texture mode, the
	// YCrCb->RGB conversion is done on the CPU.

	if (self->texture_mode == APP_TEXTURE_MODE_YCRCB) {
		app_update_texture(self, 0, self->texture_y, &frame->y);
		app_update_texture(self, 1, self->texture_cb, &frame->cb);
		app_update_texture(self, 2, self->texture_cr, &frame->cr);

		// The dimensions of the planes are always rounded up to the next
		// multiple of 16. We don't want to display these extra pixels, so
		// calculate the crop w/h and hand it over to the shader program.
		float cw = (float)frame->width / (float)frame->y.width;
		float ch = (float)frame->height / (float)frame->y.height;

        self->lastFrameCw = cw;
        self->lastFrameCh = ch;
	}
	else {
		plm_frame_to_rgb(frame, self->rgb_data, frame->width * 3);
	
		//glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame->width, frame->height, 0, GL_RGB, GL_UNSIGNED_BYTE, self->rgb_data);

        avxRasterIo iop = { 0 };
        iop.rgn.whd.w = frame->width;
        iop.rgn.whd.h = frame->height;
        AvxUpdateRaster(self->texture_rgb, 1, &iop, self->rgb_data, NIL, NIL);

		// plm_frame_to_rgb() always returns the cropped portion of the display
		// size, so the crop size is always 1.0, 1.0
        self->lastFrameCw = 1.0;
        self->lastFrameCh = 1.0;
	}
}

void app_on_audio(plm_t *mpeg, plm_samples_t *samples, void *user) {
	app_t *self = (app_t *)user;

	// Hand the decoded samples over to SDL
	
	int size = sizeof(float) * samples->count * 2;
	//SDL_QueueAudio(self->audio_device, samples->interleaved, size);

    afxUnit sampleCnt = samples->count;
    // sampleCnt = 960;

    amxBufferedTrack room;
    if (!AmxLockSinkBuffer(self->audio_device, AFX_TIMEOUT_IGNORED, NIL, sampleCnt, &room))
    {
        //room.offset = (afxSize)samples->interleaved;
        //room.frameCnt = samples->count;
        //room.range = sampleCnt;
        room.freq = plm_get_samplerate(self->plm);
        AfxStream(sampleCnt, sizeof(samples->interleaved[0]), sizeof(samples->interleaved[0]), samples->interleaved, (void*)room.offset);

        AmxUnlockSinkBuffer(self->audio_device, NIL);
    }
}



int main(int argc, char *argv[]) {
	if (argc < 2) {
        AfxReportComment("Usage: pl_mpeg_player_gl <file.mpg>");
#if 0		
        exit(1);
#else
        // debug file

        argc+= 1;
        argv[1] = "./../test.mpg";
#endif
	}
    
    afxError err = { 0 };

    afxSystemConfig sysCfg = { 0 };
    AfxConfigureSystem(&sysCfg, NIL);
    AfxBootstrapSystem(&sysCfg);

    afxUnit avxIcd = 0;
    avxSystemConfig dsyc = { 0 };
    dsyc.caps = avxAptitude_GFX;
    dsyc.accel = afxAcceleration_DPU;
    dsyc.exuCnt = 1;
    AvxConfigureDrawSystem(avxIcd, &dsyc);
    AvxEstablishDrawSystem(avxIcd, &dsyc, &dsys);
    AFX_ASSERT_OBJECTS(afxFcc_DSYS, 1, &dsys);

    afxUnit amxIcd = 0;
    amxSystemConfig msyc = { 0 };
    msyc.caps = amxAptitude_SFX;
    msyc.accel = afxAcceleration_MPU;
    msyc.exuCnt = 1;
    AmxConfigureMixSystem(amxIcd, &msyc);
    AmxEstablishMixSystem(amxIcd, &msyc, &msys);
    AFX_ASSERT_OBJECTS(afxFcc_MSYS, 1, &msys);

    afxUnit auxIcd = 0;
    afxEnvironmentConfig ecfg = { 0 };
    ecfg.dsys = dsys;
    ecfg.msys = msys;
    AfxConfigureEnvironment(auxIcd, &ecfg);
    AfxEstablishEnvironment(auxIcd, &ecfg, &env);
    AFX_ASSERT_OBJECTS(afxFcc_ENV, 1, &env);


	app_t *app = app_create(argv[1], APP_TEXTURE_MODE_YCRCB);
	while (!app->wants_to_quit) {
		app_update(app);
	}
	app_destroy(app);

    AfxDisposeObjects(1, &env);
    AfxDisposeObjects(1, &dsys);

    AfxAbolishSystem(0);
    //Sleep(3000);

	return EXIT_SUCCESS;
}
