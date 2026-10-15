// Uses PRX relocations to list every 32-bit word in data sections that points into .text
// (vtables, callbacks, jump tables), and reports how many targets are not yet function
// entries or not yet disassembled. Writes <outDir>/<program>.datapointers.json and
// <outDir>/<program>.missing_entries.txt (one address per line).
// Usage (headless, -readOnly): -postScript DataPointersIntoText.java <outDir>
//@category GoW_PSP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.reloc.Relocation;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;

public class DataPointersIntoText extends GhidraScript {
    @Override
    public void run() throws Exception {
        Path outDir = Path.of(getScriptArgs()[0]);
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        Map<String, Integer> relocsByBlock = new TreeMap<>();
        TreeSet<Long> targets = new TreeSet<>();
        int dataWordsIntoText = 0;
        Iterator<Relocation> it = currentProgram.getRelocationTable().getRelocations();
        while (it.hasNext()) {
            Relocation r = it.next();
            MemoryBlock b = currentProgram.getMemory().getBlock(r.getAddress());
            String bn = b == null ? "<none>" : b.getName();
            relocsByBlock.merge(bn, 1, Integer::sum);
            if (b == null || b.isExecute() || !b.isInitialized()) continue;
            if (r.getAddress().getOffset() % 4 != 0) continue;
            long v = getInt(r.getAddress()) & 0xffffffffL;
            Address t = toAddr(v);
            if (text.contains(t) && v % 4 == 0) { dataWordsIntoText++; targets.add(v); }
        }
        int isEntry = 0, insideFunction = 0, undefined = 0, notEntryButInstruction = 0;
        List<String> missing = new ArrayList<>();
        for (long v : targets) {
            Address t = toAddr(v);
            if (getFunctionAt(t) != null) isEntry++;
            else {
                if (getInstructionAt(t) == null) { undefined++; missing.add(t.toString()); }
                else if (getFunctionContaining(t) != null) insideFunction++;
                else { notEntryButInstruction++; missing.add(t.toString()); }
            }
        }
        Files.write(outDir.resolve(currentProgram.getName() + ".missing_entries.txt"), missing, StandardCharsets.UTF_8);
        StringBuilder j = new StringBuilder("{\"program\":\"" + currentProgram.getName() + "\",\"relocations_by_block\":{");
        int i = 0;
        for (var e : relocsByBlock.entrySet()) j.append(i++ == 0 ? "" : ",").append('"').append(e.getKey()).append("\":").append(e.getValue());
        j.append("},\"data_words_into_text\":").append(dataWordsIntoText).append(",\"distinct_targets\":").append(targets.size())
            .append(",\"targets_function_entry\":").append(isEntry).append(",\"targets_inside_function\":").append(insideFunction)
            .append(",\"targets_undefined\":").append(undefined).append(",\"targets_instruction_no_function\":").append(notEntryButInstruction).append("}");
        Files.writeString(outDir.resolve(currentProgram.getName() + ".datapointers.json"), j.toString(), StandardCharsets.UTF_8);
        println("DATAPTR " + j);
    }
}
