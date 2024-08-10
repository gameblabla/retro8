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
#include <SDL/SDL.h>
#include <dc/pvr.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

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
	for(i=0;i<16;i++)
	{
		pvr_set_pal_entry(entry+(16*i), pal_rgb[entry]);
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
    front_tex = pvr_mem_malloc(128*128);
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
    pvr_vertex_t* vert;
#ifdef _8BPP
	// This is much slower because of the fact it has to be twiddled
	uint_fast8_t x, y;
	uint16 *vtex = (uint16*)front_tex;
	for (y=0; y<128; y += 2)
	{
		for (x=0; x<128; x++)
		{
			uint_fast32_t tmp = MATH_Fast_Divide(y&127, 2);
			uint_fast32_t x_ = MATH_Fast_Divide(x, 128);
			uint_fast32_t y_ = MATH_Fast_Divide(y, 128);
			vtex[TWIDOUT(tmp, x&127) + (x_ + y_)*128*128/2] = mem[y*128+x] | (mem[(y+1)*128+x]<<8);
		}
	}
	pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_PAL8BPP| PVR_TXRFMT_8BPP_PAL(0)|PVR_TXRFMT_TWIDDLED|PVR_TXRFMT_NOSTRIDE|PVR_TXRFMT_VQ_DISABLE, 128, 128, front_tex, PVR_FILTER_NONE);
#else
	dcache_inval_range((ptr_t)(mem), 128 * 128 * 2);
	pvr_txr_load_dma(mem, front_tex, 128*128*2, 1, NULL, 0);
	pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565|PVR_TXRFMT_NONTWIDDLED|PVR_TXRFMT_NOSTRIDE|PVR_TXRFMT_VQ_DISABLE, 128, 128, front_tex, PVR_FILTER_NONE);
#endif

    pvr_dr_state_t dr_state;
    
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));
    pvr_dr_init(&dr_state);

    // Vertex 1
    vert = pvr_dr_target(dr_state);
    vert->argb = PVR_PACK_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
    vert->oargb = 0;
    vert->flags = PVR_CMD_VERTEX;
    vert->x = 80;
    vert->y = 1;
    vert->z = 1;
    vert->u = 0.0f;
    vert->v = 0.0f;
    pvr_dr_commit(vert);

    // Vertex 2
    vert = pvr_dr_target(dr_state);
    vert->argb = PVR_PACK_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
    vert->oargb = 0;
    vert->flags = PVR_CMD_VERTEX;
    vert->x = 560;
    vert->y = 1;
    vert->z = 1;
    vert->u = 1.0;
    vert->v = 0.0f;
    pvr_dr_commit(vert);

    // Vertex 3
    vert = pvr_dr_target(dr_state);
    vert->argb = PVR_PACK_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
    vert->oargb = 0;
    vert->flags = PVR_CMD_VERTEX;
    vert->x = 80;
    vert->y = 480;
    vert->z = 1;
    vert->u = 0.0f;
    vert->v = 1.0;
    pvr_dr_commit(vert);

    // Vertex 4
    vert = pvr_dr_target(dr_state);
    vert->argb = PVR_PACK_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
    vert->oargb = 0;
    vert->flags = PVR_CMD_VERTEX_EOL;
    vert->x = 560;
    vert->y = 480;
    vert->z = 1;
    vert->u = 1.0;
    vert->v = 1.0;
    pvr_dr_commit(vert);
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
	pvr_init_params_t params = {
		/* Enable Opaque, Translucent, and Punch-Thru polygons with binsize 16 */
		{ PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0,
		PVR_BINSIZE_16 },
		
		/* Vertex buffer size 512K */
		512*1024,
		
		0, // Disable Vertex DMA
		
		0, // Disable FSAA, it doesn't really look better with Pico-8 and the integer scaling being used
		
		0
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

#ifdef SDL_SOUND_DC
void audio_callback(void* data, uint8_t* cbuffer, int length)
{
	retro8::sfx::APU* apu = static_cast<retro8::sfx::APU*>(data);
	int16_t* buffer = reinterpret_cast<int16_t*>(cbuffer);
	apu->renderSounds(buffer, length /  sizeof(int16_t));
	return;
}
#else
int length = 1;
const int SAMPLES_PER_FRAME = SAMPLE_RATE / 60;
static int16_t sound_buffer[SAMPLE_RATE * 2];
static void *sound_callback(snd_stream_hnd_t hnd, int len, int *samples_returned)
{
	length = len;
	machine.sound().renderSounds(sound_buffer, length /  sizeof(int16_t));
	return (int16_t *)(sound_buffer);
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
	
	#ifdef SDL_SOUND_DC
	
	#else
	snd_stream_poll(snd_dc); 
	#endif
	
	return 1;
}

  
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
	mem = (pixel_t*)memalign(32, (128 * 128));
	Set_palette();
#else
	pvr_set_pal_format(PVR_PAL_RGB565);
	mem = (pixel_t*)memalign(32, (128 * 128) * 2);
#endif

	/* TODO : Get rid of SDL dependency for Sound */
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
	#ifndef SDL_SOUND_DC
    snd_stream_init();
    snd_dc = snd_stream_alloc(sound_callback, SND_STREAM_BUFFER_MAX);
	snd_stream_start(snd_dc,44100, 0);
	snd_stream_set_callback(snd_dc, sound_callback);
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

