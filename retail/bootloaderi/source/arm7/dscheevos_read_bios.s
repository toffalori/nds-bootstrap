/*
    NitroHax -- Cheat tool for the Nintendo DS
    Copyright (C) 2008  Michael "Chishm" Chisholm

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <nds/asminc.h>

@ Reads the ARM7 BIOS (including the secure area 0x0000-0x1204), which the
@ DScheevos cartridge boot needs for the KEY1 blowfish table. Code from a post
@ by CaitSith2 at gbadev.org. Renamed dscmReadBios for this fork.
@ void dscmReadBios (u8* dest, u32 src, u32 size)

    .arm

BEGIN_ASM_FUNC dscmReadBios
    adr r3,bios_dump+1
    bx r3
    .thumb

bios_dump:
    push {r4-r7,lr}
	mov r5, r1		@ src
	sub r1, r2, #1	@ size
	mov r2, r0		@ dest
    ldr r0,=0x5ED

loop:
    mov r6,#0x12
    sub r3,r1,r6
	add r3, r3, r5
    adr r6,ret
    push {r2-r6}
    bx r0
    .align

ret:
    strb r3,[r2,r1]
    sub r1,#1
    bpl loop

    pop {r4-r7}
    pop {r3}
    bx r3
