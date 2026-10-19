import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.symbol.Reference;
import java.io.*;
import java.util.*;

public class DumpAll extends GhidraScript {
  protected void run() throws Exception {
    String out = getScriptArgs()[0];
    DecompInterface di = new DecompInterface();
    di.openProgram(currentProgram);
    PrintWriter all = new PrintWriter(new FileWriter(out + "/all_decomp.c"));
    PrintWriter idx = new PrintWriter(new FileWriter(out + "/func_index.tsv"));
    int n = 0;
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      List<String> strs = new ArrayList<>();
      for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true))
        for (Reference r : ins.getReferencesFrom()) {
          Data d = getDataContaining(r.getToAddress());
          if (d != null && StringDataInstance.isString(d) && d.getValue() != null) {
            String s = d.getValue().toString().replaceAll("[^\\x20-\\x7e]", ".");
            if (s.length() > 90) s = s.substring(0, 90);
            if (!strs.contains(s)) strs.add(s);
          }
        }
      idx.println(f.getEntryPoint() + "\t" + f.getBody().getNumAddresses() + "\t" + String.join(" | ", strs));
      DecompileResults res = di.decompileFunction(f, 60, monitor);
      all.println("//==== FUNC " + f.getEntryPoint() + " " + f.getName() + " size=" + f.getBody().getNumAddresses());
      for (String s : strs) all.println("//  str: " + s);
      if (res.decompileCompleted()) all.println(res.getDecompiledFunction().getC());
      else all.println("// decompile failed");
      n++;
    }
    all.close(); idx.close();
    println("dumped " + n);
  }
}
