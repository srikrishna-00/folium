// SPDX-License-Identifier: Apache-2.0
//
// storage/sqlite/fts_query.h — user text -> safe FTS5 MATCH expression.
//
// FTS5's MATCH operand is a small *query language*, not a literal. Binding raw
// user input to it is not a SQL injection (the value is still a bound
// parameter) but it is an injection into that query language: a user typing
// `NEAR(` or an unbalanced quote gets SQLITE_ERROR, and a user typing `a OR b`
// silently gets different results than they asked for. Spec section 38 says
// validate external input; this is where search input is validated.
//
// Strategy: extract word characters, discard every FTS5 operator character,
// emit each surviving term as a double-quoted string, and AND them together.
// The final term also gets a `*` prefix match so that incremental "search as
// you type" works with the prefix='2 3' index configured on the table.
//
// Exposed as a public header of this target purely so it can be unit tested
// directly; it is not part of the core-facing API.

#ifndef UAPP_STORAGE_SQLITE_FTS_QUERY_H
#define UAPP_STORAGE_SQLITE_FTS_QUERY_H

#include <string>
#include <string_view>

#include "uapp/status.h"

namespace uapp::storage::sqlite {

/// Convert raw user input into an FTS5 MATCH expression.
///
/// Returns Status::SearchQueryEmpty when the input contains no usable term
/// (empty, whitespace only, or nothing but punctuation/operators) -- callers
/// must treat that as "ask the user for a better query", not as "no results".
/// Returns Status::InvalidArgument if the input exceeds the query size limit.
[[nodiscard]] Status build_fts5_match_expression(std::string_view user_query,
                                                 std::string& out);

}  // namespace uapp::storage::sqlite

#endif  // UAPP_STORAGE_SQLITE_FTS_QUERY_H
