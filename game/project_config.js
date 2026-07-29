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
Target = "ROM_ASCII8";

//-- ROM mapper total size in KB (number)
ROMSize = 1024;

//-- List of raw data files to be added to final binary (array)
RawFiles = [
	{ segment:32, file:"bin/tiles.bin" },
	{ segment:40, file:"bin/hero.bin" },
	{ segment:76, file:"bin/orc.bin" },
	{ segment:82, file:"bin/items16.bin" },
	{ segment:83, file:"bin/armor32.bin" },
];

//-- Postpone the ROM startup to let the other ROMs initialize (boolean)
InstallRAMISR = true;

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
