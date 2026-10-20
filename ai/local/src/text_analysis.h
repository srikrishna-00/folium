// SPDX-License-Identifier: Apache-2.0
//
// ai/local/src/text_analysis.h — the actual text processing behind
// LocalHeuristicProvider. NOT a public header.
//
// Everything here is deterministic, allocation-bounded, ASCII-aware and
// UTF-8-safe-by-avoidance: no byte above 0x7F is ever interpreted, only
// carried, so a multi-byte sequence is never split by a classifier that does
// not understand it. Where that costs quality it is said so out loud rather
// than hidden (see `split_sentences` and `tokenize_words`).
//
// It is separated from local_heuristic_provider.cpp so the pieces with real
// invariants -- the sentence splitter, the TF-IDF ranker, the field scanners,
// the hash embedder -- get direct tests, rather than coverage inferred from
// the provider API on top of them. Same argument
// core/conversion/src/markdown_syntax.h makes.

#ifndef UAPP_AI_LOCAL_TEXT_ANALYSIS_H
#define UAPP_AI_LOCAL_TEXT_ANALYSIS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "uapp/ai/provider.h"

namespace uapp::ai::local::detail {

/// A half-open byte range in the text being analysed.
struct Span final {
    std::size_t offset = 0;
    std::size_t length = 0;
};

/// Join a Content's messages with NO separator.
///
/// The absence of a separator is load-bearing, not laziness: every span this
/// module reports is a byte offset into this concatenation, and
/// `AIProvider::summarize/extract` validate those offsets against
/// `Content::byte_size()`, which is the sum of the message sizes with no
/// separators. Inserting a "\n" here would shift every offset by one per
/// message and the base class would correctly reject the results.
///
/// The cost: the last sentence of message N and the first of message N+1 run
/// together when a message does not end in punctuation. For the one caller
/// that exists (a single document or selection, one message) this never
/// happens; for a chat history it would, and a chat history is not something
/// this provider can summarise anyway.
[[nodiscard]] std::string concat_messages(const Content& content);

/// Lowercase A-Z only. Bytes >= 0x80 pass through untouched, so UTF-8 is
/// preserved byte-for-byte; the consequence is that "Ä" and "ä" are different
/// terms to the ranker and the classifier. Correct Unicode case folding needs
/// a table this module will not carry, and getting it half-right (e.g. only
/// Latin-1) is worse than being predictably ASCII-only.
[[nodiscard]] std::string lowercase_ascii(std::string_view text);

/// True for a byte that can be inside a word: ASCII letters, digits, `'`,
/// and any byte >= 0x80 (so a UTF-8 sequence is one word, not N).
[[nodiscard]] bool is_word_byte(unsigned char c) noexcept;

/// Maximal runs of word bytes. Runs longer than 64 bytes are still emitted
/// whole; the cap is on the number of tokens, not their length.
void tokenize_words(std::string_view text, std::vector<Span>& out);

/// A small, fixed English stop list (the ~40 highest-frequency function
/// words). `text` must already be lowercased.
///
/// WHY ONLY ENGLISH, AND WHY SMALL: an IDF-weighted ranker mostly handles
/// stopwords by itself -- a word in every sentence gets an IDF near zero --
/// so this list is a cheap head start, not the mechanism. A big
/// multi-language list would be a data dependency with a licence question
/// (spec §35) for a marginal gain, and its absence degrades quality rather
/// than correctness. Non-English text ranks slightly worse. Stated, not hidden.
[[nodiscard]] bool is_stopword(std::string_view lowered) noexcept;

/// The stop list itself, so a test can assert it is really sorted. `is_stopword`
/// binary-searches it, and an out-of-order entry would silently disable every
/// entry after it without failing anything else.
[[nodiscard]] const std::string_view* stop_list_begin() noexcept;
[[nodiscard]] const std::string_view* stop_list_end() noexcept;

/// Sentence boundaries.
///
/// A sentence ends at `.`, `!`, `?` or `;` followed by whitespace or
/// end-of-text, or at a blank line (paragraph break). Consecutive terminators
/// ("?!") collapse. Leading and trailing whitespace is excluded from each
/// span, so spans are the sentence text itself.
///
/// KNOWN WRONG CASES, none of which is a correctness bug for an extractive
/// summariser (a mis-split sentence is a slightly worse selection, never a
/// fabricated one):
///   * abbreviations -- "Dr. Smith" and "e.g. this" split early.
///   * decimal numbers are protected (a `.` between two digits is not a
///     boundary) because that case is both common and cheap to fix.
///   * CJK text has no spaces after `。`, which is not an ASCII terminator;
///     such text comes back as one long sentence and the summariser returns
///     it whole.
void split_sentences(std::string_view text, std::vector<Span>& out);

/// Upper bound on sentences considered, so a 1 MiB input of nothing but "."
/// cannot make the O(sentences x terms) ranker quadratic in a pathological
/// way. WHY 4096: at the 1 MiB input cap that is one sentence per 256 bytes,
/// which is longer than real prose; past it, selection quality is already
/// irrelevant because the input is not prose.
inline constexpr std::size_t kMaxSentences = 4096;

/// Upper bound on word tokens considered, for the same reason.
inline constexpr std::size_t kMaxTokens = 262144;

/// Score every sentence by smoothed TF-IDF over its own sentences.
///
/// This is a REAL ranking metric, not truncation:
///
///     weight(t)  = (1 + ln(tf(t))) * ln((N + 1) / (df(t) + 0.5))
///     score(S)   = ( sum over distinct non-stopword t in S of weight(t) )
///                  / sqrt(|distinct terms in S|)
///
/// `tf` is the term's count in the whole input and `df` the number of
/// sentences containing it, so a term that is frequent overall but spread
/// across every sentence (a topic word) scores lower than one that is
/// frequent and concentrated. The `1/sqrt(distinct)` divisor is there because
/// an unnormalised sum makes the longest sentence win every time, which is
/// the failure mode that makes naive extractive summarisers useless; the
/// square root rather than a full division keeps a mild, intended preference
/// for informative-and-longer over terse-and-empty.
///
/// `scores[i]` corresponds to `sentences[i]`. Returns the number scored.
void score_sentences_tfidf(std::string_view text, const std::vector<Span>& sentences,
                           std::vector<double>& scores);

/// Indices of the `k` highest-scoring sentences, returned in DOCUMENT ORDER.
///
/// Document order, not score order: a summary whose sentences are shuffled by
/// score reads as noise even when the selection is good. Ties break towards
/// the earlier sentence, which makes the whole function deterministic.
void select_top_sentences(const std::vector<double>& scores, std::size_t k,
                          std::vector<std::size_t>& out_indices);

// --- field scanners --------------------------------------------------------
// Hand-written, not std::regex. Two reasons: `std::regex`'s constructor
// throws on a malformed pattern and this tree is compiled -fno-exceptions
// (ADR-0001), so a regex is a std::terminate waiting for a typo; and
// std::regex is famously slow enough that a 1 MiB scan would blow the
// latency budget by itself.

/// Append every match of the named kind in `text`. Each scanner is
/// independent and may produce overlapping matches; `resolve_overlaps`
/// arbitrates.
void scan_emails(std::string_view text, std::vector<ExtractedField>& out);
void scan_urls(std::string_view text, std::vector<ExtractedField>& out);

/// `YYYY-MM-DD` and `YYYY/MM/DD` only, calendar-validated (including leap
/// years).
///
/// **`03/04/2026` IS DELIBERATELY NOT EXTRACTED.** It is 3 April in most of
/// the world and 4 March in the US, and nothing in the text says which. A
/// field extractor that guesses produces a confident wrong date, which is
/// strictly worse than producing nothing -- the same argument
/// `database::property.h` makes about a string that parses as a number.
/// Locale-aware date parsing belongs with a locale, which this layer does not
/// have. Recorded as NOT IMPLEMENTED rather than approximated.
void scan_iso_dates(std::string_view text, std::vector<ExtractedField>& out);

/// Signed integers and decimals, with optional `,` thousands separators.
/// A leading currency symbol ($ EUR GBP JPY, as UTF-8) makes it a `Money`
/// field instead.
///
/// The currency's `normalized` form keeps the SYMBOL rather than mapping it
/// to a code: `$` is USD, CAD, AUD, MXN and several more, and emitting "USD"
/// would be inventing information. Normalisation is limited to what is
/// actually known -- separators removed, sign preserved.
void scan_numbers(std::string_view text, bool want_number, bool want_money,
                  std::vector<ExtractedField>& out);

/// Sort by offset and drop any field overlapping an already-accepted one of
/// equal-or-higher priority (Email > Url > IsoDate > Money > Number).
///
/// Without this, `bob@example.com` yields an email AND the number `1` from a
/// `.com`-adjacent digit, and `2026-01-05` yields a date and three numbers.
/// Reporting both is not "more data", it is three wrong fields and one right
/// one.
void resolve_overlaps(std::vector<ExtractedField>& fields);

// --- embedding -------------------------------------------------------------

/// Deterministic lexical fingerprint of `text` into `dim` floats.
///
/// ===========================================================================
/// THIS IS NOT A SEMANTIC EMBEDDING AND MUST NOT BE SOLD AS ONE
/// ===========================================================================
/// It is the hashing trick over word unigrams plus character 3-grams,
/// signed-hashed to cancel collision bias, then L2-normalised. What it
/// genuinely does: identical text gets identical vectors; near-duplicate and
/// typo'd text gets near vectors; shared vocabulary raises cosine similarity.
/// That is real and useful -- it is deduplication and fuzzy lexical matching.
///
/// What it CANNOT do: "car" and "automobile" are orthogonal. A paraphrase
/// scores near zero. Negation is invisible. Semantic search built on this
/// would be lexical search with worse precision than the FTS5 index the
/// product already has (spec §16), which is exactly why
/// ai-architecture.md §6 specifies a real 384-dim model for the shipping
/// path. This exists so that the `embed()` capability has a working,
/// honestly-described implementation to test the interface against, and as
/// the place a real model drops in.
///
/// `out` must have room for `dim` floats. The result is L2-normalised, so
/// cosine similarity is a dot product.
void hash_embed(std::string_view text, std::uint32_t dim, float* out) noexcept;

/// Dot product of two `dim`-length vectors. For L2-normalised inputs this is
/// the cosine similarity.
[[nodiscard]] double dot(const float* a, const float* b, std::uint32_t dim) noexcept;

/// Count whole-word occurrences of `phrase` in `text`. Both must already be
/// lowercased. A match must not be flanked by word bytes, so "cat" does not
/// match inside "catalogue"; a multi-word phrase matches across the exact
/// bytes given, so "due date" does not match "due  date" (two spaces). The
/// strictness is intentional: a hint that silently matches more than it says
/// makes a classifier's behaviour unpredictable to whoever wrote the hints.
[[nodiscard]] std::size_t count_whole_word(std::string_view text,
                                           std::string_view phrase) noexcept;

}  // namespace uapp::ai::local::detail

#endif  // UAPP_AI_LOCAL_TEXT_ANALYSIS_H
