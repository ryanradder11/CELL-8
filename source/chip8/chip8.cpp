#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chip8/chip8.h"

// Ported as-is from ~/projects/chip8-emulator/main.cpp -- see chip8.h for
// what "as-is" means here (existing bugs/gaps intentionally kept, only
// std::cout/std::ifstream swapped for printf/fopen).

bool debug = false;

// DEBUG helper: prints an 8-bit value as "label: 0bxxxxxxxx", MSB first.
static void printBinary8(const char *label, uint8_t val) {
    printf("%s: 0b", label);
    for (int i = 7; i >= 0; i--) {
        printf("%d", (val >> i) & 1);
    }
    printf(" (%d)\n", val);
}

bool loadROM(const char *filename, Chip8 &chip) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        printf("Failed to open ROM: %s\n", filename);
        return false;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    size_t bytesRead = fread(&chip.memory[0x200], 1, (size_t) size, file);
    fclose(file);

    if (bytesRead == (size_t) size) {
        printf("Successfully loaded ROM: %s\n", filename);
        return true;
    }

    return false;
}

void emulateCycle(Chip8 &chip) {
    // 1. Fetch opcode
    uint16_t opcode = chip.memory[chip.pc] << 8 | chip.memory[chip.pc + 1];

    if (debug) {
        printf("Opcode (hex): %x\n", opcode);
        printf("Opcode (binary): ");
        for (int i = 15; i >= 0; --i) {
            printf("%d", (opcode >> i) & 1);
        }
        printf("\n");
    }

    // 2. Decode and execute
    switch (opcode & 0xF000) {
        case 0x0000: {
            switch (opcode & 0x00FF) {
                case 0xE0: {
                    // 00E0 - Clear screen
                    memset(chip.gfx, 0, sizeof(chip.gfx));
                    chip.drawFlag = true;
                    chip.pc += 2;
                    if (debug) printf("Clear screen\n");
                    break;
                }
                case 0xEE: {
                    // 00EE - Return from subroutine
                    chip.sp--;
                    chip.pc = chip.stack[chip.sp];
                    chip.pc += 2;
                    if (debug) printf("Return from subroutine to 0x%x\n", chip.pc);
                    break;
                }
                default: {
                    printf("Unknown 0x0000 opcode: %x\n", opcode);
                    break;
                }
            }
            break;
        }
        break;
        case 0x1000:
            chip.pc = opcode & 0x0FFF; // Jump to address NNN
            if (debug) printf("Jump to address: %x\n", chip.pc);
            break;
        case 0x2000:
            // 2NNN: Call subroutine at NNN
        {
            uint16_t address = opcode & 0x0FFF;
            chip.stack[chip.sp] = chip.pc; // Store current PC in the stack
            chip.sp++; // Increment stack pointer
            chip.pc = address; // Set PC to the address
            if (debug) printf("Call subroutine at address: %x\n", address);
        }
        break;
        case 0x3000:
            // 3XNN: Skip next instruction if Vx == NN
        {
            uint8_t x = (opcode & 0x0F00) >> 8;
            uint8_t nn = opcode & 0x00FF;
            if (debug) printf("3XNN: Checking if V[%d] == %d (V[%d] = %d)\n", x, nn, x, chip.V[x]);
            if (chip.V[x] == nn) {
                if (debug) printf("Condition true: skipping next instruction.\n");
                chip.pc += 4;
            } else {
                if (debug) printf("Condition false: proceeding to next instruction.\n");
                chip.pc += 2;
            }
        }
        break;
        case 0x4000:
            // 4XNN: Skip next instruction if Vx != NN
        {
            uint8_t x = (opcode & 0x0F00) >> 8;
            uint8_t nn = opcode & 0x00FF;
            if (debug) printf("4XNN: Checking if V[%d] != %d (V[%d] = %d)\n", x, nn, x, chip.V[x]);
            if (chip.V[x] != nn) {
                if (debug) printf("Condition true: skipping next instruction.\n");
                chip.pc += 4;
            } else {
                if (debug) printf("Condition false: proceeding to next instruction.\n");
                chip.pc += 2;
            }
        }
        break;
        case 0x5000:
            // 5XY0: Skip next instruction if VX == VY
            if ((opcode & 0x000F) == 0x0000) {
                uint8_t x = (opcode & 0x0F00) >> 8;
                uint8_t y = (opcode & 0x00F0) >> 4;
                if (debug) printf("5XY0: Checking if V[%d] == V[%d] (%d == %d)\n", x, y, chip.V[x], chip.V[y]);

                if (chip.V[x] == chip.V[y]) {
                    chip.pc += 4;
                } else {
                    chip.pc += 2;
                }
            } else {
                printf("Unknown 0x5000 opcode variant: %x\n", opcode);
                chip.pc += 2;
            }
            break;
        case 0x6000:
            // 6XNN: Set Vx = NN
        {
            uint8_t x = (opcode & 0x0F00) >> 8;
            uint8_t nn = opcode & 0x00FF;
            chip.V[x] = nn;
            chip.pc += 2;
        }
            if (debug) printf("Set V%d = %d\n", (opcode & 0x0F00) >> 8, opcode & 0x00FF);
            break;
        case 0x7000:
            // 7XNN: Add NN to Vx
        {
            uint8_t x = (opcode & 0x0F00) >> 8;
            uint8_t nn = opcode & 0x00FF;
            chip.V[x] += nn;
            chip.pc += 2;
        }
            if (debug) printf("Add %d to V%d\n", opcode & 0x00FF, (opcode & 0x0F00) >> 8);
            break;
        case 0x8000: {
            //all 0x8000 opcodes share the same X,Y
            uint8_t x = (opcode & 0x0F00) >> 8;
            uint8_t y = (opcode & 0x00F0) >> 4;
            switch (opcode & 0x000F) {
                case 0x0: {
                    //8XY0	LD VX, VY Copy the value in register VY into VX
                    chip.V[x] = chip.V[y];
                    chip.pc += 2;
                    if (debug) printf("Set V[%d] = V[%d] (%d)\n", x, y, chip.V[y]);
                    break;
                }
                // 8XY1 - Sets VX to (VX OR VY).
                case 0x1: {
                    chip.V[x] |= chip.V[y];
                    chip.pc += 2;
                    if (debug) printf("Set V[%d] (OR)|= V[%d] (%d)\n", x, y, chip.V[y]);
                }
                break;
                // Set VX equal to the bitwise and of the values in VX and VY.
                case 0x2: {
                    chip.V[x] = chip.V[x] & chip.V[y];
                    chip.pc += 2;
                    if (debug) printf("Set V[%d] &= V[%d] (%d)\n", x, y, chip.V[y]);
                }
                break;
                // Set VX equal to the bitwise xor of the values in VX and VY
                case 0x3: {
                    chip.V[x] = chip.V[x] ^ chip.V[y];
                    chip.pc += 2;
                    if (debug) printf("Set V[%d] (XOR)^= V[%d] (%d)\n", x, y, chip.V[y]);
                }
                break;
                // Set VX equal to VX plus VY. In the case of an overflow(carry) VF is set to 1. Otherwise 0.
                case 0x4: {
                    chip.V[x] = (chip.V[x] + chip.V[y]) & 0xff;
                    if (chip.V[y] > chip.V[x]) {
                        chip.V[0xf] = 1;
                    } else {
                        chip.V[0xf] = 0;
                    }
                    chip.pc += 2;
                }
                    if (debug) printf("Set V[%d] += V[%d] (%d), with carry\n", x, y, chip.V[y]);
                    break;
                // Set VX equal to VX minus VY. In the case of an underflow VF is set 0. Otherwise 1. (VF = VX > VY)
                case 0x5: {
                    uint8_t vx = chip.V[x];
                    uint8_t vy = chip.V[y];
                    chip.V[x] = (vx - vy) & 0xFF;
                    chip.V[0xF] = (vx >= vy) ? 1 : 0;
                    chip.pc += 2;
                    if (debug) printf("Set V[%d] -= V[%d] (%d), with borrow flag\n", x, y, chip.V[y]);
                }
                break;
                //Set VX equal to VX bitshifted right 1. VF is set to the least significant bit of VX prior to the shift.
                case 0x6: {
                    uint8_t vx = chip.V[x];

                    chip.V[x] = vx >> 1;
                    chip.V[0xF] = vx & 0x1;

                    chip.pc += 2;
                    if (debug) printf("Shift V[%d] right by 1. VF = %d\n", x, chip.V[0xF]);
                }
                break;
                //Set VX equal to VY minus VX. VF is set to 1 if VY > VX. Otherwise 0.
                case 0x7: {
                    uint8_t vx = chip.V[x];
                    uint8_t vy = chip.V[y];

                    chip.V[x] = vy - vx & 0xFF;
                    chip.V[0xF] = vy >= vx ? 1 : 0;

                    chip.pc += 2;
                    if (debug)
                        printf("Set V[%d] = V[%d] - V[%d] (%d - %d), VF = %d\n", x, y, x, chip.V[y], chip.V[x],
                               chip.V[0xF]);
                }
                break;
                // Set VX equal to VX bitshifted left 1. VF is set to the most significant bit of VX prior to the shift
                case 0xe: {
                    uint8_t vx = chip.V[x];
                    uint8_t mostSignificantBit = (vx & 0x80) ? 1 : 0;

                    chip.V[x] = (vx << 1) & 0xFF;
                    chip.V[0xF] = mostSignificantBit;

                    chip.pc += 2;
                    if (debug) printf("Shift V[%d] left by 1. VF = %d\n", x, chip.V[0xF]);
                }
                break;
                default:
                    printf("Unknown 8XY opcode: %x\n", opcode);
                    chip.pc += 2;
                    break;
            }
        }
        break;
        case 0x9000:
            // 9XY0: Skip next instruction if Vx != Vy
            if ((opcode & 0x000F) == 0x0000) {
                uint8_t x = (opcode & 0x0F00) >> 8;
                uint8_t y = (opcode & 0x00F0) >> 4;
                if (debug) printf("9XY0: Checking if V[%d] != V[%d] (%d != %d)\n", x, y, chip.V[x], chip.V[y]);
                if (chip.V[x] != chip.V[y]) {
                    chip.pc += 4;
                } else {
                    chip.pc += 2;
                }
            } else {
                printf("Unknown 0x9000 opcode: %x\n", opcode);
                chip.pc += 2;
            }
            break;
        // Set I equal to NNN.
        case 0xA000:
            chip.I = opcode & 0x0FFF;
            chip.pc += 2;
            if (debug) printf("Set I = %x\n", chip.I);
            break;
        // Set the PC to NNN plus the value in V0.
        case 0xB000:
            chip.pc = opcode & 0x0FFF + chip.V[0];
            if (debug) printf("Set PC= %x\n", chip.pc);
            break;
        // Set VX equal to a random number ranging from 0 to 255 which is logically anded with NN.
        case 0xC000: {
            uint8_t x = (opcode & 0x0F00) >> 8;
            uint8_t nn = opcode & 0x00FF;
            uint8_t rnd = rand() % 256;
            chip.V[x] = rnd & nn;
            chip.pc += 2;
            if (debug) printf("Set V[%d] = rand() & 0x%x => %d\n", x, nn, chip.V[x]);
        }
        break;
        case 0xD000:
            // DXYN: Draw sprite at (Vx, Vy) with width 8 pixels and height N pixels
        {
            uint8_t x = chip.V[(opcode & 0x0F00) >> 8];
            uint8_t y = chip.V[(opcode & 0x00F0) >> 4];
            uint8_t height = opcode & 0x000F;

            // Reset the collision flag
            chip.V[0xF] = 0;

            // Iterate over each row of the sprite
            for (int yline = 0; yline < height; yline++) {
                // Get the pixel data for the current row
                uint8_t pixel = chip.memory[chip.I + yline];

                // Iterate over each bit in the row (8 bits per row)
                for (int xline = 0; xline < 8; xline++) {
                    // Check if the current bit is set (pixel is on)
                    if ((pixel & (0x80 >> xline)) != 0) {
                        // Calculate the index in the display buffer
                        int index = x + xline + ((y + yline) * 64);

                        // Check for collision (if the pixel is already on)
                        if (chip.gfx[index] == 1) {
                            chip.V[0xF] = 1; // Set collision flag
                        }

                        // XOR the pixel (toggle it)
                        chip.gfx[index] ^= 1;
                    }
                }
            }

            chip.drawFlag = true;
            chip.pc += 2;
        }
            if (debug)
                printf("Draw sprite at (%d, %d)\n", chip.V[(opcode & 0x0F00) >> 8],
                       chip.V[(opcode & 0x00F0) >> 4]);
            break;
        case 0xE000:
            // EX9E: Skip next instruction if key with value of Vx is pressed
        {
            uint8_t x = (opcode & 0x0F00) >> 8;
            uint8_t lowByte = opcode & 0x00FF;
            if (debug)
                printf("[Ex__] opcode=%x lowByte=%x (9E=skip-if-pressed, A1=skip-if-NOT-pressed) x=%d V[x]=%d keypad[V[x]]=%d\n",
                       opcode, lowByte, x, chip.V[x], (chip.V[x] < 16) ? chip.keypad[chip.V[x]] : 255);

            if (lowByte == 0xA1) {
                if (chip.V[x] < 16 && chip.keypad[chip.V[x]] != 1) {
                    if (debug) printf("Key pressed: %d\n", chip.V[x]);
                    chip.pc += 4;
                } else {
                    chip.pc += 2;
                }
            } else if (lowByte == 0x9E) {
                if (chip.V[x] < 16 && chip.keypad[chip.V[x]] == 1) {
                    if (debug) printf("Key pressed: %d\n", chip.V[x]);
                    chip.pc += 4;
                } else {
                    chip.pc += 2;
                }
            }
        }
            if (debug) printf("Skip next instruction if key with value of V%d is pressed\n", (opcode & 0x0F00) >> 8);
            if (debug) printf("VX value: %d\n", chip.V[(opcode & 0x0F00) >> 8]);
            break;
        case 0xF000: {
            uint8_t x = (opcode & 0x0F00) >> 8;
            switch (opcode & 0x00FF) {
                // Set VX equal to the delay timer.
                case 0x07: {
                    chip.V[x] = chip.delay_timer;
                    chip.pc += 2;
                    if (debug) printf("Set V[%d] = delay_timer (%d)\n", x, chip.delay_timer);
                }
                break;
                // FX18: Set sound timer = Vx
                case 0x0018: {
                    chip.sound_timer = chip.V[x];
                    chip.pc += 2;
                    if (debug) printf("Set sound timer = V%d\n", x);
                }
                break;
                // FX15 set delay timer = Vx
                case 0x0015: {
                    chip.delay_timer = chip.V[x];
                    chip.pc += 2;
                    if (debug) printf("Set delay timer = V%d\n", x);
                }
                break;
                // Set I to the address of the CHIP-8 8x5 font sprite representing the value in VX.
                case 0x0029: {
                    // Fx29
                    chip.I = chip.V[x] * 5; // assuming font sprites start at address 0
                    chip.pc += 2;
                    if (debug)
                        printf("Set I to sprite address for character in V[%d] = %x, I = %x\n", x, chip.V[x],
                               chip.I);
                }
                break;
                // Convert that word to BCD and store the 3 digits at memory location I through I+2. I does not change.
                case 0x33: {
                    uint8_t value = chip.V[x];
                    chip.memory[chip.I] = value / 100;
                    chip.memory[chip.I + 1] = (value / 10) % 10;
                    chip.memory[chip.I + 2] = value % 10;
                    chip.pc += 2;

                    if (debug) printf("Stored BCD of V[%d] (%d) into memory at I, I+1, and I+2\n", x, value);
                }
                break;
                // Store registers V0 through Vx in memory starting at address I.
                case 0x0055: {
                    for (int i = 0; i <= x; ++i) {
                        chip.memory[chip.I + i] = chip.V[i];
                    }
                    chip.pc += 2;

                    if (debug) printf("Stored V[0] to V[%d] into memory starting at I (0x%x)\n", x, chip.I);
                }
                break;
                // Copy values from memory location I through I + X into registers V0 through VX. I does not change.
                case 0x0065: {
                    for (int i = 0; i <= x; i++) {
                        chip.V[i] = chip.memory[chip.I + i];
                    }
                    chip.pc += 2;
                    if (debug) printf("Read V[0] to V[%d] from memory starting at I (0x%x)\n", x, chip.I);
                }
                break;
                // Add VX to I. VF is set to 1 if I > 0x0FFF. Otherwise set to 0.
                case 0x1E: {
                    uint16_t sum = chip.I + chip.V[x];
                    chip.V[0xF] = (sum > 0x0FFF) ? 1 : 0;
                    chip.I = sum & 0x0FFF;
                    chip.pc += 2;

                    if (debug)
                        printf("Add V[%d] (%d) to I. VF = %d, New I = 0x%x\n", x, chip.V[x], chip.V[0xF],
                               chip.I);
                }
                break;
                case 0x0080:
                    // FF80: Custom opcode - treat as NOP or marker
                    printf("Custom opcode FF80 encountered. (Possible sprite data marker?)\n");
                    chip.pc += 2;
                    break;
                // Add other 0xF000 opcodes here...
                default:
                    printf("Unknown opcode: %x\n", opcode);
                    break;
            }
        }
        break;
        // Add more ...
        default:
            printf("Unknown opcode: %x\n", opcode);
            break;
    }

    // 3. Update timers (usually done in main loop every 60Hz)
}
