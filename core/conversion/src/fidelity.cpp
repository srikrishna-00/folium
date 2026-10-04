// SPDX-License-Identifier: Apache-2.0
#include "uapp/conversion/fidelity.h"

namespace uapp::conversion {

const char* fidelity_name(Fidelity severity) noexcept {
    switch (severity) {
        case Fidelity::Dropped:       return "dropped";
        case Fidelity::Approximated:  return "approximated";
        case Fidelity::Degraded:      return "degraded";
        case Fidelity::PreservedNote: return "preserved_note";
    }
    // Reached only if a caller casts a bogus integer into the enum. Same
    // discipline as uapp::status_code(): do not trust a closed enum.
    return "unknown";
}

void FidelityReport::note(Fidelity severity, std::string_view construct, std::size_t line,
                          std::string_view message) {
    for (FidelityEntry& e : entries_) {
        if (e.severity == severity && e.construct == construct) {
            ++e.count;
            return;
        }
    }
    FidelityEntry entry;
    entry.severity   = severity;
    entry.construct.assign(construct);
    entry.count      = 1;
    entry.first_line = line;
    entry.message.assign(message);
    entries_.push_back(std::move(entry));
}

std::size_t FidelityReport::count_of(std::string_view construct) const noexcept {
    std::size_t total = 0;
    for (const FidelityEntry& e : entries_) {
        if (e.construct == construct) { total += e.count; }
    }
    return total;
}

std::size_t FidelityReport::count_at(Fidelity severity) const noexcept {
    std::size_t total = 0;
    for (const FidelityEntry& e : entries_) {
        if (e.severity == severity) { total += e.count; }
    }
    return total;
}

}  // namespace uapp::conversion
