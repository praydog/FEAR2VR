#pragma once

namespace sdk {

class IDatabaseMgr; // opaque until the interface is mapped

// The engine's attribute/database manager, owned by gamedatabase.dll.
// Evidence: gamedatabase.dll.i64 -- exported accessor
//   ?LTGetIDatabaseMgr@@YAPAVIDatabaseMgr@@XZ
// returns the address of a static CDatabaseMgr object (singleton with lazy
// vtable init inside the dll image).
class DatabaseMgr {
public:
    // The IDatabaseMgr singleton, nullptr if gamedatabase.dll or the export
    // is unavailable.
    static IDatabaseMgr* get();
};

} // namespace sdk
