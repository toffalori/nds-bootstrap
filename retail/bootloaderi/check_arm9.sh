#!/bin/sh
# DScheevos: fails the build when the bootloader's ARM9 code reaches out of its
# own region. The ARM9 side (load.ld .arm9, the *.arm9.* objects) is copied to
# arm9ram and runs from there alone. Everything else is linked into VRAM that
# only the ARM7 maps; at those addresses the ARM9 sees video RAM. A call from
# the ARM9 code to a libgcc helper (__aeabi_uidiv for a division), to a far-call
# veneer or to any non-ARM9 function jumps there and hangs the boot.
#
# Usage: check_arm9.sh <elf> <objdump> <nm>

elf=$1
objdump=$2
nm=$3

start=$($nm "$elf" | awk '$3 == "__arm9_start" { print $1 }')
end=$($nm "$elf" | awk '$3 == "__arm9_end" { print $1 }')
if [ -z "$start" ] || [ -z "$end" ]; then
	echo "check_arm9: no __arm9_start/__arm9_end in $elf"
	exit 1
fi

$objdump -d -j .arm9 "$elf" | awk -v start="$start" -v end="$end" '
	# hex strings of one length compare like the numbers
	function pad(x) { while (length(x) < 8) x = "0" x; return tolower(x) }
	BEGIN { start = pad(start); end = pad(end) }
	# A long-branch veneer jumps through a register: its target is out of reach.
	# ld names its long thumb-to-ARM stub __x_from_thumb too, like the short one,
	# but that one loads pc from a literal ("ldr pc, [pc, #-4]"). The short
	# __x_from_thumb stubs end in a "b", checked below.
	/_veneer>:/ || /\tldr\tpc, \[pc, #-4\]/ {
		print "check_arm9: far-call stub in the ARM9 code: " $0
		bad = 1
	}
	# b, bl, blx and conditional branches with a target: "\tbl\t23be20c <name>"
	match($0, /\tb(l|lx|x)?(eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?(\.n|\.w)?\t[0-9a-f]+ </) {
		n = split(substr($0, RSTART + 1), f, "\t")
		target = pad(substr(f[2], 1, index(f[2], " ") - 1))
		if (target < start || target >= end) {
			print "check_arm9: branch out of the ARM9 code: " $0
			bad = 1
		}
	}
	END { exit bad }
'
