// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the FTS5 MATCH-expression builder. This is the component
// that stands between raw user keystrokes and SQLite's query parser, so it
// gets tested directly rather than only through the store.

#include <string>

#include "uapp/notes/note.h"
#include "uapp/status.h"
#include "uapp/storage/sqlite/fts_query.h"

#include "test_framework.h"

using uapp::Status;
using uapp::storage::sqlite::build_fts5_match_expression;

UAPP_TEST("fts_query.single term gets quoted and prefix-matched") {
    std::string expr;
    UAPP_REQUIRE_OK(build_fts5_match_expression("roadmap", expr));
    UAPP_CHECK_MSG(expr == "\"roadmap\"*", "got: " + expr);
}

UAPP_TEST("fts_query.multiple terms are ANDed") {
    std::string expr;
    UAPP_REQUIRE_OK(build_fts5_match_expression("quarterly planning", expr));
    UAPP_CHECK_MSG(expr == "\"quarterly\" AND \"planning\"*", "got: " + expr);
}

UAPP_TEST("fts_query.strips FTS5 operator characters") {
    // Each of these would be syntax to FTS5 if passed through, and several
    // would make sqlite3_step return SQLITE_ERROR outright.
    const char* hostile[] = {
        "NEAR(",
        "\"unbalanced",
        "a OR b",
        "^anchor",
        "col:value",
        "a AND (b",
        "*",
        "a - b",
        "'; DROP TABLE note; --",
    };
    for (const char* q : hostile) {
        std::string expr;
        const Status s = build_fts5_match_expression(q, expr);
        if (s == Status::SearchQueryEmpty) { continue; }  // nothing usable: fine
        UAPP_CHECK_MSG(uapp::is_ok(s), std::string("unexpected status for: ") + q);
        UAPP_CHECK_MSG(expr.find('(')  == std::string::npos, "leaked '(' from: " + std::string(q));
        UAPP_CHECK_MSG(expr.find(':')  == std::string::npos, "leaked ':' from: " + std::string(q));
        UAPP_CHECK_MSG(expr.find('^')  == std::string::npos, "leaked '^' from: " + std::string(q));
        UAPP_CHECK_MSG(expr.find('-')  == std::string::npos, "leaked '-' from: " + std::string(q));
        UAPP_CHECK_MSG(expr.find(';')  == std::string::npos, "leaked ';' from: " + std::string(q));
        // Quotes only ever appear as the delimiters we emit, never from input.
        std::size_t quotes = 0;
        for (const char c : expr) { if (c == '"') { ++quotes; } }
        UAPP_CHECK_MSG(quotes % 2 == 0, "unbalanced quotes from: " + std::string(q));
    }
}

UAPP_TEST("fts_query.bare operator words are neutralised by quoting") {
    // "OR" survives as a *term* (it is alphabetic) but the quotes stop FTS5
    // reading it as the OR operator -- which is the point of quoting.
    std::string expr;
    UAPP_REQUIRE_OK(build_fts5_match_expression("cats OR dogs", expr));
    UAPP_CHECK_MSG(expr == "\"cats\" AND \"OR\" AND \"dogs\"*", "got: " + expr);
}

UAPP_TEST("fts_query.empty and punctuation-only queries report SEARCH_QUERY_EMPTY") {
    std::string expr;
    UAPP_REQUIRE_STATUS(build_fts5_match_expression("", expr), Status::SearchQueryEmpty);
    UAPP_REQUIRE_STATUS(build_fts5_match_expression("   \t\n ", expr),
                        Status::SearchQueryEmpty);
    UAPP_REQUIRE_STATUS(build_fts5_match_expression("!!! ??? ...", expr),
                        Status::SearchQueryEmpty);
    UAPP_REQUIRE_STATUS(build_fts5_match_expression("\"\"\"", expr),
                        Status::SearchQueryEmpty);
}

UAPP_TEST("fts_query.rejects an oversized query instead of truncating it") {
    std::string expr;
    const std::string too_long(uapp::notes::kMaxQueryBytes + 1, 'a');
    UAPP_REQUIRE_STATUS(build_fts5_match_expression(too_long, expr),
                        Status::InvalidArgument);

    const std::string at_limit(uapp::notes::kMaxQueryBytes, 'a');
    UAPP_REQUIRE_OK(build_fts5_match_expression(at_limit, expr));
}

UAPP_TEST("fts_query.preserves non-ASCII bytes") {
    // Dropping bytes >= 0x80 would make the index unusable for most of the
    // world. The tokenizer segments; this layer must not mangle.
    std::string expr;
    UAPP_REQUIRE_OK(build_fts5_match_expression("caf\xc3\xa9 r\xc3\xa9sum\xc3\xa9", expr));
    UAPP_CHECK_MSG(expr.find("caf\xc3\xa9") != std::string::npos, "got: " + expr);
    UAPP_CHECK_MSG(expr.find("r\xc3\xa9sum\xc3\xa9") != std::string::npos, "got: " + expr);
}

UAPP_TEST("fts_query.caps the number of terms") {
    std::string many;
    for (int i = 0; i < 200; ++i) { many += "a "; }
    std::string expr;
    UAPP_REQUIRE_OK(build_fts5_match_expression(many, expr));
    std::size_t ands = 0;
    for (std::size_t pos = expr.find(" AND "); pos != std::string::npos;
         pos = expr.find(" AND ", pos + 1)) {
        ++ands;
    }
    UAPP_CHECK_MSG(ands < 64, "term cap not applied; AND count = " + std::to_string(ands));
}
