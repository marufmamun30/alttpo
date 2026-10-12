# Small 65816 disassembler for looking at routines of the game (LoROM).
# Usage: python dis.py <rom.sfc> <snes address, e.g. 00894A> [instructions=60] [m=1] [x=1]
# It follows REP/SEP to keep track of the register sizes, nothing else.
import sys

# addressing modes: length in bytes besides the opcode ("m" / "x" = depends on the flag)
IMP, IMM8, IMMM, IMMX, DP, DPX, DPY, ABS, ABSX, ABSY, LONG, LONGX, IND, INDX, INDY, INDL, INDLY, SR, SRY, REL, RELL, MV, AIND, AINDX, AINDL = range(25)
LEN = {IMP: 0, IMM8: 1, DP: 1, DPX: 1, DPY: 1, ABS: 2, ABSX: 2, ABSY: 2, LONG: 3, LONGX: 3, IND: 1, INDX: 1, INDY: 1,
       INDL: 1, INDLY: 1, SR: 1, SRY: 1, REL: 1, RELL: 2, MV: 2, AIND: 2, AINDX: 2, AINDL: 2}
OPS = {}
def op(code, name, mode): OPS[code] = (name, mode)
for base, name in ((0x00, "ORA"), (0x20, "AND"), (0x40, "EOR"), (0x60, "ADC"), (0x80, "STA"), (0xA0, "LDA"), (0xC0, "CMP"), (0xE0, "SBC")):
    for off, mode in ((0x01, INDX), (0x03, SR), (0x05, DP), (0x07, INDL), (0x09, IMMM), (0x0D, ABS), (0x0F, LONG), (0x11, INDY),
                      (0x12, IND), (0x13, SRY), (0x15, DPX), (0x17, INDLY), (0x19, ABSY), (0x1D, ABSX), (0x1F, LONGX)):
        op(base + off, name, mode)
op(0x89, "BIT", IMMM)
for code, name, mode in (
    (0x00, "BRK", IMM8), (0x02, "COP", IMM8), (0x04, "TSB", DP), (0x06, "ASL", DP), (0x08, "PHP", IMP), (0x0A, "ASL", IMP), (0x0B, "PHD", IMP),
    (0x0C, "TSB", ABS), (0x0E, "ASL", ABS), (0x10, "BPL", REL), (0x14, "TRB", DP), (0x16, "ASL", DPX), (0x18, "CLC", IMP), (0x1A, "INC", IMP),
    (0x1B, "TCS", IMP), (0x1C, "TRB", ABS), (0x1E, "ASL", ABSX), (0x20, "JSR", ABS), (0x22, "JSL", LONG), (0x24, "BIT", DP), (0x26, "ROL", DP),
    (0x28, "PLP", IMP), (0x2A, "ROL", IMP), (0x2B, "PLD", IMP), (0x2C, "BIT", ABS), (0x2E, "ROL", ABS), (0x30, "BMI", REL), (0x34, "BIT", DPX),
    (0x36, "ROL", DPX), (0x38, "SEC", IMP), (0x3A, "DEC", IMP), (0x3B, "TSC", IMP), (0x3C, "BIT", ABSX), (0x3E, "ROL", ABSX), (0x40, "RTI", IMP),
    (0x42, "WDM", IMM8), (0x44, "MVP", MV), (0x46, "LSR", DP), (0x48, "PHA", IMP), (0x4A, "LSR", IMP), (0x4B, "PHK", IMP), (0x4C, "JMP", ABS),
    (0x4E, "LSR", ABS), (0x50, "BVC", REL), (0x54, "MVN", MV), (0x56, "LSR", DPX), (0x58, "CLI", IMP), (0x5A, "PHY", IMP), (0x5B, "TCD", IMP),
    (0x5C, "JML", LONG), (0x5E, "LSR", ABSX), (0x60, "RTS", IMP), (0x62, "PER", RELL), (0x64, "STZ", DP), (0x66, "ROR", DP), (0x68, "PLA", IMP),
    (0x6A, "ROR", IMP), (0x6B, "RTL", IMP), (0x6C, "JMP", AIND), (0x6E, "ROR", ABS), (0x70, "BVS", REL), (0x74, "STZ", DPX), (0x76, "ROR", DPX),
    (0x78, "SEI", IMP), (0x7A, "PLY", IMP), (0x7B, "TDC", IMP), (0x7C, "JMP", AINDX), (0x7E, "ROR", ABSX), (0x80, "BRA", REL), (0x82, "BRL", RELL),
    (0x84, "STY", DP), (0x86, "STX", DP), (0x88, "DEY", IMP), (0x8A, "TXA", IMP), (0x8B, "PHB", IMP), (0x8C, "STY", ABS), (0x8E, "STX", ABS),
    (0x90, "BCC", REL), (0x94, "STY", DPX), (0x96, "STX", DPY), (0x98, "TYA", IMP), (0x9A, "TXS", IMP), (0x9B, "TXY", IMP), (0x9C, "STZ", ABS),
    (0x9E, "STZ", ABSX), (0xA0, "LDY", IMMX), (0xA2, "LDX", IMMX), (0xA4, "LDY", DP), (0xA6, "LDX", DP), (0xA8, "TAY", IMP), (0xAA, "TAX", IMP),
    (0xAB, "PLB", IMP), (0xAC, "LDY", ABS), (0xAE, "LDX", ABS), (0xB0, "BCS", REL), (0xB4, "LDY", DPX), (0xB6, "LDX", DPY), (0xB8, "CLV", IMP),
    (0xBA, "TSX", IMP), (0xBB, "TYX", IMP), (0xBC, "LDY", ABSX), (0xBE, "LDX", ABSY), (0xC0, "CPY", IMMX), (0xC2, "REP", IMM8), (0xC4, "CPY", DP),
    (0xC6, "DEC", DP), (0xC8, "INY", IMP), (0xCA, "DEX", IMP), (0xCB, "WAI", IMP), (0xCC, "CPY", ABS), (0xCE, "DEC", ABS), (0xD0, "BNE", REL),
    (0xD4, "PEI", DP), (0xD6, "DEC", DPX), (0xD8, "CLD", IMP), (0xDA, "PHX", IMP), (0xDB, "STP", IMP), (0xDC, "JML", AINDL), (0xDE, "DEC", ABSX),
    (0xE0, "CPX", IMMX), (0xE2, "SEP", IMM8), (0xE4, "CPX", DP), (0xE6, "INC", DP), (0xE8, "INX", IMP), (0xEA, "NOP", IMP), (0xEB, "XBA", IMP),
    (0xEC, "CPX", ABS), (0xEE, "INC", ABS), (0xF0, "BEQ", REL), (0xF4, "PEA", ABS), (0xF6, "INC", DPX), (0xF8, "SED", IMP), (0xFA, "PLX", IMP),
    (0xFB, "XCE", IMP), (0xFC, "JSR", AINDX), (0xFE, "INC", ABSX)):
    op(code, name, mode)

def off(addr):  # LoROM
    return ((addr & 0x7F0000) >> 1) | (addr & 0x7FFF)

def dis(rom, addr, count, m=1, x=1, out=None):
    lines = []
    for _ in range(count):
        o = off(addr)
        code = rom[o]
        name, mode = OPS.get(code, ("???", IMP))
        n = LEN.get(mode)
        if mode == IMMM: n = 1 if m else 2
        if mode == IMMX: n = 1 if x else 2
        arg = rom[o + 1:o + 1 + n]
        v = int.from_bytes(arg, "little") if n else 0
        if mode == IMP: s = ""
        elif mode in (IMM8, IMMM, IMMX): s = "#$%0*X" % (n * 2, v)
        elif mode == DP: s = "$%02X" % v
        elif mode == DPX: s = "$%02X,X" % v
        elif mode == DPY: s = "$%02X,Y" % v
        elif mode == ABS: s = "$%04X" % v
        elif mode == ABSX: s = "$%04X,X" % v
        elif mode == ABSY: s = "$%04X,Y" % v
        elif mode == LONG: s = "$%06X" % v
        elif mode == LONGX: s = "$%06X,X" % v
        elif mode == IND: s = "($%02X)" % v
        elif mode == INDX: s = "($%02X,X)" % v
        elif mode == INDY: s = "($%02X),Y" % v
        elif mode == INDL: s = "[$%02X]" % v
        elif mode == INDLY: s = "[$%02X],Y" % v
        elif mode == SR: s = "$%02X,S" % v
        elif mode == SRY: s = "($%02X,S),Y" % v
        elif mode == REL: s = "$%04X" % ((addr + 2 + (v - 256 if v > 127 else v)) & 0xFFFF)
        elif mode == RELL: s = "$%04X" % ((addr + 3 + (v - 65536 if v > 32767 else v)) & 0xFFFF)
        elif mode == MV: s = "$%02X,$%02X" % (arg[1], arg[0])
        elif mode == AIND: s = "($%04X)" % v
        elif mode == AINDX: s = "($%04X,X)" % v
        elif mode == AINDL: s = "[$%04X]" % v
        lines.append("%06X  %-12s %s %s" % (addr, " ".join("%02X" % b for b in rom[o:o + 1 + n]), name, s))
        if name == "REP":
            if v & 0x20: m = 0
            if v & 0x10: x = 0
        if name == "SEP":
            if v & 0x20: m = 1
            if v & 0x10: x = 1
        addr += 1 + n
    return lines

if __name__ == "__main__":
    rom = open(sys.argv[1], "rb").read()
    if len(rom) % 0x8000 == 0x200: rom = rom[0x200:]
    a = int(sys.argv[2], 16)
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 60
    m = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    x = int(sys.argv[5]) if len(sys.argv) > 5 else 1
    print("\n".join(dis(rom, a, n, m, x)))
