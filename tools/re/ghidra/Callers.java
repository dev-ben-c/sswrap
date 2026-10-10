// Print the functions that call the function containing each given address.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
public class Callers extends GhidraScript {
    @Override public void run() throws Exception {
        for (String a : getScriptArgs()) {
            Function f = getFunctionContaining(toAddr(Long.parseLong(a, 16)));
            if (f == null) { println("no function at " + a); continue; }
            println("== callers of " + f.getEntryPoint());
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                Function c = getFunctionContaining(r.getFromAddress());
                println("   " + r.getFromAddress() + " in " + (c != null ? c.getEntryPoint() : "?"));
            }
        }
    }
}
