#include "common.h"
#include "vm/gfx.h"

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
pixel_t* targetdata;

uint32_t frameCounter = 0;
uint32_t lastFrameTick = 0;

constexpr int SAMPLE_RATE = 44100;

r8::Machine machine;
r8::io::Loader loader;

r8::input::InputManager input;
r8::gfx::ColorTable colorTable;
int16_t* audioBuffer;

static unsigned start = timer_ms_gettime64();


#ifdef _8BPP

#define PACK_ARGB8888(a,r,g,b) ( ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF) )

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
				return pal_rgb[i];
			}
		}
		return 0;
	}
};

static void Set_Pal_col(uint8_t r, uint8_t g, uint8_t b, uint16_t entry)
{
	pal_rgb[entry] = PACK_ARGB8888(255,r,g,b);
	pal_r[entry] = r;
	pal_g[entry] = g;
	pal_b[entry] = b;
	pvr_set_pal_entry(entry, pal_rgb[entry]);
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

void deinit()
{
	delete[] audioBuffer;
	//TODO: release all structures bound to Lua etc
}

//**/


pvr_ptr_t back_tex;
pvr_ptr_t front_tex;
pvr_ptr_t tmp_tex;
void back_init()
{
#ifdef _8BPP
    back_tex = pvr_mem_malloc(128*128);
    front_tex = pvr_mem_malloc(128*128);
    tmp_tex = pvr_mem_malloc(128*128);
#else
    back_tex = pvr_mem_malloc(128*128*2);
    front_tex = pvr_mem_malloc(128*128*2);
#endif
}

/* draw background */
void draw_back()
{
#ifdef _8BPP
    pvr_txr_load_dma(mem, tmp_tex, 128*128, 1, NULL, NULL);
	pvr_txr_load_ex(tmp_tex, front_tex, 128, 128, PVR_TXRLOAD_8BPP);
#else
    pvr_txr_load_dma(mem, tmp_tex, 128*128*2, 1, NULL, NULL);
#endif
	//pvr_txr_load (mem, front_tex, 128*128*2);    
    
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t vert;
   
    //PVR_TXRFMT_PAL8BPP
    #ifdef _8BPP
	pvr_set_pal_format(PVR_TXRFMT_PAL8BPP);

    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_PAL8BPP|PVR_TXRFMT_TWIDDLED|PVR_TXRFMT_VQ_DISABLE, 128, 128, front_tex, PVR_FILTER_BILINEAR);
    #else
	pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565|PVR_TXRFMT_NONTWIDDLED|PVR_TXRFMT_NOSTRIDE|PVR_TXRFMT_VQ_DISABLE, 128, 128, front_tex, PVR_FILTER_BILINEAR);
	#endif
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    vert.argb = PVR_PACK_COLOR(1.0f, 1.0f, 1.0f, 1.0f);    
    vert.oargb = 0;
    vert.flags = PVR_CMD_VERTEX;
    
    vert.x = 1;
    vert.y = 1;
    vert.z = 1;
    vert.u = 0.0;
    vert.v = 0.0;
    pvr_prim(&vert, sizeof(vert));
    
    vert.x = 640;
    vert.y = 1;
    vert.z = 1;
    vert.u = 1.0;
    vert.v = 0.0;
    pvr_prim(&vert, sizeof(vert));
    
    vert.x = 1;
    vert.y = 480;
    vert.z = 1;
    vert.u = 0.0;
    vert.v = 1.0;
    pvr_prim(&vert, sizeof(vert));
    
    vert.x = 640;
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
    /* init kos  */
    pvr_init_defaults();
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

	++frameCounter;

	/* Of course this assumes that the actual screen refresh rate is 60 or close to it.
	 * Most games are 30 FPS though. I'm not aware of any 60 fps games aside from one
	 * which only works on Picolove. - Gameblabla */
	if (fps != 60)
	{
		uint32_t now = timer_ms_gettime64()-start;
		if (now - lastFrameTick < 1000.0/fps)
			thd_sleep(1000.0/fps - (now - lastFrameTick));
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
	frameCounter = 0;
	
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
		stegano.load({ reinterpret_cast<const uint32_t*>(out.data()), nullptr, out.size() / 4 }, machine);
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


void audio_callback(void* data, uint8_t* cbuffer, int length)
{
	retro8::sfx::APU* apu = static_cast<retro8::sfx::APU*>(data);
	int16_t* buffer = reinterpret_cast<int16_t*>(cbuffer);
	apu->renderSounds(buffer, length / sizeof(int16_t));
	return;
}


static int16_t sound_buffer[SAMPLE_RATE * 4];
static void *sound_callback(snd_stream_hnd_t hnd, int len, int *actual)
{
	machine.sound().renderSounds(sound_buffer, 2048);
	return (int16_t *)(sound_buffer);
}

uint_fast8_t retro_run()
{
	start = timer_ms_gettime64();

	int i;
    maple_device_t *cont;
	cont_state_t *state;
	
	int ret=0;		
	cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
		
	if(cont)
	{
		state = (cont_state_t *)maple_dev_status(cont);
		if (!state)
		ret = 0;
		
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

  
int main(int argc, char* argv[])
{
	snd_stream_hnd_t snd_dc = -1;
	int res = 0, while_res = 1;
	
#ifdef _8BPP
	mem = (pixel_t*)memalign(32, (128 * 128));
	targetdata = (pixel_t*)memalign(32, (128 * 128));
	pvr_set_pal_format(PVR_TXRFMT_PAL8BPP);
	Set_palette();
#else
	mem = (pixel_t*)memalign(32, (128 * 128) * 2);
#endif
	pvr_setup();

	/* TODO : Get rid of SDL dependency for Sound */
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

    /*snd_stream_init();
    snd_dc = snd_stream_alloc(sound_callback, SAMPLE_RATE * 4);
	snd_stream_start(snd_dc,44100, 0);*/
		
	while(while_res)
	{
		while_res = retro_run();
	}
	
	SDL_PauseAudio(1);
	deinit();
	SDL_Quit();
		
	return 0;
}

