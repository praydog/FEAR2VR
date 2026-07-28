-- DatabaseMgrDump.lua
--
-- Dumps the live IDatabaseMgr singleton (gamedatabase.dll) using the schema
-- mapped in reversing/fear2.genny (namespace regenny: DatabaseMgr,
-- DatabaseMgrEntry, DatabaseMgrSubRecord, HashEntry).
--
-- NO MAGIC STRUCT-FIELD OFFSETS: every field is read through the parsed .genny
-- type system (sdkgenny.Sdk / sdkgenny.StructOverlay / sdkgenny.PointerOverlay),
-- looked up BY NAME. Struct sizes (entry stride, sub-record size) come from
-- Struct:size(), never a literal byte count. See regenny-book's Scripting
-- Reference (sdkgenny.Sdk / StructOverlay / PointerOverlay / Struct / Variable)
-- for the API this relies on.
--
-- The ONE address this script hardcodes is the DatabaseMgr SINGLETON's
-- location (gamedatabase.dll + RVA 0x122BC) -- that is not a struct-field
-- offset, it's how the root instance is found at all (the .genny type system
-- describes LAYOUT, not WHERE an instance lives; this is the moral equivalent
-- of a global symbol address). Everything read past that point goes through
-- named lookups.
--
-- Reads process memory directly and does NOT depend on fear2.genny being the
-- currently-selected type in the UI, so it's safe to run at any time while
-- attached to FEAR2.exe.
--
-- Usage:
--   regenny_lua_exec_file with this path, or paste into regenny_lua_eval.
--   Requires an attached process with gamedatabase.dll loaded, and fear2.genny
--   as the open project (regenny:sdk() must return its parsed types).
--
-- Output:
--   - A short summary printed to the ReGenny Lua console.
--   - A full text report written to reversing/DatabaseMgrDump.txt next to
--     this script.
--
-- Observation from the first real dump (2026-07): the hash_entries preview
-- (bounded at 32 entries since the true length is unverified -- see
-- fear2.genny's HashEntry/DatabaseMgrSubRecord comments) shows a REPEATING
-- pattern after a handful of entries -- the true table is almost certainly
-- much shorter than 32 and the reader is picking up re-used/adjacent heap
-- memory past its end. This is expected and is exactly why the entry count
-- is not asserted here.

local DB_MGR_RVA = 0x122BC -- LTGetIDatabaseMgr singleton RVA (see fear2.genny header comment); NOT a struct-field offset
local OUT_PATH = "I:\\Programming\\projects\\fear2\\reversing\\DatabaseMgrDump.txt"
local HASH_ENTRY_PREVIEW_CAP = 32 -- safety bound on the unverified-length hash table, not a layout offset

-- ---------------------------------------------------------------------------
-- Setup: process, module, and the parsed .genny type system.
-- ---------------------------------------------------------------------------

local proc = regenny:process()
if not proc then
    print("[DatabaseMgrDump] no process attached")
    return
end

local sdk = regenny:sdk()
if not sdk then
    print("[DatabaseMgrDump] no project SDK loaded (open fear2.genny first)")
    return
end

local ns = sdk:global_ns():find_namespace("regenny")
if not ns then
    print("[DatabaseMgrDump] namespace 'regenny' not found in the loaded .genny -- is fear2.genny open?")
    return
end

local DatabaseMgr_t = ns:find_struct("DatabaseMgr")
local DatabaseMgrEntry_t = ns:find_struct("DatabaseMgrEntry")
local DatabaseMgrSubRecord_t = ns:find_struct("DatabaseMgrSubRecord")
local HashEntry_t = ns:find_struct("HashEntry")

for name, t in pairs({ DatabaseMgr = DatabaseMgr_t, DatabaseMgrEntry = DatabaseMgrEntry_t,
                        DatabaseMgrSubRecord = DatabaseMgrSubRecord_t, HashEntry = HashEntry_t }) do
    if not t then
        print("[DatabaseMgrDump] struct 'regenny." .. name .. "' not found -- fear2.genny schema changed?")
        return
    end
end

local function find_module(name_substr)
    for _, m in ipairs(proc:modules()) do
        if m.name and m.name:lower():find(name_substr:lower(), 1, true) then
            return m
        end
    end
    return nil
end

local gdb = find_module("gamedatabase.dll")
if not gdb then
    print("[DatabaseMgrDump] gamedatabase.dll not found in process module list")
    return
end

local db_mgr_addr = gdb.start + DB_MGR_RVA

-- ---------------------------------------------------------------------------
-- Named-lookup helpers. `field_slot`/`field_ptr_raw` resolve a field's
-- address via the TYPE SYSTEM's own recorded Variable:offset() -- never a
-- literal byte offset -- for the one case (the packed string pool) that needs
-- raw memory walking beyond what a single overlay field read can give.
-- ---------------------------------------------------------------------------

local function field_slot(overlay, struct_type, field_name)
    local var = struct_type:find_variable(field_name)
    assert(var, "field not found in schema: " .. struct_type:name() .. "." .. field_name)
    return overlay:address() + var:offset()
end

local function safe_u8(addr)
    if not addr then return nil end
    local ok, v = pcall(function() return proc:read_uint8(addr) end)
    if ok then return v else return nil end
end

local function field_ptr_raw(overlay, struct_type, field_name)
    local ok, v = pcall(function() return proc:read_uint32(field_slot(overlay, struct_type, field_name)) end)
    if ok then return v else return nil end
end

-- Sanitized single nul-terminated string starting at a raw address
-- (non-printable bytes escaped so the result is always safe to print).
local function read_cstr(addr, maxlen)
    if not addr or addr == 0 then return "" end
    local out = {}
    for i = 0, maxlen - 1 do
        local b = safe_u8(addr + i)
        if b == nil or b == 0 then break end
        if b >= 32 and b < 127 then
            table.insert(out, string.char(b))
        else
            table.insert(out, string.format("\\x%02X", b))
        end
    end
    return table.concat(out)
end

-- Walks a packed pool of back-to-back nul-terminated strings starting at
-- `addr`. Stops after `budget_bytes` or after 4 consecutive empty strings
-- (heuristic pool-end marker -- the true pool length is UNVERIFIED; see
-- fear2.genny's DatabaseMgrSubRecord comments on string_data/count_b).
local function read_string_pool(addr, budget_bytes, max_strings)
    local strings = {}
    if not addr or addr == 0 then return strings end
    local pos, zero_run = 0, 0
    while pos < budget_bytes and #strings < max_strings do
        local s, slen = {}, 0
        while pos < budget_bytes do
            local b = safe_u8(addr + pos)
            pos = pos + 1
            if b == nil then return strings end
            if b == 0 then break end
            slen = slen + 1
            if b >= 32 and b < 127 then
                table.insert(s, string.char(b))
            else
                table.insert(s, string.format("\\x%02X", b))
            end
        end
        if slen == 0 then
            zero_run = zero_run + 1
            if zero_run >= 4 then break end
        else
            zero_run = 0
            table.insert(strings, table.concat(s))
        end
    end
    return strings
end

-- ---------------------------------------------------------------------------
-- Dump one DatabaseMgrSubRecord overlay. Every field is read by NAME through
-- the overlay (or, for the string pool, via field_slot's named offset
-- lookup); no field's byte position is ever hardcoded here.
-- ---------------------------------------------------------------------------

local function dump_subrecord(record, out, label)
    out(string.format("  [%s] DatabaseMgrSubRecord @ 0x%08X", label, record:address()))

    local path_addr = field_ptr_raw(record, DatabaseMgrSubRecord_t, "path_data")
    out(string.format("    path_data    = 0x%08X  \"%s\"", path_addr or 0, read_cstr(path_addr, 260)))

    out(string.format("    count_a=%s  count_b=%s  count_c=%s (semantics UNVERIFIED, see fear2.genny)",
        tostring(record.count_a), tostring(record.count_b), tostring(record.count_c)))

    local self_ref_ptr = record.self_ref:ptr()
    local expected_self = record:address() + DatabaseMgrSubRecord_t:size()
    out(string.format("    self_ref     = 0x%08X  (expect this+sizeof(DatabaseMgrSubRecord)=0x%08X)  %s",
        self_ref_ptr, expected_self, self_ref_ptr == expected_self and "OK" or "MISMATCH"))

    local string_addr = field_ptr_raw(record, DatabaseMgrSubRecord_t, "string_data")
    local count_b = record.count_b
    local budget = (count_b and count_b > 0 and count_b < 0x10000) and count_b or 4096
    local names = read_string_pool(string_addr, budget, 4096)
    out(string.format("    string_data  = 0x%08X  (%d strings found, budget %d bytes)",
        string_addr or 0, #names, budget))
    for i, s in ipairs(names) do
        out(string.format("      [%d] \"%s\"", i - 1, s))
    end

    -- Hash table: bounded preview (true length unverified; see fear2.genny).
    -- Stride and struct field names come entirely from HashEntry_t/overlay.
    local hash_entries = record.hash_entries -- PointerOverlay<HashEntry>
    out(string.format("    hash_entries = 0x%08X (first up to %d entries, unverified true length)",
        hash_entries:ptr(), HASH_ENTRY_PREVIEW_CAP))
    for i = 0, HASH_ENTRY_PREVIEW_CAP - 1 do
        local e = hash_entries[i] -- StructOverlay<HashEntry> via PointerOverlay indexing (stride = HashEntry_t:size())
        if not e then break end
        out(string.format("      [%2d] hash=0x%08X  unk_04=%d unk_05=%d unk_06=%d unk_07=%d",
            i, e.hash, e.unk_04, e.unk_05, e.unk_06, e.unk_07))
    end
end

-- ---------------------------------------------------------------------------
-- Top level.
-- ---------------------------------------------------------------------------

local lines = {}
local function out(s) table.insert(lines, s) end

local db_mgr = sdkgenny.StructOverlay(db_mgr_addr, DatabaseMgr_t)

out(string.format("DatabaseMgr dump -- gamedatabase.dll @ 0x%08X, singleton @ 0x%08X (RVA 0x%X)",
    gdb.start, db_mgr:address(), DB_MGR_RVA))
out(string.format("Generated: %s", os.date()))
out("")

out(string.format("vtable        = 0x%08X", db_mgr.vtable:ptr()))
out(string.format("unk_04        = %s", tostring(db_mgr.unk_04)))
out(string.format("array_begin   = 0x%08X", db_mgr.array_begin:ptr()))
out(string.format("array_end     = 0x%08X", db_mgr.array_end:ptr()))
out(string.format("array_cap_end = 0x%08X", db_mgr.array_cap_end:ptr()))
out(string.format("unk_14        = %s", tostring(db_mgr.unk_14)))
out("")

-- Walk array_begin..array_end as a pointer range (C++-idiom: no division by
-- a hardcoded stride -- indexing already applies DatabaseMgrEntry_t:size()
-- internally via the PointerOverlay).
local entry_count = 0
local array_end_addr = db_mgr.array_end:ptr()
local i = 0
while true do
    local entry = db_mgr.array_begin[i]
    if not entry or entry:address() >= array_end_addr then break end

    out(string.format("=== DatabaseMgrEntry[%d] @ 0x%08X ===", i, entry:address()))
    out(string.format("  record_a = 0x%08X", entry.record_a:ptr()))
    out(string.format("  record_b = 0x%08X", entry.record_b:ptr()))
    if entry.record_a:ptr() ~= 0 then dump_subrecord(entry.record_a:d(), out, "record_a") end
    if entry.record_b:ptr() ~= 0 then dump_subrecord(entry.record_b:d(), out, "record_b") end
    out("")

    i = i + 1
    entry_count = entry_count + 1
end

local report = table.concat(lines, "\n")

print(string.format("[DatabaseMgrDump] gamedatabase.dll @ 0x%08X", gdb.start))
print(string.format("[DatabaseMgrDump] singleton @ 0x%08X, %d array entries", db_mgr:address(), entry_count))
print(string.format("[DatabaseMgrDump] report: %d lines, %d bytes", #lines, #report))

local f = io.open(OUT_PATH, "w")
if f then
    f:write(report)
    f:close()
    print("[DatabaseMgrDump] full report written to " .. OUT_PATH)
else
    print("[DatabaseMgrDump] FAILED to open " .. OUT_PATH .. " for writing")
end
