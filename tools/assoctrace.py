"""lldb script: step through Apple80211Associate2 one instruction at a time
(stepping over calls) and print the path as offsets from its start, the calls
it makes, and where it returns. Used as:
  lldb -b -o 'command script import tools/assoctrace.py' -o 'assoctrace' -- build/out/assocprobe en4 SSID
Read-only as an ordinary user: without root the join request itself fails."""
import lldb


def assoctrace(debugger, command, result, internal_dict):
    target = debugger.GetSelectedTarget()
    bp = target.BreakpointCreateByName("Apple80211Associate2")
    bp.SetOneShot(True)
    process = target.LaunchSimple(None, None, None)
    if not process or process.GetState() != lldb.eStateStopped:
        print("did not stop in Apple80211Associate2 (no scan entry?)")
        return
    thread = process.GetSelectedThread()
    start = thread.GetFrameAtIndex(0).GetSymbol().GetStartAddress().GetLoadAddress(target)
    depth = thread.GetNumFrames()
    path, calls = [], []
    for _ in range(200000):
        frame = thread.GetFrameAtIndex(0)
        pc = frame.GetPC()
        if thread.GetNumFrames() < depth:
            break
        off = pc - start
        path.append(off)
        insn = target.ReadInstructions(frame.GetPCAddress(), 1)[0]
        if insn.GetMnemonic(target).startswith("call"):
            calls.append((off, insn.GetOperands(target)))
        thread.StepInstruction(True)
        if process.GetState() != lldb.eStateStopped:
            break
    rax = thread.GetFrameAtIndex(0).FindRegister("rax").GetValueAsSigned()
    print("instructions: %d, returned %d (eax %d)" % (len(path), rax, ctypes_i32(rax)))
    print("calls:")
    for off, ops in calls:
        print("  +%d %s" % (off, ops))
    print("last 60 offsets:", " ".join(str(o) for o in path[-60:]))
    process.Kill()


def ctypes_i32(v):
    v &= 0xffffffff
    return v - (1 << 32) if v & 0x80000000 else v


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("command script add -f assoctrace.assoctrace assoctrace")
