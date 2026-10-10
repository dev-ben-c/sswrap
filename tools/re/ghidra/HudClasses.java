// Map Starsiege's SimGui::Hud* classes to their type records, vtables and code.
// Borland C++ type records hold a pointer to (or near) the class-name string, so for
// every class-name string we look for any data pointer into the bytes just before it.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.util.*;

public class HudClasses extends GhidraScript {
    @Override public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        Listing lst = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        FunctionManager fm = currentProgram.getFunctionManager();
        String[] classes = { "SimGui::HudBaseCtrl", "SimGui::HudMtrCtrl", "SimGui::HudMtrRadar",
            "SimGui::HudMtrAimReticle", "SimGui::HudMtrWeapDisplay", "SimGui::HudManager", "SimGui::HudSimpleText" };
        for (String cls : classes) {
            Address s = find(null, (cls + "\0").getBytes("ASCII"));
            if (s == null) { println(cls + ": string not found"); continue; }
            println("== " + cls + " name @ " + s);
            // any reference (code or data) into [s-32, s]
            for (int back = 0; back <= 32; back += 1) {
                Address a = s.subtract(back);
                for (Reference r : rm.getReferencesTo(a)) {
                    Address from = r.getFromAddress();
                    Function f = fm.getFunctionContaining(from);
                    println(String.format("   ref to name-%d from %s (%s)%s", back, from, r.getReferenceType(),
                        f != null ? " in " + f.getName() + "@" + f.getEntryPoint() : " [data]"));
                    if (f == null) {
                        // data record: dump the dwords around it, they are usually the type record
                        StringBuilder sb = new StringBuilder("      record:");
                        for (int k = -16; k <= 16; k += 4) {
                            try { sb.append(String.format(" %s=%08x", k, mem.getInt(from.add(k)) & 0xffffffffL)); } catch (Exception e) {}
                        }
                        println(sb.toString());
                        for (Reference r2 : rm.getReferencesTo(from.subtract(back == 0 ? 0 : 0))) {
                            Function f2 = fm.getFunctionContaining(r2.getFromAddress());
                            println("         record referenced from " + r2.getFromAddress() + (f2 != null ? " in " + f2.getName() : ""));
                        }
                    }
                }
            }
            // raw search: little-endian pointer values equal to s-k anywhere in initialized data
            for (int back = 0; back <= 16; back += 4) {
                long v = s.subtract(back).getOffset();
                byte[] pat = { (byte)v, (byte)(v>>8), (byte)(v>>16), (byte)(v>>24) };
                Address hit = null; int n = 0;
                Address start = currentProgram.getMinAddress();
                while ((hit = mem.findBytes(start, pat, null, true, monitor)) != null && n < 8) {
                    Function f = fm.getFunctionContaining(hit);
                    println(String.format("   raw ptr to name-%d at %s%s", back, hit, f != null ? " (code in " + f.getName() + ")" : ""));
                    start = hit.add(1); n++;
                }
            }
        }
    }
}
