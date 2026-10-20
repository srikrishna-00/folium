#include "app_state.h"

namespace uapp::api {

AppState::~AppState() {
    uapp_search_index_close(&search);
    uapp_document_service_close(&documents);
    uapp_note_service_close(&notes);
}

bool AppState::open(const std::string& database_path, std::string& out_error) {
    uapp_note_service_config_t notes_config;
    uapp_note_service_config_init(&notes_config);
    notes_config.database_path = database_path.c_str();

    uapp_status_t status = uapp_note_service_open(&notes_config, &notes);
    if (status != UAPP_STATUS_OK) {
        out_error = std::string("opening note service: ") + uapp_status_code(status);
        return false;
    }

    uapp_document_service_config_t documents_config;
    uapp_document_service_config_init(&documents_config);
    documents_config.database_path = database_path.c_str();

    status = uapp_document_service_open(&documents_config, &documents);
    if (status != UAPP_STATUS_OK) {
        out_error = std::string("opening document service: ") + uapp_status_code(status);
        uapp_note_service_close(&notes);
        return false;
    }

    // Read-only by construction (uapp_search_index_config_init's own
    // default), opened AFTER the two writers above so the index tables it
    // requires already exist on a fresh database file.
    uapp_search_index_config_t search_config;
    uapp_search_index_config_init(&search_config);
    search_config.database_path = database_path.c_str();

    status = uapp_search_index_open(&search_config, &search);
    if (status != UAPP_STATUS_OK) {
        out_error = std::string("opening search index: ") + uapp_status_code(status);
        uapp_document_service_close(&documents);
        uapp_note_service_close(&notes);
        return false;
    }

    return true;
}

}  // namespace uapp::api
