# Reverse-engineering tools

Used to find the engine internals described in [docs/engine-notes.md](../../docs/engine-notes.md).
Run them against your own copy of `Starsiege.exe`; nothing from the game is included here.

- `typeinfo.py`: lists a class's Borland type descriptor and vtables
  (`typeinfo.py Starsiege.exe 'SimGui::HudMtrRadar'`), or finds a function from one game build in
  another by its first bytes (`--find-code`).
- `symbols.py`: writes `docs/starsiege-1.004.symbols.txt` and `docs/starsiege-1.003.symbols.txt`
  from its table. Import them in Ghidra with *Script Manager → ImportSymbolsScript.py*.
- `ghidra/`: Ghidra scripts, for headless use with `analyzeHeadless … -postScript`:
  `Decomp.java` / `DecompAt.java` decompile the functions at (or containing) the given addresses,
  `Callers.java` lists the callers of the function containing an address, and the `Hud*` scripts
  found the HUD classes, their vtables and their render methods.
