// Exports static-recompilation feasibility metrics for the current program.
// Writes <outDir>/<program>.metrics.json and <outDir>/<program>.functions.csv.
// Usage (headless): -postScript ExportRecompMetrics.java <outDir>
//@category GoW_PSP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.symbol.Symbol;

import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class ExportRecompMetrics extends GhidraScript {
    static final Set<String> VFPU_MEMORY = Set.of("lv.s", "lv.q", "sv.s", "sv.q", "lvl.q", "lvr.q", "svl.q",
        "svr.q", "mfv", "mtv", "mfvc", "mtvc", "bvf", "bvt", "bvfl", "bvtl", "mfvme", "mtvme");
    static final Set<String> VFPU_PREFIX = Set.of("vpfxs", "vpfxt", "vpfxd");
    static final Set<String> FPU_MISC = Set.of("lwc1", "swc1", "mfc1", "mtc1", "cfc1", "ctc1", "bc1f", "bc1t",
        "bc1fl", "bc1tl");
    static final Set<String> ALLEGREX_EXT = Set.of("max", "min", "ext", "ins", "wsbh", "wsbw", "seb", "seh",
        "bitrev", "clz", "clo", "madd", "maddu", "msub", "msubu", "rotr", "rotrv", "movz", "movn");
    static final Set<String> PRIVILEGED = Set.of("mfic", "mtic", "halt", "cache", "sync", "eret", "mfc0", "mtc0");
    static final Set<String> BRANCH_LIKELY = Set.of("beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl", "bltzall",
        "bgezall", "bc1fl", "bc1tl", "bvfl", "bvtl");
    static final Pattern SOURCE_PATH = Pattern.compile("[\\w./\\\\:-]*\\.(cpp|cc|c|h|hpp)\\b", Pattern.CASE_INSENSITIVE);
    static final Pattern FILE_EXT = Pattern.compile("\\.([A-Za-z0-9]{2,4})$");

    static String mnemonic(Instruction ins) {
        String m = ins.getMnemonicString().toLowerCase(Locale.ROOT);
        return m.startsWith("_") ? m.substring(1) : m; // Ghidra marks delay-slot instructions with '_'
    }

    static boolean isVfpu(String m) {
        return m.startsWith("v") || VFPU_MEMORY.contains(m);
    }

    static boolean isFpu(String m) {
        return !isVfpu(m) && (m.endsWith(".s") || m.startsWith("c.") || m.startsWith("cvt.") || FPU_MISC.contains(m));
    }

    static boolean isJrRa(Instruction ins) {
        Object[] ops = ins.getOpObjects(0);
        return ops.length > 0 && ops[0] instanceof Register && ((Register) ops[0]).getName().equals("ra");
    }

    int resolvedFlowCount(ReferenceManager refs, Address from) {
        int n = 0;
        for (Reference r : refs.getReferencesFrom(from))
            if (r.getReferenceType().isFlow() && !r.getReferenceType().isFallthrough())
                n++;
        return n;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Path outDir = Path.of(args.length > 0 ? args[0] : "E:/AI/GoW_PSP/repo/re/out");
        Files.createDirectories(outDir);
        String prog = currentProgram.getName();
        Listing listing = currentProgram.getListing();
        ReferenceManager refs = currentProgram.getReferenceManager();
        Json j = new Json();
        j.str("program", prog).str("language", currentProgram.getLanguageID().toString())
            .str("format", currentProgram.getExecutableFormat()).str("image_base", currentProgram.getImageBase().toString());

        // Memory layout
        MemoryBlock text = null;
        List<String> blocks = new ArrayList<>();
        AddressSet stubSpace = new AddressSet();
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            blocks.add(new Json().str("name", b.getName()).str("start", b.getStart().toString()).num("size", b.getSize())
                .str("perm", (b.isRead() ? "r" : "-") + (b.isWrite() ? "w" : "-") + (b.isExecute() ? "x" : "-"))
                .bool("initialized", b.isInitialized()).toString());
            if (b.getName().equals(".text")) text = b;
            if (b.getName().startsWith(".sceStub.text")) stubSpace.add(b.getStart(), b.getEnd());
        }
        j.raw("memory_blocks", "[" + String.join(",", blocks) + "]");
        if (text == null) throw new IllegalStateException("No .text block");
        AddressSet textSet = new AddressSet(text.getStart(), text.getEnd());

        // Coverage of .text by instructions / data / undefined bytes
        long instrBytes = 0, instrCount = 0;
        Map<String, Long> mnem = new HashMap<>();
        long vfpu = 0, vfpuPrefix = 0, fpu = 0, allegrexExt = 0, privileged = 0, branchLikely = 0, syscalls = 0;
        long jrRa = 0, jrIndirect = 0, jrIndirectResolved = 0, jalr = 0, jalrResolved = 0, directCalls = 0;
        for (Instruction ins : listing.getInstructions(textSet, true)) {
            monitor.checkCancelled();
            instrCount++;
            instrBytes += ins.getLength();
            String m = mnemonic(ins);
            mnem.merge(m, 1L, Long::sum);
            if (isVfpu(m)) { vfpu++; if (VFPU_PREFIX.contains(m)) vfpuPrefix++; }
            else if (isFpu(m)) fpu++;
            if (ALLEGREX_EXT.contains(m)) allegrexExt++;
            if (PRIVILEGED.contains(m)) privileged++;
            if (BRANCH_LIKELY.contains(m)) branchLikely++;
            if (m.equals("syscall")) syscalls++;
            if (m.equals("jal")) directCalls++;
            if (m.equals("jr")) {
                if (isJrRa(ins)) jrRa++;
                else { jrIndirect++; if (resolvedFlowCount(refs, ins.getAddress()) > 0) jrIndirectResolved++; }
            }
            if (m.equals("jalr")) { jalr++; if (resolvedFlowCount(refs, ins.getAddress()) > 0) jalrResolved++; }
        }
        long dataBytesInText = 0;
        for (Data d : listing.getDefinedData(textSet, true)) dataBytesInText += d.getLength();
        AddressSetView undefined = listing.getUndefinedRanges(textSet, true, monitor);
        long undefinedBytes = 0;
        for (var r : undefined) undefinedBytes += r.getLength();
        j.raw("text", new Json().num("size", text.getSize()).num("instructions", instrCount).num("instruction_bytes", instrBytes)
            .num("data_bytes", dataBytesInText).num("undefined_bytes", undefinedBytes)
            .num("undefined_ranges", undefined.getNumAddressRanges()).toString());
        j.raw("instruction_classes", new Json().num("vfpu", vfpu).num("vfpu_prefix", vfpuPrefix).num("fpu", fpu)
            .num("allegrex_ext", allegrexExt).num("privileged", privileged).num("branch_likely", branchLikely)
            .num("syscall", syscalls).num("jal", directCalls).num("jr_ra", jrRa).num("jr_indirect", jrIndirect)
            .num("jr_indirect_resolved", jrIndirectResolved).num("jalr", jalr).num("jalr_resolved", jalrResolved).toString());
        List<Map.Entry<String, Long>> top = new ArrayList<>(mnem.entrySet());
        top.sort((a, b) -> Long.compare(b.getValue(), a.getValue()));
        Json mj = new Json();
        for (var e : top.subList(0, Math.min(90, top.size()))) mj.num(e.getKey(), e.getValue());
        j.raw("top_mnemonics", mj.toString()).num("distinct_mnemonics", mnem.size());

        // Functions
        List<Integer> sizes = new ArrayList<>();
        long fnCount = 0, thunks = 0, fnWithVfpu = 0, fnWithPrefix = 0, fnWithIndirectJr = 0, fnWithUnresolvedJr = 0;
        long fnAddressTaken = 0, fnUnreferenced = 0, fnDataOnly = 0, instrInFunctions = 0;
        try (PrintWriter csv = new PrintWriter(Files.newBufferedWriter(outDir.resolve(prog + ".functions.csv"), StandardCharsets.UTF_8))) {
            csv.println("address,name,instructions,bytes,vfpu,vfpu_prefix,fpu,jalr,jr_indirect,jr_indirect_unresolved,call_refs,data_refs");
            for (Function f : currentProgram.getFunctionManager().getFunctions(textSet, true)) {
                monitor.checkCancelled();
                fnCount++;
                if (f.isThunk()) thunks++;
                int n = 0, fv = 0, fp = 0, ff = 0, fj = 0, fi = 0, fu = 0;
                for (Instruction ins : listing.getInstructions(f.getBody(), true)) {
                    n++;
                    String m = mnemonic(ins);
                    if (isVfpu(m)) { fv++; if (VFPU_PREFIX.contains(m)) fp++; }
                    else if (isFpu(m)) ff++;
                    if (m.equals("jalr")) fj++;
                    if (m.equals("jr") && !isJrRa(ins)) { fi++; if (resolvedFlowCount(refs, ins.getAddress()) == 0) fu++; }
                }
                instrInFunctions += n;
                sizes.add(n);
                if (fv > 0) fnWithVfpu++;
                if (fp > 0) fnWithPrefix++;
                if (fi > 0) fnWithIndirectJr++;
                if (fu > 0) fnWithUnresolvedJr++;
                int callRefs = 0, dataRefs = 0, otherRefs = 0;
                for (Reference r : refs.getReferencesTo(f.getEntryPoint())) {
                    if (r.getReferenceType().isCall()) callRefs++;
                    else if (r.getReferenceType().isData()) dataRefs++;
                    else otherRefs++;
                }
                if (dataRefs > 0) fnAddressTaken++;
                if (callRefs + dataRefs + otherRefs == 0) fnUnreferenced++;
                if (dataRefs > 0 && callRefs == 0) fnDataOnly++;
                csv.printf("%s,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d%n", f.getEntryPoint(), f.getName().replace(',', '_'), n,
                    f.getBody().getNumAddresses(), fv, fp, ff, fj, fi, fu, callRefs, dataRefs);
            }
        }
        Collections.sort(sizes);
        int sz = sizes.size();
        j.raw("functions", new Json().num("count", fnCount).num("thunks", thunks)
            .num("instructions_in_functions", instrInFunctions).num("instructions_outside_functions", instrCount - instrInFunctions)
            .num("median_instructions", sz == 0 ? 0 : sizes.get(sz / 2)).num("p90_instructions", sz == 0 ? 0 : sizes.get((int) (sz * 0.9)))
            .num("max_instructions", sz == 0 ? 0 : sizes.get(sz - 1)).num("with_vfpu", fnWithVfpu).num("with_vfpu_prefix", fnWithPrefix)
            .num("with_indirect_jr", fnWithIndirectJr).num("with_unresolved_indirect_jr", fnWithUnresolvedJr)
            .num("address_taken", fnAddressTaken).num("data_ref_only", fnDataOnly).num("unreferenced", fnUnreferenced).toString());

        // Imports: PSP import stubs are 8 bytes each inside .sceStub.text.<library>
        Json imports = new Json();
        long importTotal = 0, importUnnamed = 0;
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (!b.getName().startsWith(".sceStub.text")) continue;
            String lib = b.getName().substring(".sceStub.text".length()).replaceFirst("^\\.", "");
            List<String> fns = new ArrayList<>();
            for (Address a = b.getStart(); a.compareTo(b.getEnd()) < 0; a = a.add(8)) {
                Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(a);
                String name = s == null ? "<none>@" + a : s.getName();
                int callers = 0;
                for (Reference r : refs.getReferencesTo(a)) if (r.getReferenceType().isCall() || r.getReferenceType().isJump()) callers++;
                fns.add(new Json().str("name", name).num("callers", callers).toString());
                importTotal++;
                if (s == null || name.startsWith("FUN_") || name.matches(".*[0-9A-Fa-f]{8}$")) importUnnamed++;
            }
            imports.raw(lib, "[" + String.join(",", fns) + "]");
        }
        j.raw("imports", imports.toString()).num("import_count", importTotal).num("import_unnamed", importUnnamed);

        List<String> exports = new ArrayList<>();
        var it = currentProgram.getSymbolTable().getExternalEntryPointIterator();
        while (it.hasNext()) {
            Address a = it.next();
            Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(a);
            exports.add(Json.quote((s == null ? "?" : s.getName()) + "@" + a));
        }
        j.raw("exports", "[" + String.join(",", exports) + "]");

        // Strings (target content: counted and sampled as data only)
        long strings = 0;
        TreeSet<String> sourcePaths = new TreeSet<>();
        Map<String, Long> exts = new HashMap<>();
        for (Data d : listing.getDefinedData(true)) {
            monitor.checkCancelled();
            if (!d.hasStringValue()) continue;
            Object v = d.getValue();
            if (!(v instanceof String)) continue;
            String s = ((String) v).trim();
            strings++;
            Matcher m = SOURCE_PATH.matcher(s);
            if (m.find() && sourcePaths.size() < 400) sourcePaths.add(m.group());
            Matcher e = FILE_EXT.matcher(s);
            if (e.find() && s.length() < 120) exts.merge(e.group(1).toLowerCase(Locale.ROOT), 1L, Long::sum);
        }
        List<Map.Entry<String, Long>> extTop = new ArrayList<>(exts.entrySet());
        extTop.sort((a, b) -> Long.compare(b.getValue(), a.getValue()));
        Json ej = new Json();
        for (var e : extTop.subList(0, Math.min(25, extTop.size()))) ej.num(e.getKey(), e.getValue());
        List<String> sp = new ArrayList<>();
        for (String s : sourcePaths) sp.add(Json.quote(s));
        j.num("strings", strings).num("source_path_strings", sourcePaths.size()).raw("source_path_samples", "[" + String.join(",", sp) + "]")
            .raw("filename_extensions", ej.toString());

        Files.writeString(outDir.resolve(prog + ".metrics.json"), j.toString(), StandardCharsets.UTF_8);
        println("METRICS written for " + prog + ": " + fnCount + " functions, " + instrCount + " instructions");
    }

    /** Minimal ordered JSON object builder. */
    static class Json {
        private final StringBuilder sb = new StringBuilder();
        private boolean first = true;

        static String quote(String s) {
            StringBuilder q = new StringBuilder("\"");
            for (char c : s.toCharArray()) {
                if (c == '"' || c == '\\') q.append('\\').append(c);
                else if (c < 0x20) q.append(String.format("\\u%04x", (int) c));
                else q.append(c);
            }
            return q.append('"').toString();
        }

        private Json key(String k) { sb.append(first ? "" : ",").append(quote(k)).append(':'); first = false; return this; }
        Json str(String k, String v) { return key(k).raw0(v == null ? "null" : quote(v)); }
        Json num(String k, long v) { return key(k).raw0(Long.toString(v)); }
        Json bool(String k, boolean v) { return key(k).raw0(Boolean.toString(v)); }
        Json raw(String k, String v) { return key(k).raw0(v); }
        private Json raw0(String v) { sb.append(v); return this; }
        @Override public String toString() { return "{" + sb + "}"; }
    }
}
