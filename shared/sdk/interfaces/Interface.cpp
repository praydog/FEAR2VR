#include "Interface.hpp"

namespace sdk::interfaces {

// Both helpers call initialize() every time on purpose. It is a cheap no-op
// once latched, and crucially it RETRIES while unlatched: an interface looked
// up before Modules::initialize() has run (or before the pattern is
// resolvable) must be able to succeed on a later call rather than being stuck
// returning nullptr for the process lifetime.

void* resolve_interface(const char* name) {
    auto& reg = Registry::get();
    reg.initialize();
    return reg.resolve(name);
}

Registry::Agreement interface_agreement(const char* name) {
    auto& reg = Registry::get();
    reg.initialize();
    return reg.agreement(name);
}

} // namespace sdk::interfaces
