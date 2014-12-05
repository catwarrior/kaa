/*
 * Copyright 2014 CyberVision, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "kaa_profile.h"

#include <string.h>

#include "avro_src/avro/io.h"

#include "kaa_common.h"
#include "kaa_mem.h"
#include "kaa_defaults.h"
#include "kaa_external.h"
#include "kaa_channel_manager.h"
#include "kaa_status.h"

extern kaa_sync_handler_fn kaa_channel_manager_get_sync_handler(kaa_channel_manager_t *self, kaa_service_t service_type);

static kaa_service_t profile_sync_services[1] = { KAA_SERVICE_PROFILE };

struct kaa_profile_manager_t {
    bool need_resync;
    kaa_bytes_t profile_body;
    kaa_digest profile_hash;
    kaa_channel_manager_t *channel_manager;
    kaa_status_t *status;
};

static kaa_endpoint_version_info_t * create_versions_info() {
    kaa_endpoint_version_info_t *vi = kaa_endpoint_version_info_create();
    if (!vi)
        return NULL;

    vi->config_version = CONFIG_SCHEMA_VERSION;
    vi->log_schema_version = LOG_SCHEMA_VERSION;
    vi->profile_version = PROFILE_SCHEMA_VERSION;
    vi->system_nf_version = SYSTEM_NF_SCHEMA_VERSION;
    vi->user_nf_version = USER_NF_SCHEMA_VERSION;
#if KAA_EVENT_SCHEMA_VERSIONS_SIZE > 0
    vi->event_family_versions = kaa_array_event_class_family_version_info_null_union_array_branch_create();
    if (!vi->event_family_versions) {
        vi->destroy(vi);
        return NULL;
    }
    vi->event_family_versions->data = NULL;

    size_t i = 0;
    for (; i < KAA_EVENT_SCHEMA_VERSIONS_SIZE; ++i) {
        kaa_event_class_family_version_info_t *ecfv = kaa_event_class_family_version_info_create();
        if (!ecfv) {
            vi->destroy(vi);
            return NULL;
        }

        ecfv->name = kaa_string_move_create(KAA_EVENT_SCHEMA_VERSIONS[i].name, NULL); // destructor is not needed
        ecfv->version = KAA_EVENT_SCHEMA_VERSIONS[i].version;
        if (vi->event_family_versions->data != NULL) {
            kaa_list_push_back(vi->event_family_versions->data, ecfv);
            // FIXME: check error
        } else {
            vi->event_family_versions->data = kaa_list_create(ecfv);
            if (!vi->event_family_versions->data) {
                KAA_FREE(ecfv->name);
                KAA_FREE(ecfv);
                vi->destroy(vi);
                return NULL;
            }
        }
    }
#else
    vi->event_family_versions = kaa_array_event_class_family_version_info_null_union_null_branch_create();
    if (!vi->event_family_versions) {
        vi->destroy(vi);
        return NULL;
    }
#endif
    return vi;
}

/**
 * PUBLIC FUNCTIONS
 */
kaa_error_t kaa_profile_manager_create(kaa_profile_manager_t ** profile_manager_p, kaa_status_t *status, kaa_channel_manager_t *channel_manager) {
    KAA_RETURN_IF_NIL3(profile_manager_p, channel_manager, status, KAA_ERR_BADPARAM);

    kaa_profile_manager_t * profile_manager = (kaa_profile_manager_t *) KAA_MALLOC(sizeof(kaa_profile_manager_t));
    if (!profile_manager)
        return KAA_ERR_NOMEM;

    profile_manager->need_resync = true;

    profile_manager->profile_body.size = 0;
    profile_manager->profile_body.buffer = NULL;
    profile_manager->channel_manager = channel_manager;
    profile_manager->status = status;

    *profile_manager_p = profile_manager;
    return KAA_ERR_NONE;
}

void kaa_profile_manager_destroy(kaa_profile_manager_t *self) {
    if (self != NULL) {
        kaa_bytes_destroy(&self->profile_body);
        KAA_FREE(self);
    }
}

kaa_error_t kaa_profile_need_profile_resync(kaa_profile_manager_t *self, bool *result)
{
    KAA_RETURN_IF_NIL2(self, result, KAA_ERR_BADPARAM);
    *result = self->need_resync;
    return KAA_ERR_NONE;
}

static void kaa_no_destroy_bytes(void *data)
{
    if (data) {
        kaa_union_t *kaa_union = (kaa_union_t*)data;
        switch (kaa_union->type) {
            case KAA_BYTES_NULL_UNION_BYTES_BRANCH:
                KAA_FREE(kaa_union->data);
                break;
            default:
                break;
        }
    }
}

kaa_error_t kaa_profile_compile_request(kaa_profile_manager_t *self, kaa_profile_sync_request_t **result)
{
    KAA_RETURN_IF_NIL2(self, result, KAA_ERR_NOMEM);

    kaa_profile_sync_request_t *request = kaa_profile_sync_request_create();
    if (!request)
        return KAA_ERR_NOMEM;

    request->version_info = create_versions_info();
    if (!request->version_info) {
        request->destroy(request);
        return KAA_ERR_NOMEM;
    }

    const char * ep_acc_token = NULL;
    if (kaa_status_get_endpoint_access_token(self->status, &ep_acc_token)) {
        request->destroy(request);
        return KAA_ERR_BAD_STATE;
    }

    if (ep_acc_token) {
        request->endpoint_access_token = kaa_string_null_union_string_branch_create();
        if (!request->endpoint_access_token) {
            request->destroy(request);
            return KAA_ERR_NOMEM;
        }
        request->endpoint_access_token->data = (void *) ep_acc_token; // destructor is not needed
    } else {
        request->endpoint_access_token = kaa_string_null_union_null_branch_create();
        if (!request->endpoint_access_token) {
            request->destroy(request);
            return KAA_ERR_NOMEM;
        }
    }

    request->profile_body = (kaa_bytes_t *) KAA_MALLOC(sizeof(kaa_bytes_t));
    if (!request->profile_body) {
        request->destroy(request);
        return KAA_ERR_NOMEM;
    }
    request->profile_body->size = self->profile_body.size;
    request->profile_body->buffer = self->profile_body.buffer;  // destructor for buffer is not needed

    bool is_registered = false;
    if (kaa_is_endpoint_registered(self->status, &is_registered)) {
        request->destroy(request);
        return KAA_ERR_BAD_STATE;
    }

    if (is_registered) {
        request->endpoint_public_key = kaa_bytes_null_union_null_branch_create();
        if (!request->endpoint_public_key) {
            request->destroy(request);
            return KAA_ERR_NOMEM;
        }
    } else {
        request->endpoint_public_key = kaa_bytes_null_union_bytes_branch_create();
        if (!request->endpoint_public_key) {
            request->destroy(request);
            return KAA_ERR_NOMEM;
        }

        kaa_bytes_t *pub_key = (kaa_bytes_t *) KAA_MALLOC(sizeof(kaa_bytes_t));
        if (!pub_key) {
            request->destroy(request);
            return KAA_ERR_NOMEM;
        }
        // FIXME: avro destructor for bytes

        bool need_deallocation = false;
        kaa_get_endpoint_public_key((char **)&pub_key->buffer, (size_t *)&pub_key->size, &need_deallocation);
        request->endpoint_public_key->data = pub_key;
        if (!need_deallocation) {
            request->endpoint_public_key->destroy = &kaa_no_destroy_bytes;
        }
    }
    *result = request;
    return KAA_ERR_NONE;
}

kaa_error_t kaa_profile_handle_sync(kaa_profile_manager_t *self, kaa_profile_sync_response_t *response) {
    KAA_RETURN_IF_NIL2(self, response, KAA_ERR_BADPARAM);

    self->need_resync = false;
    if (response->response_status == ENUM_SYNC_RESPONSE_STATUS_RESYNC) {
        self->need_resync = true;
        kaa_sync_handler_fn sync = kaa_channel_manager_get_sync_handler(self->channel_manager, profile_sync_services[0]);
        if (sync)
            (*sync)(profile_sync_services, 1);
    }
    bool is_registered = false;
    if (kaa_is_endpoint_registered(self->status, &is_registered))
        return KAA_ERR_BAD_STATE;
    if (!is_registered && kaa_set_endpoint_registered(self->status, true))
        return KAA_ERR_BAD_STATE;
    return KAA_ERR_NONE;
}

kaa_error_t kaa_profile_update_profile(kaa_profile_manager_t *self, kaa_profile_t * profile_body) {
    KAA_RETURN_IF_NIL2(self, profile_body, KAA_ERR_BADPARAM);

    size_t serialized_profile_size = profile_body->get_size(profile_body);
    char* serialized_profile = (char *) KAA_MALLOC(serialized_profile_size * sizeof(char));
    if (!serialized_profile)
        return KAA_ERR_NOMEM;

    avro_writer_t writer = avro_writer_memory(serialized_profile, serialized_profile_size);
    if (!writer) {
        KAA_FREE(serialized_profile);
        return KAA_ERR_NOMEM;
    }
    profile_body->serialize(writer, profile_body);
    avro_writer_free(writer);

    kaa_digest new_hash;
    kaa_calculate_sha_hash(serialized_profile, serialized_profile_size, new_hash);

    kaa_digest_p old_hash = NULL;
    if (kaa_status_get_profile_hash(self->status, &old_hash)) {
        KAA_FREE(serialized_profile);
        return KAA_ERR_BAD_STATE;
    }

    if (old_hash && !memcmp(new_hash, old_hash, SHA_1_DIGEST_LENGTH)) {
        self->need_resync = false;
        KAA_FREE(serialized_profile);
        return KAA_ERR_NONE;
    }

    if (kaa_status_set_profile_hash(self->status, new_hash)) {
        KAA_FREE(serialized_profile);
        return KAA_ERR_BAD_STATE;
    }

    if (self->profile_body.size > 0) {
        KAA_FREE(self->profile_body.buffer);
        self->profile_body.buffer = NULL;
    }

    self->profile_body.buffer = (uint8_t*)serialized_profile;
    self->profile_body.size = serialized_profile_size;

    self->need_resync = true;

    kaa_sync_handler_fn sync = kaa_channel_manager_get_sync_handler(self->channel_manager, profile_sync_services[0]);
    if (sync)
        (*sync)(profile_sync_services, 1);

    return KAA_ERR_NONE;
}
