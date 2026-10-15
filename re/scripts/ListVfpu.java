// Lists every VFPU instruction in .text (address, word, Ghidra's disassembly) and a
// mnemonic histogram, for building the recompiler's VFPU support.
// Writes <outDir>/<program>.vfpu.tsv and prints the histogram.
// Usage (headless, -readOnly): -postScript ListVfpu.java <outDir>
//@category GoW_PSP
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

public class ListVfpu extends GhidraScript {
    @Override
    public void run() throws Exception {
        Path outDir = Path.of(getScriptArgs()[0]);
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        Map<String, Integer> hist = new TreeMap<>();
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(outDir.resolve(currentProgram.getName() + ".vfpu.tsv"), StandardCharsets.UTF_8))) {
            w.println("address\tword\tfunction\tdisassembly");
            for (Instruction ins : currentProgram.getListing().getInstructions(new AddressSet(text.getStart(), text.getEnd()), true)) {
                int word = getInt(ins.getAddress());
                int op = (word >>> 26) & 0x3f;
                boolean vfpu = op == 18 || op == 24 || op == 25 || op == 27 || op == 50 || op == 52 || op == 53 || op == 54
                    || op == 55 || op == 58 || op == 60 || op == 61 || op == 62 || op == 63;
                if (!vfpu) continue;
                String m = ins.getMnemonicString().replaceFirst("^_", "");
                hist.merge(m, 1, Integer::sum);
                Function f = getFunctionContaining(ins.getAddress());
                w.printf("%s\t%08x\t%s\t%s%n", ins.getAddress(), word, f == null ? "" : f.getEntryPoint().toString(), ins.toString());
            }
        }
        StringBuilder sb = new StringBuilder();
        for (var e : hist.entrySet()) sb.append(e.getKey()).append('=').append(e.getValue()).append(' ');
        println("VFPU " + currentProgram.getName() + ": " + sb);
    }
}
