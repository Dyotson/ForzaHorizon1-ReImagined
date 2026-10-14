// Decomp.java: a headless Ghidra script that serves decompiles (tools/ghidra_decomp.sh).
// It reads addresses from $DECOMP_LIST (one per line, appended over time) and writes
// $DECOMP_OUT/<ADDR>.c; "quit" ends it, so does 30 minutes without a request. Before
// decompiling it clears the no-return flag of functions that return (the XEX import marks
// the __savegprlr helpers no-return, which cut every function after its prologue),
// re-disassembles after calls, and creates a function where Ghidra has none.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.AddressSet;

public class Decomp extends GhidraScript {
  public void run() throws Exception {
    String listFile = System.getenv("DECOMP_LIST");
    String outDir = System.getenv("DECOMP_OUT");
    int fixed = 0;
    int tx = currentProgram.startTransaction("fix");
    for (Function fn : currentProgram.getFunctionManager().getFunctions(true)) {
      if (!fn.hasNoReturn()) continue;
      boolean hasBlr = false;
      for (Instruction ins : currentProgram.getListing().getInstructions(fn.getBody(), true)) {
        if (ins.getMnemonicString().equals("blr")) { hasBlr = true; break; }
      }
      long e = fn.getEntryPoint().getOffset();
      // DECOMP_ALL_RETURN=1: every function returns (functions ending in a branch to the
      // register-restore helpers, like sub_82411478, were taken for noreturn).
      if (hasBlr || (e >= 0x82A7D000L && e < 0x82A7F000L) || System.getenv("DECOMP_ALL_RETURN") != null) {
        fn.setNoReturn(false); fixed++;
      }
    }
    println("noreturn fixed: " + fixed);
    DecompInterface di = new DecompInterface();
    di.openProgram(currentProgram);
    // serve loop: poll list file for new requests
    Set<String> done = new HashSet<>();
    long idle = 0;
    while (idle < 1800000) {
      List<String> lines = Files.exists(Paths.get(listFile)) ? Files.readAllLines(Paths.get(listFile)) : new ArrayList<>();
      boolean any = false;
      for (String l : lines) {
        l = l.trim();
        if (l.isEmpty() || done.contains(l)) continue;
        done.add(l);
        if (l.equals("quit")) { di.dispose(); return; }
        any = true;
        Address a = toAddr(Long.parseLong(l.replace("0x",""), 16));
        Function f = getFunctionContaining(a);
        if (f == null || !f.getEntryPoint().equals(a)) {
          if (getFunctionAt(a) == null && System.getenv("DECOMP_NOCREATE") == null) {
            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
            Function old = getFunctionContaining(a);
            if (old == null) new CreateFunctionCmd(a).applyTo(currentProgram, monitor);
            else { new CreateFunctionCmd(null, a, null, ghidra.program.model.symbol.SourceType.USER_DEFINED).applyTo(currentProgram, monitor); }
            if (getFunctionAt(a) != null) f = getFunctionAt(a);
          }
        }
        String text;
        if (f == null) text = "no function at " + l;
        else {
          new DisassembleCommand(f.getEntryPoint(), null, true).applyTo(currentProgram, monitor);
          for (int pass = 0; pass < 50; pass++) {
            boolean grew = false;
            CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
            List<Instruction> insl = new ArrayList<>();
            for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) insl.add(ins);
            for (Instruction ins : insl) {
              if (!ins.getFlowType().isCall()) continue;
              Address next = ins.getMaxAddress().add(1);
              if (ins.getFallThrough() == null) { ins.setFlowOverride(ghidra.program.model.listing.FlowOverride.NONE); }
              if (currentProgram.getListing().getInstructionAt(next) == null) {
                new DisassembleCommand(next, null, true).applyTo(currentProgram, monitor);
                grew = true;
              }
            }
            if (!grew) break;
          }
          CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
          DecompileResults r = di.decompileFunction(f, 120, monitor);
          text = r.decompileCompleted() ? r.getDecompiledFunction().getC() : ("failed: " + r.getErrorMessage());
        }
        Files.write(Paths.get(outDir, l.replace("0x","").toUpperCase() + ".c"), text.getBytes());
      }
      if (!any) { Thread.sleep(500); idle += 500; } else idle = 0;
    }
    di.dispose();
  }
}
