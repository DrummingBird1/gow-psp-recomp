// Disassembles and creates functions at relocation-derived code pointers that are not yet
// function entries (from DataPointersIntoText's <program>.missing_entries.txt).
// Run as a preScript so the following auto-analysis picks up the new code.
// Usage (headless): -preScript SeedFunctionsFromRelocations.java <outDir>
//@category GoW_PSP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

public class SeedFunctionsFromRelocations extends GhidraScript {
    @Override
    public void run() throws Exception {
        Path list = Path.of(getScriptArgs()[0]).resolve(currentProgram.getName() + ".missing_entries.txt");
        int seeded = 0, failed = 0;
        for (String line : Files.readAllLines(list, StandardCharsets.UTF_8)) {
            if (line.isBlank()) continue;
            Address a = toAddr(line.trim());
            if (getFunctionAt(a) != null) continue;
            if (getInstructionAt(a) == null) disassemble(a);
            if (getInstructionAt(a) != null && createFunction(a, null) != null) seeded++;
            else failed++;
        }
        println("SEEDED " + currentProgram.getName() + ": " + seeded + " functions, " + failed + " failed");
    }
}
