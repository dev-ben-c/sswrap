// Dump the vtables of several HUD classes side by side and flag slots that differ, i.e. methods
// each class overrides (render should be one of them).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;

public class HudVtables extends GhidraScript {
    @Override public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        FunctionManager fm = currentProgram.getFunctionManager();
        String[] names = { "HudBaseCtrl", "HudSimpleText", "HudMtrRadar", "HudMtrAimReticle", "HudMtrWeapDisplay", "HudMtrShields", "HudMtrDamage", "Control" };
        long[][] vt = new long[names.length][];
        for (int c = 0; c < names.length; c++) {
            Address s = find(null, ("SimGui::" + names[c] + "\0").getBytes("ASCII"));
            Address td = null;
            for (int back = 8; back <= 256 && s != null; back++) {
                Address a = s.subtract(back);
                try { if ((mem.getShort(a.add(6)) & 0xffff) == back) { td = a; break; } } catch (Exception e) {}
            }
            if (td == null) { println(names[c] + ": no descriptor"); continue; }
            long v = td.getOffset();
            byte[] pat = { (byte)v, (byte)(v>>8), (byte)(v>>16), (byte)(v>>24) };
            Address start = currentProgram.getMinAddress(), hit;
            while ((hit = mem.findBytes(start, pat, null, true, monitor)) != null) {
                start = hit.add(1);
                if (fm.getFunctionContaining(hit) != null) continue;
                long w1 = mem.getInt(hit.add(4)) & 0xffffffffL, w2 = mem.getInt(hit.add(8)) & 0xffffffffL;
                long f0 = mem.getInt(hit.add(12)) & 0xffffffffL;
                if (w1 == 0 && w2 == 0 && fm.getFunctionAt(toAddr(f0)) != null) {
                    Address vs = hit.add(12);
                    long[] slots = new long[120];
                    for (int k = 0; k < 120; k++) slots[k] = mem.getInt(vs.add(4 * k)) & 0xffffffffL;
                    vt[c] = slots;
                    println(names[c] + " vtable @ " + vs + " (descriptor ptr @ " + hit + ")");
                    break;
                }
            }
        }
        StringBuilder hdr = new StringBuilder("slot ");
        for (String n : names) hdr.append(String.format("%-18s", n.length() > 17 ? n.substring(0, 17) : n));
        println(hdr.toString());
        for (int k = 0; k < 120; k++) {
            java.util.Set<Long> distinct = new java.util.HashSet<>();
            StringBuilder sb = new StringBuilder(String.format("%3d  ", k));
            boolean anyFn = false;
            for (int c = 0; c < names.length; c++) {
                if (vt[c] == null) { sb.append(String.format("%-18s", "-")); continue; }
                long f = vt[c][k]; MemoryBlock b = mem.getBlock(toAddr(f)); boolean isFn = b != null && b.isExecute();
                anyFn |= isFn; if (isFn) distinct.add(f);
                sb.append(String.format("%-18s", String.format("%08x%s", f, isFn ? "" : "?")));
            }
            if (!anyFn) break;
            println(sb.toString() + (distinct.size() > 2 ? "  <== overridden per class" : ""));
        }
    }
}
