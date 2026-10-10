// Pre-script for the headless import of eldenring.exe: switches off the slowest auto-analyzers, which this project
// does not need (functions come from the exception table; decompilation happens on demand later).
// SPDX-License-Identifier: MIT
import ghidra.app.script.GhidraScript;

public class ErAnalysisOptions extends GhidraScript {
  @Override
  protected void run() throws Exception {
    String[] off = {"Decompiler Parameter ID", "Decompiler Switch Analysis", "Embedded Media", "Condense Filler Bytes"};
    for (String name : off) {
      try {
        setAnalysisOption(currentProgram, name, "false");
        println("disabled analyzer: " + name);
      } catch (Exception e) {
        println("could not disable " + name + ": " + e.getMessage());
      }
    }
  }
}
