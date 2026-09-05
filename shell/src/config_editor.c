#include "config_editor.h"

#include "shell_config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>
#include <kdl/common.h>
#include <kdl/tokenizer.h>

typedef struct edit_token {
    kdl_token_type type;
    size_t start;
    size_t end;
    char *text;
} EditToken;

typedef struct edit_span {
    size_t start;
    size_t end;
} EditSpan;

typedef struct parsed_edit_value {
    bool boolean;
    int integer;
    const char *string;
} ParsedEditValue;

static bool token_is_trivia(kdl_token_type type) {
    return
        type == KDL_TOKEN_WHITESPACE ||
        type == KDL_TOKEN_SINGLE_LINE_COMMENT ||
        type == KDL_TOKEN_MULTI_LINE_COMMENT ||
        type == KDL_TOKEN_LINE_CONTINUATION;
}

static bool token_is_scalar(kdl_token_type type) {
    return
        type == KDL_TOKEN_WORD ||
        type == KDL_TOKEN_STRING ||
        type == KDL_TOKEN_MULTILINE_STRING ||
        type == KDL_TOKEN_RAW_STRING_V1 ||
        type == KDL_TOKEN_RAW_STRING_V2 ||
        type == KDL_TOKEN_RAW_MULTILINE_STRING;
}

static bool token_is_node_end(kdl_token_type type) {
    return
        type == KDL_TOKEN_NEWLINE ||
        type == KDL_TOKEN_SEMICOLON ||
        type == KDL_TOKEN_START_CHILDREN ||
        type == KDL_TOKEN_END_CHILDREN;
}

static size_t raw_string_end(
        const char *contents,
        size_t length,
        size_t start,
        const kdl_token *token) {
    if (start >= length) {
        return 0;
    }

    size_t cursor = start;
    size_t hashes = 0;
    size_t quotes = 1;

    if (token->type == KDL_TOKEN_RAW_STRING_V1) {
        if (contents[cursor] != 'r') {
            return 0;
        }
        cursor++;
        while (cursor < length && contents[cursor] == '#') {
            hashes++;
            cursor++;
        }
    } else if (token->type == KDL_TOKEN_RAW_STRING_V2 ||
            token->type == KDL_TOKEN_RAW_MULTILINE_STRING) {
        while (cursor < length && contents[cursor] == '#') {
            hashes++;
            cursor++;
        }
    }

    if (token->type == KDL_TOKEN_MULTILINE_STRING ||
            token->type == KDL_TOKEN_RAW_MULTILINE_STRING) {
        quotes = 3;
    }

    if (cursor + quotes > length) {
        return 0;
    }

    for (size_t i = 0; i < quotes; i++) {
        if (contents[cursor + i] != '"') {
            return 0;
        }
    }

    const size_t content_start = cursor + quotes;

    if (token->value.len > length - content_start) {
        return 0;
    }

    size_t end = content_start + token->value.len;

    if (end + quotes + hashes > length) {
        return 0;
    }

    for (size_t i = 0; i < quotes; i++) {
        if (contents[end + i] != '"') {
            return 0;
        }
    }
    end += quotes;

    for (size_t i = 0; i < hashes; i++) {
        if (contents[end + i] != '#') {
            return 0;
        }
    }

    return end + hashes;
}

static char *token_semantic_text(
        const EditToken *token,
        const char *contents) {
    if (!token ||
            !contents ||
            token->end < token->start) {
        return NULL;
    }

    const kdl_str raw = {
        .data = contents + token->start,
        .len = token->end - token->start,
    };

    if (token->type == KDL_TOKEN_WORD) {
        return g_strndup(raw.data, raw.len);
    }

    if (token->type == KDL_TOKEN_STRING ||
            token->type == KDL_TOKEN_MULTILINE_STRING ||
            token->type == KDL_TOKEN_RAW_STRING_V1 ||
            token->type == KDL_TOKEN_RAW_STRING_V2 ||
            token->type == KDL_TOKEN_RAW_MULTILINE_STRING) {
        kdl_tokenizer *tokenizer =
            kdl_create_string_tokenizer(raw);

        if (!tokenizer) {
            return NULL;
        }

        kdl_token parsed = {0};
        const kdl_tokenizer_status status =
            kdl_pop_token(tokenizer, &parsed);

        if (status != KDL_TOKENIZER_OK) {
            kdl_destroy_tokenizer(tokenizer);
            return NULL;
        }

        kdl_owned_string decoded = {0};

        if (parsed.type == KDL_TOKEN_STRING) {
            decoded = kdl_unescape_v(
                KDL_VERSION_2,
                &parsed.value
            );
        } else if (parsed.type == KDL_TOKEN_MULTILINE_STRING) {
            decoded = kdl_unescape_multi_line(
                KDL_VERSION_2,
                &parsed.value
            );
        } else {
            decoded = kdl_clone_str(
                &parsed.value
            );
        }

        kdl_destroy_tokenizer(tokenizer);

        if (!decoded.data) {
            return NULL;
        }

        char *text = g_strndup(
            decoded.data,
            decoded.len
        );

        kdl_free_string(&decoded);
        return text;
    }

    return NULL;
}

static void edit_token_finish(EditToken *token) {
    if (!token) {
        return;
    }

    g_free(token->text);
    *token = (EditToken){0};
}

static bool tokenize_document(
        const char *contents,
        size_t length,
        GArray **tokens_out,
        GError **error) {
    const kdl_str document = {
        .data = contents,
        .len = length,
    };

    kdl_tokenizer *tokenizer =
        kdl_create_string_tokenizer(document);

    if (!tokenizer) {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_FAILED,
            "cannot create KDL tokenizer"
        );
        return false;
    }

    kdl_tokenizer_set_character_set(
        tokenizer,
        KDL_CHARACTER_SET_V2
    );

    GArray *tokens = g_array_new(
        false,
        true,
        sizeof(EditToken)
    );

    if (!tokens) {
        kdl_destroy_tokenizer(tokenizer);
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_NOMEM,
            "cannot allocate KDL token array"
        );
        return false;
    }

    size_t cursor = 0;
    bool ok = true;

    for (;;) {
        kdl_token token = {0};
        const kdl_tokenizer_status status =
            kdl_pop_token(
                tokenizer,
                &token
            );

        if (status == KDL_TOKENIZER_EOF) {
            break;
        }

        if (status != KDL_TOKENIZER_OK) {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "cannot tokenize KDL 2 document"
            );
            ok = false;
            break;
        }

        if (cursor > length) {
            ok = false;
            break;
        }

        size_t raw_end = 0;

        if (token.type == KDL_TOKEN_STRING ||
                token.type == KDL_TOKEN_MULTILINE_STRING ||
                token.type == KDL_TOKEN_RAW_STRING_V1 ||
                token.type == KDL_TOKEN_RAW_STRING_V2 ||
                token.type == KDL_TOKEN_RAW_MULTILINE_STRING) {
            raw_end = raw_string_end(
                contents,
                length,
                cursor,
                &token
            );
        } else if (token.type == KDL_TOKEN_SEMICOLON) {
            if (contents[cursor] != ';') {
                ok = false;
                break;
            }

            raw_end = cursor + 1;
        } else {
            if (!token.value.data ||
                    token.value.data < contents ||
                    token.value.data > contents + length) {
                ok = false;
                break;
            }

            const size_t token_start =
                (size_t)(token.value.data - contents);

            if (token_start != cursor ||
                    token.value.len > length - cursor) {
                ok = false;
                break;
            }

            raw_end = cursor + token.value.len;
        }

        if (raw_end <= cursor ||
                raw_end > length) {
            ok = false;
            break;
        }

        EditToken edit_token = {
            .type = token.type,
            .start = cursor,
            .end = raw_end,
        };

        if (token_is_scalar(token.type)) {
            edit_token.text =
                token_semantic_text(
                    &edit_token,
                    contents
                );

            if (!edit_token.text) {
                ok = false;
                break;
            }
        }

        g_array_append_val(
            tokens,
            edit_token
        );

        cursor = raw_end;
    }

    kdl_destroy_tokenizer(tokenizer);

    if (ok && cursor != length) {
        ok = false;
    }

    if (!ok) {
        for (guint i = 0; i < tokens->len; i++) {
            EditToken *token = &g_array_index(
                tokens,
                EditToken,
                i
            );
            edit_token_finish(token);
        }
        g_array_unref(tokens);

        if (!error || !*error) {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "KDL token span accounting failed"
            );
        }
        return false;
    }

    *tokens_out = tokens;
    return true;
}

static void token_array_finish(GArray *tokens) {
    if (!tokens) {
        return;
    }

    for (guint i = 0; i < tokens->len; i++) {
        EditToken *token = &g_array_index(
            tokens,
            EditToken,
            i
        );
        edit_token_finish(token);
    }

    g_array_unref(tokens);
}

static size_t next_nontrivia(
        const GArray *tokens,
        size_t index,
        size_t limit) {
    while (index < limit) {
        const EditToken *token = &g_array_index(
            tokens,
            EditToken,
            index
        );

        if (!token_is_trivia(token->type) &&
                token->type != KDL_TOKEN_SLASHDASH) {
            break;
        }

        index++;
    }

    return index;
}

static bool token_text_is(
        const GArray *tokens,
        size_t index,
        const char *value) {
    if (index >= tokens->len ||
            !value) {
        return false;
    }

    const EditToken *token = &g_array_index(
        tokens,
        EditToken,
        index
    );

    return token->text &&
        strcmp(token->text, value) == 0;
}

static bool find_first_argument_span(
        const GArray *tokens,
        size_t first,
        size_t limit,
        EditSpan *span) {
    size_t index = first;

    while (index < limit) {
        index = next_nontrivia(
            tokens,
            index,
            limit
        );

        if (index >= limit) {
            break;
        }

        const EditToken *token = &g_array_index(
            tokens,
            EditToken,
            index
        );

        if (token->type == KDL_TOKEN_START_TYPE) {
            index++;
            while (index < limit) {
                const EditToken *inner = &g_array_index(
                    tokens,
                    EditToken,
                    index++
                );
                if (inner->type == KDL_TOKEN_END_TYPE) {
                    break;
                }
            }
            continue;
        }

        if (!token_is_scalar(token->type)) {
            index++;
            continue;
        }

        const size_t after = next_nontrivia(
            tokens,
            index + 1,
            limit
        );

        if (after < limit) {
            const EditToken *next = &g_array_index(
                tokens,
                EditToken,
                after
            );

            if (next->type == KDL_TOKEN_EQUALS) {
                index = after + 1;
                index = next_nontrivia(
                    tokens,
                    index,
                    limit
                );
                if (index < limit) {
                    index++;
                }
                continue;
            }
        }

        *span = (EditSpan){
            .start = token->start,
            .end = token->end,
        };
        return true;
    }

    return false;
}

static bool append_property_spans(
        const GArray *tokens,
        size_t first,
        size_t limit,
        const char *property,
        GArray *candidates) {
    size_t index = first;
    bool found = false;

    while (index < limit) {
        index = next_nontrivia(
            tokens,
            index,
            limit
        );

        if (index >= limit) {
            break;
        }

        if (!token_text_is(
                tokens,
                index,
                property)) {
            index++;
            continue;
        }

        const size_t equals = next_nontrivia(
            tokens,
            index + 1,
            limit
        );

        if (equals >= limit ||
                g_array_index(
                    tokens,
                    EditToken,
                    equals
                ).type != KDL_TOKEN_EQUALS) {
            index++;
            continue;
        }

        const size_t value_index = next_nontrivia(
            tokens,
            equals + 1,
            limit
        );

        if (value_index >= limit) {
            return found;
        }

        const EditToken *value = &g_array_index(
            tokens,
            EditToken,
            value_index
        );

        if (token_is_scalar(value->type)) {
            EditSpan span = {
                .start = value->start,
                .end = value->end,
            };

            g_array_append_val(
                candidates,
                span
            );
            found = true;
        }

        index = value_index + 1;
    }

    return found;
}

static bool field_is_provider(
        ShellConfigEditField field) {
    return
        field == SHELL_CONFIG_EDIT_PROVIDER_PRIORITY ||
        field == SHELL_CONFIG_EDIT_PROVIDER_AUTOSTART ||
        field == SHELL_CONFIG_EDIT_PROVIDER_REQUIRED;
}

static const char *provider_field_name(
        ShellConfigEditField field) {
    switch (field) {
    case SHELL_CONFIG_EDIT_PROVIDER_PRIORITY:
        return "priority";
    case SHELL_CONFIG_EDIT_PROVIDER_AUTOSTART:
        return "autostart";
    case SHELL_CONFIG_EDIT_PROVIDER_REQUIRED:
        return "required";
    case SHELL_CONFIG_EDIT_APPEARANCE_WALLPAPER:
    case SHELL_CONFIG_EDIT_FALLBACK_ENABLED:
    case SHELL_CONFIG_EDIT_FALLBACK_TITLE:
        return NULL;
    }

    return NULL;
}

static bool append_candidate(
        GArray *candidates,
        EditSpan span) {
    if (span.end <= span.start) {
        return false;
    }

    g_array_append_val(
        candidates,
        span
    );
    return true;
}

static bool find_candidate_spans(
        const GArray *tokens,
        const ShellConfigEditRequest *request,
        GArray **candidates_out,
        GError **error) {
    GArray *candidates = g_array_new(
        false,
        true,
        sizeof(EditSpan)
    );

    if (!candidates) {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_NOMEM,
            "cannot allocate edit span list"
        );
        return false;
    }

    size_t depth = 0;
    char *provider_context = NULL;
    size_t provider_depth = SIZE_MAX;
    bool appearance_context = false;
    size_t appearance_depth = SIZE_MAX;
    bool fallback_context = false;
    size_t fallback_depth = SIZE_MAX;

    size_t index = 0;

    while (index < tokens->len) {
        const EditToken *token = &g_array_index(
            tokens,
            EditToken,
            index
        );

        if (token->type == KDL_TOKEN_END_CHILDREN) {
            if (depth > 0) {
                if (provider_depth == depth) {
                    g_clear_pointer(
                        &provider_context,
                        g_free
                    );
                    provider_depth = SIZE_MAX;
                }
                if (appearance_depth == depth) {
                    appearance_context = false;
                    appearance_depth = SIZE_MAX;
                }
                if (fallback_depth == depth) {
                    fallback_context = false;
                    fallback_depth = SIZE_MAX;
                }
                depth--;
            }
            index++;
            continue;
        }

        if (token_is_trivia(token->type) ||
                token->type == KDL_TOKEN_NEWLINE ||
                token->type == KDL_TOKEN_SEMICOLON ||
                token->type == KDL_TOKEN_SLASHDASH) {
            index++;
            continue;
        }

        if (token->type == KDL_TOKEN_START_CHILDREN) {
            depth++;
            index++;
            continue;
        }

        if (!token_is_scalar(token->type)) {
            index++;
            continue;
        }

        const size_t node_name = index;
        size_t header_end = node_name + 1;
        size_t children_token = SIZE_MAX;

        while (header_end < tokens->len) {
            const EditToken *header_token = &g_array_index(
                tokens,
                EditToken,
                header_end
            );

            if (token_is_node_end(header_token->type)) {
                if (header_token->type == KDL_TOKEN_START_CHILDREN) {
                    children_token = header_end;
                }
                break;
            }

            header_end++;
        }

        if (depth == 0 &&
                token_text_is(
                    tokens,
                    node_name,
                    "appearance")) {
            appearance_context =
                children_token != SIZE_MAX;
            appearance_depth =
                appearance_context
                    ? depth + 1
                    : SIZE_MAX;
        } else if (depth == 0 &&
                token_text_is(
                    tokens,
                    node_name,
                    "provider")) {
            EditSpan provider_id_span = {0};

            if (find_first_argument_span(
                    tokens,
                    node_name + 1,
                    header_end,
                    &provider_id_span)) {
                for (size_t j = node_name + 1;
                        j < header_end;
                        j++) {
                    const EditToken *candidate = &g_array_index(
                        tokens,
                        EditToken,
                        j
                    );

                    if (candidate->start == provider_id_span.start) {
                        g_free(provider_context);
                        provider_context = candidate->text
                            ? g_strdup(candidate->text)
                            : NULL;
                        break;
                    }
                }
            }

            if (children_token != SIZE_MAX &&
                    provider_context) {
                provider_depth = depth + 1;
            } else {
                g_clear_pointer(
                    &provider_context,
                    g_free
                );
                provider_depth = SIZE_MAX;
            }
        } else if (depth == 0 &&
                token_text_is(
                    tokens,
                    node_name,
                    "fallback")) {
            if (request->field == SHELL_CONFIG_EDIT_FALLBACK_ENABLED) {
                (void)append_property_spans(
                    tokens,
                    node_name + 1,
                    header_end,
                    "enabled",
                    candidates
                );
            }

            fallback_context =
                children_token != SIZE_MAX;
            fallback_depth =
                fallback_context
                    ? depth + 1
                    : SIZE_MAX;
        } else if (field_is_provider(request->field) &&
                provider_context &&
                provider_depth == depth &&
                request->provider_id &&
                strcmp(
                    provider_context,
                    request->provider_id) == 0 &&
                token_text_is(
                    tokens,
                    node_name,
                    provider_field_name(request->field))) {
            EditSpan span = {0};
            if (find_first_argument_span(
                    tokens,
                    node_name + 1,
                    header_end,
                    &span)) {
                append_candidate(
                    candidates,
                    span
                );
            }
        } else if (request->field == SHELL_CONFIG_EDIT_APPEARANCE_WALLPAPER &&
                appearance_context &&
                appearance_depth == depth &&
                token_text_is(
                    tokens,
                    node_name,
                    "wallpaper")) {
            EditSpan span = {0};
            if (find_first_argument_span(
                    tokens,
                    node_name + 1,
                    header_end,
                    &span)) {
                append_candidate(
                    candidates,
                    span
                );
            }
        } else if (request->field == SHELL_CONFIG_EDIT_FALLBACK_TITLE &&
                fallback_context &&
                fallback_depth == depth &&
                token_text_is(
                    tokens,
                    node_name,
                    "title")) {
            EditSpan span = {0};
            if (find_first_argument_span(
                    tokens,
                    node_name + 1,
                    header_end,
                    &span)) {
                append_candidate(
                    candidates,
                    span
                );
            }
        }

        if (children_token != SIZE_MAX) {
            depth++;
            index = children_token + 1;
        } else {
            index = header_end;
            if (index < tokens->len &&
                    g_array_index(
                        tokens,
                        EditToken,
                        index
                    ).type != KDL_TOKEN_END_CHILDREN) {
                index++;
            }
        }
    }

    g_free(provider_context);

    if (candidates->len == 0) {
        g_array_unref(candidates);
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_NOENT,
            "requested config value has no editable token span"
        );
        return false;
    }

    *candidates_out = candidates;
    return true;
}

static const ProviderConfig *find_provider(
        const ShellConfig *config,
        const char *provider_id) {
    if (!config ||
            !provider_id) {
        return NULL;
    }

    for (size_t i = 0;
            i < config->provider_count;
            i++) {
        if (config->providers[i].id &&
                strcmp(
                    config->providers[i].id,
                    provider_id) == 0) {
            return &config->providers[i];
        }
    }

    return NULL;
}

static bool parse_boolean(
        const char *value,
        bool *parsed) {
    if (strcmp(value, "true") == 0) {
        *parsed = true;
        return true;
    }

    if (strcmp(value, "false") == 0) {
        *parsed = false;
        return true;
    }

    return false;
}

static bool parse_integer(
        const char *value,
        int *parsed) {
    if (!value ||
            value[0] == '\0') {
        return false;
    }

    errno = 0;
    char *end = NULL;
    const long parsed_long =
        strtol(
            value,
            &end,
            10
        );

    if (errno != 0 ||
            !end ||
            *end != '\0' ||
            parsed_long < INT_MIN ||
            parsed_long > INT_MAX) {
        return false;
    }

    *parsed = (int)parsed_long;
    return true;
}

static bool parse_edit_value(
        const ShellConfigEditRequest *request,
        ParsedEditValue *parsed,
        GError **error) {
    if (!request ||
            !request->value ||
            request->value[0] == '\0') {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "persist value must not be empty"
        );
        return false;
    }

    switch (request->field) {
    case SHELL_CONFIG_EDIT_FALLBACK_ENABLED:
    case SHELL_CONFIG_EDIT_PROVIDER_AUTOSTART:
    case SHELL_CONFIG_EDIT_PROVIDER_REQUIRED:
        if (!parse_boolean(
                request->value,
                &parsed->boolean)) {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "persist boolean must be true or false"
            );
            return false;
        }
        return true;

    case SHELL_CONFIG_EDIT_PROVIDER_PRIORITY:
        if (!parse_integer(
                request->value,
                &parsed->integer)) {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "provider priority must be an integer in int range"
            );
            return false;
        }
        return true;

    case SHELL_CONFIG_EDIT_APPEARANCE_WALLPAPER:
    case SHELL_CONFIG_EDIT_FALLBACK_TITLE:
        parsed->string = request->value;
        return true;
    }

    g_set_error_literal(
        error,
        G_FILE_ERROR,
        G_FILE_ERROR_INVAL,
        "unknown persist field"
    );
    return false;
}

static bool semantic_matches(
        const ShellConfig *config,
        const ShellConfigEditRequest *request,
        const ParsedEditValue *value) {
    if (!config ||
            !request ||
            !value) {
        return false;
    }

    switch (request->field) {
    case SHELL_CONFIG_EDIT_APPEARANCE_WALLPAPER:
        return
            config->appearance.wallpaper &&
            strcmp(
                config->appearance.wallpaper,
                value->string
            ) == 0;

    case SHELL_CONFIG_EDIT_FALLBACK_ENABLED:
        return
            config->fallback.enabled ==
                value->boolean;

    case SHELL_CONFIG_EDIT_FALLBACK_TITLE:
        return
            config->fallback.title &&
            strcmp(
                config->fallback.title,
                value->string
            ) == 0;

    case SHELL_CONFIG_EDIT_PROVIDER_PRIORITY:
    case SHELL_CONFIG_EDIT_PROVIDER_AUTOSTART:
    case SHELL_CONFIG_EDIT_PROVIDER_REQUIRED: {
        const ProviderConfig *provider =
            find_provider(
                config,
                request->provider_id
            );

        if (!provider) {
            return false;
        }

        if (request->field == SHELL_CONFIG_EDIT_PROVIDER_PRIORITY) {
            return provider->priority == value->integer;
        }

        if (request->field == SHELL_CONFIG_EDIT_PROVIDER_AUTOSTART) {
            return provider->autostart == value->boolean;
        }

        return provider->required == value->boolean;
    }
    }

    return false;
}

static char *semantic_value_string(
        const ShellConfig *config,
        const ShellConfigEditRequest *request,
        GError **error) {
    if (!config ||
            !request) {
        return NULL;
    }

    switch (request->field) {
    case SHELL_CONFIG_EDIT_APPEARANCE_WALLPAPER:
        if (!config->appearance.wallpaper) {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_NOENT,
                "appearance wallpaper is not present"
            );
            return NULL;
        }
        return g_strdup(
            config->appearance.wallpaper
        );

    case SHELL_CONFIG_EDIT_FALLBACK_ENABLED:
        return g_strdup(
            config->fallback.enabled
                ? "true"
                : "false"
        );

    case SHELL_CONFIG_EDIT_FALLBACK_TITLE:
        if (!config->fallback.title) {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_NOENT,
                "fallback title is not present"
            );
            return NULL;
        }
        return g_strdup(
            config->fallback.title
        );

    case SHELL_CONFIG_EDIT_PROVIDER_PRIORITY:
    case SHELL_CONFIG_EDIT_PROVIDER_AUTOSTART:
    case SHELL_CONFIG_EDIT_PROVIDER_REQUIRED: {
        const ProviderConfig *provider =
            find_provider(
                config,
                request->provider_id
            );

        if (!provider) {
            g_set_error(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_NOENT,
                "provider '%s' not found",
                request->provider_id
                    ? request->provider_id
                    : ""
            );
            return NULL;
        }

        if (request->field == SHELL_CONFIG_EDIT_PROVIDER_PRIORITY) {
            return g_strdup_printf(
                "%d",
                provider->priority
            );
        }

        const bool boolean =
            request->field == SHELL_CONFIG_EDIT_PROVIDER_AUTOSTART
                ? provider->autostart
                : provider->required;

        return g_strdup(
            boolean
                ? "true"
                : "false"
        );
    }
    }

    return NULL;
}

static char *replacement_text(
        const ShellConfigEditRequest *request,
        const ParsedEditValue *value,
        GError **error) {
    switch (request->field) {
    case SHELL_CONFIG_EDIT_FALLBACK_ENABLED:
    case SHELL_CONFIG_EDIT_PROVIDER_AUTOSTART:
    case SHELL_CONFIG_EDIT_PROVIDER_REQUIRED:
        return g_strdup(
            value->boolean
                ? "#true"
                : "#false"
        );

    case SHELL_CONFIG_EDIT_PROVIDER_PRIORITY:
        return g_strdup_printf(
            "%d",
            value->integer
        );

    case SHELL_CONFIG_EDIT_APPEARANCE_WALLPAPER:
    case SHELL_CONFIG_EDIT_FALLBACK_TITLE: {
        const kdl_str source =
            kdl_str_from_cstr(
                value->string
            );

        kdl_owned_string escaped =
            kdl_escape_v(
                KDL_VERSION_2,
                &source,
                KDL_ESCAPE_DEFAULT
            );

        if (!escaped.data) {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_NOMEM,
                "cannot escape fallback title as KDL string"
            );
            return NULL;
        }

        char *replacement =
            g_strdup_printf(
                "\"%s\"",
                escaped.data
            );

        kdl_free_string(&escaped);
        return replacement;
    }
    }

    return NULL;
}

static char *replace_span(
        const char *contents,
        size_t length,
        EditSpan span,
        const char *replacement,
        size_t *candidate_length) {
    if (!contents ||
            !replacement ||
            span.start > span.end ||
            span.end > length) {
        return NULL;
    }

    const size_t replacement_length =
        strlen(replacement);

    if (span.start > SIZE_MAX - replacement_length ||
            span.start + replacement_length > SIZE_MAX - (length - span.end)) {
        return NULL;
    }

    const size_t total =
        span.start +
        replacement_length +
        (length - span.end);

    char *candidate = g_malloc(
        total + 1
    );

    if (!candidate) {
        return NULL;
    }

    memcpy(
        candidate,
        contents,
        span.start
    );
    memcpy(
        candidate + span.start,
        replacement,
        replacement_length
    );
    memcpy(
        candidate + span.start + replacement_length,
        contents + span.end,
        length - span.end
    );

    candidate[total] = '\0';
    *candidate_length = total;
    return candidate;
}

static bool write_all_fd(
        int fd,
        const char *data,
        size_t length) {
    size_t offset = 0;

    while (offset < length) {
        const ssize_t written = write(
            fd,
            data + offset,
            length - offset
        );

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        offset += (size_t)written;
    }

    return true;
}

static bool write_candidate_file(
        int fd,
        const char *contents,
        size_t length,
        GError **error) {
    if (ftruncate(fd, 0) < 0 ||
            lseek(fd, 0, SEEK_SET) < 0 ||
            !write_all_fd(
                fd,
                contents,
                length) ||
            fsync(fd) < 0) {
        const int saved_errno = errno;
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(saved_errno),
            "cannot write config edit candidate: %s",
            strerror(saved_errno)
        );
        return false;
    }

    return true;
}

static bool file_contents_equal(
        const char *path,
        const char *expected,
        size_t expected_length,
        GError **error) {
    g_autofree char *current = NULL;
    gsize current_length = 0;

    if (!g_file_get_contents(
            path,
            &current,
            &current_length,
            error)) {
        return false;
    }

    return
        current_length == expected_length &&
        memcmp(
            current,
            expected,
            expected_length
        ) == 0;
}

void shell_config_edit_result_finish(
        ShellConfigEditResult *result) {
    if (!result) {
        return;
    }

    g_free(result->path);
    g_free(result->old_value);
    g_free(result->new_value);
    *result = (ShellConfigEditResult){0};
}

bool shell_config_edit_persist(
        const char *path,
        const ShellConfigEditRequest *request,
        ShellConfigEditResult *result,
        GError **error) {
    if (!path ||
            path[0] == '\0' ||
            !request ||
            !result) {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "invalid config edit request"
        );
        return false;
    }

    *result = (ShellConfigEditResult){0};

    if (field_is_provider(request->field) &&
            (!request->provider_id ||
             request->provider_id[0] == '\0')) {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "provider id is required for this persist field"
        );
        return false;
    }

    ParsedEditValue parsed = {0};
    if (!parse_edit_value(
            request,
            &parsed,
            error)) {
        return false;
    }

    struct stat path_stat = {0};
    if (lstat(path, &path_stat) < 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(errno),
            "cannot inspect config '%s': %s",
            path,
            strerror(errno)
        );
        return false;
    }

    if (!S_ISREG(path_stat.st_mode)) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "config '%s' is not a regular file",
            path
        );
        return false;
    }

    int lock_fd = open(
        path,
        O_RDONLY |
            O_CLOEXEC |
            O_NOFOLLOW
    );

    if (lock_fd < 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(errno),
            "cannot open config '%s': %s",
            path,
            strerror(errno)
        );
        return false;
    }

    bool ok = false;
    g_autofree char *contents = NULL;
    gsize length = 0;
    g_autofree char *directory = NULL;
    g_autofree char *template = NULL;
    g_autofree char *replacement = NULL;
    g_autofree char *selected = NULL;
    g_autofree char *old_value = NULL;
    size_t selected_length = 0;
    int temp_fd = -1;
    GArray *tokens = NULL;
    GArray *candidates = NULL;

    if (flock(lock_fd, LOCK_EX) < 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(errno),
            "cannot lock config '%s': %s",
            path,
            strerror(errno)
        );
        goto out;
    }

    if (!g_file_get_contents(
            path,
            &contents,
            &length,
            error)) {
        goto out;
    }

    ShellConfig original;
    shell_config_init(&original);
    GError *original_error = NULL;

    if (!shell_config_load(
            &original,
            path,
            false,
            &original_error)) {
        g_propagate_prefixed_error(
            error,
            original_error,
            "current config is invalid; refusing persist edit: "
        );
        shell_config_finish(&original);
        goto out;
    }

    old_value = semantic_value_string(
        &original,
        request,
        error
    );

    if (!old_value) {
        shell_config_finish(&original);
        goto out;
    }

    if (semantic_matches(
            &original,
            request,
            &parsed)) {
        result->changed = false;
        result->path = g_strdup(path);
        result->old_value = g_strdup(old_value);
        result->new_value = g_strdup(old_value);
        shell_config_finish(&original);
        ok =
            result->path &&
            result->old_value &&
            result->new_value;
        goto out;
    }

    shell_config_finish(&original);

    if (!tokenize_document(
            contents,
            length,
            &tokens,
            error) ||
            !find_candidate_spans(
                tokens,
                request,
                &candidates,
                error)) {
        goto out;
    }

    replacement = replacement_text(
        request,
        &parsed,
        error
    );

    if (!replacement) {
        goto out;
    }

    directory = g_path_get_dirname(path);
    template = g_build_filename(
        directory,
        ".shell.kdl.edit.XXXXXX",
        NULL
    );

    if (!directory ||
            !template) {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_NOMEM,
            "cannot allocate config edit temp path"
        );
        goto out;
    }

    temp_fd = g_mkstemp_full(
        template,
        O_RDWR | O_CLOEXEC,
        path_stat.st_mode & 0777
    );

    if (temp_fd < 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(errno),
            "cannot create config edit temp file: %s",
            strerror(errno)
        );
        goto out;
    }

    if (fchmod(
            temp_fd,
            path_stat.st_mode & 0777) < 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(errno),
            "cannot preserve config mode on edit candidate: %s",
            strerror(errno)
        );
        goto out;
    }

    size_t matches = 0;

    for (guint i = 0;
            i < candidates->len;
            i++) {
        const EditSpan span = g_array_index(
            candidates,
            EditSpan,
            i
        );

        size_t candidate_length = 0;
        g_autofree char *candidate =
            replace_span(
                contents,
                length,
                span,
                replacement,
                &candidate_length
            );

        if (!candidate) {
            continue;
        }

        GError *write_error = NULL;
        if (!write_candidate_file(
                temp_fd,
                candidate,
                candidate_length,
                &write_error)) {
            g_propagate_error(
                error,
                write_error
            );
            goto out;
        }

        ShellConfig candidate_config;
        shell_config_init(&candidate_config);
        GError *candidate_error = NULL;

        const bool candidate_valid =
            shell_config_load(
                &candidate_config,
                template,
                false,
                &candidate_error
            );

        const bool candidate_matches =
            candidate_valid &&
            semantic_matches(
                &candidate_config,
                request,
                &parsed
            );

        shell_config_finish(
            &candidate_config
        );
        g_clear_error(
            &candidate_error
        );

        if (!candidate_matches) {
            continue;
        }

        matches++;

        if (matches == 1) {
            selected = g_steal_pointer(
                &candidate
            );
            selected_length =
                candidate_length;
        }
    }

    if (matches == 0) {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "no token span produced the requested validated semantic config"
        );
        goto out;
    }

    if (matches != 1) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "persist target is ambiguous: %zu validated spans",
            matches
        );
        goto out;
    }

    if (!write_candidate_file(
            temp_fd,
            selected,
            selected_length,
            error)) {
        goto out;
    }

    ShellConfig final_candidate;
    shell_config_init(&final_candidate);
    GError *final_error = NULL;

    if (!shell_config_load(
            &final_candidate,
            template,
            false,
            &final_error) ||
            !semantic_matches(
                &final_candidate,
                request,
                &parsed)) {
        if (final_error) {
            g_propagate_prefixed_error(
                error,
                final_error,
                "final config edit validation failed: "
            );
        } else {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "final config edit semantic validation failed"
            );
        }
        shell_config_finish(
            &final_candidate
        );
        goto out;
    }

    shell_config_finish(
        &final_candidate
    );

    GError *compare_error = NULL;
    if (!file_contents_equal(
            path,
            contents,
            length,
            &compare_error)) {
        if (compare_error) {
            g_propagate_prefixed_error(
                error,
                compare_error,
                "cannot recheck current config before publish: "
            );
        } else {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_AGAIN,
                "config changed concurrently; persist edit not published"
            );
        }
        goto out;
    }

    if (close(temp_fd) < 0) {
        const int saved_errno = errno;
        temp_fd = -1;
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(saved_errno),
            "cannot close validated config edit: %s",
            strerror(saved_errno)
        );
        goto out;
    }
    temp_fd = -1;

    if (rename(
            template,
            path) < 0) {
        const int saved_errno = errno;
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(saved_errno),
            "cannot atomically publish config edit '%s': %s",
            path,
            strerror(saved_errno)
        );
        goto out;
    }

    template[0] = '\0';

    const int directory_fd = open(
        directory,
        O_RDONLY |
            O_DIRECTORY |
            O_CLOEXEC
    );

    if (directory_fd < 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(errno),
            "config edit published but directory cannot be opened for fsync: %s",
            strerror(errno)
        );
        goto out;
    }

    if (fsync(directory_fd) < 0) {
        const int saved_errno = errno;
        close(directory_fd);
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(saved_errno),
            "config edit published but directory fsync failed: %s",
            strerror(saved_errno)
        );
        goto out;
    }

    close(directory_fd);

    result->changed = true;
    result->path = g_strdup(path);
    result->old_value = g_strdup(old_value);
    result->new_value = g_strdup(request->value);

    ok =
        result->path &&
        result->old_value &&
        result->new_value;

out:
    if (temp_fd >= 0) {
        close(temp_fd);
    }

    if (template &&
            template[0] != '\0') {
        (void)unlink(template);
    }

    if (candidates) {
        g_array_unref(candidates);
    }

    token_array_finish(tokens);

    if (lock_fd >= 0) {
        (void)flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }

    if (!ok) {
        shell_config_edit_result_finish(
            result
        );
    }

    return ok;
}
