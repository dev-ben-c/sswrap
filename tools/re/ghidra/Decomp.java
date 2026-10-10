// Decompile the functions given as script arguments (hex addresses) and print C.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;

public class Decomp extends GhidraScript {
    @Override public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (String a : getScriptArgs()) {
            Function f = getFunctionAt(toAddr(Long.parseLong(a, 16)));
            if (f == null) f = createFunction(toAddr(Long.parseLong(a, 16)), null);
            if (f == null) { println("no function at " + a); continue; }
            DecompileResults r = d.decompileFunction(f, 60, monitor);
            println("===== " + a + " =====\n" + (r.decompileCompleted() ? r.getDecompiledFunction().getC() : r.getErrorMessage()));
        }
    }
}
