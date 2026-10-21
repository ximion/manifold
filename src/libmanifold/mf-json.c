/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-json-private.h"

#include "mf-utils.h"

/**
 * mf_json_set_field_error:
 *
 * Set an error about a field of a JSON document: "<context>: <key> <what>",
 * or "<key> <what>" if there is no context.
 */
static void
mf_json_set_field_error(GError **error, MfError code, const char *context, const char *key, const char *what)
{
    g_set_error(error,
                MF_ERROR,
                code,
                "%s%s%s %s",
                context != NULL ? context : "",
                context != NULL ? ": " : "",
                key,
                what);
}

/**
 * mf_json_parse_object:
 * @data: a JSON document, NUL terminated
 * @context: (nullable): what the document is, for the message
 *
 * Returns: the document, which must be an object, or %NULL
 */
sd_json_variant *
mf_json_parse_object(const char *data, const char *context, MfError code, GError **error)
{
    sd_json_variant *variant = NULL;
    unsigned int line = 0;
    unsigned int column = 0;

    int r = sd_json_parse(data, 0, &variant, &line, &column);
    if (r < 0) {
        g_set_error(error,
                    MF_ERROR,
                    code,
                    "%s%snot valid JSON (line %u, column %u): %s",
                    context != NULL ? context : "",
                    context != NULL ? " is " : "",
                    line,
                    column,
                    g_strerror(-r));
        return NULL;
    }
    if (!sd_json_variant_is_object(variant)) {
        g_set_error(error,
                    MF_ERROR,
                    code,
                    "%s%snot a JSON object",
                    context != NULL ? context : "",
                    context != NULL ? " is " : "");
        sd_json_variant_unref(variant);
        return NULL;
    }

    return variant;
}

/**
 * mf_json_get_string_field:
 *
 * Returns: the string at @key, which has to be there, or %NULL
 */
const char *
mf_json_get_string_field(sd_json_variant *object,
                         const char *context,
                         const char *key,
                         MfError code,
                         GError **error)
{
    sd_json_variant *value = sd_json_variant_by_key(object, key);

    if (value == NULL || sd_json_variant_is_null(value)) {
        mf_json_set_field_error(error, code, context, key, "is missing");
        return NULL;
    }
    if (!sd_json_variant_is_string(value)) {
        mf_json_set_field_error(error, code, context, key, "is not a string");
        return NULL;
    }

    return sd_json_variant_string(value);
}

/**
 * mf_json_get_unsigned_field:
 *
 * Returns: %TRUE if @key holds a non-negative integer, which is stored in @ret
 */
gboolean
mf_json_get_unsigned_field(sd_json_variant *object,
                           const char *context,
                           const char *key,
                           MfError code,
                           guint64 *ret,
                           GError **error)
{
    sd_json_variant *value = sd_json_variant_by_key(object, key);

    if (value == NULL || !sd_json_variant_is_unsigned(value)) {
        mf_json_set_field_error(error, code, context, key, "is missing or not a non-negative integer");
        return FALSE;
    }

    *ret = sd_json_variant_unsigned(value);
    return TRUE;
}

/**
 * mf_json_get_boolean_field:
 *
 * Returns: %TRUE if @key holds a boolean, which is stored in @ret
 */
gboolean
mf_json_get_boolean_field(sd_json_variant *object,
                          const char *context,
                          const char *key,
                          MfError code,
                          gboolean *ret,
                          GError **error)
{
    sd_json_variant *value = sd_json_variant_by_key(object, key);

    if (value == NULL || !sd_json_variant_is_boolean(value)) {
        mf_json_set_field_error(error, code, context, key, "is missing or not a boolean");
        return FALSE;
    }

    *ret = sd_json_variant_boolean(value);
    return TRUE;
}

/**
 * mf_json_get_hash_field:
 *
 * Returns: the hex encoded hash at @key, checked to be one, or %NULL
 */
const char *
mf_json_get_hash_field(sd_json_variant *object,
                       const char *context,
                       const char *key,
                       MfError code,
                       GError **error)
{
    const char *hash = mf_json_get_string_field(object, context, key, code, error);

    if (hash == NULL)
        return NULL;
    if (!mf_hex_hash_is_valid(hash)) {
        mf_json_set_field_error(error, code, context, key, "is not a valid hash");
        return NULL;
    }

    return hash;
}

/**
 * mf_json_parse_file:
 * @path: a file holding a JSON object
 *
 * Returns: the object, or %NULL
 */
sd_json_variant *
mf_json_parse_file(const char *path, GError **error)
{
    g_autofree char *data = NULL;

    if (!g_file_get_contents(path, &data, NULL, error))
        return NULL;
    return mf_json_parse_object(data, path, MF_ERROR_FAILED, error);
}

/**
 * mf_json_get_string:
 *
 * Returns: the string at @key of @object, or %NULL if there is none
 */
const char *
mf_json_get_string(sd_json_variant *object, const char *key)
{
    sd_json_variant *value = sd_json_variant_by_key(object, key);

    return sd_json_variant_is_string(value) ? sd_json_variant_string(value) : NULL;
}

/**
 * mf_json_write_file:
 * @path: the file to write
 * @json: what to write into it
 *
 * Write JSON to a file, formatted for humans to read.
 *
 * Returns: %TRUE on success
 */
gboolean
mf_json_write_file(const char *path, sd_json_variant *json, GError **error)
{
    g_autofree char *formatted = NULL;
    int r;

    r = sd_json_variant_format(json, SD_JSON_FORMAT_PRETTY | SD_JSON_FORMAT_NEWLINE, &formatted);
    if (r < 0) {
        g_set_error(error, MF_ERROR, MF_ERROR_FAILED, "Unable to format %s: %s", path, g_strerror(-r));
        return FALSE;
    }
    return g_file_set_contents(path, formatted, -1, error);
}
