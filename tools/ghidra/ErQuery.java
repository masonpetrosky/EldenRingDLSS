// Read-only queries against the analyzed eldenring.exe project, for headless runs:
//   analyzeHeadless <projdir> EldenRing -process eldenring.exe -noanalysis -readOnly -scriptPath tools/ghidra
//     -postScript ErQuery.java <output file> <command> <args...>
// Commands (addresses are static virtual addresses in hex):
//   decompile <va> [...]   C code of the functions containing the addresses
//   xrefs <va> [...]       references to the addresses, with the instruction and its function
//   calls <va>             calls to the function at <va>, with their functions
//   writes <hexdisp> [max] instructions storing to [register + disp], with their functions
// SPDX-License-Identifier: MIT
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import java.io.FileWriter;
import java.io.PrintWriter;

public class ErQuery extends GhidraScript {
  private PrintWriter out;

  private Address va(String hex) {
    return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(Long.parseUnsignedLong(hex, 16));
  }

  private String where(Address a) {
    Function f = getFunctionContaining(a);
    return f == null ? "(no function)" : f.getName() + "@" + f.getEntryPoint();
  }

  @Override
  protected void run() throws Exception {
    String[] args = getScriptArgs();
    out = new PrintWriter(new FileWriter(args[0]));
    try {
      String command = args[1];
      if (command.equals("decompile")) {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        for (int i = 2; i < args.length; ++i) {
          Function f = getFunctionContaining(va(args[i]));
          if (f == null) {
            out.println("== " + args[i] + ": no function");
            continue;
          }
          DecompileResults r = decomp.decompileFunction(f, 300, monitor);
          out.println("== " + args[i] + " in " + f.getName() + " @ " + f.getEntryPoint());
          out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "decompile failed: " +
                      r.getErrorMessage());
        }
      } else if (command.equals("decompileat")) {
        // decompileat <entry va> [...]: like decompile, creating the function at the entry first if Ghidra has none
        // there (in memory only; the project is opened read-only)
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        for (int i = 2; i < args.length; ++i) {
          Address entry = va(args[i]);
          Function f = getFunctionAt(entry);
          if (f == null) {
            disassemble(entry);
            f = createFunction(entry, null);
          }
          if (f == null) {
            out.println("== " + args[i] + ": could not create a function");
            continue;
          }
          DecompileResults r = decomp.decompileFunction(f, 300, monitor);
          out.println("== " + args[i] + " " + f.getName() + " body " + f.getBody().getMinAddress() + ".." +
                      f.getBody().getMaxAddress());
          out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "decompile failed: " +
                      r.getErrorMessage());
        }
      } else if (command.equals("xrefs")) {
        for (int i = 2; i < args.length; ++i) {
          out.println("== references to " + args[i]);
          for (Reference ref : getReferencesTo(va(args[i]))) {
            Instruction ins = getInstructionAt(ref.getFromAddress());
            out.println("  " + ref.getFromAddress() + " " + ref.getReferenceType() + " " +
                        (ins == null ? "" : ins.toString()) + "   in " + where(ref.getFromAddress()));
          }
        }
      } else if (command.equals("calls")) {
        Function target = getFunctionAt(va(args[2]));
        out.println("== calls to " + (target == null ? args[2] : target.getName()));
        for (Reference ref : getReferencesTo(va(args[2]))) {
          if (ref.getReferenceType().isCall())
            out.println("  " + ref.getFromAddress() + " in " + where(ref.getFromAddress()));
        }
      } else if (command.equals("writes")) {
        long disp = Long.parseLong(args[2], 16);
        int max = args.length > 3 ? Integer.parseInt(args[3]) : 500;
        int found = 0;
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext() && found < max && !monitor.isCancelled()) {
          Instruction ins = it.next();
          if (ins.getNumOperands() < 1) continue;
          String op0 = ins.getDefaultOperandRepresentation(0);
          if (!op0.contains("[") || !op0.toLowerCase().contains("+ 0x" + Long.toHexString(disp) + "]")) continue;
          String mnemonic = ins.getMnemonicString().toLowerCase();
          if (mnemonic.startsWith("cmp") || mnemonic.startsWith("test") || mnemonic.startsWith("lea")) continue;
          out.println("  " + ins.getAddress() + " " + ins + "   in " + where(ins.getAddress()));
          ++found;
        }
      } else if (command.equals("symbols")) {
        // symbols <substring>: symbols whose full name contains the text
        String needle = args[2];
        ghidra.program.model.symbol.SymbolIterator syms = currentProgram.getSymbolTable().getAllSymbols(true);
        int n = 0;
        while (syms.hasNext() && n < 200) {
          ghidra.program.model.symbol.Symbol s = syms.next();
          String full = s.getName(true);
          if (full.contains(needle)) {
            out.println("  " + s.getAddress() + " " + full);
            ++n;
          }
        }
      } else if (command.equals("near")) {
        // near <va> [...]: the nearest symbol at or up to 0x400 bytes before each address
        for (int i = 2; i < args.length; ++i) {
          Address a = va(args[i]);
          String found = "none";
          for (int back = 0; back <= 0x400; back += 8) {
            ghidra.program.model.symbol.Symbol[] syms = currentProgram.getSymbolTable().getSymbols(a.subtract(back));
            if (syms.length > 0) {
              found = syms[0].getName(true) + " at " + a.subtract(back) + " (+" + Integer.toHexString(back) + ")";
              break;
            }
          }
          out.println("  " + args[i] + ": " + found);
        }
      } else if (command.equals("ptrs")) {
        // ptrs <va> <count>: 8-byte pointers from va, with the function at each
        Address a = va(args[2]);
        int count = Integer.parseInt(args[3]);
        for (int i = 0; i < count; ++i) {
          long p = getLong(a.add(i * 8L));
          Address target = va(Long.toHexString(p));
          Function f = getFunctionAt(target);
          out.println("  [" + i + "] " + Long.toHexString(p) + " " + (f == null ? where(target) : f.getName()));
        }
      } else if (command.equals("bytepair")) {
        // bytepair <hexA> <hexB> [window]: functions reading byte [reg + A] and byte [reg + B] within `window`
        // instructions, with the instructions in between that mention a float constant or 100.
        long a = Long.parseLong(args[2], 16), b = Long.parseLong(args[3], 16);
        int window = args.length > 4 ? Integer.parseInt(args[4]) : 40;
        String na = "+ 0x" + Long.toHexString(a) + "]", nb = "+ 0x" + Long.toHexString(b) + "]";
        java.util.ArrayDeque<Instruction> recent = new java.util.ArrayDeque<>();
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        int found = 0;
        while (it.hasNext() && found < 300 && !monitor.isCancelled()) {
          Instruction ins = it.next();
          recent.addLast(ins);
          if (recent.size() > window) recent.removeFirst();
          String s = ins.toString().toLowerCase();
          if (!(s.contains("byte ptr") && s.contains(nb))) continue;
          Instruction match = null;
          for (Instruction r : recent) {
            String t = r.toString().toLowerCase();
            if (t.contains("byte ptr") && t.contains(na)) match = r;
          }
          if (match == null) continue;
          out.println("== " + match.getAddress() + " .. " + ins.getAddress() + " in " + where(ins.getAddress()));
          Instruction cur = match;
          for (int k = 0; k < window + 12 && cur != null; ++k, cur = cur.getNext())
            out.println("    " + cur.getAddress() + " " + cur);
          ++found;
          recent.clear();
        }
      } else {
        out.println("unknown command " + command);
      }
    } finally {
      out.close();
    }
  }
}
