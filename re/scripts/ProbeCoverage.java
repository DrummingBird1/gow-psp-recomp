// Classifies undefined bytes in .text (padding vs. possible code) and probes whether
// the non-zero ranges decode as Allegrex code. Run with -readOnly: probe disassembly
// is discarded. Writes <outDir>/<program>.coverage.json.
// Usage (headless): -postScript ProbeCoverage.java <outDir>
//@category GoW_PSP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Bookmark;
import ghidra.program.model.listing.BookmarkType;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;

public class ProbeCoverage extends GhidraScript {
    @Override
    public void run() throws Exception {
        Path outDir = Path.of(getScriptArgs()[0]);
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        AddressSet textSet = new AddressSet(text.getStart(), text.getEnd());

        // Errors recorded by the original auto-analysis
        int errorBookmarks = 0;
        Map<String, Integer> errorKinds = new TreeMap<>();
        Iterator<Bookmark> bm = currentProgram.getBookmarkManager().getBookmarksIterator(BookmarkType.ERROR);
        while (bm.hasNext()) {
            Bookmark b = bm.next();
            errorBookmarks++;
            String k = b.getCategory() + ": " + b.getComment().replaceAll("[0-9a-fA-F]{8}", "#");
            errorKinds.merge(k.length() > 120 ? k.substring(0, 120) : k, 1, Integer::sum);
        }

        AddressSetView undefined = currentProgram.getListing().getUndefinedRanges(textSet, true, monitor);
        long zeroBytes = 0, nonZeroBytes = 0, nonZeroRanges = 0, decodedRanges = 0, failedRanges = 0, probeInstructions = 0;
        Map<String, Integer> failedOpcodes = new TreeMap<>();
        List<AddressRange> nonZero = new ArrayList<>();
        for (AddressRange r : undefined) {
            byte[] bytes = getBytes(r.getMinAddress(), (int) r.getLength());
            int nz = 0;
            for (byte b : bytes) if (b != 0) nz++;
            zeroBytes += bytes.length - nz;
            nonZeroBytes += nz;
            if (nz > 0) nonZero.add(r);
        }
        for (AddressRange r : nonZero) {
            monitor.checkCancelled();
            nonZeroRanges++;
            // Skip leading zero words (alignment) before probing
            Address a = r.getMinAddress();
            while (a.compareTo(r.getMaxAddress()) < 0 && getInt(a) == 0) a = a.add(4);
            int word = getInt(a);
            disassemble(a);
            Instruction ins = getInstructionAt(a);
            if (ins == null) {
                failedRanges++;
                failedOpcodes.merge(String.format("op=0x%02x", (word >>> 26) & 0x3f), 1, Integer::sum);
            } else {
                decodedRanges++;
                for (Instruction i : currentProgram.getListing().getInstructions(new AddressSet(r.getMinAddress(), r.getMaxAddress()), true))
                    probeInstructions++;
            }
        }
        StringBuilder j = new StringBuilder("{");
        j.append("\"program\":\"").append(currentProgram.getName()).append("\"");
        j.append(",\"error_bookmarks\":").append(errorBookmarks);
        j.append(",\"error_kinds\":{");
        int i = 0;
        for (var e : errorKinds.entrySet())
            j.append(i++ == 0 ? "" : ",").append('"').append(e.getKey().replace("\\", "\\\\").replace("\"", "'")).append("\":").append(e.getValue());
        j.append("},\"undefined_ranges\":").append(undefined.getNumAddressRanges());
        j.append(",\"undefined_zero_bytes\":").append(zeroBytes).append(",\"undefined_nonzero_bytes\":").append(nonZeroBytes);
        j.append(",\"nonzero_ranges\":").append(nonZeroRanges).append(",\"probe_decoded_ranges\":").append(decodedRanges);
        j.append(",\"probe_failed_ranges\":").append(failedRanges).append(",\"probe_instructions\":").append(probeInstructions);
        j.append(",\"probe_failed_opcodes\":{");
        i = 0;
        for (var e : failedOpcodes.entrySet()) j.append(i++ == 0 ? "" : ",").append('"').append(e.getKey()).append("\":").append(e.getValue());
        j.append("}}");
        Files.writeString(outDir.resolve(currentProgram.getName() + ".coverage.json"), j.toString(), StandardCharsets.UTF_8);
        println("COVERAGE " + j);
    }
}
