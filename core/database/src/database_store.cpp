// SPDX-License-Identifier: Apache-2.0
//
// The out-of-line destructor and constructor for the DatabaseStore port.
//
// They exist for the same reason BlockStore's do: an abstract class whose
// virtual destructor is defined inline emits the vtable into every
// translation unit that includes the header. One definition here gives the
// class a single key function and therefore one vtable, which is what keeps
// the archive small and the symbol unambiguous to `nm -u`.

#include "uapp/database/database_store.h"

namespace uapp::database {

DatabaseStore::DatabaseStore() noexcept = default;
DatabaseStore::~DatabaseStore() = default;

}  // namespace uapp::database
