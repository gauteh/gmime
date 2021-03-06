/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*  GMime
 *  Copyright (C) 2000-2014 Jeffrey Stedfast
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <gmime/gmime.h>
#include <gmime/gmime-pkcs7-context.h>

#include "testsuite.h"

#define GPG_PATH "/usr/bin/gpg"

extern int verbose;

#define v(x) if (verbose > 3) x

static gboolean
request_passwd (GMimeCryptoContext *ctx, const char *user_id, const char *prompt_ctx, gboolean reprompt, GMimeStream *response, GError **err)
{
	g_mime_stream_write_string (response, "no.secret\n");
	
	return TRUE;
}

static GMimeSignatureStatus
get_sig_status (GMimeSignatureList *signatures)
{
	GMimeSignatureStatus status = 0;
	GMimeSignature *sig;
	int i;
	
	if (!signatures || signatures->array->len == 0)
		return GMIME_SIGNATURE_STATUS_RED;
	
	for (i = 0; i < g_mime_signature_list_length (signatures); i++) {
		sig = g_mime_signature_list_get_signature (signatures, i);
		status |= sig->status;
	}
	
	return status;
}

static void
print_verify_results (GMimeSignatureList *signatures)
{
	GMimeSignatureStatus status;
	GMimeSignature *sig;
	int count, i;
	
	status = get_sig_status (signatures);
	
	if ((status & GMIME_SIGNATURE_STATUS_RED) != 0)
		fputs ("BAD\n", stdout);
	else if ((status & GMIME_SIGNATURE_STATUS_GREEN) != 0)
		fputs ("GOOD\n", stdout);
	else
		fputs ("UNKNOWN\n", stdout);
	
	fputs ("\nSignatures:\n", stdout);
	count = g_mime_signature_list_length (signatures);
	for (i = 0; i < count; i++) {
		sig = g_mime_signature_list_get_signature (signatures, i);
		
		fprintf (stdout, "\tName: %s\n", sig->cert->name ? sig->cert->name : "(null)");
		fprintf (stdout, "\tKeyId: %s\n", sig->cert->keyid ? sig->cert->keyid : "(null)");
		fprintf (stdout, "\tFingerprint: %s\n", sig->cert->fingerprint ? sig->cert->fingerprint : "(null)");
		fprintf (stdout, "\tTrust: ");
		
		switch (sig->cert->trust) {
		case GMIME_CERTIFICATE_TRUST_NONE:
			fputs ("None\n", stdout);
			break;
		case GMIME_CERTIFICATE_TRUST_NEVER:
			fputs ("Never\n", stdout);
			break;
		case GMIME_CERTIFICATE_TRUST_UNDEFINED:
			fputs ("Undefined\n", stdout);
			break;
		case GMIME_CERTIFICATE_TRUST_MARGINAL:
			fputs ("Marginal\n", stdout);
			break;
		case GMIME_CERTIFICATE_TRUST_FULLY:
			fputs ("Fully\n", stdout);
			break;
		case GMIME_CERTIFICATE_TRUST_ULTIMATE:
			fputs ("Ultimate\n", stdout);
			break;
		}
		
		fprintf (stdout, "\tStatus: ");
		if ((sig->status & GMIME_SIGNATURE_STATUS_RED) != 0)
			fputs ("BAD\n", stdout);
		else if ((sig->status & GMIME_SIGNATURE_STATUS_GREEN) != 0)
			fputs ("GOOD\n", stdout);
		else
			fputs ("UNKNOWN\n", stdout);
		
		fprintf (stdout, "\tSignature made on %s", ctime (&sig->created));
		if (sig->expires != (time_t) 0)
			fprintf (stdout, "\tSignature expires on %s", ctime (&sig->expires));
		else
			fprintf (stdout, "\tSignature never expires\n");
		
		fprintf (stdout, "\tErrors: ");
		if (sig->status & GMIME_SIGNATURE_STATUS_SIG_EXPIRED)
			fputs ("Expired, ", stdout);
		if (sig->status & GMIME_SIGNATURE_STATUS_KEY_MISSING)
			fputs ("No Pub Key, ", stdout);
		if (sig->status & GMIME_SIGNATURE_STATUS_KEY_EXPIRED)
			fputs ("Key Expired, ", stdout);
		if (sig->status & GMIME_SIGNATURE_STATUS_KEY_REVOKED)
			fputs ("Key Revoked", stdout);
		fputc ('\n', stdout);
		
		if (i + 1 < count)
			fputc ('\n', stdout);
	}
}

#define MULTIPART_SIGNED_CONTENT "This is a test of the emergency broadcast system \
with an sha1 detach-sign.\n\nFrom now on, there will be text to try and break     \t\
  \nvarious things. For example, the F in \"From\" in the previous line...\n...and \
the first dot of this line have been pre-encoded in the QP encoding in order to test \
that GMime properly treats MIME part content as opaque.\nIf this still verifies okay, \
then we have ourselves a winner I guess...\n"

static void
test_multipart_signed (GMimeCryptoContext *ctx)
{
	GMimeSignatureList *signatures;
	GMimeMultipartSigned *mps;
	GMimeDataWrapper *content;
	InternetAddressList *list;
	InternetAddress *mailbox;
	GMimeMessage *message;
	GMimeStream *stream;
	GMimeParser *parser;
	GError *err = NULL;
	GMimePart *part;
	Exception *ex;
	
	part = g_mime_part_new_with_type ("text", "plain");
	
	stream = g_mime_stream_mem_new ();
	g_mime_stream_write_string (stream, MULTIPART_SIGNED_CONTENT);
#if 0
	"This is a test of the emergency broadcast system with an sha1 detach-sign.\n\n"
		"From now on, there will be text to try and break     \t  \n"
		"various things. For example, the F in \"From\" in the previous line...\n"
		"...and the first dot of this line have been pre-encoded in the QP encoding "
		"in order to test that GMime properly treats MIME part content as opaque.\n"
		"If this still verifies okay, then we have ourselves a winner I guess...\n";
#endif
	
	g_mime_stream_reset (stream);
	content = g_mime_data_wrapper_new_with_stream (stream, GMIME_CONTENT_ENCODING_DEFAULT);
	g_object_unref (stream);
	
	g_mime_part_set_content_object (part, content);
	g_object_unref (content);
	
	/* create the multipart/signed container part */
	mps = g_mime_multipart_signed_new ();
	
	/* sign the part */
	g_mime_multipart_signed_sign (mps, GMIME_OBJECT (part), ctx, "alice@example.net",
				      GMIME_DIGEST_ALGO_SHA1, &err);
	g_object_unref (part);
	
	if (err != NULL) {
		ex = exception_new ("signing failed: %s", err->message);
		g_object_unref (mps);
		g_error_free (err);
		throw (ex);
	}
	
	message = g_mime_message_new (TRUE);
	
	mailbox = internet_address_mailbox_new ("Jeffrey Stedfast", "fejj@helixcode.com");
	list = g_mime_message_get_from (message);
	internet_address_list_add (list, mailbox);
	g_object_unref (mailbox);
	
	mailbox = internet_address_mailbox_new ("Jeffrey Stedfast", "fejj@helixcode.com");
	list = g_mime_message_get_reply_to (message);
	internet_address_list_add (list, mailbox);
	g_object_unref (mailbox);
	
	mailbox = internet_address_mailbox_new ("Federico Mena-Quintero", "federico@helixcode.com");
	list = g_mime_message_get_addresses (message, GMIME_ADDRESS_TYPE_TO);
	internet_address_list_add (list, mailbox);
	g_object_unref (mailbox);
	
	g_mime_message_set_subject (message, "This is a test message", NULL);
	g_mime_object_set_header ((GMimeObject *) message, "X-Mailer", "main.c");
	g_mime_message_set_mime_part (message, GMIME_OBJECT (mps));
	g_object_unref (mps);
	
	stream = g_mime_stream_mem_new ();
	g_mime_object_write_to_stream ((GMimeObject *) message, stream);
	g_mime_stream_reset (stream);
	g_object_unref (message);
	
	parser = g_mime_parser_new ();
	g_mime_parser_init_with_stream (parser, stream);
	g_object_unref (stream);
	
	message = g_mime_parser_construct_message (parser);
	g_object_unref (parser);
	
	if (!GMIME_IS_MULTIPART_SIGNED (message->mime_part)) {
		ex = exception_new ("resultant top-level mime part not a multipart/signed?");
		g_object_unref (message);
		throw (ex);
	}
	
	mps = (GMimeMultipartSigned *) message->mime_part;
	
	v(fputs ("Trying to verify signature... ", stdout));
	if (!(signatures = g_mime_multipart_signed_verify (mps, ctx, &err))) {
		ex = exception_new ("%s", err->message);
		v(fputs ("failed.\n", stdout));
		g_error_free (err);
		throw (ex);
	}
	
	v(print_verify_results (signatures));
	g_object_unref (signatures);
	
	g_object_unref (message);
}

#define MULTIPART_ENCRYPTED_CONTENT "This is a test of multipart/encrypted.\n"

static void
test_multipart_encrypted (GMimeCryptoContext *ctx, gboolean sign)
{
	GMimeSignatureStatus status;
	GMimeStream *cleartext, *stream;
	GMimeMultipartEncrypted *mpe;
	GMimeDecryptResult *result;
	GMimeDataWrapper *content;
	InternetAddressList *list;
	InternetAddress *mailbox;
	GMimeObject *decrypted;
	GPtrArray *recipients;
	GMimeMessage *message;
	Exception *ex = NULL;
	GMimeParser *parser;
	GByteArray *buf[2];
	GError *err = NULL;
	GMimePart *part;
	
	cleartext = g_mime_stream_mem_new ();
	g_mime_stream_write_string (cleartext, MULTIPART_ENCRYPTED_CONTENT);
	g_mime_stream_reset (cleartext);
	
	content = g_mime_data_wrapper_new ();
	g_mime_data_wrapper_set_stream (content, cleartext);
	g_object_unref (cleartext);
	
	part = g_mime_part_new_with_type ("text", "plain");
	g_mime_part_set_content_object (part, content);
	g_object_unref (content);
	
	/* hold onto this for comparison later */
	cleartext = g_mime_stream_mem_new ();
	g_mime_object_write_to_stream ((GMimeObject *) part, cleartext);
	g_mime_stream_reset (cleartext);
	
	/* create the multipart/encrypted container part */
	mpe = g_mime_multipart_encrypted_new ();
	
	/* encrypt the part */
	recipients = g_ptr_array_new ();
	g_ptr_array_add (recipients, "alice@example.net");
	g_mime_multipart_encrypted_encrypt (mpe, GMIME_OBJECT (part), ctx, sign,
					    "alice@example.net", GMIME_DIGEST_ALGO_SHA256,
					    recipients, &err);
	g_ptr_array_free (recipients, TRUE);
	g_object_unref (part);
	
	if (err != NULL) {
		ex = exception_new ("encryption failed: %s", err->message);
		g_object_unref (cleartext);
		g_object_unref (mpe);
		g_error_free (err);
		throw (ex);
	}
	
	message = g_mime_message_new (TRUE);
	
	mailbox = internet_address_mailbox_new ("Jeffrey Stedfast", "fejj@helixcode.com");
	list = g_mime_message_get_from (message);
	internet_address_list_add (list, mailbox);
	g_object_unref (mailbox);
	
	mailbox = internet_address_mailbox_new ("Jeffrey Stedfast", "fejj@helixcode.com");
	list = g_mime_message_get_reply_to (message);
	internet_address_list_add (list, mailbox);
	g_object_unref (mailbox);
	
	mailbox = internet_address_mailbox_new ("Federico Mena-Quintero", "federico@helixcode.com");
	list = g_mime_message_get_addresses (message, GMIME_ADDRESS_TYPE_TO);
	internet_address_list_add (list, mailbox);
	g_object_unref (mailbox);
	
	g_mime_message_set_subject (message, "This is a test message", NULL);
	g_mime_object_set_header ((GMimeObject *) message, "X-Mailer", "main.c");
	g_mime_message_set_mime_part (message, GMIME_OBJECT (mpe));
	g_object_unref (mpe);
	
	stream = g_mime_stream_mem_new ();
	g_mime_object_write_to_stream ((GMimeObject *) message, stream);
	g_mime_stream_reset (stream);
	g_object_unref (message);
	
	parser = g_mime_parser_new ();
	g_mime_parser_init_with_stream (parser, stream);
	g_object_unref (stream);
	
	message = g_mime_parser_construct_message (parser);
	g_object_unref (parser);
	
	if (!GMIME_IS_MULTIPART_ENCRYPTED (message->mime_part)) {
		ex = exception_new ("resultant top-level mime part not a multipart/encrypted?");
		g_object_unref (message);
		throw (ex);
	}
	
	mpe = (GMimeMultipartEncrypted *) message->mime_part;
	
	/* okay, now to test our decrypt function... */
	decrypted = g_mime_multipart_encrypted_decrypt (mpe, ctx, &result, &err);
	if (!decrypted || err != NULL) {
		ex = exception_new ("decryption failed: %s", err->message);
		g_object_unref (cleartext);
		g_error_free (err);
		throw (ex);
	}
	
	if (result->signatures)
		v(print_verify_results (result->signatures));
	
	if (sign) {
		status = get_sig_status (result->signatures);
		
		if ((status & GMIME_SIGNATURE_STATUS_RED) != 0)
			ex = exception_new ("signature status expected to be GOOD");
	} else {
		if (result->signatures)
			ex = exception_new ("signature status expected to be NONE");
	}
	
	g_object_unref (result);
	
	if (ex != NULL) {
		g_object_unref (cleartext);
		throw (ex);
	}
	
	stream = g_mime_stream_mem_new ();
	g_mime_object_write_to_stream (decrypted, stream);
	
	buf[0] = GMIME_STREAM_MEM (cleartext)->buffer;
	buf[1] = GMIME_STREAM_MEM (stream)->buffer;
	
	if (buf[0]->len != buf[1]->len || memcmp (buf[0]->data, buf[1]->data, buf[0]->len) != 0)
		ex = exception_new ("decrypted data does not match original cleartext");
	
	g_object_unref (cleartext);
	g_object_unref (stream);
	
	if (ex != NULL)
		throw (ex);
}

static void
import_key (GMimeCryptoContext *ctx, const char *path)
{
	GMimeStream *stream;
	GError *err = NULL;
	Exception *ex;
	int fd;
	
	if ((fd = open (path, O_RDONLY, 0)) == -1)
		throw (exception_new ("open() failed: %s", g_strerror (errno)));
	
	stream = g_mime_stream_fs_new (fd);
	g_mime_crypto_context_import_keys (ctx, stream, &err);
	g_object_unref (stream);
	
	if (err != NULL) {
		ex = exception_new ("%s", err->message);
		g_error_free (err);
		throw (ex);
	}
}

int main (int argc, char *argv[])
{
	const char *datadir = "data/smime";
	GMimeCryptoContext *ctx;
	struct stat st;
	char *key;
	int i;
	
	g_mime_init ();
	
	testsuite_init (argc, argv);
	
	if (testsuite_setup_gpghome (GPG_PATH) != 0)
		return EXIT_FAILURE;
	
	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-') {
			datadir = argv[i];
			break;
		}
	}
	
	if (i < argc && (stat (datadir, &st) == -1 || !S_ISDIR (st.st_mode)))
		return EXIT_FAILURE;
	
	testsuite_start ("S/MIME implementation");
	
	ctx = g_mime_pkcs7_context_new (request_passwd);
	g_mime_crypto_context_set_always_trust (ctx, TRUE);
	
	if (g_mime_crypto_context_set_retrieve_session_key (ctx, TRUE, NULL) == 0) {
		fprintf (stderr, "GMimePkcs7Context should not have allowed us to set retrieve_session_key to TRUE, since it is not implemented.\n");
		return EXIT_FAILURE;
	}
	if (g_mime_crypto_context_get_retrieve_session_key (ctx)) {
		fprintf (stderr, "GMimePkcs7Context should have returned FALSE for get_retrieve_session_key.\n");
		return EXIT_FAILURE;
	}
	
	testsuite_check ("GMimePkcs7Context::import");
	try {
		key = g_build_filename (datadir, "gmime-cert.p7", NULL);
		import_key (ctx, key);
		g_free (key);
		
		key = g_build_filename (datadir, "gmime-cert.p12", NULL);
		import_key (ctx, key);
		g_free (key);
		
		testsuite_check_passed ();
	} catch (ex) {
		testsuite_check_failed ("GMimePkcs7Context::import failed: %s", ex->message);
		return EXIT_FAILURE;
	} finally;
	
	testsuite_check ("multipart/signed");
	try {
		test_multipart_signed (ctx);
		testsuite_check_passed ();
	} catch (ex) {
		testsuite_check_failed ("multipart/signed failed: %s", ex->message);
	} finally;
	
	testsuite_check ("multipart/encrypted");
	try {
		test_multipart_encrypted (ctx, FALSE);
		testsuite_check_passed ();
	} catch (ex) {
		testsuite_check_failed ("multipart/encrypted failed: %s", ex->message);
	} finally;
	
	testsuite_check ("multipart/encrypted+sign");
	try {
		test_multipart_encrypted (ctx, TRUE);
		testsuite_check_passed ();
	} catch (ex) {
		testsuite_check_failed ("multipart/encrypted+sign failed: %s", ex->message);
	} finally;
	
	g_object_unref (ctx);
	
	testsuite_end ();
	
	g_mime_shutdown ();
	
	if (testsuite_destroy_gpghome () != 0)
		return EXIT_FAILURE;
	
	return testsuite_exit ();
}
