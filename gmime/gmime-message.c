/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*  GMime
 *  Copyright (C) 2000-2014 Jeffrey Stedfast
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, 51 Franklin Street, Fifth Floor, Boston, MA
 *  02110-1301, USA.
 */


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <ctype.h>
#include <locale.h>

#include "gmime-message.h"
#include "gmime-multipart.h"
#include "gmime-multipart-signed.h"
#include "gmime-multipart-encrypted.h"
#include "gmime-part.h"
#include "gmime-utils.h"
#include "gmime-common.h"
#include "gmime-stream-mem.h"
#include "gmime-table-private.h"
#include "gmime-parse-utils.h"
#include "gmime-internal.h"
#include "gmime-events.h"


/**
 * SECTION: gmime-message
 * @title: GMimeMessage
 * @short_description: Messages
 * @see_also:
 *
 * A #GMimeMessage represents an rfc822 message.
 **/

static void g_mime_message_class_init (GMimeMessageClass *klass);
static void g_mime_message_init (GMimeMessage *message, GMimeMessageClass *klass);
static void g_mime_message_finalize (GObject *object);

/* GMimeObject class methods */
static void message_prepend_header (GMimeObject *object, const char *header, const char *value, const char *raw_value, gint64 offset);
static void message_append_header (GMimeObject *object, const char *header, const char *value, const char *raw_value, gint64 offset);
static void message_set_header (GMimeObject *object, const char *header, const char *value, const char *raw_value, gint64 offset);
static const char *message_get_header (GMimeObject *object, const char *header);
static gboolean message_remove_header (GMimeObject *object, const char *header);
static char *message_get_headers (GMimeObject *object);
static ssize_t message_write_to_stream (GMimeObject *object, GMimeStream *stream, gboolean content_only);
static void message_encode (GMimeObject *object, GMimeEncodingConstraint constraint);

/*static ssize_t write_structured (GMimeParserOptions *options, GMimeStream *stream, const char *name, const char *value);*/
static ssize_t write_references (GMimeParserOptions *options, GMimeStream *stream, const char *name, const char *value);
static ssize_t write_addrspec (GMimeParserOptions *options, GMimeStream *stream, const char *name, const char *value);
static ssize_t write_received (GMimeParserOptions *options, GMimeStream *stream, const char *name, const char *value);
static ssize_t write_subject (GMimeParserOptions *options, GMimeStream *stream, const char *name, const char *value);
static ssize_t write_msgid (GMimeParserOptions *options, GMimeStream *stream, const char *name, const char *value);


static void sender_changed (InternetAddressList *list, gpointer args, GMimeMessage *message);
static void from_changed (InternetAddressList *list, gpointer args, GMimeMessage *message);
static void reply_to_changed (InternetAddressList *list, gpointer args, GMimeMessage *message);
static void to_list_changed (InternetAddressList *list, gpointer args, GMimeMessage *message);
static void cc_list_changed (InternetAddressList *list, gpointer args, GMimeMessage *message);
static void bcc_list_changed (InternetAddressList *list, gpointer args, GMimeMessage *message);


static GMimeObjectClass *parent_class = NULL;

static struct {
	const char *name;
	GMimeEventCallback changed_cb;
} address_types[] = {
	{ "Sender",   (GMimeEventCallback) sender_changed   },
	{ "From",     (GMimeEventCallback) from_changed     },
	{ "Reply-To", (GMimeEventCallback) reply_to_changed },
	{ "To",       (GMimeEventCallback) to_list_changed  },
	{ "Cc",       (GMimeEventCallback) cc_list_changed  },
	{ "Bcc",      (GMimeEventCallback) bcc_list_changed }
};

#define N_ADDRESS_TYPES G_N_ELEMENTS (address_types)

static char *rfc822_headers[] = {
	"Return-Path",
	"Received",
	"Date",
	"From",
	"Reply-To",
	"Subject",
	"Sender",
	"To",
	"Cc",
};


GType
g_mime_message_get_type (void)
{
	static GType type = 0;
	
	if (!type) {
		static const GTypeInfo info = {
			sizeof (GMimeMessageClass),
			NULL, /* base_class_init */
			NULL, /* base_class_finalize */
			(GClassInitFunc) g_mime_message_class_init,
			NULL, /* class_finalize */
			NULL, /* class_data */
			sizeof (GMimeMessage),
			0,   /* n_preallocs */
			(GInstanceInitFunc) g_mime_message_init,
		};
		
		type = g_type_register_static (GMIME_TYPE_OBJECT, "GMimeMessage", &info, 0);
	}
	
	return type;
}


static void
g_mime_message_class_init (GMimeMessageClass *klass)
{
	GMimeObjectClass *object_class = GMIME_OBJECT_CLASS (klass);
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	
	parent_class = g_type_class_ref (GMIME_TYPE_OBJECT);
	
	gobject_class->finalize = g_mime_message_finalize;
	
	object_class->prepend_header = message_prepend_header;
	object_class->append_header = message_append_header;
	object_class->remove_header = message_remove_header;
	object_class->set_header = message_set_header;
	object_class->get_header = message_get_header;
	object_class->get_headers = message_get_headers;
	object_class->write_to_stream = message_write_to_stream;
	object_class->encode = message_encode;
}

static void
connect_changed_event (GMimeMessage *message, GMimeAddressType type)
{
	InternetAddressList *addrlist;
	
	addrlist = message->addrlists[type];
	
	g_mime_event_add (addrlist->priv, address_types[type].changed_cb, message);
}

static void
disconnect_changed_event (GMimeMessage *message, GMimeAddressType type)
{
	InternetAddressList *addrlist;
	
	addrlist = message->addrlists[type];
	
	g_mime_event_remove (addrlist->priv, address_types[type].changed_cb, message);
}

static void
block_changed_event (GMimeMessage *message, GMimeAddressType type)
{
	InternetAddressList *addrlist;
	
	addrlist = message->addrlists[type];
	
	g_mime_event_block (addrlist->priv, address_types[type].changed_cb, message);
}

static void
unblock_changed_event (GMimeMessage *message, GMimeAddressType type)
{
	InternetAddressList *addrlist;
	
	addrlist = message->addrlists[type];
	
	g_mime_event_unblock (addrlist->priv, address_types[type].changed_cb, message);
}

static void
g_mime_message_init (GMimeMessage *message, GMimeMessageClass *klass)
{
	GMimeHeaderList *headers = ((GMimeObject *) message)->headers;
	guint i;
	
	message->addrlists = g_new (InternetAddressList *, N_ADDRESS_TYPES);
	message->message_id = NULL;
	message->mime_part = NULL;
	message->subject = NULL;
	message->tz_offset = 0;
	message->date = 0;
	
	/* initialize recipient lists */
	for (i = 0; i < N_ADDRESS_TYPES; i++) {
		message->addrlists[i] = internet_address_list_new ();
		connect_changed_event (message, i);
	}
	
	g_mime_header_list_register_writer (headers, "Reply-To", write_addrspec);
	g_mime_header_list_register_writer (headers, "Sender", write_addrspec);
	g_mime_header_list_register_writer (headers, "From", write_addrspec);
	g_mime_header_list_register_writer (headers, "To", write_addrspec);
	g_mime_header_list_register_writer (headers, "Cc", write_addrspec);
	g_mime_header_list_register_writer (headers, "Bcc", write_addrspec);
	
	g_mime_header_list_register_writer (headers, "Resent-Reply-To", write_addrspec);
	g_mime_header_list_register_writer (headers, "Resent-Sender", write_addrspec);
	g_mime_header_list_register_writer (headers, "Resent-From", write_addrspec);
	g_mime_header_list_register_writer (headers, "Resent-To", write_addrspec);
	g_mime_header_list_register_writer (headers, "Resent-Cc", write_addrspec);
	g_mime_header_list_register_writer (headers, "Resent-Bcc", write_addrspec);
	
	g_mime_header_list_register_writer (headers, "Subject", write_subject);
	g_mime_header_list_register_writer (headers, "Received", write_received);
	g_mime_header_list_register_writer (headers, "Message-Id", write_msgid);
	g_mime_header_list_register_writer (headers, "References", write_references);
}

static void
g_mime_message_finalize (GObject *object)
{
	GMimeMessage *message = (GMimeMessage *) object;
	guint i;
	
	/* disconnect changed handlers */
	for (i = 0; i < N_ADDRESS_TYPES; i++) {
		disconnect_changed_event (message, i);
		g_object_unref (message->addrlists[i]);
	}
	
	g_free (message->addrlists);
	g_free (message->message_id);
	g_free (message->subject);
	
	/* unref child mime part */
	if (message->mime_part)
		g_object_unref (message->mime_part);
	
	G_OBJECT_CLASS (parent_class)->finalize (object);
}


typedef void (* token_skip_t) (const char **in);

struct _received_token {
	char *token;
	size_t len;
	token_skip_t skip;
};

static void skip_cfws_atom (const char **in);
static void skip_domain    (const char **in);
static void skip_addr      (const char **in);
static void skip_msgid     (const char **in);

static struct _received_token received_tokens[] = {
	{ "from ", 5, skip_domain    },
	{ "by ",   3, skip_domain    },
	{ "via ",  4, skip_cfws_atom },
	{ "with ", 5, skip_cfws_atom },
	{ "id ",   3, skip_msgid     },
	{ "for ",  4, skip_addr      }
};

static void
skip_cfws_atom (const char **in)
{
	skip_cfws (in);
	skip_atom (in);
}

static void
skip_domain_subliteral (const char **in)
{
	const char *inptr = *in;
	
	while (*inptr && *inptr != '.' && *inptr != ']') {
		if (is_dtext (*inptr)) {
			inptr++;
		} else if (is_lwsp (*inptr)) {
			skip_cfws (&inptr);
		} else {
			break;
		}
	}
	
	*in = inptr;
}

static void
skip_domain_literal (const char **in)
{
	const char *inptr = *in;
	
	skip_cfws (&inptr);
	while (*inptr && *inptr != ']') {
		skip_domain_subliteral (&inptr);
		if (*inptr && *inptr != ']')
			inptr++;
	}
	
	*in = inptr;
}

static void
skip_domain (const char **in)
{
	const char *save, *inptr = *in;
	
	while (inptr && *inptr) {
		skip_cfws (&inptr);
		if (*inptr == '[') {
			/* domain literal */
			inptr++;
			skip_domain_literal (&inptr);
			if (*inptr == ']')
				inptr++;
		} else {
			skip_atom (&inptr);
		}
		
		save = inptr;
		skip_cfws (&inptr);
		if (*inptr != '.') {
			inptr = save;
			break;
		}
		
		inptr++;
	}
	
	*in = inptr;
}

static void
skip_addrspec (const char **in)
{
	const char *inptr = *in;
	
	skip_cfws (&inptr);
	skip_word (&inptr);
	skip_cfws (&inptr);
	
	while (*inptr == '.') {
		inptr++;
		skip_cfws (&inptr);
		skip_word (&inptr);
		skip_cfws (&inptr);
	}
	
	if (*inptr == '@') {
		inptr++;
		skip_domain (&inptr);
	}
	
	*in = inptr;
}

static void
skip_addr (const char **in)
{
	const char *inptr = *in;
	
	skip_cfws (&inptr);
	if (*inptr == '<') {
		inptr++;
		skip_addrspec (&inptr);
		if (*inptr == '>')
			inptr++;
	} else {
		skip_addrspec (&inptr);
	}
	
	*in = inptr;
}

static void
skip_msgid (const char **in)
{
	const char *inptr = *in;
	
	skip_cfws (&inptr);
	if (*inptr == '<') {
		inptr++;
		skip_addrspec (&inptr);
		if (*inptr == '>')
			inptr++;
	} else {
		skip_atom (&inptr);
	}
	
	*in = inptr;
}


struct _received_part {
	struct _received_part *next;
	const char *start;
	size_t len;
};

static ssize_t
write_received (GMimeParserOptions *options, GMimeStream *stream, const char *name, const char *value)
{
	struct _received_part *parts, *part, *tail;
	const char *inptr, *lwsp = NULL;
	ssize_t nwritten;
	GString *str;
	size_t len;
	guint i;
	
	while (is_lwsp (*value))
		value++;
	
	if (*value == '\0')
		return 0;
	
	str = g_string_new (name);
	g_string_append_len (str, ": ", 2);
	len = 10;
	
	tail = parts = part = g_alloca (sizeof (struct _received_part));
	part->start = inptr = value;
	part->next = NULL;
	
	while (*inptr) {
		for (i = 0; i < G_N_ELEMENTS (received_tokens); i++) {
			if (!strncmp (inptr, received_tokens[i].token, received_tokens[i].len)) {
				if (inptr > part->start) {
					g_assert (lwsp != NULL);
					part->len = lwsp - part->start;
					
					part = g_alloca (sizeof (struct _received_part));
					part->start = inptr;
					part->next = NULL;
					
					tail->next = part;
					tail = part;
				}
				
				inptr += received_tokens[i].len;
				received_tokens[i].skip (&inptr);
				
				lwsp = inptr;
				while (is_lwsp (*inptr))
					inptr++;
				
				if (*inptr == ';') {
					inptr++;
					
					part->len = inptr - part->start;
					
					lwsp = inptr;
					while (is_lwsp (*inptr))
						inptr++;
					
					part = g_alloca (sizeof (struct _received_part));
					part->start = inptr;
					part->next = NULL;
					
					tail->next = part;
					tail = part;
				}
				
				break;
			}
		}
		
		if (i == G_N_ELEMENTS (received_tokens)) {
			while (*inptr && !is_lwsp (*inptr))
				inptr++;
			
			lwsp = inptr;
			while (is_lwsp (*inptr))
				inptr++;
		}
		
		if (*inptr == '(') {
			skip_comment (&inptr);
			
			lwsp = inptr;
			while (is_lwsp (*inptr))
				inptr++;
		}
	}
	
	part->len = lwsp - part->start;
	
	lwsp = NULL;
	part = parts;
	do {
		len += lwsp ? part->start - lwsp : 0;
		if (len + part->len > GMIME_FOLD_LEN && part != parts) {
			g_string_append (str, "\n\t");
			len = 1;
		} else if (lwsp) {
			g_string_append_len (str, lwsp, (size_t) (part->start - lwsp));
		}
		
		g_string_append_len (str, part->start, part->len);
		lwsp = part->start + part->len;
		len += part->len;
		
		part = part->next;
	} while (part != NULL);
	
	g_string_append_c (str, '\n');
	
	nwritten = g_mime_stream_write (stream, str->str, str->len);
	g_string_free (str, TRUE);
	
	return nwritten;
}

static ssize_t
write_subject (GMimeParserOptions *options, GMimeStream *stream, const char *name, const char *value)
{
	char *folded;
	ssize_t n;
	
	folded = _g_mime_utils_unstructured_header_fold (options, name, value);
	n = g_mime_stream_write_string (stream, folded);
	g_free (folded);
	
	return n;
}

static ssize_t
write_msgid (GMimeParserOptions *options, GMimeStream *stream, const char *name, const char *value)
{
	/* Note: we don't want to wrap the Message-Id header - seems to
	   break a lot of clients (and servers) */
	return g_mime_stream_printf (stream, "%s: %s\n", name, value);
}

static ssize_t
write_references (GMimeParserOptions *options, GMimeStream *stream, const char *name, const char *value)
{
	GMimeReferences *references, *reference;
	ssize_t nwritten;
	GString *folded;
	size_t len, n;
	
	/* Note: we don't want to break in the middle of msgid tokens as
	   it seems to break a lot of clients (and servers) */
	references = g_mime_references_decode (value);
	folded = g_string_new (name);
	g_string_append_c (folded, ':');
	len = folded->len;
	
	reference = references;
	while (reference != NULL) {
		n = strlen (reference->msgid);
		if (len > 1 && len + n + 3 >= GMIME_FOLD_LEN) {
			g_string_append_len (folded, "\n\t", 2);
			len = 1;
		} else {
			g_string_append_c (folded, ' ');
			len++;
		}
		
		g_string_append_c (folded, '<');
		g_string_append_len (folded, reference->msgid, n);
		g_string_append_c (folded, '>');
		len += n + 2;
		
		reference = reference->next;
	}
	
	g_mime_references_clear (&references);
	
	g_string_append_len (folded, "\n", 1);
	nwritten = g_mime_stream_write (stream, folded->str, folded->len);
	g_string_free (folded, TRUE);
	
	return nwritten;
}

#if 0
static ssize_t
write_structured (GMimeParserOptions *options, GMimeStream *stream, const char *name, const char *value)
{
	char *folded;
	ssize_t n;
	
	folded = _g_mime_utils_structured_header_fold (options, name, value);
	n = g_mime_stream_write_string (stream, folded);
	g_free (folded);
	
	return n;
}
#endif

static ssize_t
write_addrspec (GMimeParserOptions *options, GMimeStream *stream, const char *name, const char *value)
{
	InternetAddressList *addrlist;
	GString *str;
	ssize_t n;
	
	str = g_string_new (name);
	g_string_append (str, ": ");
	
	if (value && (addrlist = internet_address_list_parse (options, value))) {
		internet_address_list_writer (addrlist, str);
		g_object_unref (addrlist);
	}
	
	g_string_append_c (str, '\n');
	
	n = g_mime_stream_write (stream, str->str, str->len);
	g_string_free (str, TRUE);
	
	return n;
}

enum {
	HEADER_SENDER,
	HEADER_FROM,
	HEADER_REPLY_TO,
	HEADER_TO,
	HEADER_CC,
	HEADER_BCC,
	HEADER_SUBJECT,
	HEADER_DATE,
	HEADER_MESSAGE_ID,
	HEADER_MIME_VERSION,
	HEADER_UNKNOWN
};

static const char *message_headers[] = {
	"Sender",
	"From",
	"Reply-To",
	"To",
	"Cc",
	"Bcc",
	"Subject",
	"Date",
	"Message-Id",
	"MIME-Version",
};

enum {
	PREPEND,
	APPEND,
	SET
};

static void
message_add_addresses_from_string (GMimeParserOptions *options, GMimeMessage *message, int action, GMimeAddressType type, const char *str)
{
	InternetAddressList *addrlist, *list;
	
	addrlist = message->addrlists[type];
	
	if (action == SET)
		internet_address_list_clear (addrlist);
	
	if ((list = internet_address_list_parse (options, str))) {
		if (action == PREPEND)
			internet_address_list_prepend (addrlist, list);
		else
			internet_address_list_append (addrlist, list);
		
		g_object_unref (list);
	}
}

static gboolean
process_header (GMimeObject *object, int action, const char *header, const char *value)
{
	GMimeParserOptions *options = _g_mime_header_list_get_options (object->headers);
	GMimeMessage *message = (GMimeMessage *) object;
	InternetAddressList *addrlist;
	time_t date;
	int offset;
	guint i;
	
	for (i = 0; i < G_N_ELEMENTS (message_headers); i++) {
		if (!g_ascii_strcasecmp (message_headers[i], header))
			break;
	}
	
	switch (i) {
	case HEADER_SENDER:
		block_changed_event (message, GMIME_ADDRESS_TYPE_SENDER);
		message_add_addresses_from_string (options, message, SET, GMIME_ADDRESS_TYPE_SENDER, value);
		unblock_changed_event (message, GMIME_ADDRESS_TYPE_SENDER);
		break;
	case HEADER_FROM:
		block_changed_event (message, GMIME_ADDRESS_TYPE_FROM);
		message_add_addresses_from_string (options, message, SET, GMIME_ADDRESS_TYPE_FROM, value);
		unblock_changed_event (message, GMIME_ADDRESS_TYPE_FROM);
		break;
	case HEADER_REPLY_TO:
		block_changed_event (message, GMIME_ADDRESS_TYPE_REPLY_TO);
		message_add_addresses_from_string (options, message, SET, GMIME_ADDRESS_TYPE_REPLY_TO, value);
		unblock_changed_event (message, GMIME_ADDRESS_TYPE_REPLY_TO);
		break;
	case HEADER_TO:
		block_changed_event (message, GMIME_ADDRESS_TYPE_TO);
		message_add_addresses_from_string (options, message, action, GMIME_ADDRESS_TYPE_TO, value);
		unblock_changed_event (message, GMIME_ADDRESS_TYPE_TO);
		break;
	case HEADER_CC:
		block_changed_event (message, GMIME_ADDRESS_TYPE_CC);
		message_add_addresses_from_string (options, message, action, GMIME_ADDRESS_TYPE_CC, value);
		unblock_changed_event (message, GMIME_ADDRESS_TYPE_CC);
		break;
	case HEADER_BCC:
		block_changed_event (message, GMIME_ADDRESS_TYPE_BCC);
		message_add_addresses_from_string (options, message, action, GMIME_ADDRESS_TYPE_BCC, value);
		unblock_changed_event (message, GMIME_ADDRESS_TYPE_BCC);
		break;
	case HEADER_SUBJECT:
		g_free (message->subject);
		message->subject = g_mime_utils_header_decode_text (options, value);
		break;
	case HEADER_DATE:
		if (value) {
			date = g_mime_utils_header_decode_date (value, &offset);
			message->date = date;
			message->tz_offset = offset;
		}
		break;
	case HEADER_MESSAGE_ID:
		g_free (message->message_id);
		message->message_id = g_mime_utils_decode_message_id (value);
		break;
	case HEADER_MIME_VERSION:
		break;
	default:
		return FALSE;
	}
	
	return TRUE;
}

static void
message_prepend_header (GMimeObject *object, const char *header, const char *value, const char *raw_value, gint64 offset)
{
	GMimeMessage *message = (GMimeMessage *) object;
	
	/* Content-* headers don't belong on the message, they belong on the part. */
	if (!g_ascii_strncasecmp ("Content-", header, 8)) {
		if (message->mime_part)
			_g_mime_object_prepend_header (message->mime_part, header, value, raw_value, offset);
		return;
	}
	
	if (!process_header (object, PREPEND, header, value))
		GMIME_OBJECT_CLASS (parent_class)->prepend_header (object, header, value, raw_value, offset);
	else
		_g_mime_header_list_prepend (object->headers, header, value, raw_value, offset);
}

static void
message_append_header (GMimeObject *object, const char *header, const char *value, const char *raw_value, gint64 offset)
{
	GMimeMessage *message = (GMimeMessage *) object;
	
	/* Content-* headers don't belong on the message, they belong on the part. */
	if (!g_ascii_strncasecmp ("Content-", header, 8)) {
		if (message->mime_part)
			_g_mime_object_append_header (message->mime_part, header, value, raw_value, offset);
		return;
	}
	
	if (!process_header (object, APPEND, header, value))
		GMIME_OBJECT_CLASS (parent_class)->append_header (object, header, value, raw_value, offset);
	else
		_g_mime_header_list_append (object->headers, header, value, raw_value, offset);
}

static void
message_set_header (GMimeObject *object, const char *header, const char *value, const char *raw_value, gint64 offset)
{
	GMimeMessage *message = (GMimeMessage *) object;
	
	/* Content-* headers don't belong on the message, they belong on the part. */
	if (!g_ascii_strncasecmp ("Content-", header, 8)) {
		if (message->mime_part)
			_g_mime_object_set_header (message->mime_part, header, value, raw_value, offset);
		return;
	}
	
	if (!process_header (object, SET, header, value))
		GMIME_OBJECT_CLASS (parent_class)->set_header (object, header, value, raw_value, offset);
	else
		_g_mime_header_list_set (object->headers, header, value, raw_value, offset);
}

static const char *
message_get_header (GMimeObject *object, const char *header)
{
	GMimeMessage *message = (GMimeMessage *) object;
	const char *value;
	
	/* Content-* headers don't belong on the message, they belong on the part. */
	if (g_ascii_strncasecmp ("Content-", header, 8) != 0) {
		if ((value = GMIME_OBJECT_CLASS (parent_class)->get_header (object, header)))
			return value;
		
		if (!g_ascii_strcasecmp ("MIME-Version", header))
			return "1.0";
	} else if (message->mime_part) {
		return g_mime_object_get_header (message->mime_part, header);
	}
	
	return NULL;
}

static void
remove_address_header (GMimeMessage *message, GMimeAddressType type)
{
	InternetAddressList *addrlist;
	
	block_changed_event (message, type);
	addrlist = message->addrlists[type];
	internet_address_list_clear (addrlist);
	unblock_changed_event (message, type);
}

static gboolean
message_remove_header (GMimeObject *object, const char *header)
{
	GMimeMessage *message = (GMimeMessage *) object;
	guint i;
	
	/* Content-* headers don't belong on the message, they belong on the part. */
	if (!g_ascii_strncasecmp ("Content-", header, 8)) {
		if (message->mime_part)
			return g_mime_object_remove_header (message->mime_part, header);
		
		return FALSE;
	}
	
	for (i = 0; i < G_N_ELEMENTS (message_headers); i++) {
		if (!g_ascii_strcasecmp (message_headers[i], header))
			break;
	}
	
	switch (i) {
	case HEADER_SENDER:
		remove_address_header (message, GMIME_ADDRESS_TYPE_SENDER);
		break;
	case HEADER_FROM:
		remove_address_header (message, GMIME_ADDRESS_TYPE_FROM);
		break;
	case HEADER_REPLY_TO:
		remove_address_header (message, GMIME_ADDRESS_TYPE_REPLY_TO);
		break;
	case HEADER_TO:
		remove_address_header (message, GMIME_ADDRESS_TYPE_TO);
		break;
	case HEADER_CC:
		remove_address_header (message, GMIME_ADDRESS_TYPE_CC);
		break;
	case HEADER_BCC:
		remove_address_header (message, GMIME_ADDRESS_TYPE_BCC);
		break;
	case HEADER_SUBJECT:
		g_free (message->subject);
		message->subject = NULL;
		break;
	case HEADER_DATE:
		message->date = 0;
		message->tz_offset = 0;
		break;
	case HEADER_MESSAGE_ID:
		g_free (message->message_id);
		message->message_id = NULL;
		break;
	default:
		break;
	}
	
	return GMIME_OBJECT_CLASS (parent_class)->remove_header (object, header);
}


static ssize_t
write_headers_to_stream (GMimeObject *object, GMimeStream *stream)
{
	GMimeMessage *message = (GMimeMessage *) object;
	GMimeObject *mime_part = message->mime_part;
	ssize_t nwritten, total = 0;
	
	if (mime_part != NULL) {
		int body_count = g_mime_header_list_get_count (mime_part->headers);
		int count = g_mime_header_list_get_count (object->headers);
		GMimeHeader *header, *body_header;
		gint64 body_offset, offset;
		int body_index = 0;
		int index = 0;
		
		while (index < count && body_index < body_count) {
			body_header = g_mime_header_list_get_header (mime_part->headers, body_index);
			if ((body_offset = g_mime_header_get_offset (body_header)) < 0)
				break;
			
			header = g_mime_header_list_get_header (object->headers, index);
			offset = g_mime_header_get_offset (header);
			
			if (offset >= 0 && offset < body_offset) {
				if ((nwritten = g_mime_header_write_to_stream (header, stream)) == -1)
					return -1;
				
				total += nwritten;
				index++;
			} else {
				if ((nwritten = g_mime_header_write_to_stream (body_header, stream)) == -1)
					return -1;
				
				total += nwritten;
				body_index++;
			}
		}
		
		while (index < count) {
			header = g_mime_header_list_get_header (object->headers, index);
			
			if ((nwritten = g_mime_header_write_to_stream (header, stream)) == -1)
				return -1;
			
			total += nwritten;
			index++;
		}
		
		while (body_index < body_count) {
			header = g_mime_header_list_get_header (mime_part->headers, body_index);
			
			if ((nwritten = g_mime_header_write_to_stream (header, stream)) == -1)
				return -1;
			
			total += nwritten;
			body_index++;
		}
		
		return total;
	}
	
	return g_mime_header_list_write_to_stream (object->headers, stream);
}


static char *
message_get_headers (GMimeObject *object)
{
	GMimeStream *stream;
	GByteArray *ba;
	char *str;
	
	ba = g_byte_array_new ();
	stream = g_mime_stream_mem_new ();
	g_mime_stream_mem_set_byte_array (GMIME_STREAM_MEM (stream), ba);
	write_headers_to_stream (object, stream);
	g_object_unref (stream);
	g_byte_array_append (ba, (unsigned char *) "", 1);
	str = (char *) ba->data;
	g_byte_array_free (ba, FALSE);
	
	return str;
}

static ssize_t
message_write_to_stream (GMimeObject *object, GMimeStream *stream, gboolean content_only)
{
	GMimeMessage *message = (GMimeMessage *) object;
	GMimeObject *mime_part = message->mime_part;
	ssize_t nwritten, total = 0;
	
	if (!content_only) {
		if ((nwritten = write_headers_to_stream (object, stream)) == -1)
			return -1;
		
		total += nwritten;
		
		if ((nwritten = g_mime_stream_write (stream, "\n", 1)) == -1)
			return -1;
		
		total += nwritten;
	}
	
	if (mime_part) {
		if ((nwritten = GMIME_OBJECT_GET_CLASS (mime_part)->write_to_stream (mime_part, stream, TRUE)) == -1)
			return -1;
		
		total += nwritten;
	}
	
	return total;
}

static void
message_encode (GMimeObject *object, GMimeEncodingConstraint constraint)
{
	GMimeMessage *message = (GMimeMessage *) object;
	
	if (message->mime_part != NULL)
		g_mime_object_encode (message->mime_part, constraint);
}



/**
 * g_mime_message_new:
 * @pretty_headers: make pretty headers 
 *
 * If @pretty_headers is %TRUE, then the standard rfc822 headers are
 * initialized so as to put headers in a nice friendly order. This is
 * strictly a cosmetic thing, so if you are unsure, it is safe to say
 * no (%FALSE).
 *
 * Returns: an empty #GMimeMessage object.
 **/
GMimeMessage *
g_mime_message_new (gboolean pretty_headers)
{
	GMimeHeaderList *headers;
	GMimeMessage *message;
	guint i;
	
	message = g_object_newv (GMIME_TYPE_MESSAGE, 0, NULL);
	
	if (pretty_headers) {
		/* Populate with the "standard" rfc822 headers so we can have a standard order */
		headers = ((GMimeObject *) message)->headers;
		for (i = 0; i < G_N_ELEMENTS (rfc822_headers); i++) 
			g_mime_header_list_set (headers, rfc822_headers[i], NULL);
	}
	
	return message;
}


/**
 * g_mime_message_get_sender:
 * @message: A #GMimeMessage
 *
 * Gets the parsed list of addresses in the Sender header.
 *
 * Returns: the parsed list of addresses in the Sender header.
 **/
InternetAddressList *
g_mime_message_get_sender (GMimeMessage *message)
{
	g_return_val_if_fail (GMIME_IS_MESSAGE (message), NULL);
	
	return message->addrlists[GMIME_ADDRESS_TYPE_SENDER];
}


/**
 * g_mime_message_get_from:
 * @message: A #GMimeMessage
 *
 * Gets the parsed list of addresses in the From header.
 *
 * Returns: the parsed list of addresses in the From header.
 **/
InternetAddressList *
g_mime_message_get_from (GMimeMessage *message)
{
	g_return_val_if_fail (GMIME_IS_MESSAGE (message), NULL);
	
	return message->addrlists[GMIME_ADDRESS_TYPE_FROM];
}


/**
 * g_mime_message_get_reply_to:
 * @message: A #GMimeMessage
 *
 * Gets the parsed list of addresses in the Reply-To header.
 *
 * Returns: the parsed list of addresses in the Reply-To header.
 **/
InternetAddressList *
g_mime_message_get_reply_to (GMimeMessage *message)
{
	g_return_val_if_fail (GMIME_IS_MESSAGE (message), NULL);
	
	return message->addrlists[GMIME_ADDRESS_TYPE_REPLY_TO];
}


/**
 * g_mime_message_get_to:
 * @message: A #GMimeMessage
 *
 * Gets combined list of parsed addresses in the To header(s).
 *
 * Returns: the parsed list of addresses in the To header(s).
 **/
InternetAddressList *
g_mime_message_get_to (GMimeMessage *message)
{
	g_return_val_if_fail (GMIME_IS_MESSAGE (message), NULL);
	
	return message->addrlists[GMIME_ADDRESS_TYPE_TO];
}


/**
 * g_mime_message_get_cc:
 * @message: A #GMimeMessage
 *
 * Gets combined list of parsed addresses in the Cc header(s).
 *
 * Returns: the parsed list of addresses in the Cc header(s).
 **/
InternetAddressList *
g_mime_message_get_cc (GMimeMessage *message)
{
	g_return_val_if_fail (GMIME_IS_MESSAGE (message), NULL);
	
	return message->addrlists[GMIME_ADDRESS_TYPE_CC];
}


/**
 * g_mime_message_get_bcc:
 * @message: A #GMimeMessage
 *
 * Gets combined list of parsed addresses in the Bcc header(s).
 *
 * Returns: the parsed list of addresses in the Bcc header(s).
 **/
InternetAddressList *
g_mime_message_get_bcc (GMimeMessage *message)
{
	g_return_val_if_fail (GMIME_IS_MESSAGE (message), NULL);
	
	return message->addrlists[GMIME_ADDRESS_TYPE_BCC];
}


static void
sync_internet_address_list (InternetAddressList *list, GMimeMessage *message, const char *name)
{
	GMimeObject *object = (GMimeObject *) message;
	char *string;
	
	string = internet_address_list_to_string (list, TRUE);
	g_mime_header_list_set (object->headers, name, string);
	g_free (string);
}

static void
sync_address_header (GMimeMessage *message, GMimeAddressType type)
{
	InternetAddressList *list = message->addrlists[type];
	const char *name = address_types[type].name;
	
	sync_internet_address_list (list, message, name);
}

static void
sender_changed (InternetAddressList *list, gpointer args, GMimeMessage *message)
{
	sync_address_header (message, GMIME_ADDRESS_TYPE_SENDER);
}

static void
from_changed (InternetAddressList *list, gpointer args, GMimeMessage *message)
{
	sync_address_header (message, GMIME_ADDRESS_TYPE_FROM);
}

static void
reply_to_changed (InternetAddressList *list, gpointer args, GMimeMessage *message)
{
	sync_address_header (message, GMIME_ADDRESS_TYPE_REPLY_TO);
}

static void
to_list_changed (InternetAddressList *list, gpointer args, GMimeMessage *message)
{
	sync_address_header (message, GMIME_ADDRESS_TYPE_TO);
}

static void
cc_list_changed (InternetAddressList *list, gpointer args, GMimeMessage *message)
{
	sync_address_header (message, GMIME_ADDRESS_TYPE_CC);
}

static void
bcc_list_changed (InternetAddressList *list, gpointer args, GMimeMessage *message)
{
	sync_address_header (message, GMIME_ADDRESS_TYPE_BCC);
}


/**
 * g_mime_message_add_mailbox:
 * @message: A #GMimeMessage
 * @type: A #GMimeAddressType
 * @name: The name of the mailbox (or %NULL)
 * @addr: The address of the mailbox
 *
 * Add a mailbox of a chosen type to the MIME message.
 *
 * Note: The @name (and @addr) strings should be in UTF-8.
 **/
void
g_mime_message_add_mailbox (GMimeMessage *message, GMimeAddressType type, const char *name, const char *addr)
{
	InternetAddressList *addrlist;
	InternetAddress *ia;
	
	g_return_if_fail (GMIME_IS_MESSAGE (message));
	g_return_if_fail (type < N_ADDRESS_TYPES);
	g_return_if_fail (addr != NULL);
	
	addrlist = message->addrlists[type];
	ia = internet_address_mailbox_new (name, addr);
	internet_address_list_add (addrlist, ia);
	g_object_unref (ia);
}


/**
 * g_mime_message_get_addresses:
 * @message: A #GMimeMessage
 * @type: A #GMimeAddressType
 *
 * Gets a list of addresses of the specified @type from the @message.
 *
 * Returns: (transfer none): a list of addresses of the specified
 * @type from the @message.
 **/
InternetAddressList *
g_mime_message_get_addresses (GMimeMessage *message, GMimeAddressType type)
{
	g_return_val_if_fail (GMIME_IS_MESSAGE (message), NULL);
	g_return_val_if_fail (type < N_ADDRESS_TYPES, NULL);
	
	return message->addrlists[type];
}


/**
 * g_mime_message_get_all_recipients:
 * @message: A #GMimeMessage
 *
 * Gets the complete list of recipients for @message.
 *
 * Returns: (transfer full): a newly allocated #InternetAddressList
 * containing all recipients of the message or %NULL if no recipients
 * are set.
 **/
InternetAddressList *
g_mime_message_get_all_recipients (GMimeMessage *message)
{
	InternetAddressList *recipients, *list = NULL;
	guint i;
	
	g_return_val_if_fail (GMIME_IS_MESSAGE (message), NULL);
	
	for (i = GMIME_ADDRESS_TYPE_TO; i <= GMIME_ADDRESS_TYPE_BCC; i++) {
		recipients = message->addrlists[i];
		
		if (internet_address_list_length (recipients) == 0)
			continue;
		
		if (list == NULL)
			list = internet_address_list_new ();
		
		internet_address_list_append (list, recipients);
	}
	
	return list;
}


/**
 * g_mime_message_set_subject:
 * @message: A #GMimeMessage
 * @subject: Subject string
 * @charset: The charset to use for encoding the subject or %NULL to use the default
 *
 * Set the subject of a @message.
 *
 * Note: The @subject string should be in UTF-8.
 **/
void
g_mime_message_set_subject (GMimeMessage *message, const char *subject, const char *charset)
{
	char *encoded;
	
	g_return_if_fail (GMIME_IS_MESSAGE (message));
	g_return_if_fail (subject != NULL);
	
	g_free (message->subject);
	message->subject = g_mime_strdup_trim (subject);
	
	encoded = g_mime_utils_header_encode_text (message->subject, charset);
	g_mime_object_set_header (GMIME_OBJECT (message), "Subject", encoded);
	g_free (encoded);
}


/**
 * g_mime_message_get_subject:
 * @message: A #GMimeMessage
 *
 * Gets the subject of the @message.
 *
 * Returns: the subject of the @message in a form suitable for display
 * or %NULL if no subject is set. If not %NULL, the returned string
 * will be in UTF-8.
 **/
const char *
g_mime_message_get_subject (GMimeMessage *message)
{
	g_return_val_if_fail (GMIME_IS_MESSAGE (message), NULL);
	
	return message->subject;
}


/**
 * g_mime_message_set_date:
 * @message: A #GMimeMessage
 * @date: a date to be used in the Date header
 * @tz_offset: timezone offset (in +/- hours)
 * 
 * Sets the Date header on a MIME Message.
 **/
void
g_mime_message_set_date (GMimeMessage *message, time_t date, int tz_offset)
{
	char *str;
	
	g_return_if_fail (GMIME_IS_MESSAGE (message));
	
	message->date = date;
	message->tz_offset = tz_offset;
	
	str = g_mime_utils_header_format_date (date, tz_offset);
	g_mime_object_set_header (GMIME_OBJECT (message), "Date", str);
	g_free (str);
}


/**
 * g_mime_message_get_date:
 * @message: A #GMimeMessage
 * @date: (out): pointer to a date in time_t
 * @tz_offset: (out): pointer to timezone offset (in +/- hours)
 * 
 * Stores the date in time_t format in @date. If @tz_offset is
 * non-%NULL, then the timezone offset in will be stored in
 * @tz_offset.
 **/
void
g_mime_message_get_date (GMimeMessage *message, time_t *date, int *tz_offset)
{
	g_return_if_fail (GMIME_IS_MESSAGE (message));
	g_return_if_fail (date != NULL);
	
	*date = message->date;
	
	if (tz_offset)
		*tz_offset = message->tz_offset;
}


/**
 * g_mime_message_get_date_as_string:
 * @message: A #GMimeMessage
 *
 * Gets the message's sent-date in string format.
 * 
 * Returns: a newly allocated string containing the Date header value.
 **/
char *
g_mime_message_get_date_as_string (GMimeMessage *message)
{
	g_return_val_if_fail (GMIME_IS_MESSAGE (message), NULL);
	
	return g_mime_utils_header_format_date (message->date, message->tz_offset);
}


/**
 * g_mime_message_set_date_as_string:
 * @message: A #GMimeMessage
 * @str: a date string
 *
 * Sets the sent-date of the message.
 **/
void
g_mime_message_set_date_as_string (GMimeMessage *message, const char *str)
{
	int tz_offset;
	time_t date;
	char *buf;
	
	g_return_if_fail (GMIME_IS_MESSAGE (message));
	
	date = g_mime_utils_header_decode_date (str, &tz_offset);
	message->tz_offset = tz_offset;
	message->date = date;
	
	buf = g_mime_utils_header_format_date (date, tz_offset);
	g_mime_object_set_header (GMIME_OBJECT (message), "Date", buf);
	g_free (buf);
}


/**
 * g_mime_message_set_message_id: 
 * @message: A #GMimeMessage
 * @message_id: message-id (addr-spec portion)
 *
 * Set the Message-Id on a message.
 **/
void
g_mime_message_set_message_id (GMimeMessage *message, const char *message_id)
{
	char *msgid;
	
	g_return_if_fail (GMIME_IS_MESSAGE (message));
	g_return_if_fail (message_id != NULL);
	
	g_free (message->message_id);
	message->message_id = g_mime_strdup_trim (message_id);
	
	msgid = g_strdup_printf ("<%s>", message_id);
	g_mime_object_set_header (GMIME_OBJECT (message), "Message-Id", msgid);
	g_free (msgid);
}


/**
 * g_mime_message_get_message_id: 
 * @message: A #GMimeMessage
 *
 * Gets the Message-Id header of @message.
 *
 * Returns: the Message-Id of a message.
 **/
const char *
g_mime_message_get_message_id (GMimeMessage *message)
{
	g_return_val_if_fail (GMIME_IS_MESSAGE (message), NULL);
	
	return message->message_id;
}


/**
 * g_mime_message_get_mime_part:
 * @message: A #GMimeMessage
 *
 * Gets the toplevel MIME part contained within @message.
 *
 * Returns: (transfer none): the toplevel MIME part of @message.
 **/
GMimeObject *
g_mime_message_get_mime_part (GMimeMessage *message)
{
	g_return_val_if_fail (GMIME_IS_MESSAGE (message), NULL);
	
	if (message->mime_part == NULL)
		return NULL;
	
	return message->mime_part;
}


/**
 * g_mime_message_set_mime_part:
 * @message: A #GMimeMessage
 * @mime_part: The root-level MIME Part
 *
 * Set the root-level MIME part of the message.
 **/
void
g_mime_message_set_mime_part (GMimeMessage *message, GMimeObject *mime_part)
{
	g_return_if_fail (GMIME_IS_OBJECT (mime_part));
	g_return_if_fail (GMIME_IS_MESSAGE (message));
	
	if (message->mime_part == mime_part)
		return;
	
	if (message->mime_part)
		g_object_unref (message->mime_part);
	
	if (mime_part) {
		GMimeHeaderList *headers = ((GMimeObject *) message)->headers;
		GMimeHeader *header;
		int i;
		
		if (!g_mime_header_list_contains (headers, "MIME-Version"))
			g_mime_header_list_append (headers, "MIME-Version", "1.0");
		
		for (i = 0; i < g_mime_header_list_get_count (mime_part->headers); i++) {
			header = g_mime_header_list_get_header (mime_part->headers, i);
			_g_mime_header_set_offset (header, -1);
		}
		
		g_object_ref (mime_part);
	}
	
	message->mime_part = mime_part;
}


/**
 * g_mime_message_foreach:
 * @message: A #GMimeMessage
 * @callback: (scope call): function to call on each of the mime parts
 *   contained by the mime message
 * @user_data: user-supplied callback data
 *
 * Recursively calls @callback on each of the mime parts in the mime message.
 **/
void
g_mime_message_foreach (GMimeMessage *message, GMimeObjectForeachFunc callback, gpointer user_data)
{
	g_return_if_fail (GMIME_IS_MESSAGE (message));
	g_return_if_fail (callback != NULL);
	
	callback ((GMimeObject *) message, message->mime_part, user_data);
	
	if (GMIME_IS_MULTIPART (message->mime_part))
		g_mime_multipart_foreach ((GMimeMultipart *) message->mime_part, callback, user_data);
}

static gboolean
part_is_textual (GMimeObject *mime_part)
{
	GMimeContentType *type;
	
	type = g_mime_object_get_content_type (mime_part);
	
	return g_mime_content_type_is_type (type, "text", "*");
}

static GMimeObject *
multipart_guess_body (GMimeMultipart *multipart)
{
	GMimeContentType *type;
	GMimeObject *mime_part;
	int count, i;
	
	if (GMIME_IS_MULTIPART_ENCRYPTED (multipart)) {
		/* nothing more we can do */
		return (GMimeObject *) multipart;
	}
	
	type = g_mime_object_get_content_type ((GMimeObject *) multipart);
	if (g_mime_content_type_is_type (type, "multipart", "alternative")) {
		/* very likely that this is the body - leave it up to
		 * our caller to decide which format of the body it
		 * wants to use. */
		return (GMimeObject *) multipart;
	}
	
	count = g_mime_multipart_get_count (multipart);
	
	if (count >= 1 && GMIME_IS_MULTIPART_SIGNED (multipart)) {
		/* if the body is in here, it has to be the first part */
		count = 1;
	}
	
	for (i = 0; i < count; i++) {
		mime_part = g_mime_multipart_get_part (multipart, i);
		
		if (GMIME_IS_MULTIPART (mime_part)) {
			if ((mime_part = multipart_guess_body ((GMimeMultipart *) mime_part)))
				return mime_part;
		} else if (GMIME_IS_PART (mime_part)) {
			if (part_is_textual (mime_part))
				return mime_part;
		}
	}
	
	return NULL;
}


/**
 * g_mime_message_get_body:
 * @message: A #GMimeMessage
 *
 * Attempts to identify the MIME part containing the body of the
 * message.
 *
 * Returns: (transfer none): a #GMimeObject containing the textual
 * content that appears to be the main body of the message.
 *
 * Note: This function is NOT guarenteed to always work as it
 * makes some assumptions that are not necessarily true. It is
 * recommended that you traverse the MIME structure yourself.
 **/
GMimeObject *
g_mime_message_get_body (GMimeMessage *message)
{
	GMimeObject *mime_part;
	
	g_return_val_if_fail (GMIME_IS_MESSAGE (message), NULL);
	
	if (!(mime_part = message->mime_part))
		return NULL;
	
	if (GMIME_IS_MULTIPART (mime_part))
		return multipart_guess_body ((GMimeMultipart *) mime_part);
	else if (GMIME_IS_PART (mime_part) && part_is_textual (mime_part))
		return mime_part;
	
	return NULL;
}
