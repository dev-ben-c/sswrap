// Borland C++ type descriptors (__tpdsc): u32 size, u16 mask, u16 nameOffset, ..., name inline.
// For each class name, walk back to the descriptor whose nameOffset points at the name, then
// find every pointer to that descriptor (vtables and RTTI code point at it).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;

public class HudTypeinfo extends GhidraScript {
    @Override public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        FunctionManager fm = currentProgram.getFunctionManager();
        String[] classes = { "SimGui::HudBaseCtrl", "SimGui::HudMtrCtrl", "SimGui::HudMtrRadar",
            "SimGui::HudMtrAimReticle", "SimGui::HudManager", "SimGui::HudSimpleText", "SimGui::Control" };
        for (String cls : classes) {
            Address s = find(null, (cls + "\0").getBytes("ASCII"));
            if (s == null) { println(cls + ": not found"); continue; }
            Address td = null;
            for (int back = 8; back <= 256; back++) {
                Address a = s.subtract(back);
                try { if ((mem.getShort(a.add(6)) & 0xffff) == back) { td = a; break; } } catch (Exception e) {}
            }
            println("== " + cls + " name@" + s + " descriptor@" + td);
            if (td == null) continue;
            long v = td.getOffset();
            byte[] pat = { (byte)v, (byte)(v>>8), (byte)(v>>16), (byte)(v>>24) };
            Address start = currentProgram.getMinAddress(), hit; int n = 0;
            while ((hit = mem.findBytes(start, pat, null, true, monitor)) != null && n < 12) {
                Function f = fm.getFunctionContaining(hit);
                StringBuilder sb = new StringBuilder(String.format("   ptr@%s%s  next:", hit,
                    f != null ? " (code " + f.getEntryPoint() + ")" : ""));
                if (f == null) for (int k = 4; k <= 32; k += 4) {
                    long w = mem.getInt(hit.add(k)) & 0xffffffffL;
                    Function g = fm.getFunctionAt(toAddr(w));
                    sb.append(String.format(" %08x%s", w, g != null ? "*" : ""));
                }
                println(sb.toString());
                start = hit.add(1); n++;
            }
        }
    }
}
