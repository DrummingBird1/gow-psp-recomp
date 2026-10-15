// Writes an operand-normalized fingerprint per .text function, for cross-game matching.
// Registers are kept; immediates, addresses and relocated constants become '#'.
// Writes <outDir>/<program>.fingerprints.csv (address,name,instructions,sha1).
// Usage (headless, -readOnly): -postScript FunctionFingerprints.java <outDir>
//@category GoW_PSP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;

import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.HexFormat;

public class FunctionFingerprints extends GhidraScript {
    @Override
    public void run() throws Exception {
        Path outDir = Path.of(getScriptArgs()[0]);
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        try (PrintWriter csv = new PrintWriter(Files.newBufferedWriter(outDir.resolve(currentProgram.getName() + ".fingerprints.csv"), StandardCharsets.UTF_8))) {
            csv.println("address,name,instructions,sha1");
            for (Function f : currentProgram.getFunctionManager().getFunctions(new AddressSet(text.getStart(), text.getEnd()), true)) {
                monitor.checkCancelled();
                MessageDigest sha = MessageDigest.getInstance("SHA-1");
                int n = 0;
                for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                    n++;
                    StringBuilder sb = new StringBuilder(ins.getMnemonicString().replaceFirst("^_", ""));
                    for (int op = 0; op < ins.getNumOperands(); op++) {
                        sb.append(op == 0 ? ' ' : ',');
                        for (Object o : ins.getOpObjects(op)) sb.append(o instanceof Register ? ((Register) o).getName() : "#");
                    }
                    sha.update(sb.append(';').toString().getBytes(StandardCharsets.UTF_8));
                }
                csv.printf("%s,%s,%d,%s%n", f.getEntryPoint(), f.getName().replace(',', '_'), n, HexFormat.of().formatHex(sha.digest()));
            }
        }
        println("FINGERPRINTS written for " + currentProgram.getName());
    }
}
