Let's Go Recomp
===============

A native Windows port of Pokemon: Let's Go, Eevee! and Pikachu! (Nintendo Switch) by static recompilation.
Project page, full documentation and source code: https://github.com/raulzrrs/letsgo-recomp

NO GAME FILES ARE INCLUDED. You need your own legally obtained copy of Pokemon: Let's Go, Eevee! or Pikachu!,
extracted (the exefs and romfs folders).

Supported: Pokemon: Let's Go, Eevee! (title id 0100187003A36000) and Pikachu! (010003F003A34000), any version.
The experimental options below need version 1.0.0 (the base game) of either; Pikachu 1.0.0
was tested up to the name screen of a new game. Cartridge or eShop, any region.
Packed files (.xci, .nsp, .nsz, .nca) must be extracted first.

Requirements
------------
- Windows 10 or 11, 64-bit
- A CPU with SSE4.2 (Intel Nehalem / 2008 or newer, AMD Bulldozer / 2011 or newer); checked at startup. AVX2
  (Intel Haswell / 2013 or newer, AMD Excavator / Zen or newer) is recommended: without it the game code is built
  for older CPUs during setup, with the same results but slower
- A GPU whose driver supports Vulkan 1.3 (GPUs that report less are skipped); see vulkan.gpuinfo.org for yours
- RAM: the game used up to 3.8 GB while playing, the one-time setup 5.2 GB (measured). Tested on a 32 GB machine
- About 6 GB of free disk space (measured: 5.7 GB at the peak of the one-time setup, 4.6 GB afterwards)

How to play
-----------
1. Unzip this folder anywhere you can write to (e.g. D:\Games\LetsGoRecomp). Not inside Program Files.
2. Open LetsGoRecomp.exe. It is not code-signed: if Windows says "Windows protected your PC", click "More info"
   and then "Run anyway".
3. Click "Select game folder..." and pick the folder with your extracted game (the one that holds exefs and romfs,
   or any folder above it).
4. Wait while the game files are copied and the game code is recompiled for your PC. This happens only once and
   takes a while (22 minutes measured on a 20-thread CPU, longer with fewer threads).
5. Click Play.

Experimental options (start screen; version 1.0.0 of either game: greyed out for updated games)
- Frame rate limit (Graphics page, also in the F12 menu): 30 (the game's), 60 or 120 fps - the overworld at that rate
  without speed-ups; menus, the Pokedex, battles and captures stay at 30, as on the console.
- Wild battles (Game page, off by default): touching a wild Pokemon starts a classic battle against it, then the capture.
The controller stick dead zone is on the Controls page.

Both games: one folder holds one set-up game, so unzip this twice (e.g. LetsGoRecomp-Eevee and LetsGoRecomp-Pikachu)
and set each one up with its game.

Everything stays in this folder: settings (larecomp.ini), saves (save\, or save-pikachu\ for Pikachu), the game files (extracted\), mods and
translations (mods\) and the log (lasrecomp.log). To move or back up the game, move or copy the whole folder.

Translations and mods
---------------------
On the start screen's Mods page, click "Install a mod..." and pick the file you downloaded (.zip, .rar or .7z, as it
came; packs made for Atmosphere / LayeredFS work: translations, textures, models). It comes ticked; several can be on
at once (where two replace the same file, the higher one in the list wins).
Mods of the game's code (exefs, IPS patches, cheats) don't work in this port.

Portugues (Brasil): para jogar em portugues, baixe a traducao PT-BR de fas do grupo Pokemon Switch Brasil, abra a pagina
"Mods", clique em "Instalar mod..." e escolha o arquivo baixado (.rar ou .zip, do jeito que veio). Ela ja vem marcada
na lista.
A traducao substitui os textos em ingles e espanhol: num jogo novo, escolha English ou Espanol na tela de idioma do
jogo. O programa abre em portugues quando o Windows esta em portugues.

Controls
--------
Keyboard: WASD walk, Space/Enter = A, Q/Backspace = B, Tab = X (menu), R = Y, Z/C = L/R, = and - = Plus/Minus,
arrows = D-pad, Esc = close, F12 = settings (volume, image, effects; on a controller: - and + together, then Settings).
All keys can be changed on the start screen.
Mouse: aims Poke Balls (like the Switch's motion controls), left button throws; outside of aiming, the left button is
a finger on the touch screen (Play with your partner). Right button = B.
Controllers: any controller SDL recognizes (Xbox, PlayStation, Switch Pro...), handheld-mode layout. The right stick
plus ZR is a finger on the touch screen.

Troubleshooting
---------------
- "Not Pokemon: Let's Go, Eevee! or Pikachu!": pick the folder of your own extracted game.
- Packed game files (.xci / .nsp) must be extracted first.
- "The game code builder is missing": your antivirus may have quarantined lasgen\lasgen.exe (a PyInstaller
  program, sometimes flagged by mistake). Restore it or add this folder as an exception.
- The game closes or doesn't start: see lasrecomp.log in this folder and report it on the project page.

Legal
-----
Fan project, not affiliated with Nintendo, The Pokemon Company, Game Freak or Creatures. MIT licensed (LICENSE.txt),
based on LAS Recomp by Peiikai (LICENSE-LASRecomp.txt, https://github.com/peiikai/LASRecomp).
Bundled: SDL3 (zlib), FFmpeg (LGPL 2.1, built with only the decoders the game needs; source: https://ffmpeg.org),
shaderc (Apache 2.0), libiconv (LGPL), Zig (MIT), the Baloo 2 font (SIL OFL 1.1, Baloo2-OFL.txt),
AMD's FidelityFX SDK runtime for FSR 3.1 (MIT, amd_fidelityfx_vk.dll, FidelityFX-LICENSE.txt), and the
GCC / MinGW-w64 runtime libraries (GPL with the runtime library exception).
