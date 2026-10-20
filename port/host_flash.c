/*
 * The cartridge's 128 KiB flash save chip, emulated on gHostFlash.
 * Replaces src/libs/agb_flash*.c; the core writes gHostFlash to the save
 * file after the game has changed it (Port_FlashDirty).
 */
#include <string.h>

#include "port/port.h"
#include "global.h"
#include "gba/flash_internal.h"

#define SECTOR_SIZE 0x1000

u8 gFlashTimeoutFlag;
u8 (*PollFlashStatus)(u8 *);
u16 (*WaitForFlashWrite)(u8 phase, u8 *addr, u8 lastData);
u16 (*ProgramFlashSector)(u16 sectorNum, void *src);
const struct FlashType *gFlash;
u16 (*ProgramFlashByte)(u16 sectorNum, u32 offset, u8 data);
u16 gFlashNumRemainingBytes;
u16 (*EraseFlashChip)(void);
u16 (*EraseFlashSector)(u16 sectorNum);
const u16 *gFlashMaxTime;

static const u16 sMaxTime[] = {10, 65469, 0, 20, 65469, 0, 2000, 65469, 0, 2000, 65469, 0};

static const struct FlashType sType = {
    FLASH_ROM_SIZE_1M,
    {SECTOR_SIZE, 12, 32, 0},
    {3, 1},
    {{0xC2, 0x09}}, /* Macronix MX29L010 */
};

static u16 HostProgramByte(u16 sectorNum, u32 offset, u8 data)
{
    if (sectorNum >= 32 || offset >= SECTOR_SIZE)
        return 0x80FF;
    gHostFlash[sectorNum * SECTOR_SIZE + offset] = data;
    Port_FlashDirty();
    return 0;
}

static u16 HostProgramSector(u16 sectorNum, void *src)
{
    if (sectorNum >= 32)
        return 0x80FF;
    memcpy(gHostFlash + sectorNum * SECTOR_SIZE, src, SECTOR_SIZE);
    Port_FlashDirty();
    return 0;
}

static u16 HostEraseChip(void)
{
    memset(gHostFlash, 0xFF, sizeof(gHostFlash));
    Port_FlashDirty();
    return 0;
}

static u16 HostEraseSector(u16 sectorNum)
{
    if (sectorNum >= 32)
        return 0x80FF;
    memset(gHostFlash + sectorNum * SECTOR_SIZE, 0xFF, SECTOR_SIZE);
    Port_FlashDirty();
    return 0;
}

static u16 HostWaitForWrite(u8 phase, u8 *addr, u8 lastData)
{
    (void)phase;
    (void)addr;
    (void)lastData;
    return 0;
}

u16 IdentifyFlash(void)
{
    ProgramFlashByte = HostProgramByte;
    ProgramFlashSector = HostProgramSector;
    EraseFlashChip = HostEraseChip;
    EraseFlashSector = HostEraseSector;
    WaitForFlashWrite = HostWaitForWrite;
    gFlashMaxTime = sMaxTime;
    gFlash = &sType;
    return 0;
}

u16 SetFlashTimerIntr(u8 timerNum, void (**intrFunc)(void))
{
    (void)timerNum;
    (void)intrFunc;
    return 0;
}

void ReadFlash(u16 sectorNum, u32 offset, void *dest, u32 size)
{
    u32 start = sectorNum * SECTOR_SIZE + offset;
    if (start >= sizeof(gHostFlash))
        return;
    if (start + size > sizeof(gHostFlash))
        size = sizeof(gHostFlash) - start;
    memcpy(dest, gHostFlash + start, size);
}

u32 ProgramFlashSectorAndVerify(u16 sectorNum, u8 *src)
{
    return HostProgramSector(sectorNum, src);
}

u32 ProgramFlashSectorAndVerifyNBytes(u16 sectorNum, void *dataSrc, u32 n)
{
    if (sectorNum >= 32 || n > SECTOR_SIZE)
        return 0x80FF;
    HostEraseSector(sectorNum);
    memcpy(gHostFlash + sectorNum * SECTOR_SIZE, dataSrc, n);
    Port_FlashDirty();
    return 0;
}

u16 ReadFlashId(void)
{
    return sType.ids.joined;
}

void SwitchFlashBank(u8 bankNum)
{
    (void)bankNum;
}
