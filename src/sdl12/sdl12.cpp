#include "common.h"
#include "vm/gfx.h"

#include "io/loader.h"
#include "io/stegano.h"
#include "vm/machine.h"
#include "vm/input.h"

#include <cstring>
#include <stdint.h>
#include <future>
#include <SDL/SDL.h>

namespace r8 = retro8;
using pixel_t =
#ifdef _8BPP
uint8_t;
#else
uint16_t;
#endif

std::future<void> _initFuture;

uint32_t frameCounter = 0;
uint32_t lastFrameTick = 0;

constexpr int SAMPLE_RATE = 44100;

r8::Machine machine;
r8::io::Loader loader;

r8::input::InputManager input;
r8::gfx::ColorTable colorTable;
int16_t* audioBuffer;

#ifdef SDL_TRIPLEBUF
#define SDL_FLAGS SDL_HWSURFACE | SDL_TRIPLEBUF
#else
#define SDL_FLAGS SDL_HWSURFACE | SDL_DOUBLEBUF
#endif

#ifndef IPU_SCALING
SDL_Surface *real_screen;
#endif
SDL_Surface *sdl_screen;

SDL_Color colors[256];

struct ColorMapper
{
	r8::gfx::ColorTable::pixel_t operator()(uint8_t r, uint8_t g, uint8_t b) const
	{
		return SDL_MapRGB(sdl_screen->format, r, g, b);
	}
};

#ifdef _8BPP

static void Set_Pal_col(uint8_t r, uint8_t g, uint8_t b, uint16_t entry)
{
	if (entry > 255) return;
	colors[entry].r = r;
	colors[entry].g = g;
	colors[entry].b = b;
}

void Set_palette(void)
{
	Set_Pal_col(0, 0, 0, 0);
	Set_Pal_col(11, 17, 32, 1);
	Set_Pal_col(49, 14, 32, 2);
	Set_Pal_col(0, 60, 32, 3);
		
	Set_Pal_col(67, 32, 21, 4);
	Set_Pal_col(37, 34, 31, 5);
	Set_Pal_col(76, 76, 78, 6);
	Set_Pal_col(100, 94, 94, 7);
		
	Set_Pal_col(100, 0, 32, 8);
	Set_Pal_col(100, 64, 0, 9);
	Set_Pal_col(100, 92, 15, 10);
	Set_Pal_col(0, 89, 21, 11);
		
	Set_Pal_col(16, 68, 100, 12);
		
	Set_Pal_col(51, 46, 61, 13);
	Set_Pal_col(100, 47, 65, 14);
	Set_Pal_col(100, 80, 67, 15);

	SDL_SetPalette(sdl_screen, SDL_LOGPAL|SDL_PHYSPAL, colors, 0, 16);
}

#endif

uint32_t Platform::getTicks() { return SDL_GetTicks(); }

void deinit()
{
	if (sdl_screen) SDL_FreeSurface(sdl_screen);
#ifndef IPU_SCALING
	if (real_screen) SDL_FreeSurface(real_screen);
#endif
	delete[] audioBuffer;
	//TODO: release all structures bound to Lua etc
}


void flip_screen()
{
	int fps = machine.code().require60fps() ? 60 : 30;
	auto* data = machine.memory().screenData();
	auto* screenPalette = machine.memory().paletteAt(r8::gfx::SCREEN_PALETTE_INDEX);
	auto output = static_cast<pixel_t*>(sdl_screen->pixels);

	for (size_t i = 0; i < r8::gfx::BYTES_PER_SCREEN; ++i)
	{
		const r8::gfx::color_byte_t* pixels = data + i;
		const auto rc1 = colorTable.get(screenPalette->get((pixels)->low()));
		const auto rc2 = colorTable.get(screenPalette->get((pixels)->high()));

		*(output) = rc1;
		*((output)+1) = rc2;
		(output) += 2;
	}

	#ifndef IPU_SCALING
	SDL_SoftStretch(sdl_screen, NULL, real_screen, NULL);
	SDL_Flip(real_screen);
	#else
	SDL_Flip(sdl_screen);
	#endif
	++frameCounter;

	/* Of course this assumes that the actual screen refresh rate is 60 or close to it.
	 * Most games are 30 FPS though. I'm not aware of any 60 fps games aside from one
	 * which only works on Picolove. - Gameblabla */
	#ifdef SDL_TRIPLEBUF
	if (fps != 60) {
	#endif
	//SDL_Delay(1000.0/fps);
	uint32_t now = SDL_GetTicks();
	if (now - lastFrameTick < 1000.0/fps)
		SDL_Delay(1000.0/fps - (now - lastFrameTick));
	lastFrameTick = SDL_GetTicks();
	//while((1000/fps) > SDL_GetTicks()-start)SDL_Delay((1000/fps)-(SDL_GetTicks()-start));
	#ifdef SDL_TRIPLEBUF
	}
	#endif
}


bool load_game(char* rom_name)
{
	size_t sz = 0;
	FILE* fp;
	uint8_t* bdata;

	input.reset();
	machine.setflip(flip_screen);
	machine.sound().init();
	frameCounter = 0;
	
	if (strstr(rom_name, ".PNG") || strstr(rom_name, ".png"))
	{
		fp = fopen(rom_name, "rb");
		if (!fp)
			return false;

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


uint_fast8_t retro_run()
{
	Uint32 start;
	SDL_Event Event;

	/* manage input */
	while (SDL_PollEvent(&Event))
    {
		switch(Event.key.keysym.sym)
		{
			case SDLK_LEFT:
				input.manageKey(0, 0, Event.type == SDL_KEYDOWN);
			break;
			case SDLK_RIGHT:
				input.manageKey(0, 1, Event.type == SDL_KEYDOWN);
			break;
			case SDLK_UP:
				input.manageKey(0, 2, Event.type == SDL_KEYDOWN);
			break;
			case SDLK_DOWN:
				input.manageKey(0, 3, Event.type == SDL_KEYDOWN);
			break;

			case SDLK_z:
			case SDLK_LCTRL:
				input.manageKey(0, 4, Event.type == SDL_KEYDOWN);
			break;

			case SDLK_x:
			case SDLK_LALT:
				input.manageKey(0, 5, Event.type == SDL_KEYDOWN);
			break;

			case SDLK_a:
			case SDLK_SPACE:
				input.manageKey(1, 4, Event.type == SDL_KEYDOWN);
			break;

			case SDLK_s:
			case SDLK_LSHIFT:
				input.manageKey(1, 5, Event.type == SDL_KEYDOWN);
			break;
			case SDLK_ESCAPE:
				return 0;
			break;
			default:
			break;
		}
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
	int res = 0, while_res = 1;
	audioBuffer = new int16_t[SAMPLE_RATE * 2];
	
	SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO);
	SDL_ShowCursor(0);
	#ifdef IPU_SCALING
	sdl_screen = SDL_SetVideoMode(128, 128, sizeof(pixel_t) * 8, SDL_FLAGS
#ifdef RS90
	| SDL_YUV444
#endif
	);
	#ifdef _8BPP
	Set_palette();
	#endif
	#else
	sdl_screen = SDL_CreateRGBSurface(SDL_HWSURFACE, 128, 128, sizeof(pixel_t) * 8, 0,0,0,0);
	real_screen = SDL_SetVideoMode(320, 240, sizeof(pixel_t) * 8, SDL_FLAGS);
	if (!real_screen)
	{
		real_screen = SDL_SetVideoMode(240, 160, sizeof(pixel_t) * 8, SDL_FLAGS);
		if (!real_screen)
		{
			return 0;
		}
	}
	#endif
	
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
	
	if (argc != 2) {
		printf("Usage: %s GAME_ROM\n", argv[0]);
		return 1;
	}
	res = load_game(argv[1]);
	if (!res)
	{
		printf("Could not load game '%s'!\n", argv[1]);
		return 0;
	}
		
	while(while_res)
	{
		while_res = retro_run();
	}
	
	SDL_PauseAudio(1);
	deinit();
	SDL_Quit();
		
	return 0;
}

