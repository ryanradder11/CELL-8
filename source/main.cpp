#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>

#include <sys/process.h>
#include <sysutil/sysutil.h>
#include <io/pad.h>

#include "rsxutil.h"
#include "font/font5x7.h"
#include "chip8/chip8.h"
#include "romlist/romlist.h"
#include "sound/sound.h"
#include "screen/screen.h"

SYS_PROCESS_PARAM(1001, 0x100000);

static u32 running = 0;
static bool mainDebug = false;

extern "C" {
static void program_exit_callback()
{
	soundQuit();
	finish();
}

static void sysutil_exit_callback(u64 status, u64 param, void *usrdata)
{
	(void) param;
	(void) usrdata;

	if (status == SYSUTIL_EXIT_GAME) {
		running = 0;
	}
}
}

static void updateChip8Keys(Chip8 &chip, const padData &paddata)
{
	// DEBUG: only prints when at least one button is actually held (and
	// only if `mainDebug` is on), so it doesn't spam every frame while idle.
	if (mainDebug && (paddata.BTN_UP || paddata.BTN_DOWN || paddata.BTN_LEFT || paddata.BTN_RIGHT ||
	    paddata.BTN_TRIANGLE || paddata.BTN_CIRCLE || paddata.BTN_CROSS || paddata.BTN_SQUARE ||
	    paddata.BTN_L1 || paddata.BTN_R1 || paddata.BTN_L2 || paddata.BTN_R2 ||
	    paddata.BTN_SELECT || paddata.BTN_START || paddata.BTN_L3 || paddata.BTN_R3)) {
		printf("[pad] UP=%d DOWN=%d LEFT=%d RIGHT=%d TRI=%d CIR=%d CRO=%d SQU=%d L1=%d R1=%d L2=%d R2=%d SEL=%d STA=%d L3=%d R3=%d\n",
			paddata.BTN_UP, paddata.BTN_DOWN, paddata.BTN_LEFT, paddata.BTN_RIGHT,
			paddata.BTN_TRIANGLE, paddata.BTN_CIRCLE, paddata.BTN_CROSS, paddata.BTN_SQUARE,
			paddata.BTN_L1, paddata.BTN_R1, paddata.BTN_L2, paddata.BTN_R2,
			paddata.BTN_SELECT, paddata.BTN_START, paddata.BTN_L3, paddata.BTN_R3);
	}

	chip.keypad[0x1] = paddata.BTN_L1;
	chip.keypad[0x2] = paddata.BTN_UP;
	chip.keypad[0x3] = paddata.BTN_R1;
	// chip.keypad[0xC] = paddata.BTN_L3;

	chip.keypad[0x4] = paddata.BTN_LEFT;
	chip.keypad[0x5] = paddata.BTN_CIRCLE;
	chip.keypad[0x6] = paddata.BTN_RIGHT;
	// chip.keypad[0xD] = paddata.BTN_SQUARE;

	chip.keypad[0x7] = paddata.BTN_L2;
	chip.keypad[0x8] = paddata.BTN_DOWN;
	chip.keypad[0x9] = paddata.BTN_R2;
	// chip.keypad[0xE] = paddata.BTN_R2;

	chip.keypad[0xA] = paddata.BTN_L3;
	chip.keypad[0x0] = paddata.BTN_START;
	chip.keypad[0xB] = paddata.BTN_R3;
	// chip.keypad[0xF] = paddata.BTN_R3;
}

int main(void)
{
	printf("Cell-8: RSX hello world starting...\n");

	initScreen();
	soundInit();

	atexit(program_exit_callback);
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_exit_callback, NULL);

	running = 1;

	ioPadInit(7);

	screenInit();
	setRenderTarget(curr_fb);

	// Populate ROM_LIST by scanning the HDD (creating that folder if it's not present)
	romlist_init();
	if (mainDebug) {
		printf("[romlist] ROM_COUNT=%d\n", ROM_COUNT);
		for (int i = 0; i < ROM_COUNT; i++) {
			printf("[romlist] %d: %s -> %s\n", i, ROM_LIST[i].name, ROM_LIST[i].path);
		}
	}

	// Intro screen: show the title + subtitle until the player presses X,
	const char *title = "CELL-8";
	const char *subtitle = "PRESS X TO START";
	const u32 titleScale = 6;
	const u32 subtitleScale = 3;

	padInfo padinfo;
	padData paddata;
	bool introRunning = true;

	while (introRunning && running) {
		sysUtilCheckCallback();

		ioPadGetInfo(&padinfo);
		for (int i = 0; i < MAX_PADS; i++) {
			if (padinfo.status[i]) {
				ioPadGetData(i, &paddata);
				if (paddata.BTN_CROSS) {
					introRunning = false;
				}
			}
		}

		// Per PSL1GHT convention, only ever write into the buffer that
		// isn't currently on screen -- flip()/curr_fb already track that.
		u32 *buf = color_buffer[curr_fb];
		u32 pitchPixels = color_pitch / 4;
		memset(buf, 0, display_height * color_pitch);

		s32 titleX = (display_width - textWidth5x7(title, titleScale)) / 2;
		s32 subtitleX = (display_width - textWidth5x7(subtitle, subtitleScale)) / 2;

		drawText5x7(buf, pitchPixels, titleX, display_height/2 - 40, title, 0x00ffffff, titleScale);
		drawText5x7(buf, pitchPixels, subtitleX, display_height/2 + 20, subtitle, 0x00aaaaaa, subtitleScale);

		flip();
	}

	// Persists across returns to the menu (SELECT during gameplay), so the
	// previously-played ROM stays highlighted instead of resetting to 0.
	int selectedRom = 0;

	// DEBUG: print pad connection status once, so we know whether RPCS3
	// even reports a connected pad.
	if (mainDebug) {
		ioPadGetInfo(&padinfo);
		printf("[pad] max=%d\n", padinfo.max);
		for (int i = 0; i < MAX_PADS; i++) {
			printf("[pad] status[%d]=%d\n", i, padinfo.status[i]);
		}
	}

	// Outer loop: pick a ROM in the menu, play it until SELECT sends us
	// back here to pick another one (or the app is asked to exit).
	while (running) {
		bool prevUp = false, prevDown = false, prevCross = false;
		ioPadGetInfo(&padinfo);
		for (int i = 0; i < MAX_PADS; i++) {
			if (padinfo.status[i]) {
				ioPadGetData(i, &paddata);
				prevUp = paddata.BTN_UP;
				prevDown = paddata.BTN_DOWN;
				prevCross = paddata.BTN_CROSS;
			}
		}

		bool romChosen = false;

		while (!romChosen && running) {
			sysUtilCheckCallback();

			bool curUp = false, curDown = false, curCross = false;
			ioPadGetInfo(&padinfo);
			for (int i = 0; i < MAX_PADS; i++) {
				if (padinfo.status[i]) {
					ioPadGetData(i, &paddata);
					curUp = paddata.BTN_UP;
					curDown = paddata.BTN_DOWN;
					curCross = paddata.BTN_CROSS;
				}
			}

			if (curUp && !prevUp) {
				selectedRom = (selectedRom - 1 + ROM_COUNT) % ROM_COUNT;
			}
			if (curDown && !prevDown) {
				selectedRom = (selectedRom + 1) % ROM_COUNT;
			}
			if (curCross && !prevCross) {
				romChosen = true;
			}
			prevUp = curUp;
			prevDown = curDown;
			prevCross = curCross;

			static u8 menuGrid[SCREEN_MAX_COLS * SCREEN_MAX_ROWS];
			memset(menuGrid, 0, sizeof(menuGrid));

			const char *menuTitle = "SELECT ROM";
			drawText5x7ToGrid(menuGrid, SCREEN_MAX_COLS, SCREEN_MAX_ROWS, (SCREEN_MAX_COLS - textWidth5x7(menuTitle, 1)) / 2, 0, menuTitle);

			const int VISIBLE_ROWS = 14;
			const s32 entryY = 8;
			const s32 entryLineHeight = 8; // 7 glyph rows + 1 gap
			const s32 nameMaxChars = 38;   // ~40 chars/line - 2 for the "> "/"  " prefix

			int scrollOffset = selectedRom - VISIBLE_ROWS / 2;
			if (scrollOffset > ROM_COUNT - VISIBLE_ROWS) scrollOffset = ROM_COUNT - VISIBLE_ROWS;
			if (scrollOffset < 0) scrollOffset = 0;

			for (int row = 0; row < VISIBLE_ROWS; row++) {
				int i = scrollOffset + row;
				if (i >= ROM_COUNT) break;

				char entry[2 + nameMaxChars + 1];
				entry[0] = (i == selectedRom) ? '>' : ' ';
				entry[1] = ' ';
				strncpy(entry + 2, ROM_LIST[i].name, nameMaxChars);
				entry[2 + nameMaxChars] = '\0';

				drawText5x7ToGrid(menuGrid, SCREEN_MAX_COLS, SCREEN_MAX_ROWS, 0, entryY + row * entryLineHeight, entry);
			}

			screenDraw(menuGrid, SCREEN_MAX_COLS, SCREEN_MAX_ROWS, 0);
			flip();
		}

		if (!running) break;

		Chip8 chip = {};
		chip.pc = 0x200; // Start of most CHIP-8 programs
		loadROM(ROM_LIST[selectedRom].path, chip);

		static const u32 kMaxBrightness = 3; // frames a cell fades over after turning off
		u8 sBrightness[64 * 32] = {};

		bool prevSelect = false;
		ioPadGetInfo(&padinfo);
		for (int i = 0; i < MAX_PADS; i++) {
			if (padinfo.status[i]) {
				ioPadGetData(i, &paddata);
				prevSelect = paddata.BTN_SELECT;
			}
		}

		bool backToMenu = false;

		while (running && !backToMenu) {
			sysUtilCheckCallback();

			bool curSelect = false;
			ioPadGetInfo(&padinfo);
			for (int i = 0; i < MAX_PADS; i++) {
				if (padinfo.status[i]) {
					ioPadGetData(i, &paddata);
					updateChip8Keys(chip, paddata);
					curSelect = paddata.BTN_SELECT;
				}
			}

			if (curSelect && !prevSelect) {
				backToMenu = true;
			}
			prevSelect = curSelect;

			if (backToMenu) {
				break; // skip simulating/rendering a frame we're about to leave
			}

			for (int cycle = 0; cycle < 3; cycle++) {
				emulateCycle(chip);
			}

			// 60Hz timers, decremented once per rendered frame.
			if (chip.delay_timer > 0) chip.delay_timer--;
			if (chip.sound_timer > 0) chip.sound_timer--;
			soundSetActive(chip.sound_timer > 0);

			for (u32 i = 0; i < 64 * 32; i++) {
				if (chip.gfx[i])
					sBrightness[i] = kMaxBrightness;
				else if (sBrightness[i] > 0)
					sBrightness[i]--;
			}

			screenDraw(sBrightness, 64, 32, 0, kMaxBrightness);

			chip.drawFlag = false;

			flip();
		}
	}

	printf("Cell-8: exiting...\n");
	return 0;
}
