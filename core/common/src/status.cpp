// SPDX-License-Identifier: Apache-2.0
#include "uapp/status.h"

namespace uapp {

const char* status_code(Status s) noexcept {
    switch (s) {
        case Status::Ok:                return "OK";
        case Status::InvalidArgument:   return "INVALID_ARGUMENT";
        case Status::NotFound:          return "NOT_FOUND";
        case Status::AlreadyExists:     return "ALREADY_EXISTS";
        case Status::Internal:          return "INTERNAL";
        case Status::IdMalformed:       return "ID_MALFORMED";
        case Status::NoteTitleTooLong:  return "NOTE_TITLE_TOO_LONG";
        case Status::NoteBodyTooLarge:  return "NOTE_BODY_TOO_LARGE";
        case Status::NoteIdMalformed:   return "NOTE_ID_MALFORMED";
        case Status::StoreOpenFailed:   return "STORE_OPEN_FAILED";
        case Status::StoreSchemaFailed: return "STORE_SCHEMA_FAILED";
        case Status::StoreReadFailed:   return "STORE_READ_FAILED";
        case Status::StoreWriteFailed:  return "STORE_WRITE_FAILED";
        case Status::StoreReadOnly:     return "STORE_READ_ONLY";
        case Status::SearchQueryEmpty:  return "SEARCH_QUERY_EMPTY";
        case Status::SearchFailed:      return "SEARCH_FAILED";

        case Status::BlockTypeUnsupported:   return "BLOCK_TYPE_UNSUPPORTED";
        case Status::BlockDepthExceeded:     return "BLOCK_DEPTH_EXCEEDED";
        case Status::BlockTextTooLarge:      return "BLOCK_TEXT_TOO_LARGE";
        case Status::BlockMalformed:         return "BLOCK_MALFORMED";
        case Status::BlockPositionInvalid:   return "BLOCK_POSITION_INVALID";
        case Status::BlockCycleDetected:     return "BLOCK_CYCLE_DETECTED";
        case Status::PageTitleTooLong:       return "PAGE_TITLE_TOO_LONG";
        case Status::PageBlockLimitExceeded: return "PAGE_BLOCK_LIMIT_EXCEEDED";

        case Status::FileEmpty:       return "FILE_EMPTY";
        case Status::FilePathInvalid: return "FILE_PATH_INVALID";
        case Status::FileOpenFailed:  return "FILE_OPEN_FAILED";
        case Status::FileReadFailed:  return "FILE_READ_FAILED";

        case Status::PdfMalformed:           return "PDF_MALFORMED";
        case Status::PdfPasswordRequired:    return "PDF_PASSWORD_REQUIRED";
        case Status::PdfSecurityUnsupported: return "PDF_SECURITY_UNSUPPORTED";
        case Status::PdfPageNotFound:        return "PDF_PAGE_NOT_FOUND";
        case Status::PdfPageLoadFailed:      return "PDF_PAGE_LOAD_FAILED";
        case Status::PdfRenderFailed:        return "PDF_RENDER_FAILED";
        case Status::PdfRenderSizeInvalid:   return "PDF_RENDER_SIZE_INVALID";
        case Status::PdfBufferTooSmall:      return "PDF_BUFFER_TOO_SMALL";
        case Status::PdfNotOpen:             return "PDF_NOT_OPEN";

        case Status::ConversionInputTooLarge:  return "CONVERSION_INPUT_TOO_LARGE";
        case Status::ConversionInputMalformed: return "CONVERSION_INPUT_MALFORMED";
        case Status::ConversionOutputTooLarge: return "CONVERSION_OUTPUT_TOO_LARGE";

        case Status::DatabasePropertyLimitExceeded:
            return "DATABASE_PROPERTY_LIMIT_EXCEEDED";
        case Status::DatabaseRowLimitExceeded:
            return "DATABASE_ROW_LIMIT_EXCEEDED";
        case Status::DatabaseOptionLimitExceeded:
            return "DATABASE_OPTION_LIMIT_EXCEEDED";
        case Status::DatabaseViewLimitExceeded:
            return "DATABASE_VIEW_LIMIT_EXCEEDED";
        case Status::DatabaseViewSpecTooComplex:
            return "DATABASE_VIEW_SPEC_TOO_COMPLEX";
        case Status::DatabaseNameTooLong:
            return "DATABASE_NAME_TOO_LONG";
        case Status::DatabaseValueTooLarge:
            return "DATABASE_VALUE_TOO_LARGE";
        case Status::DatabaseValueMalformed:
            return "DATABASE_VALUE_MALFORMED";
        case Status::DatabaseValueTypeMismatch:
            return "DATABASE_VALUE_TYPE_MISMATCH";
        case Status::DatabaseOptionNotInSchema:
            return "DATABASE_OPTION_NOT_IN_SCHEMA";
        case Status::DatabasePropertyNotInSchema:
            return "DATABASE_PROPERTY_NOT_IN_SCHEMA";
        case Status::DatabasePropertyDuplicated:
            return "DATABASE_PROPERTY_DUPLICATED";
        case Status::DatabasePropertyTypeUnsupported:
            return "DATABASE_PROPERTY_TYPE_UNSUPPORTED";
        case Status::DatabaseViewKindUnsupported:
            return "DATABASE_VIEW_KIND_UNSUPPORTED";
        case Status::DatabaseFilterOperatorUnsupported:
            return "DATABASE_FILTER_OPERATOR_UNSUPPORTED";
        case Status::DatabaseSchemaMalformed:
            return "DATABASE_SCHEMA_MALFORMED";

        case Status::CryptoNotInitialised:
            return "CRYPTO_NOT_INITIALISED";
        case Status::CryptoRandomFailed:
            return "CRYPTO_RANDOM_FAILED";
        case Status::CryptoKeyInvalid:
            return "CRYPTO_KEY_INVALID";
        case Status::CryptoAuthFailed:
            return "CRYPTO_AUTH_FAILED";
        case Status::CryptoSealedBlobBad:
            return "CRYPTO_SEALED_BLOB_BAD";
        case Status::CryptoMessageTooLarge:
            return "CRYPTO_MESSAGE_TOO_LARGE";
        case Status::CryptoKdfFailed:
            return "CRYPTO_KDF_FAILED";
        case Status::CryptoSecureAllocFailed:
            return "CRYPTO_SECURE_ALLOC_FAILED";
        case Status::CryptoPassphraseTooShort:
            return "CRYPTO_PASSPHRASE_TOO_SHORT";

        case Status::KeyRecordVersionUnsupported:
            return "KEY_RECORD_VERSION_UNSUPPORTED";
        case Status::KeyRecordRoleMismatch:
            return "KEY_RECORD_ROLE_MISMATCH";
        case Status::KeyScopeIdInvalid:
            return "KEY_SCOPE_ID_INVALID";

        case Status::AiProviderUnavailable:      return "AI_PROVIDER_UNAVAILABLE";
        case Status::AiCapabilityNotImplemented: return "AI_CAPABILITY_NOT_IMPLEMENTED";
        case Status::AiProviderNotRegistered:    return "AI_PROVIDER_NOT_REGISTERED";
        case Status::AiProviderDuplicated:       return "AI_PROVIDER_DUPLICATED";
        case Status::AiNoEligibleProvider:       return "AI_NO_ELIGIBLE_PROVIDER";
        case Status::AiPolicyBlocked:            return "AI_POLICY_BLOCKED";
        case Status::AiConsentRequired:          return "AI_CONSENT_REQUIRED";
        case Status::AiConsentDenied:            return "AI_CONSENT_DENIED";
        case Status::AiConsentScopeMismatch:     return "AI_CONSENT_SCOPE_MISMATCH";
        case Status::AiConsentExpired:           return "AI_CONSENT_EXPIRED";
        case Status::AiConsentTokenSpent:        return "AI_CONSENT_TOKEN_SPENT";
        case Status::AiInputEmpty:               return "AI_INPUT_EMPTY";
        case Status::AiInputTooLarge:            return "AI_INPUT_TOO_LARGE";
        case Status::AiInputMalformed:           return "AI_INPUT_MALFORMED";
        case Status::AiLabelSetInvalid:          return "AI_LABEL_SET_INVALID";
        case Status::AiResponseMalformed:        return "AI_RESPONSE_MALFORMED";
        case Status::AiRateLimited:              return "AI_RATE_LIMITED";
        case Status::AiContentPolicyRejected:    return "AI_CONTENT_POLICY_REJECTED";
        case Status::AiConsentRevoked:           return "AI_CONSENT_REVOKED";

        case Status::OcrNotOpen:                 return "OCR_NOT_OPEN";
        case Status::OcrDataPathInvalid:         return "OCR_DATA_PATH_INVALID";
        case Status::OcrLanguageUnavailable:     return "OCR_LANGUAGE_UNAVAILABLE";
        case Status::OcrLanguageNotImplemented:  return "OCR_LANGUAGE_NOT_IMPLEMENTED";
        case Status::OcrLanguageMalformed:       return "OCR_LANGUAGE_MALFORMED";
        case Status::OcrEngineInitFailed:        return "OCR_ENGINE_INIT_FAILED";
        case Status::OcrImageSizeInvalid:        return "OCR_IMAGE_SIZE_INVALID";
        case Status::OcrImageBufferTooSmall:     return "OCR_IMAGE_BUFFER_TOO_SMALL";
        case Status::OcrRecognitionFailed:       return "OCR_RECOGNITION_FAILED";
        case Status::OcrResultTooLarge:          return "OCR_RESULT_TOO_LARGE";
        case Status::PdfWriterPageCountExceeded:    return "PDF_WRITER_PAGE_COUNT_EXCEEDED";
        case Status::PdfWriterDimensionTooLarge:    return "PDF_WRITER_DIMENSION_TOO_LARGE";
        case Status::PdfWriterTotalPixelsTooLarge:  return "PDF_WRITER_TOTAL_PIXELS_TOO_LARGE";
        case Status::PdfWriterRotationInvalid:      return "PDF_WRITER_ROTATION_INVALID";
        case Status::PdfWriterEncodeFailed:         return "PDF_WRITER_ENCODE_FAILED";
        case Status::PdfWriterImageMalformed:       return "PDF_WRITER_IMAGE_MALFORMED";
    }
    // Reached only if a caller casts a bogus integer into Status. We do not
    // trust that it cannot happen just because the enum is closed.
    return "UNKNOWN_STATUS";
}

const char* status_message(Status s) noexcept {
    switch (s) {
        case Status::Ok:
            return "Success.";
        case Status::InvalidArgument:
            return "An argument was missing or structurally invalid.";
        case Status::NotFound:
            return "The requested note does not exist.";
        case Status::AlreadyExists:
            return "A note with that identifier already exists.";
        case Status::Internal:
            return "An internal error occurred; this is a defect, please report it.";
        case Status::IdMalformed:
            return "Identifier is not 32 hexadecimal characters.";
        case Status::NoteTitleTooLong:
            return "Note title exceeds the maximum allowed length.";
        case Status::NoteBodyTooLarge:
            return "Note body exceeds the maximum allowed size.";
        case Status::NoteIdMalformed:
            return "Note identifier is not 32 lowercase hexadecimal characters.";
        case Status::StoreOpenFailed:
            return "Could not open the local note database.";
        case Status::StoreSchemaFailed:
            return "Could not create or migrate the local note database schema.";
        case Status::StoreReadFailed:
            return "Reading from the local note database failed.";
        case Status::StoreWriteFailed:
            return "Writing to the local note database failed.";
        case Status::StoreReadOnly:
            return "The local note database was opened read-only; writes are rejected.";
        case Status::SearchQueryEmpty:
            return "The search query contained no searchable terms.";
        case Status::SearchFailed:
            return "The full-text search index could not answer the query.";

        case Status::BlockTypeUnsupported:
            return "That block type is recognised but not implemented in this build.";
        case Status::BlockDepthExceeded:
            return "Blocks are nested deeper than the maximum allowed depth.";
        case Status::BlockTextTooLarge:
            return "Block text exceeds the maximum allowed size.";
        case Status::BlockMalformed:
            return "Block content violates the rules of its own block type.";
        case Status::BlockPositionInvalid:
            return "The requested block position is past the end of its sibling list.";
        case Status::BlockCycleDetected:
            return "That move would make a block a descendant of itself.";
        case Status::PageTitleTooLong:
            return "Page title exceeds the maximum allowed length.";
        case Status::PageBlockLimitExceeded:
            return "The page already holds the maximum allowed number of blocks.";

        case Status::FileEmpty:
            return "The file or buffer contains no bytes, so there is nothing to work on.";
        case Status::FilePathInvalid:
            return "The file path was empty or contained an embedded NUL byte.";
        case Status::FileOpenFailed:
            return "The file could not be opened.";
        case Status::FileReadFailed:
            return "Reading the file failed before all requested bytes were available.";

        case Status::PdfMalformed:
            return "The bytes are not a PDF, or the PDF is damaged beyond recovery.";
        case Status::PdfPasswordRequired:
            return "The PDF is encrypted and the supplied password was missing or wrong.";
        case Status::PdfSecurityUnsupported:
            return "The PDF uses a security handler that is not supported.";
        case Status::PdfPageNotFound:
            return "The requested page index is outside the document's page range.";
        case Status::PdfPageLoadFailed:
            return "The page exists but could not be loaded.";
        case Status::PdfRenderFailed:
            return "Rendering the page into the supplied buffer failed.";
        case Status::PdfRenderSizeInvalid:
            return "The requested render width, height or stride is not usable.";
        case Status::PdfBufferTooSmall:
            return "The supplied pixel buffer is too small for the requested render size.";
        case Status::PdfNotOpen:
            return "No PDF document is open on this handle.";

        case Status::ConversionInputTooLarge:
            return "The document being converted is larger than the conversion size limit.";
        case Status::ConversionInputMalformed:
            return "The document being converted contains a NUL byte and is not valid text.";
        case Status::ConversionOutputTooLarge:
            return "Converting this document would produce output past the conversion "
                   "size limit.";

        case Status::DatabasePropertyLimitExceeded:
            return "The database already defines the maximum allowed number of properties.";
        case Status::DatabaseRowLimitExceeded:
            return "The database already holds the maximum allowed number of rows.";
        case Status::DatabaseOptionLimitExceeded:
            return "The property's option set exceeds the maximum allowed number of options.";
        case Status::DatabaseViewLimitExceeded:
            return "The database already holds the maximum allowed number of saved views.";
        case Status::DatabaseViewSpecTooComplex:
            return "The view references more filters or sort keys than are allowed.";
        case Status::DatabaseNameTooLong:
            return "A database, property, option or view name exceeds its maximum length.";
        case Status::DatabaseValueTooLarge:
            return "A text property value exceeds the maximum allowed size.";
        case Status::DatabaseValueMalformed:
            return "A property value violates the rules of its own property type.";
        case Status::DatabaseValueTypeMismatch:
            return "A property value's type does not match the type its property declares.";
        case Status::DatabaseOptionNotInSchema:
            return "The selected option is not one the property defines.";
        case Status::DatabasePropertyNotInSchema:
            return "The referenced property is not one the database defines.";
        case Status::DatabasePropertyDuplicated:
            return "A property or option identifier or name appears twice in one schema.";
        case Status::DatabasePropertyTypeUnsupported:
            return "That property type is recognised but not implemented in this build.";
        case Status::DatabaseViewKindUnsupported:
            return "That view kind is recognised but not implemented in this build.";
        case Status::DatabaseFilterOperatorUnsupported:
            return "That filter operator is not applicable to that property type.";
        case Status::DatabaseSchemaMalformed:
            return "The database schema is structurally invalid.";

        case Status::CryptoNotInitialised:
            return "The cryptographic library has not been initialised.";
        case Status::CryptoRandomFailed:
            return "The platform random number generator could not be used.";
        case Status::CryptoKeyInvalid:
            return "A key or public key is the wrong size or is not a usable value.";
        // Same wording for every cause, on purpose: see the note in status.h.
        case Status::CryptoAuthFailed:
            return "The ciphertext did not authenticate under the supplied key.";
        case Status::CryptoSealedBlobBad:
            return "The sealed blob is too short to contain a nonce and a tag.";
        case Status::CryptoMessageTooLarge:
            return "The message or associated data exceeds the encryption size limit.";
        case Status::CryptoKdfFailed:
            return "Deriving a key from the passphrase failed, most likely out of memory.";
        case Status::CryptoSecureAllocFailed:
            return "A guarded secure allocation for key material could not be made.";
        case Status::CryptoPassphraseTooShort:
            return "The passphrase is shorter than the minimum accepted length.";

        case Status::KeyRecordVersionUnsupported:
            return "This build cannot read that wrapped-key record format version.";
        case Status::KeyRecordRoleMismatch:
            return "That wrapped-key record holds a different kind of key than was requested.";
        case Status::KeyScopeIdInvalid:
            return "The workspace or document identifier is empty or too long.";

        case Status::AiProviderUnavailable:
            return "That AI provider is registered but is not usable right now.";
        case Status::AiCapabilityNotImplemented:
            return "That AI provider does not implement the requested capability.";
        case Status::AiProviderNotRegistered:
            return "No AI provider is registered under that identifier.";
        case Status::AiProviderDuplicated:
            return "An AI provider is already registered under that identifier.";
        case Status::AiNoEligibleProvider:
            return "No registered AI provider satisfies that selection.";
        case Status::AiPolicyBlocked:
            return "The workspace AI policy forbids sending this content off the device.";
        case Status::AiConsentRequired:
            return "Sending this content to that provider needs explicit user consent first.";
        case Status::AiConsentDenied:
            return "The user declined to send this content to that provider.";
        case Status::AiConsentScopeMismatch:
            return "That consent grant was for a different provider, scope or content.";
        case Status::AiConsentExpired:
            return "That consent grant has expired.";
        case Status::AiConsentTokenSpent:
            return "That consent token has already been used; grants are single-use.";
        case Status::AiInputEmpty:
            return "The request carried no usable text.";
        case Status::AiInputTooLarge:
            return "The request content is larger than this build accepts.";
        case Status::AiInputMalformed:
            return "The request text contains a NUL byte or a disallowed control character.";
        case Status::AiLabelSetInvalid:
            return "The classification label set is empty, too large, or has duplicates.";
        case Status::AiResponseMalformed:
            return "The AI provider returned a response that violates its own contract.";
        case Status::AiRateLimited:
            return "The AI provider is rate limiting this client.";
        case Status::AiContentPolicyRejected:
            return "The AI provider refused this content under its content policy.";
        case Status::AiConsentRevoked:
            return "That consent grant was revoked; it stops future sends only.";

        case Status::OcrNotOpen:
            return "The OCR engine is not open.";
        case Status::OcrDataPathInvalid:
            return "The OCR language-data directory is missing or unusable.";
        case Status::OcrLanguageUnavailable:
            return "No OCR language data for that language is installed on this device.";
        case Status::OcrLanguageNotImplemented:
            return "This build has no OCR language pack for that language.";
        case Status::OcrLanguageMalformed:
            return "That OCR language tag is empty, too long, or not lowercase letters.";
        case Status::OcrEngineInitFailed:
            return "The OCR engine could not initialise with that language data.";
        case Status::OcrImageSizeInvalid:
            return "The image size, stride or pixel count is outside what OCR accepts.";
        case Status::OcrImageBufferTooSmall:
            return "The supplied pixel buffer is too small for that image geometry.";
        case Status::OcrRecognitionFailed:
            return "The OCR engine failed while recognising that image.";
        case Status::OcrResultTooLarge:
            return "That image produced more OCR output than this build will return.";
        case Status::PdfWriterPageCountExceeded:
            return "More pages were requested than this build's PDF writer allows.";
        case Status::PdfWriterDimensionTooLarge:
            return "A page image's width or height is past this build's PDF writer limit.";
        case Status::PdfWriterTotalPixelsTooLarge:
            return "The combined size of every page's image is past this build's PDF writer limit.";
        case Status::PdfWriterRotationInvalid:
            return "A page's rotation must be 0, 1, 2 or 3 quarter turns.";
        case Status::PdfWriterEncodeFailed:
            return "The JPEG encoder refused a page image that otherwise passed every check.";
        case Status::PdfWriterImageMalformed:
            return "Those bytes are not a recognisable image format.";
    }
    return "Unrecognised status code.";
}

}  // namespace uapp
