// Names PSP import stubs from the module's .lib.stub table using a NID database,
// and writes <outDir>/<program>.imports.json with PPSSPP implementation status.
// Usage (headless): -postScript NamePspImports.java <nid.tsv> <outDir>
//@category GoW_PSP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;

public class NamePspImports extends GhidraScript {
    record Nid(String name, String modules, boolean implemented) {}

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Map<Long, Nid> db = new HashMap<>();
        for (String line : Files.readAllLines(Path.of(args[0]), StandardCharsets.UTF_8)) {
            String[] f = line.split("\t");
            if (f.length == 4) db.put(Long.parseLong(f[0], 16), new Nid(f[1], f[2], f[3].equals("1")));
        }
        Path outDir = Path.of(args[1]);
        Memory mem = currentProgram.getMemory();
        MemoryBlock stubs = mem.getBlock(".lib.stub");
        if (stubs == null) throw new IllegalStateException("No .lib.stub block");

        StringBuilder json = new StringBuilder("{\"program\":\"" + currentProgram.getName() + "\",\"libraries\":[");
        int total = 0, named = 0, implemented = 0, libs = 0;
        Address a = stubs.getStart();
        while (a.compareTo(stubs.getEnd()) < 0) {
            long libNamePtr = mem.getInt(a) & 0xffffffffL;
            int entLen = mem.getByte(a.add(8)) & 0xff;
            int funcCount = mem.getShort(a.add(10)) & 0xffff;
            long nidPtr = mem.getInt(a.add(12)) & 0xffffffffL;
            long stubPtr = mem.getInt(a.add(16)) & 0xffffffffL;
            if (entLen < 5) throw new IllegalStateException("Bad stub entry length at " + a);
            String lib = readCString(toAddr(libNamePtr));
            json.append(libs++ == 0 ? "" : ",").append("{\"name\":\"").append(lib).append("\",\"functions\":[");
            for (int i = 0; i < funcCount; i++) {
                long nid = mem.getInt(toAddr(nidPtr + 4L * i)) & 0xffffffffL;
                Address stub = toAddr(stubPtr + 8L * i);
                Nid n = db.get(nid);
                String name = n != null ? n.name() : String.format("%s_%08X", lib, nid);
                Function f = getFunctionAt(stub);
                if (f == null) f = createFunction(stub, name);
                if (f != null) f.setName(name, SourceType.IMPORTED);
                setPlateComment(stub, String.format("PSP import %s NID 0x%08X%s", lib, nid,
                    n == null ? " (not in PPSSPP NID table)" : n.implemented() ? " (PPSSPP: implemented)" : " (PPSSPP: listed, unimplemented)"));
                int callers = 0;
                for (Reference r : getReferencesTo(stub)) if (r.getReferenceType().isCall() || r.getReferenceType().isJump()) callers++;
                total++;
                if (n != null) named++;
                if (n != null && n.implemented()) implemented++;
                json.append(i == 0 ? "" : ",").append(String.format(
                    "{\"nid\":\"%08X\",\"name\":\"%s\",\"stub\":\"%s\",\"callers\":%d,\"ppsspp\":\"%s\"}",
                    nid, name, stub, callers, n == null ? "unknown" : n.implemented() ? "implemented" : "unimplemented"));
            }
            json.append("]}");
            a = a.add(entLen * 4L);
        }
        json.append("],\"total\":").append(total).append(",\"named\":").append(named)
            .append(",\"ppsspp_implemented\":").append(implemented).append("}");
        Files.writeString(outDir.resolve(currentProgram.getName() + ".imports.json"), json.toString(), StandardCharsets.UTF_8);
        println("IMPORTS " + currentProgram.getName() + ": " + total + " total, " + named + " named, " + implemented + " implemented in PPSSPP");
    }

    String readCString(Address a) throws Exception {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 128; i++) {
            byte b = currentProgram.getMemory().getByte(a.add(i));
            if (b == 0) break;
            sb.append((char) b);
        }
        return sb.toString();
    }
}
