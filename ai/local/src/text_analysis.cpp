// SPDX-License-Identifier: Apache-2.0
#include "text_analysis.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace uapp::ai::local::detail {
namespace {

constexpr std::uint64_t kFnvPrime  = 1099511628211ULL;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;

[[nodiscard]] std::uint64_t fnv1a(std::string_view s, std::uint64_t seed) noexcept {
    std::uint64_t h = seed;
    for (const char c : s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= kFnvPrime;
    }
    return h;
}

[[nodiscard]] bool is_ws(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

[[nodiscard]] bool is_digit(unsigned char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] bool is_alpha(unsigned char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] unsigned char at(std::string_view s, std::size_t i) noexcept {
    return i < s.size() ? static_cast<unsigned char>(s[i]) : 0u;
}

// The highest-frequency English function words, SORTED, so lookup is a
// binary search. An entry added out of order silently disables everything
// after it, which is why `local.text.stopwords` asserts the sortedness
// directly over `stop_list_begin()/end()` instead of trusting review.
constexpr std::string_view kStopList[] = {
    "a",    "about", "all",  "also", "an",   "and",  "any",  "are",  "as",
    "at",   "be",    "been", "but",  "by",   "can",  "do",   "for",  "from",
    "had",  "has",   "have", "he",   "her",  "his",  "how",  "i",    "if",
    "in",   "into",  "is",   "it",   "its",  "may",  "not",  "of",   "on",
    "or",   "our",   "she",  "so",   "than", "that", "the",  "their", "them",
    "then", "there", "these", "they", "this", "to",  "was",  "we",   "were",
    "what", "when",  "which", "who", "will", "with", "would", "you", "your",
};

[[nodiscard]] bool days_in_month(int year, int month, int& out_days) noexcept {
    if (month < 1 || month > 12) { return false; }
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    out_days = kDays[month - 1];
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        if (leap) { out_days = 29; }
    }
    return true;
}

/// Priority for overlap arbitration. Lower wins.
[[nodiscard]] int field_priority(FieldKind k) noexcept {
    switch (k) {
        case FieldKind::Email:   return 0;
        case FieldKind::Url:     return 1;
        case FieldKind::IsoDate: return 2;
        case FieldKind::Money:   return 3;
        case FieldKind::Number:  return 4;
    }
    return 5;
}

}  // namespace

std::string concat_messages(const Content& content) {
    std::size_t total = 0;
    for (const Message& m : content.messages()) { total += m.text.size(); }
    std::string joined;
    joined.reserve(total);
    for (const Message& m : content.messages()) { joined += m.text; }
    return joined;
}

std::string lowercase_ascii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32u)
                                             : static_cast<char>(c));
    }
    return out;
}

bool is_word_byte(unsigned char c) noexcept {
    return is_alpha(c) || is_digit(c) || c == '\'' || c >= 0x80u;
}

void tokenize_words(std::string_view text, std::vector<Span>& out) {
    out.clear();
    std::size_t i = 0;
    while (i < text.size() && out.size() < kMaxTokens) {
        if (!is_word_byte(at(text, i))) { ++i; continue; }
        const std::size_t start = i;
        while (i < text.size() && is_word_byte(at(text, i))) { ++i; }
        out.push_back(Span{start, i - start});
    }
}

bool is_stopword(std::string_view lowered) noexcept {
    return std::binary_search(std::begin(kStopList), std::end(kStopList), lowered);
}

const std::string_view* stop_list_begin() noexcept { return std::begin(kStopList); }
const std::string_view* stop_list_end() noexcept { return std::end(kStopList); }

void split_sentences(std::string_view text, std::vector<Span>& out) {
    out.clear();
    std::size_t i     = 0;
    std::size_t start = 0;
    const std::size_t n = text.size();

    const auto push = [&out, text](std::size_t b, std::size_t e) {
        while (b < e && is_ws(static_cast<unsigned char>(text[b]))) { ++b; }
        while (e > b && is_ws(static_cast<unsigned char>(text[e - 1]))) { --e; }
        if (e > b) { out.push_back(Span{b, e - b}); }
    };

    while (i < n && out.size() < kMaxSentences) {
        const unsigned char c = at(text, i);

        if (c == '.' && is_digit(at(text, i + 1)) && i > 0 && is_digit(at(text, i - 1))) {
            // A decimal point, not a full stop. Cheap, common, worth special-casing.
            ++i;
            continue;
        }

        if (c == '.' || c == '!' || c == '?' || c == ';') {
            std::size_t end = i + 1;
            while (end < n) {
                const unsigned char t = at(text, end);
                if (t == '.' || t == '!' || t == '?' || t == ';' || t == '"' || t == '\'' ||
                    t == ')' || t == ']') {
                    ++end;
                } else {
                    break;
                }
            }
            if (end >= n || is_ws(at(text, end))) {
                push(start, end);
                i     = end;
                start = end;
                continue;
            }
            i = end;
            continue;
        }

        // Paragraph break: a newline followed by (whitespace then) another
        // newline. Structure is a sentence boundary even without punctuation,
        // which is what makes headings and list items separate candidates.
        if (c == '\n') {
            std::size_t j = i + 1;
            while (j < n && is_ws(at(text, j)) && at(text, j) != '\n') { ++j; }
            if (j < n && at(text, j) == '\n') {
                push(start, i);
                i     = j + 1;
                start = i;
                continue;
            }
        }
        ++i;
    }
    if (out.size() < kMaxSentences) { push(start, n); }
}

void score_sentences_tfidf(std::string_view text, const std::vector<Span>& sentences,
                           std::vector<double>& scores) {
    scores.assign(sentences.size(), 0.0);
    if (sentences.empty()) { return; }

    // Pass 1: per-sentence distinct term sets, plus global tf and df.
    std::map<std::string, std::size_t> term_freq;      // tf over the whole input
    std::map<std::string, std::size_t> doc_freq;       // df over sentences
    std::vector<std::vector<std::string>> per_sentence(sentences.size());

    std::vector<Span> tokens;
    for (std::size_t s = 0; s < sentences.size(); ++s) {
        const Span sp = sentences[s];
        const std::string_view sentence = text.substr(sp.offset, sp.length);
        tokenize_words(sentence, tokens);

        std::vector<std::string> distinct;
        for (const Span& t : tokens) {
            std::string term = lowercase_ascii(sentence.substr(t.offset, t.length));
            if (term.size() < 2 || is_stopword(term)) { continue; }
            ++term_freq[term];
            if (std::find(distinct.begin(), distinct.end(), term) == distinct.end()) {
                distinct.push_back(term);
            }
        }
        for (const std::string& term : distinct) { ++doc_freq[term]; }
        per_sentence[s] = std::move(distinct);
    }

    const double n = static_cast<double>(sentences.size());
    for (std::size_t s = 0; s < sentences.size(); ++s) {
        double sum = 0.0;
        for (const std::string& term : per_sentence[s]) {
            const double tf = static_cast<double>(term_freq[term]);
            const double df = static_cast<double>(doc_freq[term]);
            const double w  = (1.0 + std::log(tf)) * std::log((n + 1.0) / (df + 0.5));
            sum += w;
        }
        const double distinct = static_cast<double>(per_sentence[s].size());
        scores[s] = distinct > 0.0 ? sum / std::sqrt(distinct) : 0.0;
    }
}

void select_top_sentences(const std::vector<double>& scores, std::size_t k,
                          std::vector<std::size_t>& out_indices) {
    out_indices.clear();
    if (k == 0 || scores.empty()) { return; }
    if (k >= scores.size()) {
        for (std::size_t i = 0; i < scores.size(); ++i) { out_indices.push_back(i); }
        return;
    }

    std::vector<std::size_t> order(scores.size());
    for (std::size_t i = 0; i < order.size(); ++i) { order[i] = i; }
    // Stable sort with an explicit index tiebreak: deterministic even if the
    // sort were not stable, which matters because `deterministic = true` is a
    // claim in this provider's descriptor.
    std::stable_sort(order.begin(), order.end(),
                     [&scores](std::size_t a, std::size_t b) {
                         if (scores[a] != scores[b]) { return scores[a] > scores[b]; }
                         return a < b;
                     });
    order.resize(k);
    std::sort(order.begin(), order.end());
    out_indices = std::move(order);
}

// --- scanners --------------------------------------------------------------

void scan_emails(std::string_view text, std::vector<ExtractedField>& out) {
    const std::size_t n = text.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (text[i] != '@') { continue; }

        // Local part: walk left over RFC-5322 atext we are willing to accept.
        std::size_t start = i;
        while (start > 0) {
            const unsigned char c = at(text, start - 1);
            const bool ok = is_alpha(c) || is_digit(c) || c == '.' || c == '_' ||
                            c == '%' || c == '+' || c == '-';
            if (!ok) { break; }
            --start;
        }
        if (start == i) { continue; }
        if (at(text, start) == '.' || at(text, i - 1) == '.') { continue; }

        // Domain: letters, digits, '.', '-'; must end in a 2+ letter TLD.
        std::size_t end = i + 1;
        while (end < n) {
            const unsigned char c = at(text, end);
            if (is_alpha(c) || is_digit(c) || c == '.' || c == '-') { ++end; } else { break; }
        }
        while (end > i + 1 && (at(text, end - 1) == '.' || at(text, end - 1) == '-')) {
            --end;
        }
        const std::size_t domain_len = end - (i + 1);
        if (domain_len < 4) { continue; }  // "a.bc" is the shortest acceptable

        std::size_t last_dot = std::string_view::npos;
        for (std::size_t j = i + 1; j < end; ++j) {
            if (text[j] == '.') { last_dot = j; }
        }
        if (last_dot == std::string_view::npos || last_dot == i + 1) { continue; }
        std::size_t tld_alpha = 0;
        for (std::size_t j = last_dot + 1; j < end; ++j) {
            if (!is_alpha(at(text, j))) { tld_alpha = 0; break; }
            ++tld_alpha;
        }
        if (tld_alpha < 2) { continue; }

        ExtractedField f;
        f.kind       = FieldKind::Email;
        f.offset     = start;
        f.length     = end - start;
        f.value      = std::string(text.substr(start, f.length));
        f.normalized = lowercase_ascii(f.value);
        out.push_back(std::move(f));
        i = end;
    }
}

void scan_urls(std::string_view text, std::vector<ExtractedField>& out) {
    static constexpr std::string_view kSchemes[] = {"http://", "https://"};
    const std::string lowered = lowercase_ascii(text);
    for (const std::string_view scheme : kSchemes) {
        std::size_t pos = 0;
        while ((pos = lowered.find(scheme, pos)) != std::string::npos) {
            std::size_t end = pos + scheme.size();
            while (end < text.size()) {
                const unsigned char c = at(text, end);
                if (is_ws(c) || c == '<' || c == '>' || c == '"' || c == '\'' ||
                    c == '`' || c < 0x20u) {
                    break;
                }
                ++end;
            }
            // Trailing sentence punctuation is not part of the URL.
            while (end > pos + scheme.size()) {
                const unsigned char c = at(text, end - 1);
                if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' ||
                    c == '?' || c == ')' || c == ']' || c == '}') {
                    --end;
                } else {
                    break;
                }
            }
            if (end > pos + scheme.size()) {
                ExtractedField f;
                f.kind   = FieldKind::Url;
                f.offset = pos;
                f.length = end - pos;
                f.value  = std::string(text.substr(pos, f.length));
                // Verbatim. Canonicalising a URL means parsing authority,
                // percent-encoding and default ports; doing a third of that
                // produces a "normalised" form that is not canonical and not
                // the original either.
                f.normalized = f.value;
                out.push_back(std::move(f));
            }
            pos = end > pos ? end : pos + 1;
        }
    }
}

void scan_iso_dates(std::string_view text, std::vector<ExtractedField>& out) {
    const std::size_t n = text.size();
    if (n < 10) { return; }
    for (std::size_t i = 0; i + 10 <= n; ++i) {
        if (i > 0 && (is_digit(at(text, i - 1)) || is_word_byte(at(text, i - 1)))) {
            continue;
        }
        if (!is_digit(at(text, i)) || !is_digit(at(text, i + 1)) ||
            !is_digit(at(text, i + 2)) || !is_digit(at(text, i + 3))) {
            continue;
        }
        const unsigned char sep = at(text, i + 4);
        if (sep != '-' && sep != '/') { continue; }
        if (at(text, i + 7) != sep) { continue; }
        if (!is_digit(at(text, i + 5)) || !is_digit(at(text, i + 6)) ||
            !is_digit(at(text, i + 8)) || !is_digit(at(text, i + 9))) {
            continue;
        }
        if (i + 10 < n && is_digit(at(text, i + 10))) { continue; }

        const auto num2 = [text](std::size_t p) {
            return (static_cast<int>(at(text, p)) - '0') * 10 +
                   (static_cast<int>(at(text, p + 1)) - '0');
        };
        const int year  = num2(i) * 100 + num2(i + 2);
        const int month = num2(i + 5);
        const int day   = num2(i + 8);

        int max_day = 0;
        if (year < 1 || !days_in_month(year, month, max_day)) { continue; }
        if (day < 1 || day > max_day) { continue; }

        ExtractedField f;
        f.kind   = FieldKind::IsoDate;
        f.offset = i;
        f.length = 10;
        f.value  = std::string(text.substr(i, 10));
        // Canonical ISO-8601 with '-' separators, whatever the source used.
        f.normalized = f.value;
        f.normalized[4] = '-';
        f.normalized[7] = '-';
        out.push_back(std::move(f));
        i += 9;
    }
}

void scan_numbers(std::string_view text, bool want_number, bool want_money,
                  std::vector<ExtractedField>& out) {
    if (!want_number && !want_money) { return; }

    // Currency symbols we recognise, as the exact UTF-8 bytes. ASCII '$' plus
    // U+00A3 GBP, U+00A5 JPY, U+20AC EUR.
    static constexpr std::string_view kSymbols[] = {"$", "\xC2\xA3", "\xC2\xA5",
                                                    "\xE2\x82\xAC"};

    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        // Optional currency prefix.
        std::size_t      sym_len = 0;
        std::string_view symbol;
        for (const std::string_view s : kSymbols) {
            if (text.compare(i, s.size(), s) == 0) {
                sym_len = s.size();
                symbol  = s;
                break;
            }
        }
        std::size_t p = i + sym_len;

        // Optional sign.
        const std::size_t sign_at = p;
        if (at(text, p) == '+' || at(text, p) == '-') { ++p; }
        const bool negative = sign_at < n && at(text, sign_at) == '-';

        if (!is_digit(at(text, p))) {
            i += (sym_len > 0 ? sym_len : 1);
            continue;
        }
        // Not a number if it is glued to a word: "abc123" is an identifier.
        if (sym_len == 0 && i > 0 && is_word_byte(at(text, i - 1)) &&
            !is_digit(at(text, i - 1))) {
            ++i;
            continue;
        }

        std::string digits;
        while (p < n) {
            const unsigned char c = at(text, p);
            if (is_digit(c)) {
                digits.push_back(static_cast<char>(c));
                ++p;
            } else if (c == ',' && is_digit(at(text, p + 1)) && !digits.empty()) {
                ++p;  // thousands separator
            } else {
                break;
            }
        }
        std::string frac;
        if (at(text, p) == '.' && is_digit(at(text, p + 1))) {
            ++p;
            while (p < n && is_digit(at(text, p))) {
                frac.push_back(static_cast<char>(at(text, p)));
                ++p;
            }
        }
        // Trailing word bytes mean this was an identifier or a unit glued on.
        if (p < n && is_word_byte(at(text, p)) && !is_digit(at(text, p))) {
            i = p;
            continue;
        }

        const bool is_money = sym_len > 0;
        if ((is_money && want_money) || (!is_money && want_number)) {
            ExtractedField f;
            f.kind   = is_money ? FieldKind::Money : FieldKind::Number;
            f.offset = i;
            f.length = p - i;
            f.value  = std::string(text.substr(i, f.length));

            std::string canonical;
            if (is_money) { canonical += std::string(symbol); }
            if (negative) { canonical.push_back('-'); }
            canonical += digits.empty() ? "0" : digits;
            if (!frac.empty()) {
                canonical.push_back('.');
                canonical += frac;
            }
            f.normalized = std::move(canonical);
            out.push_back(std::move(f));
        }
        i = p > i ? p : i + 1;
    }
}

void resolve_overlaps(std::vector<ExtractedField>& fields) {
    std::stable_sort(fields.begin(), fields.end(),
                     [](const ExtractedField& a, const ExtractedField& b) {
                         if (a.offset != b.offset) { return a.offset < b.offset; }
                         if (field_priority(a.kind) != field_priority(b.kind)) {
                             return field_priority(a.kind) < field_priority(b.kind);
                         }
                         return a.length > b.length;
                     });

    // `fields` is now sorted by offset, and anything added to `kept` is
    // non-overlapping, so `kept` has ascending offsets AND ascending ends.
    // The furthest point claimed so far is therefore the LAST kept field's
    // end, and a candidate (whose offset is >= every kept offset) overlaps
    // something iff it starts before that point. One comparison, not a scan.
    //
    // That matters: the honest worst case here is a 1 MiB input of nothing but
    // numbers, which produces a quarter of a million candidates. A scan over
    // `kept` made this function the single slowest thing in the test suite
    // (~130 s for one case); this makes it linear.
    std::vector<ExtractedField> kept;
    kept.reserve(fields.size());
    std::size_t furthest_end = 0;
    for (ExtractedField& f : fields) {
        if (!kept.empty() && f.offset < furthest_end) { continue; }
        furthest_end = f.offset + f.length;
        kept.push_back(std::move(f));
    }
    fields = std::move(kept);
}

// --- embedding -------------------------------------------------------------

void hash_embed(std::string_view text, std::uint32_t dim, float* out) noexcept {
    if (dim == 0 || out == nullptr) { return; }
    for (std::uint32_t d = 0; d < dim; ++d) { out[d] = 0.0F; }

    const std::string lowered = lowercase_ascii(text);
    const std::string_view view{lowered};

    const auto add = [out, dim](std::string_view term, double weight) {
        const std::uint64_t h    = fnv1a(term, kFnvOffset);
        const std::uint32_t idx  = static_cast<std::uint32_t>(h % dim);
        // Signed hashing: the sign comes from a bit far from the index bits,
        // so collisions cancel on average instead of always accumulating.
        const double       sign  = ((h >> 40u) & 1u) != 0u ? 1.0 : -1.0;
        out[idx] = static_cast<float>(static_cast<double>(out[idx]) + sign * weight);
    };

    // Word unigrams, weight 1.
    std::vector<Span> tokens;
    tokenize_words(view, tokens);
    for (const Span& t : tokens) { add(view.substr(t.offset, t.length), 1.0); }

    // Character 3-grams over the whole lowered text, weight 0.5. They are what
    // makes a typo'd or inflected word still land near its original, which a
    // pure unigram fingerprint cannot do.
    if (view.size() >= 3) {
        for (std::size_t i = 0; i + 3 <= view.size(); ++i) {
            add(view.substr(i, 3), 0.5);
        }
    }

    double norm = 0.0;
    for (std::uint32_t d = 0; d < dim; ++d) {
        norm += static_cast<double>(out[d]) * static_cast<double>(out[d]);
    }
    if (norm > 0.0) {
        const double inv = 1.0 / std::sqrt(norm);
        for (std::uint32_t d = 0; d < dim; ++d) {
            out[d] = static_cast<float>(static_cast<double>(out[d]) * inv);
        }
    }
}

double dot(const float* a, const float* b, std::uint32_t dim) noexcept {
    if (a == nullptr || b == nullptr) { return 0.0; }
    double sum = 0.0;
    for (std::uint32_t d = 0; d < dim; ++d) {
        sum += static_cast<double>(a[d]) * static_cast<double>(b[d]);
    }
    return sum;
}

std::size_t count_whole_word(std::string_view text, std::string_view phrase) noexcept {
    if (phrase.empty() || phrase.size() > text.size()) { return 0; }
    std::size_t count = 0;
    std::size_t pos   = 0;
    while ((pos = text.find(phrase, pos)) != std::string_view::npos) {
        const bool left_ok  = pos == 0 || !is_word_byte(at(text, pos - 1));
        const std::size_t after = pos + phrase.size();
        const bool right_ok = after >= text.size() || !is_word_byte(at(text, after));
        if (left_ok && right_ok) { ++count; }
        pos += 1;
    }
    return count;
}

}  // namespace uapp::ai::local::detail
