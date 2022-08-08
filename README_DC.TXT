Pico-8/Retro8 port by gameblabla
=================================

I've ported Retro8, a simple and portable pico-8 interpreter.
This will allow you to play most games made up until 2017 or so.
I initially wanted to port FAKE-08 instead but i encountered some complications with this one
so for now, this will do.

I've bundled around 5 games to show it off but you can also run your own games as well
if you place a game.p8.png or game.p8 in the CD folder and then rebuild it as CDI.

My source code is on github :
https://github.com/gameblabla/retro8/tree/dreamcast

This version is not fully optimized, as it still relies on SDL for audio.
if you can help me on that, i would appreciate it.

Once this dependency is removed, i will make another release.


Changelog:
V1.2
- ADSR sound fixes from libretro core
- Fixes for Celeste duplicated sprites and other issues

V1.1
- Better performance
Switched to 8bpp rendering.
Games like Spaceman8 run much faster now.
