/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 *  Authors: Jeffrey Stedfast <fejj@ximian.com>
 *
 *  Copyright 2001 Ximian, Inc. (www.ximian.com)
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
 *  Foundation, Inc., 59 Temple Street #330, Boston, MA 02111-1307, USA.
 *
 */


#ifndef __GMIME_STREAM_FS_H__
#define __GMIME_STREAM_FS_H__

#ifdef __cplusplus
extern "C" {
#pragma }
#endif /* __cplusplus */

#include <unistd.h>
#include <gmime/gmime-stream.h>

#define GMIME_TYPE_STREAM_FS            (g_mime_stream_fs_get_type ())
#define GMIME_STREAM_FS(obj)            (GMIME_CHECK_CAST ((obj), GMIME_TYPE_STREAM_FS, GMimeStreamFs))
#define GMIME_STREAM_FS_CLASS(klass)    (GMIME_CHECK_CLASS_CAST ((klass), GMIME_TYPE_STREAM_FS, GMimeStreamFsClass))
#define GMIME_IS_STREAM_FS(obj)         (GMIME_CHECK_TYPE ((obj), GMIME_TYPE_STREAM_FS))
#define GMIME_IS_STREAM_FS_CLASS(klass) (GMIME_CHECK_CLASS_TYPE ((klass), GMIME_TYPE_STREAM_FS))
#define GMIME_STREAM_FS_GET_CLASS(obj)  (GMIME_CHECK_GET_CLASS ((obj), GMIME_TYPE_STREAM_FS, GMimeStreamFsClass))

typedef struct _GMimeStreamFs GMimeStreamFs;
typedef struct _GMimeStreamFsClass GMimeStreamFsClass;

struct _GMimeStreamFs {
	GMimeStream parent_object;
	
	gboolean owner;
	gboolean eos;
	int fd;
};

struct _GMimeStreamFsClass {
	GMimeStreamClass parent_class;
	
};


GType g_mime_stream_fs_get_type (void);

GMimeStream *g_mime_stream_fs_new (int fd);
GMimeStream *g_mime_stream_fs_new_with_bounds (int fd, off_t start, off_t end);

gboolean g_mime_stream_fs_get_owner (GMimeStreamFs *stream);
void g_mime_stream_fs_set_owner (GMimeStreamFs *stream, gboolean owner);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __GMIME_STREAM_FS_H__ */
