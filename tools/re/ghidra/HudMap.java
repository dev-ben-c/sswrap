// For every SimGui::Hud* class: Borland type descriptor -> vtable -> slot 30 (onRender).
// Also prints how slot 30 returns (ret vs ret N) to pin the calling convention.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;

public class HudMap extends GhidraScript {
    Memory mem; FunctionManager fm;
    Address descriptor(Address s) throws Exception {
        for (int back = 8; back <= 256; back++) {
            Address a = s.subtract(back);
            try { if ((mem.getShort(a.add(6)) & 0xffff) == back) return a; } catch (Exception e) {}
        }
        return null;
    }
    Address vtable(Address td) throws Exception {
        long v = td.getOffset();
        byte[] pat = { (byte)v, (byte)(v>>8), (byte)(v>>16), (byte)(v>>24) };
        Address start = currentProgram.getMinAddress(), hit;
        while ((hit = mem.findBytes(start, pat, null, true, monitor)) != null) {
            start = hit.add(1);
            if (fm.getFunctionContaining(hit) != null) continue;
            long w1 = mem.getInt(hit.add(4)) & 0xffffffffL, w2 = mem.getInt(hit.add(8)) & 0xffffffffL;
            long f0 = mem.getInt(hit.add(12)) & 0xffffffffL;
            MemoryBlock b = mem.getBlock(toAddr(f0));
            if (w1 == 0 && w2 == 0 && b != null && b.isExecute()) return hit.add(12);
        }
        return null;
    }
    @Override public void run() throws Exception {
        mem = currentProgram.getMemory(); fm = currentProgram.getFunctionManager();
        Listing lst = currentProgram.getListing();
        // every "SimGui::Hud<Name>\0" string that is a plain class name
        Address start = currentProgram.getMinAddress(), s;
        byte[] pat = "SimGui::Hud".getBytes("ASCII");
        java.util.Set<String> seen = new java.util.TreeSet<>();
        while ((s = mem.findBytes(start, pat, null, true, monitor)) != null) {
            start = s.add(1);
            StringBuilder n = new StringBuilder();
            for (int i = 0; i < 64; i++) { byte c = mem.getByte(s.add(i)); if (c == 0) break; n.append((char) c); }
            String name = n.toString();
            if (!name.matches("SimGui::Hud[A-Za-z]+") || !seen.add(name)) continue;
            Address td = descriptor(s), vt = td == null ? null : vtable(td);
            if (vt == null) { println(String.format("%-28s no vtable", name)); continue; }
            long r = mem.getInt(vt.add(30 * 4)) & 0xffffffffL;
            Function f = fm.getFunctionAt(toAddr(r));
            String ret = "?";
            if (f != null) {
                for (Instruction ins : lst.getInstructions(f.getBody(), true))
                    if (ins.getMnemonicString().startsWith("RET")) { ret = ins.toString(); break; }
            }
            println(String.format("%-28s vtable=%s slot30@%s -> %08x  %s", name, vt, vt.add(120), r, ret));
        }
    }
}
