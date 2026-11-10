// Ghidra headless script: decompile every function that references a string
// containing one of the given substrings. Arguments: output file, then the
// substrings. Used on Protect's media server (stripped) to read its parsers.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.*;

public class DecompileByString extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0]);
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Map<Function, List<String>> found = new LinkedHashMap<>();
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Data d = it.next();
            if (!StringDataInstance.isString(d)) continue;
            String s = StringDataInstance.getStringDataInstance(d).getStringValue();
            if (s == null) continue;
            boolean hit = false;
            for (int i = 1; i < args.length; i++) if (s.contains(args[i])) hit = true;
            if (!hit) continue;
            for (Reference r : getReferencesTo(d.getAddress())) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f != null) found.computeIfAbsent(f, k -> new ArrayList<>()).add(s.trim());
            }
        }
        for (Map.Entry<Function, List<String>> e : found.entrySet()) {
            Function f = e.getKey();
            out.println("//==== " + f.getName() + " @ " + f.getEntryPoint() + " size " + f.getBody().getNumAddresses()
                        + " refs " + e.getValue());
            DecompileResults r = dec.decompileFunction(f, 120, monitor);
            out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
        }
        out.close();
        println("functions: " + found.size());
    }
}
