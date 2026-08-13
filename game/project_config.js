// ─────────────────────────────────────────────────────────────────────────────
//  Black Tiger MSX2+ - engine prototype - 2026 Fausto Pracek
// ─────────────────────────────────────────────────────────────────────────────

//-- Do not auto-launch the emulator at end of build
DoRun = false;

//-- Project name (string). Will be use for output filename
ProjName = "blacktiger";

//-- List of project modules to build (array). If empty, ProjName will be added
ProjModules = [ ProjName ];

//-- List of library modules to build (array)
LibModules = [ "system", "input", "vdp", "print", "memory", "debug", "math", "bios" ];

//-- Target MSX machine version (string)
Machine = "2P";

//-- Target program format (string)
Target = "ROM_YAMANOOTO";

//-- ROM mapper total size in KB (number)
ROMSize = 8192;

//-- List of raw data files to be added to final binary (array)
RawFiles = [
	{ segment:32, file:"bin/tiles.bin" },
	{ segment:40, file:"bin/hero.bin" },
	{ segment:78, file:"bin/orc.bin" },
	{ segment:83, file:"bin/items16.bin" },
	{ segment:84, file:"bin/armor32.bin" },
	{ segment:85, file:"bin/hud.bin" },
	{ segment:86, file:"bin/level1.bin" },
];

//-- Postpone the ROM startup to let the other ROMs initialize (boolean)
//-- Yamanooto target: page 0 stays BIOS ROM — interrupts run through the
//-- BIOS ISR and we take over via the H_KEYI hook (see IsrHook)
InstallRAMISR = false;

//-- Use banked call and trampoline functions (boolean)
BankedCall = true;

//-- Overwrite RAM starting address (number)
// ForceRamAddr = 0xC000;

//-- Skip compilation of unmodified files (boolean)
//   NOTE: keep false — the skip check misses msxgl_config.h changes
CompileSkipOld = false;

//-- Display extra information during build (boolean)
Verbose = true;

//-- Add generated binary to emulator's media list (boolean)
Standalone = true;

//-- Emulator path
Emulator = "E:\\Dropbox\\FAUSTO\\SVILUPPI\\MSX\\EMULATORI\\openMSX\\openMSX.exe";

//-- Force the emulated machine to be at 60 Hz (boolean)
Emul60Hz = true;
