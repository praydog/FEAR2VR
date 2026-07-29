#include "Delegates.hpp"

#include "Memory.hpp"
#include "Modules.hpp"

namespace sdk {

namespace {

// Delegate_Detach in gameclient.dll, the one method all 329 delegate vtables share in slot 2.
constexpr uintptr_t kDetachOffset = 0xEE9F0;

}  // namespace

uintptr_t Delegates::detach_fn() {
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return 0;
    }
    return gc->base + kDetachOffset;
}

bool Delegates::is_delegate_vtable(uintptr_t vtable) {
    const auto detach = detach_fn();
    if (vtable == 0 || detach == 0) {
        return false;
    }
    const auto slot = mem::read_ptr(vtable + kDetachSlot * sizeof(void*));
    return slot.has_value() && *slot == detach;
}

std::optional<Delegates::Listener> Delegates::read_node(uintptr_t node) {
    if (node == 0) {
        return std::nullopt;
    }

    const auto vtable = mem::read_ptr(node + kNodeVtable);
    const auto owner = mem::read_ptr(node + kNodeOwner);
    const auto subject = mem::read_ptr(node + kNodeSubject);
    if (!vtable.has_value() || !owner.has_value() || !subject.has_value()) {
        return std::nullopt;
    }

    Listener l{};
    l.node = node;
    l.vtable = *vtable;
    l.owner = *owner;
    l.subject = *subject;
    l.vtable_valid = is_delegate_vtable(*vtable);
    return l;
}

std::optional<Delegates::Listener> Delegates::read_node_from_link(uintptr_t link) {
    if (link < kNodeLink) {
        return std::nullopt;
    }
    return read_node(link - kNodeLink);
}

std::vector<Delegates::Listener> Delegates::listeners(uintptr_t subject_head, size_t limit) {
    std::vector<Listener> out;
    if (subject_head == 0 || limit == 0) {
        return out;
    }

    // The head is a link pair; its `next` is the first node's LINK address, and the walk ends by returning
    // here. An empty list self-points, so this terminates immediately for one.
    auto link = mem::read_ptr(subject_head + sizeof(void*));
    if (!link.has_value()) {
        return out;
    }

    while (*link != 0 && *link != subject_head && out.size() < limit) {
        if (auto node = read_node_from_link(*link)) {
            out.push_back(*node);
        } else {
            break;  // an unreadable node means the chain is not a list; stop rather than guess past it
        }

        const auto next = mem::read_ptr(*link + sizeof(void*));
        if (!next.has_value()) {
            break;
        }
        link = next;
    }

    return out;
}

bool Delegates::is_listening(uintptr_t subject_head, uintptr_t owner) {
    if (owner == 0) {
        return false;
    }
    for (const auto& l : listeners(subject_head)) {
        if (l.owner == owner) {
            return true;
        }
    }
    return false;
}

}  // namespace sdk
