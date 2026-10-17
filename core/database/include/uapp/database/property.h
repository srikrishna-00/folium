// SPDX-License-Identifier: Apache-2.0
//
// core/database/property.h — the typed property system of spec section 15.
//
// Spec section 15 lists thirteen property types. SIX are implemented here
// (Text, Number, Checkbox, Select, Multi-select, Date); the other seven are
// declared, given a permanent numeric value, and reported as
// Status::DatabasePropertyTypeUnsupported with a machine-readable reason
// (`property_type_not_implemented_reason`). They are NOT fake-implemented.
//
// ---------------------------------------------------------------------------
// WHY A VARIANT AND NOT A STRINGLY-TYPED BAG
// ---------------------------------------------------------------------------
// The tempting shape for "a row is a set of property values" is
// `std::map<std::string, std::string>` with a per-type parse on read. It is
// rejected for the same three reasons core/blocks/block.h rejects a class
// hierarchy for BlockContent, plus one that is specific to this module:
//
//  1. ADR-0001 exports a pure C99 ABI. A closed tagged union maps to C as
//     `{ int32_t type; union { ... } payload; }` mechanically.
//  2. ADR-0001 bans RTTI, so there is no dynamic_cast to recover a type.
//  3. The set of property types is CLOSED and owned by us, which is exactly
//     when a variant beats open polymorphism: adding one should be a compile
//     error everywhere it matters.
//  4. **A STRING THAT PARSES AS A NUMBER IS NOT A NUMBER.** The entire point
//     of this module is that a Number property sorts numerically, a Date
//     property compares chronologically and a Select property references one
//     of *its own database's defined options* rather than naming it. With a
//     string bag, `"10" < "9"` is true, `"2026-1-5"` and `"2026-01-05"` are
//     different values for the same day, and renaming a select option
//     silently orphans every row that used it. Each of those is a silent
//     wrong answer, which is worse than an error.
//
// A Select value therefore holds an `OptionId`, not an option NAME. Renaming
// "In progress" to "Doing" is one UPDATE of one row of `db_select_option`,
// and every row that selected it keeps selecting it.
//
// ---------------------------------------------------------------------------
// EXTENSION POINT: adding a property type
// ---------------------------------------------------------------------------
//   1. Add a payload struct with real typed fields (never an opaque blob).
//   2. Append it to `PropertyValue` **in the same ordinal position as its
//      PropertyType value** — the static_asserts at the bottom enforce that
//      the variant index and the enum value agree, because the persisted
//      `db_property.type` column stores that integer.
//   3. Extend `validate_property_value` and `validate_value_for_property`.
//   4. Extend the legality table in view.h (`filter_operator_is_legal`) and
//      the sort-key extractor, or the type will be unfilterable/unsortable.
//   5. Extend the encode/decode pair and the query compiler in
//      storage/sqlite/src/sqlite_database_store.cpp.
//   6. Drop its entry from the NOT-IMPLEMENTED table in property.cpp.

#ifndef UAPP_CORE_DATABASE_PROPERTY_H
#define UAPP_CORE_DATABASE_PROPERTY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "uapp/object_id.h"
#include "uapp/status.h"

namespace uapp::database {

// --- identifiers -----------------------------------------------------------
// Distinct types over the same 16 bytes, so an OptionId cannot be passed
// where a PropertyId is wanted. See core/common/object_id.h.

struct PropertyIdTag;
struct OptionIdTag;

using PropertyId = ObjectId<PropertyIdTag>;
using OptionId   = ObjectId<OptionIdTag>;

// --- hard limits -----------------------------------------------------------
// Enforced at the domain boundary so nothing unbounded reaches storage
// (spec section 38: "Validate buffer sizes", "Validate external input").
// Every one of these is a REAL constraint with a named source, in the style
// of blocks::kMaxBlockDepth; none of them is a round number picked for looks.

/// Maximum properties (columns) in one database.
///
/// WHY A LIMIT: a row read materialises every property value the row has, and
/// a Table view read materialises that for every row returned. The property
/// count is therefore the multiplier on every read in this module, and it is
/// user-controlled.
///
/// WHY 100: database-design.md section 3.4 states the performance budget for
/// this module as "10k rows x 20 properties, filtered+sorted view in <= 100
/// ms". 100 is five times that budget's property count — comfortably above
/// any table a human maintains by hand — while keeping the worst-case single
/// row bounded at 100 values.
inline constexpr std::size_t kMaxPropertiesPerDatabase = 100;

/// Maximum rows in one database.
///
/// WHY 100 000: ten times the 10k-row figure in the section 3.4 budget, and
/// consistent with section 7's capacity table, which gives a heavy user
/// 500 000 BLOCKS across the whole library. A single user-defined database
/// holding a fifth of that is already past what the EAV shape is proven for
/// (section 11 lists "EAV view performance at extreme property counts is
/// unproven" as a known gap), so the cap is where the evidence stops rather
/// than where the arithmetic does.
inline constexpr std::size_t kMaxRowsPerDatabase = 100000;

/// Maximum options in one Select / Multi-select option set.
///
/// WHY 256: a select option set is a CONTROLLED VOCABULARY — the user picks
/// from it in a dropdown. Past a couple of hundred entries a dropdown is not
/// a usable control and the right model is a Relation to another database
/// (NOT IMPLEMENTED, see the reason table). 256 also keeps the option set
/// cheap to load eagerly with every schema read, which is what makes
/// validating a Select value a hash-free linear scan over a small vector
/// rather than a query.
inline constexpr std::size_t kMaxSelectOptionsPerProperty = 256;

/// Maximum bytes in one Text property value.
///
/// WHY NOT blocks::kMaxBlockTextBytes (1 MiB): a block is DOCUMENT CONTENT; a
/// Text property value is a CELL. A Table view read touches every visible
/// cell of every returned row, so this bound (not the row count alone) is
/// what bounds a view read: at 4 KiB, the budget's 10k x 20 worst case is
/// 800 MB of text, which is already why views paginate — at 1 MiB it would be
/// 200 GB and the limit would be decorative.
///
/// WHY 4096: it holds a long paragraph, which is the most anyone types into a
/// table cell, and it is the point past which the content belongs in the row's
/// own page rather than in the cell. (Rows-open-as-pages is NOT IMPLEMENTED;
/// see database.h. Until it lands this cap is the honest ceiling, not a
/// pointer at an escape hatch that exists.)
inline constexpr std::size_t kMaxPropertyTextBytes = 4096;

/// Maximum bytes in a property name. A property name is a COLUMN HEADER; it
/// has to fit in one. Matches the order of magnitude of a filename, not of a
/// title.
inline constexpr std::size_t kMaxPropertyNameBytes = 128;

/// Maximum bytes in a select option's name. Same reasoning as the property
/// name: it is a chip in a cell and a row in a dropdown.
inline constexpr std::size_t kMaxOptionNameBytes = 128;

/// Date values are an instant in UTC, in milliseconds. The bounds are
/// 0001-01-01T00:00:00.000Z .. 9999-12-31T23:59:59.999Z.
///
/// WHY BOUND IT AT ALL: an unbounded int64 millisecond instant overflows
/// every ms-to-calendar conversion a renderer will do, and a value outside
/// the proleptic Gregorian range every calendar implementation agrees on is
/// not a date the product can display. Rejecting at the domain boundary is
/// cheaper than discovering it in a date formatter.
inline constexpr std::int64_t kMinDateUnixMs = -62135596800000LL;
inline constexpr std::int64_t kMaxDateUnixMs = 253402300799999LL;

// --- property types --------------------------------------------------------

/// Every property type in spec section 15. Values are PERSISTED in the
/// `db_property.type` column: append-only, never renumber, never reuse.
enum class PropertyType : std::int32_t {
    // --- implemented -------------------------------------------------------
    Text        = 0,
    Number      = 1,
    /// Spec section 15 calls it "Boolean"; every comparable product calls the
    /// control a checkbox. The name here follows the UI because that is what
    /// a property picker will show; the payload is a plain `bool`.
    Checkbox    = 2,
    Select      = 3,
    MultiSelect = 4,
    Date        = 5,

    // --- declared, NOT IMPLEMENTED ----------------------------------------
    // Each has a reason; see property_type_not_implemented_reason().
    // Constructing a value of one is impossible (there is no variant
    // alternative for it) and reading one out of a database yields
    // Status::DatabasePropertyTypeUnsupported rather than a silently-wrong
    // text cell.
    Person   = 6,
    Url      = 7,
    Email    = 8,
    File     = 9,
    Relation = 10,
    Rollup   = 11,
    Formula  = 12,
};

/// The first PropertyType value that is not implemented in this build.
inline constexpr std::int32_t kFirstUnimplementedPropertyType = 6;

/// Number of PropertyType values that exist at all (implemented or not).
inline constexpr std::int32_t kPropertyTypeCount = 13;

// --- value payloads --------------------------------------------------------
// Real typed fields. None of these is a stringly-typed escape hatch.

struct TextValue final {
    /// MUST be non-empty. An empty string and "the property is unset" would
    /// otherwise be two spellings of one state, and then `is empty` as a
    /// filter operator has two answers. A row omits the property instead.
    std::string text;
};

struct NumberValue final {
    /// MUST be finite. NaN and the infinities are rejected for a concrete
    /// reason, not for tidiness: NaN compares false against everything
    /// including itself, so a sort key of NaN makes the ordering depend on
    /// the sort algorithm's pivot choices — and SQLite stores a bound NaN as
    /// SQL NULL, which would silently turn a present value into an absent
    /// one on the next read.
    double number = 0.0;
};

struct CheckboxValue final {
    bool checked = false;
};

struct SelectValue final {
    /// Identifies one option of the property's own option set BY ID. A free
    /// string that happens to match an option name is exactly what this type
    /// exists to prevent.
    OptionId option;
};

struct MultiSelectValue final {
    /// MUST be non-empty (same argument as TextValue) and MUST NOT repeat an
    /// option. Order is the user's chosen order and is preserved; it is NOT
    /// the sort order, which comes from the schema's option order.
    std::vector<OptionId> options;
};

struct DateValue final {
    /// An instant in UTC, milliseconds since the Unix epoch, within
    /// [kMinDateUnixMs, kMaxDateUnixMs].
    ///
    /// NOT IMPLEMENTED, deliberately and separately recorded because each is
    /// a real feature rather than an oversight:
    ///   * DATE-ONLY vs DATE-TIME. "2026-10-04" and "2026-10-04T09:00Z" are
    ///     different user intents, and the difference changes what `on` means
    ///     (instant equality vs same-calendar-day). Same-calendar-day needs a
    ///     timezone, and a timezone needs a tz database — a dependency with
    ///     its own spec section 35 licence review.
    ///   * DATE RANGES (start + end). A second instant plus the invariant
    ///     end >= start, plus range-aware filter operators (overlaps, within)
    ///     that the flat operator set here has no room for.
    /// Until those land, `on` is exact instant equality and is documented as
    /// such wherever it is offered.
    std::int64_t unix_ms = 0;
};

inline bool operator==(const TextValue& a, const TextValue& b) { return a.text == b.text; }
inline bool operator==(const NumberValue& a, const NumberValue& b) {
    // Bit-identical comparison is intended: these are stored values being
    // compared for equality, not computed results being compared for
    // closeness. A tolerance here would make `=` non-transitive.
    return a.number == b.number;
}
inline bool operator==(const CheckboxValue& a, const CheckboxValue& b) {
    return a.checked == b.checked;
}
inline bool operator==(const SelectValue& a, const SelectValue& b) {
    return a.option == b.option;
}
inline bool operator==(const MultiSelectValue& a, const MultiSelectValue& b) {
    return a.options == b.options;
}
inline bool operator==(const DateValue& a, const DateValue& b) {
    return a.unix_ms == b.unix_ms;
}

/// The tagged union. ALTERNATIVE ORDER IS LOAD-BEARING: index N must be the
/// payload for PropertyType == N, because that integer is what gets
/// persisted. The static_asserts below make a mis-ordered edit a compile
/// error.
using PropertyValue = std::variant<TextValue,         // 0
                                   NumberValue,       // 1
                                   CheckboxValue,     // 2
                                   SelectValue,       // 3
                                   MultiSelectValue,  // 4
                                   DateValue>;        // 5

static_assert(std::variant_size_v<PropertyValue> ==
                  static_cast<std::size_t>(kFirstUnimplementedPropertyType),
              "PropertyValue must hold exactly the implemented PropertyType values");
static_assert(std::is_same_v<std::variant_alternative_t<0, PropertyValue>, TextValue>);
static_assert(std::is_same_v<std::variant_alternative_t<1, PropertyValue>, NumberValue>);
static_assert(std::is_same_v<std::variant_alternative_t<2, PropertyValue>, CheckboxValue>);
static_assert(std::is_same_v<std::variant_alternative_t<3, PropertyValue>, SelectValue>);
static_assert(std::is_same_v<std::variant_alternative_t<4, PropertyValue>, MultiSelectValue>);
static_assert(std::is_same_v<std::variant_alternative_t<5, PropertyValue>, DateValue>);

/// The type tag of a value. Total, and cannot disagree with the variant.
[[nodiscard]] PropertyType property_type_of(const PropertyValue& value) noexcept;

/// Stable lowercase identifier ("text", "multi_select", ...). Matches the
/// `type` strings database-design.md section 3.4 sketches. Never null;
/// returns "unknown" for an out-of-range integer cast into the enum.
[[nodiscard]] const char* property_type_name(PropertyType type) noexcept;

[[nodiscard]] bool property_type_is_implemented(PropertyType type) noexcept;

/// Why a declared-but-absent type is absent. Returns nullptr for an
/// implemented type. This is the machine-readable form of the spec section 5
/// "NOT IMPLEMENTED" marker, so the reason cannot rot in a comment.
[[nodiscard]] const char* property_type_not_implemented_reason(PropertyType type) noexcept;

/// True iff this type's values carry a defined option set (Select,
/// Multi-select). Used by schema validation to require an option set where
/// one is meaningful and to forbid one where it is not.
[[nodiscard]] bool property_type_has_options(PropertyType type) noexcept;

// --- the schema ------------------------------------------------------------

struct SelectOption final {
    OptionId    id;
    std::string name;
};

/// One property DEFINITION: the column, not the cell.
struct PropertyDefinition final {
    PropertyId   id;
    std::string  name;
    PropertyType type = PropertyType::Text;

    /// The option set. MUST be non-empty for Select / Multi-select and MUST
    /// be empty for every other type. Its ORDER is the schema's option order,
    /// which is what sorting by a Select property sorts by — users mean "To
    /// do, In progress, Done", not "Done, In progress, To do" alphabetically.
    std::vector<SelectOption> options;
};

/// Find an option by id. Returns nullptr when the property does not define
/// it, which is the Status::DatabaseOptionNotInSchema path.
[[nodiscard]] const SelectOption* find_option(const PropertyDefinition& property,
                                              const OptionId& id) noexcept;

/// Index of an option in the definition's option order, or `options.size()`
/// if absent. This index IS the sort key for Select and Multi-select.
[[nodiscard]] std::size_t option_order(const PropertyDefinition& property,
                                       const OptionId& id) noexcept;

/// Validate a property definition in isolation: non-nil id, legal name,
/// implemented type, option set present/absent as the type requires, option
/// ids non-nil and unique, option names legal and unique, option count within
/// kMaxSelectOptionsPerProperty.
[[nodiscard]] Status validate_property_definition(const PropertyDefinition& property) noexcept;

// --- values ----------------------------------------------------------------

/// Validate a value against ITS OWN TYPE's rules only: text size and
/// non-emptiness and absence of embedded NUL, number finiteness, date range,
/// multi-select non-emptiness / uniqueness / size. Knows nothing about any
/// schema.
[[nodiscard]] Status validate_property_value(const PropertyValue& value) noexcept;

/// Validate a value against a property DEFINITION: everything
/// validate_property_value checks, plus that the value's type IS the
/// property's type (Status::DatabaseValueTypeMismatch) and that every option
/// it references is one the property defines
/// (Status::DatabaseOptionNotInSchema).
///
/// This is the function that makes "you cannot write a string into a Number
/// property" and "you cannot select an option that is not in the set" true
/// rather than aspirational. It is called on write AND on read, because a
/// database file is attacker-reachable (spec section 28).
[[nodiscard]] Status validate_value_for_property(const PropertyDefinition& property,
                                                 const PropertyValue& value) noexcept;

/// Shared name validation: non-empty is NOT required (an unnamed column is a
/// real transient state), but an over-long name or an embedded NUL is an
/// error. `limit` is the per-kind cap above.
[[nodiscard]] Status validate_name(std::string_view name, std::size_t limit) noexcept;

}  // namespace uapp::database

#endif  // UAPP_CORE_DATABASE_PROPERTY_H
