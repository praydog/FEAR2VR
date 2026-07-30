import ida_name, idc

ROWS = [(2057524, 'CLTInput', 'ILTInput.Default'), (2058200, 'CLTInput', 'ILTInput.Default'), (2068168, 'CLTRenderer', 'ILTRenderer.Default'), (2069248, 'ILTClientContentTransfer', 'ILTClientContentTransfer.Default'), (2069252, 'CLTGameUtil', '-'), (2072100, 'CLTGameUtil', '-'), (2072104, 'CLTInput', 'ILTInput.Default'), (2081120, 'CWin32CustomRender', 'ILTCustomRender.Default'), (2081124, 'CLTRenderer', 'ILTRenderer.Default'), (2081128, 'CLTTextureString', 'ILTTextureString.Default'), (2081132, 'CD3DDrawPrim', 'ILTDrawPrim.Default'), (2081136, 'CLTClient', 'ILTClient.Default'), (2081140, 'CLTTextureMgr', 'ILTTextureMgr.Default'), (2081144, 'CLTModelClient', 'ILTModel.Client'), (2087200, 'CD3DDrawPrim', 'ILTDrawPrim.Default'), (2087208, 'CLTInput', 'ILTInput.Default'), (2093832, 'CLTInput', 'ILTInput.Default'), (2095028, 'CLTInput', 'ILTInput.Default'), (2096360, 'CLTInput', 'ILTInput.Default'), (2096936, 'CLTInput', 'ILTInput.Default'), (2102996, 'CLTInput', 'ILTInput.Default'), (2103080, 'CLTRenderer', 'ILTRenderer.Default'), (2103084, 'CWin32CustomRender', 'ILTCustomRender.Default'), (2114016, 'CLTModelClient', 'ILTModel.Client'), (2114020, 'CLTPhysicsClient', 'ILTPhysics.Client'), (2114024, 'CLTClient', 'ILTClient.Default'), (2114028, 'CLTCommonClient', 'ILTCommon.Client'), (2114292, 'CLTClient', 'ILTClient.Default'), (2114296, 'CLTServer', 'ILTServer.Default'), (2131888, 'CLTClient', 'ILTClient.Default')]

def run():
    print("module check: %s" % idc.get_name(0x10004670))
    IDB = 0x10000000
    named, skipped = 0, 0
    for off, cls, iface in ROWS:
        ea = IDB + off
        short = iface.split(".")[0] if iface != "-" else cls
        cur = idc.get_name(ea) or ""
        if cur.startswith("dword_") or cur.startswith("off_") or cur == "":
            if ida_name.set_name(ea, "g_p" + short, ida_name.SN_NOWARN | ida_name.SN_FORCE):
                named += 1
        else:
            skipped += 1
        note = ("gameclient's resolved pointer to %s (implementation %s).\n" % (iface, cls)) if iface != "-" else \
               ("gameclient's pointer to a %s object. THE REGISTRY PUBLISHES NO MATCHING INTERFACE NAME on this\n"
                "build, so this slot is unaccounted -- a real state, not a broken mapping.\n" % cls)
        idc.set_cmt(ea, note +
                    "Found by scanning .data for pointers to catalogued vtables, excluding console-variable\n"
                    "cache-pair owner words. Read via sdk::interfaces::Registry::gameclient_interface_slots().", 1)
    print("interface slots named %d, already-named skipped %d (of %d)" % (named, skipped, len(ROWS)))
    for ea in (0x102041E4, 0x101FC170, 0x101FC168, 0x101F9304):
        print("  0x%08X -> %s" % (ea, idc.get_name(ea)))
run()
