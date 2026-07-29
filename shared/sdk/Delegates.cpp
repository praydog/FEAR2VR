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

bool Delegates::is_delegate_list(uintptr_t head) {
    if (head == 0) {
        return false;
    }
    // Empty and unreadable are both rejected: see the header on why a scan wants only channels in use.
    if (mem::classify_link(head) != mem::LinkState::Linked) {
        return false;
    }
    const auto found = listeners(head);
    if (found.empty()) {
        return false;
    }
    for (const auto& l : found) {
        if (!l.vtable_valid) {
            return false;
        }
    }
    return true;
}

std::vector<Delegates::Channel> Delegates::find_channels(uintptr_t object, size_t span) {
    std::vector<Channel> out;
    if (object == 0 || span < 2 * sizeof(void*)) {
        return out;
    }

    for (uintptr_t off = 0; off + 2 * sizeof(void*) <= span; off += sizeof(void*) / 2) {
        const auto head = object + off;
        if (!is_delegate_list(head)) {
            continue;
        }
        Channel ch{};
        ch.offset = off;
        ch.head = head;
        ch.listeners = listeners(head).size();
        out.push_back(ch);
    }
    return out;
}

std::vector<uintptr_t> Delegates::channels_listened_to(uintptr_t object, uintptr_t owner, size_t span) {
    std::vector<uintptr_t> out;
    if (owner == 0) {
        return out;
    }
    for (const auto& ch : find_channels(object, span)) {
        if (is_listening(ch.head, owner)) {
            out.push_back(ch.offset);
        }
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
