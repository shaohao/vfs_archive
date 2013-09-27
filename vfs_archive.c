#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <deadbeef/deadbeef.h>

#include <archive_entry.h>
#include <archive.h>

/*---------------------------------------------------------------------------*/

#ifdef DEBUG
#define trace(...) { fprintf(stderr, __VA_ARGS__); }
#else
#define trace(fmt,...)
#endif

#define min(x,y) ((x)<(y)?(x):(y))

static DB_functions_t *deadbeef;
static DB_vfs_t plugin;

#define POOL_SIZE (1024*5)

typedef struct {
	void *data;

	size_t size;
	size_t offset;
	size_t end;
	size_t isfull;
} cbuffer_t;

typedef struct {
	DB_FILE file;

	char *aname;
	char *fname;

	struct archive *a;

	int64_t offset;
	int64_t size;

	cbuffer_t *buffer;
} archive_file_t;

/* Zip format is supported by the builtin vfs_zip plugin */
#define DEFAULT_FORMATS "tar;par;cpio;iso;ar;xar;lha;lzh;rar;cab;7z;xz"
#define DEFAULT_FILTERS "gz;bz2;Z;uu;xz;lzip;lzma"
#define FMT_MAX 200
static char schemes[FMT_MAX];
static const char *scheme_names[FMT_MAX] = { NULL };

#define FORMAT_KEY "archive.formats"
#define FILTER_KEY "archive.filters"

/*---------------------------------------------------------------------------*/

void
malloc_cbuffer(cbuffer_t *buffer, size_t sz)
{
	buffer->data = calloc(sz, sizeof(char));
	buffer->size = sz;
}

void
free_cbuffer(cbuffer_t *buffer)
{
	free(buffer->data);
}

void
init_cbuffer(cbuffer_t *buffer)
{
	buffer->end = 0;
	buffer->offset = 0;
	buffer->isfull = 0;
}

size_t
read_from_cbuffer(cbuffer_t *buffer, void *ptr, size_t sz)
{
	size_t valid;

	if (buffer->offset <= buffer->end)
		valid = buffer->end - buffer->offset;
	else
		valid = buffer->end + (buffer->size - buffer->offset);

	if (0 == valid)
		return 0;

	size_t read_sz = min(valid, sz);

	if (buffer->offset < buffer->end) {
		memcpy(ptr, buffer->data + buffer->offset, read_sz);
		buffer->offset += read_sz;
	}
	else {
		size_t offset_gap = buffer->size - buffer->offset;
		if (offset_gap >= read_sz) {
			memcpy(ptr, buffer->data + buffer->offset, read_sz);
			buffer->offset += read_sz;
		}
		else {
			memcpy(ptr, buffer->data + buffer->offset, offset_gap);
			memcpy(ptr, buffer->data, read_sz - offset_gap);
			buffer->offset = read_sz - offset_gap;
		}
	}

	return read_sz;
}

void
write_to_cbuffer(cbuffer_t *buffer, const void *ptr, size_t sz)
{
	assert(buffer->offset == buffer->end);

	size_t write_sz = min(sz, buffer->size);
	const void *write_ptr = ptr + (sz - write_sz);
	size_t end_gap = buffer->size - buffer->end;
	if (end_gap >= write_sz) {
		memcpy(buffer->data + buffer->end, write_ptr, write_sz);
		buffer->end += write_sz;
		if (!buffer->isfull && buffer->end >= buffer->size)
			buffer->isfull = 1;
	}
	else {
		memcpy(buffer->data + buffer->end, write_ptr, end_gap);
		memcpy(buffer->data, write_ptr + end_gap, write_sz - end_gap);
		buffer->end = write_sz - end_gap;
		buffer->isfull = 1;
	}

	buffer->offset = buffer->end;
}

int64_t
seek_in_cbuffer(cbuffer_t *buffer, int64_t delta)
{
	if (delta == 0)
		return 0;

	size_t valid;
	if (buffer->offset <= buffer->end)
		valid = buffer->end - buffer->offset;
	else
		valid = buffer->end + (buffer->size - buffer->offset);

	size_t bvalid;
	if (buffer->isfull)
		bvalid = buffer->size - valid;
	else
		bvalid = buffer->end - valid;

	/* seek delta is too large */
	if (   (delta > 0 && delta > valid)
		|| (delta < 0 && -delta > bvalid)
	) {
		return -1;
	}

	if (delta < 0) {
		buffer->offset += delta;
		if (buffer->offset < 0)
			buffer->offset += buffer->size;
	}
	else /* if (delta > 0) */ {
		buffer->offset += delta;
		if (buffer->offset >= buffer->size)
			buffer->offset -= buffer->size;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/

struct archive_entry *
open_archive_entry(struct archive *a, const char *aname, const char *fname)
{
	struct archive_entry *ae;
	int found;

	if (!a)
		return NULL;

	if (ARCHIVE_OK != archive_read_open_filename(a, aname, 10240))
		return NULL;

	/* find the desired file from archive */
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

size_t
read_data(archive_file_t *af, void *ptr, size_t sz)
{
	size_t n;

	n = read_from_cbuffer(af->buffer, ptr, sz);

	if (n < sz) {
		ssize_t rb = archive_read_data(af->a, ptr+n, sz-n);
		if (rb < 0)
			rb = 0;

		if (rb) {
			write_to_cbuffer(af->buffer, ptr+n, rb);
			n += rb;
		}
	}

	af->offset += n;

	return n;
}

int
seek_data(archive_file_t *af, int64_t offset)
{
	if (0 == seek_in_cbuffer(af->buffer, offset - af->offset)) {
		af->offset = offset;
		return 0;
	}

	if (offset < af->offset) {
		/* reopen */
		archive_read_free(af->a);

		af->a = archive_read_new();
		archive_read_support_format_all(af->a);
		archive_read_support_filter_all(af->a);

		if (!open_archive_entry(af->a, af->aname, af->fname))
			return -1;

		af->offset = 0;
		init_cbuffer(af->buffer);
	}

	char buf[4096];
	int64_t n = offset - af->offset;
	while (n > 0) {
		size_t sz = min(n, sizeof(buf));
		size_t rb = read_data(af, buf, sz);
		n -= rb;
		assert(n >= 0);
		if (rb != sz)
			break;
	}
	if (n > 0)
		return -1;

	return 0;
}

void
ext2scheme(const char *exts, char **schemes_p)
{
	const char *p;
	char scheme[10];
	int i;

	i = 0;
	for (p = exts; *p; p++) {
		if (*p == ';') {
			scheme[i] = '\0';
			sprintf(*schemes_p, "%s://", scheme);
			*schemes_p += strlen(scheme) + 3 + 1;
			i = 0;
		}
		else {
			scheme[i++] = *p;
		}
	}
	if (i) {
		scheme[i] = '\0';
		sprintf(*schemes_p, "%s://", scheme);
		*schemes_p += strlen(scheme) + 3 + 1;
	}
}

void
load_scheme_names(void)
{
	char formats[100];
	char filters[100];
	int i;
	char *p;

	if (scheme_names[0])
		return ;

	p = schemes;

	deadbeef->conf_get_str(
		FORMAT_KEY,
		DEFAULT_FORMATS,
		formats,
		sizeof(formats)
	);
	ext2scheme(formats, &p);

	deadbeef->conf_get_str(
		FILTER_KEY,
		DEFAULT_FILTERS,
		filters,
		sizeof(filters)
	);
	ext2scheme(filters, &p);

	i = 0;
	p = schemes;
	while (*p) {
		scheme_names[i++] = p;
		trace("scheme_names: %s\n", p);
		p += strlen(p) + 1;

	}
	scheme_names[i] = '\0';
}

/*---------------------------------------------------------------------------*/

const char **
vfs_archive_get_schemes(void)
{
	trace("[vfs_archive_get_schemes]\n");
	load_scheme_names();

	return scheme_names;
}

int
vfs_archive_is_streaming(void)
{
	trace("[vfs_archive_is_streaming]\n");
	return 0;
}

/* fname must have form of zip://full_filepath.zip:full_filepath_in_zip */
DB_FILE*
vfs_archive_open(const char *fname)
{
	trace("[vfs_archive_open] %s\n", fname);
	const char *scheme = NULL;
	for (const char **p = &(scheme_names[0]); *p; p++) {
		if (!strncasecmp(fname, *p, strlen(*p))) {
			scheme = *p;
			break;
		}
	}
	if (!scheme)
		return NULL;

	/* get the full path of this archive file */
	fname += strlen(scheme);
	const char *colon = strchr(fname, ':');
	if (!colon) {
		return NULL;
	}

	char aname[colon-fname+1];
	memcpy(aname, fname, colon-fname);
	aname[colon-fname] = '\0';

	/* get the compressed file entry in this archive */
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

	af->buffer = (cbuffer_t *)malloc(sizeof(cbuffer_t));
	malloc_cbuffer(af->buffer, POOL_SIZE);
	init_cbuffer(af->buffer);

	return (DB_FILE*)af;
}

void
vfs_archive_close(DB_FILE *f)
{
	trace("[vfs_archive_close]\n");
	archive_file_t *af = (archive_file_t *)f;

	archive_read_free(af->a);

	free(af->aname);

	free(af->fname);

	free_cbuffer(af->buffer);
	free(af->buffer);

	free(af);
}

size_t
vfs_archive_read(void *ptr, size_t size, size_t nmemb, DB_FILE *f)
{
	archive_file_t *af = (archive_file_t *)f;

	trace("[vfs_archive_read] sz: %d, offset: %ld\n", (int)(size * nmemb), af->offset);

	size_t rb = read_data(af, ptr, size * nmemb);
	return rb / size;
}

int
vfs_archive_seek(DB_FILE *f, int64_t offset, int whence)
{
	archive_file_t *af = (archive_file_t *)f;

	/* try builtin seek function first */
	if (0 <= archive_seek_data(af->a, offset, whence))
		return 0;

	if (whence == SEEK_CUR) {
		offset = af->offset + offset;
	}
	else if (whence == SEEK_END) {
		offset = af->size + offset;
	}

	if (offset < 0 || offset > af->size)
		return -1;

	trace("[vfs_archive_seek] old-offset: %ld, new-offset: %ld\n", af->offset, offset);

	return seek_data(af, offset);
}

int64_t
vfs_archive_tell(DB_FILE *f)
{
	archive_file_t *af = (archive_file_t *)f;
	trace("[vfs_archive_tell] offset: %ld\n", af->offset);
	return af->offset;
}

void
vfs_archive_rewind(DB_FILE *f)
{
	trace("[vfs_archive_rewind]\n");
	archive_file_t *af = (archive_file_t *)f;

	/* reopen */
	archive_read_free(af->a);

	af->a = archive_read_new();

	archive_read_support_format_all(af->a);
	archive_read_support_filter_all(af->a);

	assert(open_archive_entry(af->a, af->aname, af->fname));

	af->offset = 0;
	init_cbuffer(af->buffer);
}

int64_t
vfs_archive_getlength(DB_FILE *f)
{
	trace("[vfs_archive_getlength]\n");
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
	trace("[vfs_archive_scandir]\n");

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
			"rar://%s:%s", dir, archive_entry_pathname(ae) /*TODO: fix rar */
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
	trace("[vfs_archive_is_container]\n");

	load_scheme_names();

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

static const char settings_dlg[] =
	"property \"Formats\" entry archive.formats \"" DEFAULT_FORMATS "\";\n"
	"property \"Filters\" entry archvie.filters \"" DEFAULT_FILTERS "\";\n"
;

/* define plugin interface */
static DB_vfs_t plugin = {
	.plugin.api_vmajor = 1,
	.plugin.api_vminor = 0,
	.plugin.version_major = 2,
	.plugin.version_minor = 0,
	.plugin.type = DB_PLUGIN_VFS,
	.plugin.id = "vfs_archive",
	.plugin.name = "Archive vfs",
	.plugin.descr = "play files directly from archive files",
	.plugin.copyright =
		"Copyright (C) 2013 Shao Hao <shaohao@users.sourceforge.net>\n"
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
	.plugin.configdialog = settings_dlg,
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

