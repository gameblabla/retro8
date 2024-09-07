#include "common.h"
#include "vm/gfx.h"
#include "dc.h"

#include "io/loader.h"
#include "io/stegano.h"
#include "vm/machine.h"
#include "vm/input.h"

#include <cstring>
#include <stdint.h>
#include <kos.h>
#include <kos/malloc.h>
#include <SDL/SDL.h>
#include <dc/pvr.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

#define _8BPP 1
#define SDL_SOUND_DC 1

#define FRAMEBUFFER_WIDTH 128
#define FRAMEBUFFER_HEIGHT 128

//Here we define the size of a framebuffer texture we want
#define FRAMEBUFFER_PIXELS (FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT)

//The codebook for a VQ texture is always 2048 bytes
#define CODEBOOK_SIZE 2048

typedef struct {
	unsigned char codebook[CODEBOOK_SIZE];
	unsigned char texture[FRAMEBUFFER_PIXELS];
} VQ_Texture;

VQ_Texture* framebuffer;

namespace r8 = retro8;
using pixel_t =
#ifdef _8BPP
uint8_t;
#else
uint16_t;
#endif

pixel_t* mem;

uint32_t frameCounter = 0;
uint32_t lastFrameTick = 0;

constexpr int SAMPLE_RATE = 44100;

r8::Machine machine;
r8::io::Loader loader;

r8::input::InputManager input;
r8::gfx::ColorTable colorTable;

static unsigned start = timer_ms_gettime64();

unsigned char* codebook;
uint16_t* codebookEntry;
uint32_t codebookIdx;

#ifdef _8BPP

#ifdef _32BPP_PAL
#define PACK_ARGB8888(a,r,g,b) ( ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF) )
#else
#define PACK_RGB565(r,g,b) ( ((r >> 3) & 0x1f) << 11 | ((g >> 2) & 0x3f) << 5 | (b >> 3) & 0x1f )
#endif

uint32_t pal_rgb[16];
uint8_t pal_r[16];
uint8_t pal_g[16];
uint8_t pal_b[16];

struct ColorMapper
{
	r8::gfx::ColorTable::pixel_t operator()(uint8_t r, uint8_t g, uint8_t b) const
	{
		uint_fast8_t i;
		for(i=0;i<16;i++)
		{
			if (pal_r[i] == r && pal_g[i] == g && pal_b[i] == b)
			{
				return i;
			}
		}
		return 0;
	}
};

static void Set_Pal_col(uint8_t r, uint8_t g, uint8_t b, uint16_t entry)
{
	int i;
	pal_r[entry] = r;
	pal_g[entry] = g;
	pal_b[entry] = b;
	#ifdef _32BPP_PAL
	pal_rgb[entry] = PACK_ARGB8888(255,r,g,b);
	#else
	pal_rgb[entry] = PACK_RGB565(r,g,b);
	#endif
	
	codebookEntry = (uint16_t*)codebook;

    // Store the packed color value as a 16-bit (2-byte) entry
    uint16_t codebookValue = (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));

    // Now fill the codebook for this color
    for (i = 0; i < 16; i++) 
    {
        // Calculate the index for the codebook entry
        int32_t codebookIdx = (entry + (16 * i)) * 4;  // Each entry is 4 16-bit values

        // Fill all four pixels of the codebook entry with the same color value
        codebookEntry[codebookIdx + 0] = codebookValue;
        codebookEntry[codebookIdx + 1] = codebookValue;
        codebookEntry[codebookIdx + 2] = codebookValue;
        codebookEntry[codebookIdx + 3] = codebookValue;
    }
}

void Set_palette(void)
{
	uint_fast8_t i;
	for(i=0;i<16;i++)
	{
		Set_Pal_col(retro8::gfx::pico8_pal[i][0], retro8::gfx::pico8_pal[i][1], retro8::gfx::pico8_pal[i][2], i);
	}
}

#else

struct ColorMapper
{
	r8::gfx::ColorTable::pixel_t operator()(uint8_t r, uint8_t g, uint8_t b) const
	{
		uint16_t blue = (b >> 3) & 0x1f;
		uint16_t green = ((g >> 2) & 0x3f) << 5;
		uint16_t red = ((r >> 3) & 0x1f) << 11;
		return (uint16_t) (red | green | blue);
	}
};

#endif

uint32_t Platform::getTicks() { return timer_ms_gettime64()-start; }

//**/

pvr_ptr_t front_tex;
void back_init()
{
#ifdef _8BPP
    front_tex = pvr_mem_malloc(sizeof(VQ_Texture));
#else
    front_tex = pvr_mem_malloc(128*128*2);
#endif
}

/*
static semaphore_t dmadone = SEM_INITIALIZER(1);
static void dma_callback(ptr_t data __attribute__((unused)))    {
    sem_signal(&dmadone);
}*/

/* Linear/iterative twiddling algorithm from Marcus' tatest */
#define TWIDTAB(x) ( (x&1)|((x&2)<<1)|((x&4)<<2)|((x&8)<<3)|((x&16)<<4)| \
	((x&32)<<5)|((x&64)<<6)|((x&128)<<7)|((x&256)<<8)|((x&512)<<9) )
#define TWIDOUT(x, y) ( TWIDTAB((y)) | (TWIDTAB((x)) << 1) )
#define MIN(a, b) ( (a)<(b)? (a):(b) )

/* draw background */
void draw_back()
{
	pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t vert;
#ifdef _8BPP
	pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
	
	pvr_txr_load(framebuffer, front_tex, sizeof(VQ_Texture));
	
	pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_VQ_ENABLE | PVR_TXRFMT_NONTWIDDLED, FRAMEBUFFER_WIDTH * 4, FRAMEBUFFER_HEIGHT, front_tex, PVR_FILTER_BILINEAR);
#else
	dcache_inval_range((ptr_t)(mem), 128 * 128 * 2);
	pvr_txr_load_dma(mem, front_tex, 128*128*2, 1, NULL, 0);
	pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565|PVR_TXRFMT_NONTWIDDLED|PVR_TXRFMT_NOSTRIDE|PVR_TXRFMT_VQ_DISABLE, 128, 128, front_tex, PVR_FILTER_NONE);
#endif

    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    vert.argb = PVR_PACK_COLOR(1.0f, 1.0f, 1.0f, 1.0f);    
    vert.oargb = 0;
    vert.flags = PVR_CMD_VERTEX;
    
    vert.x = 80;
    vert.y = 1;
    vert.z = 1;
    vert.u = 0.0;
    vert.v = 0.0;
    pvr_prim(&vert, sizeof(vert));
    
    vert.x = 560;
    vert.y = 1;
    vert.z = 1;
    vert.u = 1.0;
    vert.v = 0.0;
    pvr_prim(&vert, sizeof(vert));
    
    vert.x = 80;
    vert.y = 480;
    vert.z = 1;
    vert.u = 0.0;
    vert.v = 1.0;
    pvr_prim(&vert, sizeof(vert));
    
    vert.x = 560;
    vert.y = 480;
    vert.z = 1;
    vert.u = 1.0;
    vert.v = 1.0;
    vert.flags = PVR_CMD_VERTEX_EOL;
    pvr_prim(&vert, sizeof(vert));
}


/* draw one frame */
void draw_frame()
{
    pvr_wait_ready();
    pvr_scene_begin();

	pvr_list_begin(PVR_LIST_OP_POLY);

	

    draw_back();

    pvr_list_finish();

    pvr_scene_finish();
     
}

void pvr_setup()
{
	pvr_init_params_t params =  {
		/* Enable opaque and translucent polygons with size 16 */
		{ PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0 },

		/* Vertex buffer size 512K */
		512 * 1024
	};

    /* init kos  */
    vid_set_mode(DM_640x480, PM_RGB565);
	pvr_init(&params);
    pvr_dma_init();
   
    /* init background */
    back_init();
}


/***/


void flip_screen()
{
	int fps = machine.code().require60fps() ? 60 : 30;
	auto* data = machine.memory().screenData();
	auto* screenPalette = machine.memory().paletteAt(r8::gfx::SCREEN_PALETTE_INDEX);
	auto output = static_cast<pixel_t*>(mem);

	for (size_t i = 0; i < r8::gfx::BYTES_PER_SCREEN; ++i)
	{
		const r8::gfx::color_byte_t* pixels = data + i;
		const auto rc1 = colorTable.get(screenPalette->get((pixels)->low()));
		const auto rc2 = colorTable.get(screenPalette->get((pixels)->high()));

		*(output) = rc1;
		*((output)+1) = rc2;
		(output) += 2;
	}

	
	draw_frame();

	/* Of course this assumes that the actual screen refresh rate is 60 or close to it.
	 * Most games are 30 FPS though. I'm not aware of any 60 fps games aside from one
	 * which only works on Picolove. - Gameblabla */
	if (fps != 60)
	{
		uint32_t now = timer_ms_gettime64()-start;
		if (now - lastFrameTick < MATH_Fast_Divide(1000.0,fps))
			thd_sleep(MATH_Fast_Divide(1000.0,fps) - (now - lastFrameTick));
		lastFrameTick = timer_ms_gettime64()-start;
	}
}


bool load_game(char* rom_name)
{
	size_t sz = 0;
	FILE* fp;
	uint8_t* bdata;
	
	fp = fopen(rom_name, "rb");
	if (!fp) return false;

	input.reset();
	machine.setflip(flip_screen);
	machine.sound().init();
	
	if (strstr(rom_name, ".PNG") || strstr(rom_name, ".png"))
	{
		fseek(fp, 0 , SEEK_END);
		sz = ftell(fp);
		fseek(fp, 0 , SEEK_SET);
		bdata = (uint8_t*)malloc(sz);
		fread(bdata, sz, 1, fp);
		fclose(fp);
		
		std::vector<uint8_t> out;
		unsigned long width, height;
		auto result = Platform::loadPNG(out, width, height, (uint8_t*)bdata, sz, true);
		assert(result == 0);
		
		if (bdata) free(bdata);
		
		r8::io::Stegano stegano;
		stegano.load({ reinterpret_cast<const uint32_t*>(out.data()), nullptr, MATH_Fast_Divide(out.size(), 4) }, machine);
	}
	else
	{
		fclose(fp);
		//TODO: not efficient since it's copied and it's not checking for '\0'
		std::string raw(rom_name);
		r8::io::Loader loader;
		loader.loadFile(raw, machine);
	}
		
	machine.memory().backupCartridge();

	machine.code().init();

	return true;
}

	snd_stream_hnd_t snd_dc = -1;


#ifndef SDL_SOUND_DC
int length = 1;
const int SAMPLES_PER_FRAME = SAMPLE_RATE / 60;
static int16_t* sound_buffer;
static unsigned buffer_frames;
static unsigned buffer_size;
static kthread_t *sound_thread;
static int sound_init = 0;

// `smp_req` and `smp_recv` are actually BYTES, not samples.
static void *sound_callback(snd_stream_hnd_t hnd, int smp_req, int *smp_recv) {

    // Calculate the number of frames requested
    size_t frames = MAX_REAL(0, smp_req) / (sizeof(int16_t) * 1); // `len` is in bytes, convert to frames

    // Ensure that we do not exceed the buffer's capacity
    if (frames > buffer_frames)
        frames = buffer_frames;

	machine.sound().renderSounds(sound_buffer, frames * sizeof(int16_t) * 1);

    // Set the number of samples returned (in bytes)
    *smp_recv = frames * sizeof(int16_t) * 1; // Convert frames back to bytes

    // Return the buffer containing the decoded audio data
    return sound_buffer;
}
#endif

uint_fast8_t retro_run()
{
	start = timer_ms_gettime64();
	int i;
    maple_device_t *cont;
	cont_state_t *state;
	
	cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
		
	if(cont)
	{
		state = (cont_state_t *)maple_dev_status(cont);
			if (state->buttons & CONT_START)
				input.manageKey(1, 4, 1);
			else
				input.manageKey(1, 4, 0);
				
			if (state->buttons & CONT_X)
				input.manageKey(1, 5, 1);
			else
				input.manageKey(1, 5, 0);
					
			if (state->buttons & CONT_A) 
				input.manageKey(0, 4, 1);
			else
				input.manageKey(0, 4, 0);
				
			if (state->buttons & CONT_B) 
				input.manageKey(0, 5, 1);
			else
				input.manageKey(0, 5, 0);
				
			if (state->buttons & CONT_DPAD_UP) 
				input.manageKey(0, 2, 1);
			else
				input.manageKey(0, 2, 0);
				
			if (state->buttons & CONT_DPAD_DOWN) 
				input.manageKey(0, 3, 1);
			else
				input.manageKey(0, 3, 0);
				
			if (state->buttons & CONT_DPAD_LEFT) 
				input.manageKey(0, 0, 1);
			else
				input.manageKey(0, 0, 0);
				
			if (state->buttons & CONT_DPAD_RIGHT) 
				input.manageKey(0, 1, 1);
			else
				input.manageKey(0, 1, 0);
				
	}

	input.tick();

	machine.code().update();
	machine.code().draw();
	
	flip_screen();
	input.manageKeyRepeat();
	
	
	return 1;
}



#ifdef SDL_SOUND_DC
void audio_callback(void* data, uint8_t* cbuffer, int length)
{
	retro8::sfx::APU* apu = static_cast<retro8::sfx::APU*>(data);
	int16_t* buffer = reinterpret_cast<int16_t*>(cbuffer);
	apu->renderSounds(buffer, DIVIDE_REAL(length ,  sizeof(int16_t)));
	return;
}
#else

static void *dc_audio_thread(void *dud)
{
    snd_stream_init();

    buffer_frames = 2048;
    buffer_size = buffer_frames * sizeof(int16_t) * 1;
    
    sound_buffer = (int16_t*)malloc(buffer_size);
    snd_dc = snd_stream_alloc(sound_callback, buffer_size);
    snd_stream_start(snd_dc, 44100, 0);
    
    sound_init = 1;
	
	while(1)
	{
		snd_stream_poll(snd_dc);
		thd_sleep(10);
	}

  snd_stream_destroy(snd_dc);
  snd_stream_shutdown();

  free(sound_buffer);

  return NULL;
}
#endif

  
int main(int argc, char* argv[])
{
	int res = 0, while_res = 1;
	pvr_setup();
#ifdef _8BPP
	#ifdef _32BPP_PAL
	pvr_set_pal_format(PVR_PAL_ARGB8888);
	#else
	pvr_set_pal_format(PVR_PAL_RGB565);
	#endif
	

	// Initialize VQ Texture First - align on a 64 byte boundary so it's storage queue movable
	framebuffer = (VQ_Texture*)aligned_alloc(64, sizeof(VQ_Texture));
	
	codebook = (unsigned char*)&(framebuffer -> codebook);
	
	mem = (unsigned char*)&(framebuffer -> texture);
	//mem = (pixel_t*)aligned_alloc(64, (128 * 128));
	Set_palette();
#else
	pvr_set_pal_format(PVR_PAL_RGB565);
	mem = (pixel_t*)aligned_alloc(32, (128 * 128) * 2);
#endif

	printf("Initializing audio buffer of %zu bytes\n", sizeof(int16_t) * SAMPLE_RATE * 2);

	colorTable.init(ColorMapper());
	machine.font().load();
	machine.code().loadAPI();
	input.setMachine(&machine);
	
	res = load_game("/cd/game.p8.png");
	if (!res)
	{
		res = load_game("/cd/game.p8");
		if (!res) return 0;
	}

	if (!res)
	{
		printf("Could not load game '%s'!\n", argv[1]);
		return 0;
	}

	#ifdef SDL_SOUND_DC
	SDL_Init(SDL_INIT_AUDIO);
	SDL_AudioSpec wantSpec, spec;
	wantSpec.freq = 44100;
	wantSpec.format = AUDIO_S16SYS;
	wantSpec.channels = 1;
	wantSpec.samples = 2048;
	wantSpec.userdata = &machine.sound();
	wantSpec.callback = audio_callback;

	SDL_OpenAudio(&wantSpec, &spec);
	SDL_PauseAudio(0);
	#else
	sound_init = 0;
    sound_thread = thd_create(0, dc_audio_thread, NULL);
    while(sound_init == 0)
    {
		
	}
	#endif
		
	while(while_res)
	{
		while_res = retro_run();
	}
	#ifdef SDL_SOUND_DC
	SDL_PauseAudio(1);
	SDL_Quit();
	#endif
		
	return 0;
}

