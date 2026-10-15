// Decompiles an evenly spaced sample of .text functions plus every VFPU-using function,
// and records success, timing and decompiler warning markers.
// Writes <outDir>/<program>.decompile.json (summary) and .decompile.csv (per function).
// Usage (headless, -readOnly): -postScript SampleDecompile.java <outDir> <sampleSize>
//@category GoW_PSP
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;

import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;

public class SampleDecompile extends GhidraScript {
    static final String[] MARKERS = { "halt_baddata", "UNRECOVERED_JUMPTABLE", "WARNING", "unaff_", "in_", "CONCAT", "SUB4" };

    static boolean usesVfpu(GhidraScript s, Function f) {
        for (Instruction ins : s.getCurrentProgram().getListing().getInstructions(f.getBody(), true)) {
            String m = ins.getMnemonicString().toLowerCase(Locale.ROOT).replaceFirst("^_", "");
            if (m.startsWith("v") || m.startsWith("lv.") || m.startsWith("sv.") || m.equals("mfv") || m.equals("mtv")) return true;
        }
        return false;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Path outDir = Path.of(args[0]);
        int sampleSize = Integer.parseInt(args[1]);
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        List<Function> all = new ArrayList<>();
        currentProgram.getFunctionManager().getFunctions(new AddressSet(text.getStart(), text.getEnd()), true).forEach(all::add);
        LinkedHashMap<Function, String> chosen = new LinkedHashMap<>();
        int stride = Math.max(1, all.size() / sampleSize);
        for (int i = 0; i < all.size(); i += stride) chosen.put(all.get(i), "sample");
        for (Function f : all) if (usesVfpu(this, f)) chosen.merge(f, "vfpu", (a, b) -> a + "+" + b);

        DecompInterface d = new DecompInterface();
        d.setOptions(new DecompileOptions());
        d.openProgram(currentProgram);
        Map<String, int[]> stats = new TreeMap<>(); // group -> [count, ok, timeouts/fail, ms total]
        Map<String, Integer> markerHits = new TreeMap<>();
        try (PrintWriter csv = new PrintWriter(Files.newBufferedWriter(outDir.resolve(currentProgram.getName() + ".decompile.csv"), StandardCharsets.UTF_8))) {
            csv.println("address,group,ok,ms,c_lines," + String.join(",", MARKERS));
            for (var e : chosen.entrySet()) {
                monitor.checkCancelled();
                Function f = e.getKey();
                long t0 = System.nanoTime();
                DecompileResults r = d.decompileFunction(f, 60, monitor);
                long ms = (System.nanoTime() - t0) / 1_000_000;
                boolean ok = r.decompileCompleted() && r.getDecompiledFunction() != null;
                String c = ok ? r.getDecompiledFunction().getC() : "";
                StringBuilder row = new StringBuilder(String.format("%s,%s,%d,%d,%d", f.getEntryPoint(), e.getValue(), ok ? 1 : 0, ms, c.split("\n").length));
                for (String m : MARKERS) {
                    int hits = c.split(java.util.regex.Pattern.quote(m), -1).length - 1;
                    row.append(',').append(hits);
                    if (hits > 0) markerHits.merge(e.getValue().contains("vfpu") ? "vfpu:" + m : "sample:" + m, 1, Integer::sum);
                }
                csv.println(row);
                for (String g : e.getValue().split("\\+")) {
                    int[] s = stats.computeIfAbsent(g, k -> new int[4]);
                    s[0]++; if (ok) s[1]++; else s[2]++; s[3] += (int) ms;
                }
            }
        }
        d.dispose();
        StringBuilder j = new StringBuilder("{\"program\":\"" + currentProgram.getName() + "\",\"functions_total\":" + all.size() + ",\"groups\":{");
        int i = 0;
        for (var e : stats.entrySet())
            j.append(i++ == 0 ? "" : ",").append(String.format("\"%s\":{\"count\":%d,\"ok\":%d,\"failed\":%d,\"avg_ms\":%d}",
                e.getKey(), e.getValue()[0], e.getValue()[1], e.getValue()[2], e.getValue()[0] == 0 ? 0 : e.getValue()[3] / e.getValue()[0]));
        j.append("},\"functions_with_marker\":{");
        i = 0;
        for (var e : markerHits.entrySet()) j.append(i++ == 0 ? "" : ",").append('"').append(e.getKey()).append("\":").append(e.getValue());
        j.append("}}");
        Files.writeString(outDir.resolve(currentProgram.getName() + ".decompile.json"), j.toString(), StandardCharsets.UTF_8);
        println("DECOMPILE " + j);
    }
}
