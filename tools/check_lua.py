"""Release check for WaCa mods' Lua: every file compiles, and no file writes or reads a global by accident.
usage: python check_lua.py <folder> [<folder> ...]      (e.g. python tools/check_lua.py Mods examples)
Needs: pip install lupa luaparser
- compile: each file through a real Lua's load() (lupa's Lua is 5.5, stricter than UE4SS's 5.4 - a for loop's variable
  is read-only there; assigning to one is reported, so the mods stay loadable by a newer UE4SS)
- globals: a name assigned / read that no enclosing scope declares local. The usual cause is a local function called
  above its own definition - in Lua that call is a nil global, an error at run time only (2026-11-04: three of them,
  a scene's fade never lifted, a cat stayed drawn, a role was never asked). UE4SS's and WaCa Core's native globals are
  known; a mod that means a global on purpose lists it with  --allow Name,Name
Exit code 1 when anything is found."""
import glob, os, sys
import lupa
from luaparser import ast, astnodes as A

KNOWN = set("""_G _VERSION assert collectgarbage dofile error getmetatable ipairs load loadfile next pairs pcall print
rawequal rawget rawlen rawset require select setmetatable tonumber tostring type xpcall coroutine debug io math os package
string table utf8 self arg ExecuteInGameThread ExecuteInGameThreadWithDelay ExecuteWithDelay ExecuteAsync LoopAsync
LoopInGameThread LoopInGameThreadWithDelay RegisterHook UnregisterHook RegisterKeyBind RegisterKeyBindAsync
IsKeyBindRegistered RegisterCustomEvent UnregisterCustomEvent RegisterConsoleCommandHandler
RegisterConsoleCommandGlobalHandler RegisterProcessConsoleExecPreHook RegisterProcessConsoleExecPostHook
RegisterCallFunctionByNameWithArgumentsPreHook RegisterBeginPlayPreHook RegisterBeginPlayPostHook
RegisterInitGameStatePreHook RegisterInitGameStatePostHook RegisterLoadMapPreHook RegisterLoadMapPostHook
NotifyOnNewObject FindFirstOf FindAllOf FindObject FindObjects StaticFindObject StaticConstructObject LoadAsset
CreateInvalidObject FName FText FString UEHelpers Key ModifierKey IterateGameDirectories CancelDelayedAction
RetriggerDelayedAction PauseDelayedAction UnpauseDelayedAction IsDelayedActionValid ExecuteInGameThreadAsync
GetKismetMathLibrary GetKismetSystemLibrary GetWorldContextObject ForEachUObject DumpAllObjects GenerateSDK
WaCaNative_OnEvent""".split())

def scopes(tree):
    """(names written, names read) that no local declares"""
    writes, reads = set(), set()

    def walk(node, s):
        if isinstance(node, list):
            for n in node: walk(n, s)
        elif isinstance(node, A.Block):
            inner = set(s)
            for st in node.body: stmt(st, inner)
        elif isinstance(node, A.AnonymousFunction):
            fn(node, s)
        elif isinstance(node, A.Name):
            if node.id not in s: reads.add(node.id)
        elif isinstance(node, A.Index):
            walk(node.value, s)
            if node.notation != A.IndexNotation.DOT: walk(node.idx, s)
        elif isinstance(node, A.Field):
            if getattr(node, "between_brackets", False) or not isinstance(node.key, A.Name): walk(node.key, s)
            walk(node.value, s)
        elif isinstance(node, A.Invoke):
            walk(node.source, s); walk(node.args, s)
        elif isinstance(node, (A.Goto, A.Label)):
            pass
        elif isinstance(node, A.Node):
            for k, v in node.__dict__.items():
                if not k.startswith("_") and isinstance(v, (A.Node, list)): walk(v, s)

    def fn(f, s, extra=()):
        walk(f.body, set(s) | set(extra) | {a.id for a in f.args if isinstance(a, A.Name)})

    def stmt(st, s):
        if isinstance(st, A.LocalAssign):
            walk(st.values, s)
            s.update(x.id for x in st.targets if isinstance(x, A.Name))
        elif isinstance(st, A.LocalFunction):
            s.add(st.name.id); fn(st, s)
        elif isinstance(st, A.Assign):
            walk(st.values, s)
            for x in st.targets:
                if isinstance(x, A.Name):
                    if x.id not in s: writes.add(x.id)
                else: walk(x, s)
        elif isinstance(st, A.Function):
            if isinstance(st.name, A.Name):
                if st.name.id not in s: writes.add(st.name.id)
            else: walk(st.name, s)
            fn(st, s)
        elif isinstance(st, A.Method):
            walk(st.source, s); fn(st, s, ["self"])
        elif isinstance(st, A.Fornum):
            walk([st.start, st.stop, st.step], s); walk(st.body, set(s) | {st.target.id})
        elif isinstance(st, A.Forin):
            walk(st.iter, s); walk(st.body, set(s) | {x.id for x in st.targets if isinstance(x, A.Name)})
        else:
            walk(st, s)

    walk(tree.body, set())
    return writes, reads

def main(argv):
    allow = set()
    if "--allow" in argv:
        i = argv.index("--allow"); allow = set(argv[i + 1].split(",")); argv = argv[:i] + argv[i + 2:]
    lua = lupa.LuaRuntime()
    compile_err = lua.eval("function(s, n) local f, e = load(s, n) return e end")
    files = [p for root in argv for p in sorted(glob.glob(os.path.join(root, "**", "*.lua"), recursive=True))]
    bad = 0
    for p in files:
        src = open(p, encoding="utf-8").read()
        e = compile_err(src, "=" + os.path.relpath(p))
        if e:
            bad += 1; print("compile:", e); continue
        try:
            writes, reads = scopes(ast.parse(src))
        except Exception as ex:
            print(f"parse: {p}: {type(ex).__name__} {str(ex)[:100]} (luaparser - check by hand)"); continue
        ok = KNOWN | allow
        w = sorted(n for n in writes if n not in ok)
        r = sorted(n for n in reads - writes if n not in ok and not n.startswith("WaCaNative_"))
        if w: bad += 1; print(f"globals: {p} writes {', '.join(w)}")
        if r: bad += 1; print(f"globals: {p} reads {', '.join(r)} - nothing declares it (a local defined below its use?)")
    print(f"{len(files)} Lua file(s), {bad} finding(s)")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
