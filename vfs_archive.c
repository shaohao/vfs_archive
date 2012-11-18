#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <deadbeef/deadbeef.h>

#include <archive_entry.h>
#include <archive.h>

//-----------------------------------------------------------------------------

#ifdef DEBUG
#define trace(...) { fprintf(stderr, __VA_ARGS__); }
#else
#define trace(fmt,...)
#endif

#define min(x,y) ((x)<(y)?(x):(y))

static DB_functions_t *deadbeef;
static DB_vfs_t plugin;

typedef struct {
	DB_FILE file;

	char *aname;
	char *fname;

	struct archive *a;

	int64_t offset;
	int64_t size;
} archive_file_t;

static const char *scheme_names[] = {
	"7z://",
	"tar://",
	"cpio://",
	"iso://",
//	"zip://", /* supported by vfs_zip */
	"ar://",
	"cab://",
	"lha://",
	"rar://",
	"xar://",
	NULL,
};

struct archive_entry *
open_archive_entry(struct archive *a, const char *aname, const char *fname)
{
	struct archive_entry *ae;
	int found;

	if (!a)
		return NULL;

	if (ARCHIVE_OK != archive_read_open_filename(a, aname, 10240))
		return NULL;

	// find the desired file from archive
	trace("searching file %s\n", fname);
	found = 0;
	while (ARCHIVE_OK == archive_read_next_header(a, &ae)) {
		if (!strcmp(fname, archive_entry_pathname(ae))) {
			trace("file %s found\n", fname);
			found = 1;
			break;
		}
		archive_read_data_skip(a);
	}

	return found ? ae : NULL;
}

//-----------------------------------------------------------------------------

const char **
vfs_archive_get_schemes(void)
{
	return scheme_names;
}

int
vfs_archive_is_streaming(void)
{
	return 0;
}

// fname must have form of zip://full_filepath.zip:full_filepath_in_zip
DB_FILE*
vfs_archive_open(const char *fname)
{
	trace("[vfs_archive_open] %s\n", fname);
	int found = 0;
	for (const char **p = &(scheme_names[0]); *p; p++) {
		if (!strncasecmp(fname, *p, strlen(*p))) {
			found = 1;
			break;
		}
	}
	if (!found)
		return NULL;

	// get the full path of this archive file
	fname += 6;
	const char *colon = strchr(fname, ':');
	if (!colon) {
		return NULL;
	}

	char aname[colon-fname+1];
	memcpy(aname, fname, colon-fname);
	aname[colon-fname] = '\0';

	// get the compressed file entry in this archive
	fname = colon+1;

	struct archive *a = archive_read_new();

	archive_read_support_format_all(a);
	archive_read_support_filter_all(a);

	struct archive_entry *ae = open_archive_entry(a, aname, fname);
	if (!ae)
		return NULL;

	archive_file_t *af = (archive_file_t *)malloc(sizeof(archive_file_t));
	memset(af, 0, sizeof(archive_file_t));
	af->file.vfs = &plugin;
	af->aname = (char *)calloc((strlen(aname) + 1), sizeof(char));
	strcpy(af->aname, aname);
	af->fname = (char *)calloc((strlen(fname) + 1), sizeof(char));
	strcpy(af->fname, fname);
	af->a = a;
	af->offset = 0;
	af->size = archive_entry_size(ae);

	return (DB_FILE*)af;
}

void
vfs_archive_close(DB_FILE *f)
{
	archive_file_t *af = (archive_file_t *)f;

	if (af->a)
		archive_read_free(af->a);

	if (af->aname)
		free(af->aname);

	if (af->fname)
		free(af->fname);

	free(af);
}

size_t
vfs_archive_read(void *ptr, size_t size, size_t nmemb, DB_FILE *f)
{
	trace("[vfs_archive_read]\n");
	archive_file_t *af = (archive_file_t *)f;

	ssize_t rb = archive_read_data(af->a, ptr, size * nmemb);
	if (rb < 0)
		rb = 0;

	af->offset += rb;

	return rb / size;
}

int
vfs_archive_seek(DB_FILE *f, int64_t offset, int whence)
{
	trace("[vfs_archive_seek]");
	archive_file_t *af = (archive_file_t *)f;

	if (whence == SEEK_CUR) {
		offset = af->offset + offset;
	}
	else if (whence == SEEK_END) {
		offset = af->size + offset;
	}

	if (offset < 0 || offset > af->size)
		return -1;
	else if (offset < af->offset) {
		/* reopen */
		archive_read_free(af->a);

		af->a = archive_read_new();
		archive_read_support_format_all(af->a);
		archive_read_support_filter_all(af->a);

		if (!open_archive_entry(af->a, af->aname, af->fname))
			return -1;

		af->offset = 0;
	}

	char buf[4096];
	int64_t n = offset - af->offset;
	while (n > 0) {
		int sz = min(n, sizeof(buf));
		ssize_t rb = archive_read_data(af->a, buf, sz);
		n -= rb;
		assert(n >= 0);
		af->offset += rb;
		if (rb != sz)
			break;
	}
	if (n > 0)
		return -1;

	return 0;
}

int64_t
vfs_archive_tell(DB_FILE *f)
{
	archive_file_t *af = (archive_file_t *)f;
	return af->offset;
}

void
vfs_archive_rewind(DB_FILE *f)
{
	archive_file_t *af = (archive_file_t *)f;

	/* reopen */
	archive_read_free(af->a);

	af->a = archive_read_new();

	archive_read_support_format_all(af->a);
	archive_read_support_filter_all(af->a);

	assert(open_archive_entry(af->a, af->aname, af->fname));

	af->offset = 0;
}

int64_t
vfs_archive_getlength(DB_FILE *f)
{
	archive_file_t *af = (archive_file_t *)f;
	return af->size;
}

int
vfs_archive_scandir(
	const char *dir,
	struct dirent ***namelist,
	int (*selector)(const struct dirent *),
	int (*cmp)(const struct dirent **, const struct dirent **)
)
{
	int n;

	struct archive *a;
	struct archive_entry *ae;

	a = archive_read_new();

	archive_read_support_format_all(a);
	archive_read_support_filter_all(a);

	if (ARCHIVE_OK != archive_read_open_filename(a, dir, 10240))
		return -1;

	n = 0;
	while (ARCHIVE_OK == archive_read_next_header(a, &ae)) {
		*namelist = realloc(*namelist, sizeof(void *) * (n + 1));
		(*namelist)[n] = (struct dirent *)malloc(sizeof(struct dirent));
		memset((*namelist)[n], 0, sizeof(struct dirent));
		snprintf(
			(*namelist)[n]->d_name,
			sizeof((*namelist)[n]->d_name),
			"rar://%s:%s", dir, archive_entry_pathname(ae)
		);

		archive_read_data_skip(a);

		n++;
	}

	archive_read_free(a);

	return n;
}

int
vfs_archive_is_container(const char *fname)
{
	const char *ext = strrchr(fname, '.');
	int found = 0;
	for (const char **p = &(scheme_names[0]); *p; p++) {
		char *colon = strchr(*p, ':');
		if (!strncasecmp(ext+1, *p, (colon - *p))) {
			found = 1;
			break;
		}
	}
	return found;
}

/* boilerplate */

static DB_vfs_t plugin = {
	.plugin.api_vmajor = 1,
	.plugin.api_vminor = 0,
	.plugin.version_major = 1,
	.plugin.version_minor = 0,
	.plugin.type = DB_PLUGIN_VFS,
	.plugin.id = "vfs_archive",
	.plugin.name = "Archive vfs",
	.plugin.descr = "play files directly from archive files",
	.plugin.copyright =
		"Copyright (C) 2012 Shao Hao <shaohao@users.sourceforge.net>\n"
		"\n"
		"This program is free software; you can redistribute it and/or\n"
		"modify it under the terms of the GNU General Public License\n"
		"as published by the Free Software Foundation; either version 2\n"
		"of the License, or (at your option) any later version.\n"
		"\n"
		"This program is distributed in the hope that it will be useful,\n"
		"but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
		"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
		"GNU General Public License for more details.\n"
		"\n"
		"You should have received a copy of the GNU General Public License\n"
		"along with this program; if not, write to the Free Software\n"
		"Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n",
	.plugin.website = "http://github.com/shaohao/archive_archive",
	.open = vfs_archive_open,
	.close = vfs_archive_close,
	.read = vfs_archive_read,
	.seek = vfs_archive_seek,
	.tell = vfs_archive_tell,
	.rewind = vfs_archive_rewind,
	.getlength = vfs_archive_getlength,
	.get_schemes = vfs_archive_get_schemes,
	.is_streaming = vfs_archive_is_streaming,
	.is_container = vfs_archive_is_container,
	.scandir = vfs_archive_scandir,
};

DB_plugin_t *
vfs_archive_load(DB_functions_t *api)
{
	deadbeef = api;
	return DB_PLUGIN(&plugin);
}

