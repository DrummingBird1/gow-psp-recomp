// Exports the recompiler input for the current program:
//   <outDir>/<program>.recomp.json  - functions (entry, name, body ranges), jump tables
//                                    (computed jr -> targets), resolved jalr targets
//   <outDir>/<program>.image0.bin   - loaded image at Ghidra's base (initialized blocks;
//                                    gaps and .bss are zero), for verifying the relocator
// Usage (headless, -readOnly): -postScript ExportRecompInput.java <outDir>
//@category GoW_PSP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;

public class ExportRecompInput extends GhidraScript {
    static String hex(Address a) { return "0x" + Long.toHexString(a.getOffset()); }

    List<String> flowTargets(Address from) {
        List<String> t = new ArrayList<>();
        for (Reference r : getReferencesFrom(from))
            if (r.getReferenceType().isFlow() && !r.getReferenceType().isFallthrough()) t.add(hex(r.getToAddress()));
        Collections.sort(t);
        return t;
    }

    @Override
    public void run() throws Exception {
        Path outDir = Path.of(getScriptArgs()[0]);
        String prog = currentProgram.getName();
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        AddressSet textSet = new AddressSet(text.getStart(), text.getEnd());

        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(outDir.resolve(prog + ".recomp.json"), StandardCharsets.UTF_8))) {
            w.print("{\"program\":\"" + prog + "\",\"image_base\":\"" + hex(currentProgram.getImageBase()) + "\"");
            w.print(",\"text\":[\"" + hex(text.getStart()) + "\",\"" + hex(text.getEnd()) + "\"]");
            w.print(",\"functions\":[");
            boolean first = true;
            for (Function f : currentProgram.getFunctionManager().getFunctions(textSet, true)) {
                monitor.checkCancelled();
                StringBuilder ranges = new StringBuilder();
                for (AddressRange r : f.getBody()) {
                    if (ranges.length() > 0) ranges.append(',');
                    ranges.append("[\"").append(hex(r.getMinAddress())).append("\",\"").append(hex(r.getMaxAddress())).append("\"]");
                }
                w.print((first ? "" : ",") + "{\"addr\":\"" + hex(f.getEntryPoint()) + "\",\"name\":\"" + f.getName().replace("\"", "'")
                    + "\",\"ranges\":[" + ranges + "]}");
                first = false;
            }
            w.print("],\"jumptables\":[");
            first = true;
            StringBuilder calls = new StringBuilder();
            for (Instruction ins : currentProgram.getListing().getInstructions(textSet, true)) {
                monitor.checkCancelled();
                String m = ins.getMnemonicString().replaceFirst("^_", "");
                if (m.equals("jr")) {
                    Object[] ops = ins.getOpObjects(0);
                    boolean ra = ops.length > 0 && ops[0] instanceof Register && ((Register) ops[0]).getName().equals("ra");
                    if (ra) continue;
                    List<String> t = flowTargets(ins.getAddress());
                    w.print((first ? "" : ",") + "{\"at\":\"" + hex(ins.getAddress()) + "\",\"targets\":[" + quoteAll(t) + "]}");
                    first = false;
                } else if (m.equals("jalr")) {
                    List<String> t = flowTargets(ins.getAddress());
                    if (t.isEmpty()) continue;
                    calls.append(calls.length() == 0 ? "" : ",").append("{\"at\":\"").append(hex(ins.getAddress()))
                        .append("\",\"targets\":[").append(quoteAll(t)).append("]}");
                }
            }
            w.print("],\"indirect_calls\":[" + calls + "]}");
        }

        // Loaded image at the current base, for relocator verification
        long end = 0;
        for (MemoryBlock b : currentProgram.getMemory().getBlocks())
            if (b.isLoaded() && !b.isOverlay() && b.getStart().getAddressSpace().isMemorySpace() && !b.getName().startsWith("_elf")
                && !b.getName().startsWith(".rel") && !b.getName().startsWith(".s") && !b.getName().equals("unallocated_0"))
                end = Math.max(end, b.getEnd().getOffset() + 1);
        byte[] image = new byte[(int) end];
        StringBuilder blocks = new StringBuilder();
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            long s = b.getStart().getOffset();
            if (!b.isInitialized() || s + b.getSize() > end || b.getName().startsWith("_elf") || b.getName().startsWith(".rel")
                || b.getName().equals(".shstrtab") || b.getName().equals(".strtab") || b.getName().equals(".symtab") || b.getName().equals("unallocated_0"))
                continue;
            b.getBytes(b.getStart(), image, (int) s, (int) b.getSize());
            blocks.append(blocks.length() == 0 ? "" : ",").append(String.format("{\"name\":\"%s\",\"start\":%d,\"size\":%d}", b.getName(), s, b.getSize()));
        }
        Files.write(outDir.resolve(prog + ".image0.bin"), image);
        Files.writeString(outDir.resolve(prog + ".image0.json"), "{\"size\":" + end + ",\"blocks\":[" + blocks + "]}", StandardCharsets.UTF_8);
        println("RECOMP-INPUT " + prog + " image=" + end + " bytes");
    }

    static String quoteAll(List<String> t) {
        StringBuilder sb = new StringBuilder();
        for (String s : t) sb.append(sb.length() == 0 ? "" : ",").append('"').append(s).append('"');
        return sb.toString();
    }
}
