// SPDX-License-Identifier: Apache-2.0
#include "uapp/storage/sqlite/fts_query.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "uapp/notes/note.h"

namespace uapp::storage::sqlite {
namespace {

/// Is this byte part of a search term?
///
/// ASCII alphanumerics are, and so is every byte >= 0x80. Keeping the high
/// bytes matters: they are UTF-8 continuation and lead bytes, so dropping them
/// would make the index unsearchable in any non-Latin script. We do not try to
/// segment here -- that is the tokenizer's job (unicode61 today, ICU for CJK
/// in Phase 9 per database-design.md section 3.9). All we do is refuse to pass
/// through the ASCII punctuation that FTS5 treats as syntax.
constexpr bool is_term_byte(unsigned char c) noexcept {
    if (c >= 0x80u) { return true; }
    if (c >= '0' && c <= '9') { return true; }
    if (c >= 'a' && c <= 'z') { return true; }
    if (c >= 'A' && c <= 'Z') { return true; }
    return false;
}

/// Cap on the number of terms fed to FTS5 from one query. A pathological
/// 1 KiB query of single characters would otherwise become a 500-clause AND.
constexpr std::size_t kMaxTerms = 32;

}  // namespace

Status build_fts5_match_expression(std::string_view user_query, std::string& out) {
    if (user_query.size() > notes::kMaxQueryBytes) {
        return Status::InvalidArgument;
    }

    std::vector<std::string> terms;
    std::string current;

    for (const char ch : user_query) {
        const auto byte = static_cast<unsigned char>(ch);
        if (is_term_byte(byte)) {
            current.push_back(ch);
        } else if (!current.empty()) {
            terms.push_back(current);
            current.clear();
            if (terms.size() >= kMaxTerms) { break; }
        }
    }
    if (!current.empty() && terms.size() < kMaxTerms) {
        terms.push_back(current);
    }

    if (terms.empty()) {
        return Status::SearchQueryEmpty;
    }

    // Build: "term1" AND "term2" AND "term3"*
    //
    // Every term is wrapped in double quotes, which makes it an FTS5 *string*
    // rather than syntax. A term can never contain a double quote itself --
    // is_term_byte() rejects 0x22 -- so there is no escaping case to get
    // wrong. That is why the filter is an allow-list, not a deny-list.
    std::string expr;
    expr.reserve(user_query.size() + terms.size() * 8u);
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (i != 0) { expr += " AND "; }
        expr += '"';
        expr += terms[i];
        expr += '"';
        if (i + 1 == terms.size()) {
            expr += '*';  // prefix-match the last term (prefix='2 3' index)
        }
    }

    out = expr;
    return Status::Ok;
}

}  // namespace uapp::storage::sqlite
