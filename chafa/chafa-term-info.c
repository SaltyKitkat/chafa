/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/* Copyright (C) 2020 Hans Petter Jansson
 *
 * This file is part of Chafa, a program that turns images into character art.
 *
 * Chafa is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Chafa is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Chafa.  If not, see <http://www.gnu.org/licenses/>. */

#include "config.h"

#include "chafa.h"
#include "internal/chafa-private.h"
#include "internal/chafa-string-util.h"

/**
 * SECTION:chafa-term-info
 * @title: ChafaTermInfo
 * @short_description: Describes a particular terminal type
 *
 * A #ChafaTermInfo describes the characteristics of one particular kind
 * of display terminal. It stores control sequences that can be used to
 * move the cursor, change text attributes, mark the beginning and end of
 * sixel graphics data, etc.
 *
 * #ChafaTermInfo also implements an efficient low-level API for formatting
 * these sequences with marshaled arguments so they can be sent to the
 * terminal.
 **/

/* Maximum number of arguments + 1 for the sentinel */
#define CHAFA_TERM_SEQ_ARGS_MAX 8
#define ARG_INDEX_SENTINEL 255

typedef struct
{
    guint8 pre_len;
    guint8 arg_index;
}
SeqArgInfo;

struct ChafaTermInfo
{
    gint refs;
    gchar seq_str [CHAFA_TERM_SEQ_MAX] [CHAFA_TERM_SEQ_LENGTH_MAX];
    SeqArgInfo seq_args [CHAFA_TERM_SEQ_MAX] [CHAFA_TERM_SEQ_ARGS_MAX];
    gchar *unparsed_str [CHAFA_TERM_SEQ_MAX];
};

typedef struct
{
    guint n_args;
    guint type_size;
}
SeqMeta;

static const SeqMeta seq_meta [CHAFA_TERM_SEQ_MAX] =
{
#define CHAFA_TERM_SEQ_DEF(name, NAME, n_args, arg_proc, arg_type, ...)  \
    [CHAFA_TERM_SEQ_##NAME] = { n_args, sizeof (arg_type) },
#include <chafa-term-seq-def.h>
#undef CHAFA_TERM_SEQ_DEF
};

/* Avoid memcpy() because it inlines to a sizeable amount of code (it
 * doesn't know our strings are always short). We also take the liberty
 * of unconditionally copying a byte even if n=0. This simplifies the
 * generated assembly quite a bit. */
static inline void
copy_bytes (gchar *out, const gchar *in, gint n)
{
    gint i = 0;

    do
    {
        *(out++) = *(in++);
        i++;
    }
    while (i < n);
}

static gboolean
parse_seq_args (gchar *out, SeqArgInfo *arg_info, const gchar *in,
                gint n_args, gint arg_len_max, GError **error)
{
    gint i, j, k;
    gint pre_len = 0;
    gint result = FALSE;

    g_assert (n_args < CHAFA_TERM_SEQ_ARGS_MAX);

    for (k = 0; k < CHAFA_TERM_SEQ_ARGS_MAX; k++)
    {
        arg_info [k].pre_len = 0;
        arg_info [k].arg_index = ARG_INDEX_SENTINEL;
    }

    for (i = 0, j = 0, k = 0;
         j < CHAFA_TERM_SEQ_LENGTH_MAX && k < CHAFA_TERM_SEQ_ARGS_MAX && in [i];
         i++)
    {
        gchar c = in [i];

        if (c == '%')
        {
            i++;
            c = in [i];

            if (c == '%')
            {
                /* "%%" -> literal "%" */
                out [j++] = '%';
                pre_len++;
            }
            else if (c >= '1' && c <= '8')  /* arg # 0-7 */
            {
                /* "%n" -> argument n-1 */

                arg_info [k].arg_index = c - '1';
                arg_info [k].pre_len = pre_len;

                if (arg_info [k].arg_index >= n_args)
                {
                    /* Bad argument index (out of range) */
                    g_set_error (error, CHAFA_TERM_INFO_ERROR, CHAFA_TERM_INFO_ERROR_BAD_ARGUMENTS,
                                 "Control sequence had too many arguments.");
                    goto out;
                }

                pre_len = 0;
                k++;
            }
            else
            {
                /* Bad "%?" escape */
                goto out;
            }
        }
        else
        {
            out [j++] = c;
            pre_len++;
        }
    }

    if (k == CHAFA_TERM_SEQ_ARGS_MAX)
    {
        /* Too many argument expansions */
        g_set_error (error, CHAFA_TERM_INFO_ERROR, CHAFA_TERM_INFO_ERROR_BAD_ARGUMENTS,
                     "Control sequence had too many arguments.");
        goto out;
    }

    /* Reserve an extra byte for copy_byte() and chafa_format_dec_u8() excess. */
    if (j + k * arg_len_max + 1 > CHAFA_TERM_SEQ_LENGTH_MAX)
    {
        /* Formatted string may be too long */
        g_set_error (error, CHAFA_TERM_INFO_ERROR, CHAFA_TERM_INFO_ERROR_SEQ_TOO_LONG,
                     "Control sequence too long.");
        goto out;
    }

    arg_info [k].pre_len = pre_len;
    arg_info [k].arg_index = ARG_INDEX_SENTINEL;

    result = TRUE;

out:
    return result;
}

#define EMIT_SEQ_DEF(inttype, intformatter)                             \
    static gchar *                                                      \
    emit_seq_##inttype (const ChafaTermInfo *term_info, gchar *out, ChafaTermSeq seq, \
                        inttype *args, gint n_args)                     \
    {                                                                   \
        const gchar *seq_str;                                           \
        const SeqArgInfo *seq_args;                                     \
        gint ofs = 0;                                                   \
        gint i;                                                         \
                                                                        \
        seq_str = &term_info->seq_str [seq] [0];                        \
        seq_args = &term_info->seq_args [seq] [0];                      \
                                                                        \
        if (seq_args [0].arg_index == ARG_INDEX_SENTINEL)               \
            return out;                                                 \
                                                                        \
        for (i = 0; i < n_args; i++)                                    \
        {                                                               \
            copy_bytes (out, &seq_str [ofs], seq_args [i].pre_len);     \
            out += seq_args [i].pre_len;                                \
            ofs += seq_args [i].pre_len;                                \
            out = chafa_format_dec_uint_0_to_9999 (out, args [seq_args [i].arg_index]); \
        }                                                               \
                                                                        \
        copy_bytes (out, &seq_str [ofs], seq_args [i].pre_len);         \
        out += seq_args [i].pre_len;                                    \
                                                                        \
        return out;                                                     \
    }

EMIT_SEQ_DEF(guint, chafa_format_dec_uint_0_to_9999)
EMIT_SEQ_DEF(guint8, chafa_format_dec_u8)

static gchar *
emit_seq_0_args_uint (const ChafaTermInfo *term_info, gchar *out, ChafaTermSeq seq)
{
    copy_bytes (out, &term_info->seq_str [seq] [0], term_info->seq_args [seq] [0].pre_len);
    return out + term_info->seq_args [seq] [0].pre_len;
}

static gchar *
emit_seq_1_args_uint (const ChafaTermInfo *term_info, gchar *out, ChafaTermSeq seq, guint arg0)
{
    return emit_seq_guint (term_info, out, seq, &arg0, 1);
}

static gchar *
emit_seq_1_args_uint8 (const ChafaTermInfo *term_info, gchar *out, ChafaTermSeq seq, guint8 arg0)
{
    return emit_seq_guint8 (term_info, out, seq, &arg0, 1);
}

static gchar *
emit_seq_2_args_uint (const ChafaTermInfo *term_info, gchar *out, ChafaTermSeq seq, guint arg0, guint arg1)
{
    guint args [2];

    args [0] = arg0;
    args [1] = arg1;
    return emit_seq_guint (term_info, out, seq, args, 2);
}

static gchar *
emit_seq_2_args_uint8 (const ChafaTermInfo *term_info, gchar *out, ChafaTermSeq seq, guint arg0, guint arg1)
{
    guint8 args [2];

    args [0] = arg0;
    args [1] = arg1;
    return emit_seq_guint8 (term_info, out, seq, args, 2);
}

static gchar *
emit_seq_3_args_uint (const ChafaTermInfo *term_info, gchar *out, ChafaTermSeq seq, guint8 arg0, guint8 arg1, guint8 arg2)
{
    guint args [3];

    args [0] = arg0;
    args [1] = arg1;
    args [2] = arg2;
    return emit_seq_guint (term_info, out, seq, args, 3);
}

static gchar *
emit_seq_3_args_uint8 (const ChafaTermInfo *term_info, gchar *out, ChafaTermSeq seq, guint8 arg0, guint8 arg1, guint8 arg2)
{
    guint8 args [3];

    args [0] = arg0;
    args [1] = arg1;
    args [2] = arg2;
    return emit_seq_guint8 (term_info, out, seq, args, 3);
}

static gchar *
emit_seq_6_args_uint8 (const ChafaTermInfo *term_info, gchar *out, ChafaTermSeq seq, guint8 arg0, guint8 arg1, guint8 arg2, guint8 arg3, guint8 arg4, guint8 arg5)
{
    guint8 args [6];

    args [0] = arg0;
    args [1] = arg1;
    args [2] = arg2;
    args [3] = arg3;
    args [4] = arg4;
    args [5] = arg5;
    return emit_seq_guint8 (term_info, out, seq, args, 6);
}

/* Public */

G_DEFINE_QUARK (chafa-term-info-error-quark, chafa_term_info_error)

/**
 * chafa_term_info_new:
 *
 * Creates a new, blank #ChafaTermInfo.
 *
 * Returns: The new #ChafaTermInfo
 **/
ChafaTermInfo *
chafa_term_info_new (void)
{
    ChafaTermInfo *term_info;
    gint i;

    term_info = g_new0 (ChafaTermInfo, 1);
    term_info->refs = 1;

    for (i = 0; i < CHAFA_TERM_SEQ_MAX; i++)
    {
        term_info->seq_args [i] [0].arg_index = ARG_INDEX_SENTINEL;
    }

    return term_info;
}

/**
 * chafa_term_info_copy:
 * @term_info: A #ChafaTermInfo to copy.
 *
 * Creates a new #ChafaTermInfo that's a copy of @term_info.
 *
 * Returns: The new #ChafaTermInfo
 **/
ChafaTermInfo *
chafa_term_info_copy (const ChafaTermInfo *term_info)
{
    ChafaTermInfo *new_term_info;
    gint i;

    g_return_val_if_fail (term_info != NULL, NULL);

    new_term_info = g_new (ChafaTermInfo, 1);
    memcpy (new_term_info, term_info, sizeof (ChafaTermInfo));
    new_term_info->refs = 1;

    for (i = 0; i < CHAFA_TERM_SEQ_MAX; i++)
    {
        if (new_term_info->unparsed_str [i])
            new_term_info->unparsed_str [i] = g_strdup (new_term_info->unparsed_str [i]);
    }

    return new_term_info;
}

/**
 * chafa_term_info_ref:
 * @term_info: #ChafaTermInfo to add a reference to.
 *
 * Adds a reference to @term_info.
 **/
void
chafa_term_info_ref (ChafaTermInfo *term_info)
{
    gint refs;

    g_return_if_fail (term_info != NULL);
    refs = g_atomic_int_get (&term_info->refs);
    g_return_if_fail (refs > 0);

    g_atomic_int_inc (&term_info->refs);
}

/**
 * chafa_term_info_unref:
 * @term_info: #ChafaTermInfo to remove a reference from.
 *
 * Removes a reference from @term_info.
 **/
void
chafa_term_info_unref (ChafaTermInfo *term_info)
{
    gint refs;

    g_return_if_fail (term_info != NULL);
    refs = g_atomic_int_get (&term_info->refs);
    g_return_if_fail (refs > 0);

    if (g_atomic_int_dec_and_test (&term_info->refs))
    {
        gint i;

        for (i = 0; i < CHAFA_TERM_SEQ_MAX; i++)
            g_free (term_info->unparsed_str [i]);

        g_free (term_info);
    }
}

/**
 * chafa_term_info_have_seq:
 * @term_info: A #ChafaTermInfo.
 * @seq: A #ChafaTermSeq to query for.
 *
 * Checks if @term_info can emit @seq.
 *
 * Returns: %TRUE if @seq can be emitted, %FALSE otherwise
 **/
gboolean
chafa_term_info_have_seq (const ChafaTermInfo *term_info, ChafaTermSeq seq)
{
    g_return_val_if_fail (term_info != NULL, FALSE);
    g_return_val_if_fail (seq >= 0 && seq < CHAFA_TERM_SEQ_MAX, FALSE);

    return term_info->unparsed_str [seq] ? TRUE : FALSE;
}

/**
 * chafa_term_info_get_seq:
 * @term_info: A #ChafaTermInfo.
 * @seq: A #ChafaTermSeq to query for.
 *
 * Gets the string equivalent of @seq stored in @term_info.
 *
 * Returns: An unformatted string sequence, or %NULL if not set.
 **/
const gchar *
chafa_term_info_get_seq (ChafaTermInfo *term_info, ChafaTermSeq seq)
{
    g_return_val_if_fail (term_info != NULL, NULL);
    g_return_val_if_fail (seq >= 0 && seq < CHAFA_TERM_SEQ_MAX, NULL);

    return term_info->unparsed_str [seq];
}

/**
 * chafa_term_info_set_seq:
 * @term_info: A #ChafaTermInfo.
 * @seq: A #ChafaTermSeq to query for.
 * @str: A control sequence string, or %NULL to clear.
 * @error: A return location for error details, or %NULL.
 *
 * Sets the control sequence string equivalent of @seq stored in @term_info to @str.
 *
 * The string may contain argument indexes to be substituted with integers on
 * formatting. The indexes are preceded by a percentage character and start at 1,
 * i.e. \%1, \%2, \%3, etc.
 *
 * The string's length after formatting must not exceed %CHAFA_TERM_SEQ_LENGTH_MAX
 * bytes. Each argument can add up to four digits, or three for those specified as
 * 8-bit integers. If the string could potentially exceed this length when
 * formatted, chafa_term_info_set_seq() will return %FALSE.
 *
 * If parsing fails or @str is too long, any previously existing sequence
 * will be left untouched.
 *
 * Passing %NULL for @str clears the corresponding control sequence.
 *
 * Returns: %TRUE if parsing succeeded, %FALSE otherwise
 **/
gboolean
chafa_term_info_set_seq (ChafaTermInfo *term_info, ChafaTermSeq seq, const gchar *str,
                         GError **error)
{
    gchar seq_str [CHAFA_TERM_SEQ_MAX];
    SeqArgInfo seq_args [CHAFA_TERM_SEQ_ARGS_MAX];
    gboolean result = FALSE;

    g_return_val_if_fail (term_info != NULL, FALSE);
    g_return_val_if_fail (seq >= 0 && seq < CHAFA_TERM_SEQ_MAX, FALSE);

    if (!str)
    {
        term_info->seq_str [seq] [0] = '\0';
        term_info->seq_args [seq] [0].pre_len = 0;
        term_info->seq_args [seq] [0].arg_index = ARG_INDEX_SENTINEL;

        g_free (term_info->unparsed_str [seq]);
        term_info->unparsed_str [seq] = NULL;
        result = TRUE;
    }
    else
    {
        result = parse_seq_args (&seq_str [0], &seq_args [0], str,
                                 seq_meta [seq].n_args,
                                 seq_meta [seq].type_size == 1 ? 3 : 4,
                                 error);

        if (result == TRUE)
        {
            memcpy (&term_info->seq_str [seq] [0], &seq_str [0],
                    CHAFA_TERM_SEQ_MAX);
            memcpy (&term_info->seq_args [seq] [0], &seq_args [0],
                    CHAFA_TERM_SEQ_ARGS_MAX * sizeof (SeqArgInfo));

            g_free (term_info->unparsed_str [seq]);
            term_info->unparsed_str [seq] = g_strdup (str);
        }
    }

    return result;
}

#define DEFINE_EMIT_SEQ_0_none_void(func_name, seq_name) \
gchar *chafa_term_info_emit_##func_name(const ChafaTermInfo *term_info, gchar *dest) \
{ return emit_seq_0_args_uint (term_info, dest, CHAFA_TERM_SEQ_##seq_name); }

#define DEFINE_EMIT_SEQ_1_none_guint(func_name, seq_name) \
gchar *chafa_term_info_emit_##func_name(const ChafaTermInfo *term_info, gchar *dest, guint arg) \
{ return emit_seq_1_args_uint (term_info, dest, CHAFA_TERM_SEQ_##seq_name, arg); }

#define DEFINE_EMIT_SEQ_1_none_guint8(func_name, seq_name) \
gchar *chafa_term_info_emit_##func_name(const ChafaTermInfo *term_info, gchar *dest, guint8 arg) \
{ return emit_seq_1_args_uint8 (term_info, dest, CHAFA_TERM_SEQ_##seq_name, arg); }

#define DEFINE_EMIT_SEQ_1_aix16fg_guint8(func_name, seq_name) \
gchar *chafa_term_info_emit_##func_name(const ChafaTermInfo *term_info, gchar *dest, guint8 arg) \
{ return emit_seq_1_args_uint8 (term_info, dest, CHAFA_TERM_SEQ_##seq_name, arg + (arg < 8 ? 30 : (90 - 8))); }

#define DEFINE_EMIT_SEQ_1_aix16bg_guint8(func_name, seq_name) \
gchar *chafa_term_info_emit_##func_name(const ChafaTermInfo *term_info, gchar *dest, guint8 arg) \
{ return emit_seq_1_args_uint8 (term_info, dest, CHAFA_TERM_SEQ_##seq_name, arg + (arg < 8 ? 40 : (100 - 8))); }

#define DEFINE_EMIT_SEQ_2_none_guint(func_name, seq_name) \
gchar *chafa_term_info_emit_##func_name(const ChafaTermInfo *term_info, gchar *dest, guint arg0, guint arg1) \
{ return emit_seq_2_args_uint (term_info, dest, CHAFA_TERM_SEQ_##seq_name, arg0, arg1); }

#define DEFINE_EMIT_SEQ_2_none_guint8(func_name, seq_name) \
gchar *chafa_term_info_emit_##func_name(const ChafaTermInfo *term_info, gchar *dest, guint8 arg0, guint8 arg1) \
{ return emit_seq_2_args_uint8 (term_info, dest, CHAFA_TERM_SEQ_##seq_name, arg0, arg1); }

#define DEFINE_EMIT_SEQ_2_aix16fgbg_guint8(func_name, seq_name) \
gchar *chafa_term_info_emit_##func_name(const ChafaTermInfo *term_info, gchar *dest, guint8 arg0, guint8 arg1) \
{ return emit_seq_2_args_uint8 (term_info, dest, CHAFA_TERM_SEQ_##seq_name, arg0 + (arg0 < 8 ? 30 : (90 - 8)), arg1 + (arg1 < 8 ? 40 : (100 - 8))); }

#define DEFINE_EMIT_SEQ_3_none_guint(func_name, seq_name) \
gchar *chafa_term_info_emit_##func_name(const ChafaTermInfo *term_info, gchar *dest, guint arg0, guint arg1, guint arg2) \
{ return emit_seq_3_args_uint (term_info, dest, CHAFA_TERM_SEQ_##seq_name, arg0, arg1, arg2); }

#define DEFINE_EMIT_SEQ_3_none_guint8(func_name, seq_name) \
gchar *chafa_term_info_emit_##func_name(const ChafaTermInfo *term_info, gchar *dest, guint8 arg0, guint8 arg1, guint8 arg2) \
{ return emit_seq_3_args_uint8 (term_info, dest, CHAFA_TERM_SEQ_##seq_name, arg0, arg1, arg2); }

#define DEFINE_EMIT_SEQ_6_none_guint8(func_name, seq_name) \
gchar *chafa_term_info_emit_##func_name(const ChafaTermInfo *term_info, gchar *dest, guint8 arg0, guint8 arg1, guint8 arg2, guint8 arg3, guint8 arg4, guint8 arg5) \
{ return emit_seq_6_args_uint8 (term_info, dest, CHAFA_TERM_SEQ_##seq_name, arg0, arg1, arg2, arg3, arg4, arg5); }

#define CHAFA_TERM_SEQ_DEF(name, NAME, n_args, arg_proc, arg_type, ...)  \
    DEFINE_EMIT_SEQ_##n_args##_##arg_proc##_##arg_type(name, NAME)
#include "chafa-term-seq-def.h"
#undef CHAFA_TERM_SEQ_DEF
