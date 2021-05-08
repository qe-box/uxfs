
/*
 *  uxfs.c - Bridge for user interface as virtual filesystem
 *  Copyright (C) 2021  Wolfgang Zekoll, <wzk@quietsche-entchen.de>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#define FUSE_USE_VERSION 34

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>

#include <stdarg.h>
#include <grp.h>
#include <pwd.h>
#include <assert.h>
#include <pthread.h>

#include <fuse.h>


#define	T_START		1
#define	T_END		2
#define T_BOTH		(T_START | T_END)

#define	MAX_ARGS	32
#define	MIN_FREE	4
#define	LINE_MAX	1024

#define	M_READ		1
#define	M_WRITE		2
#define	M_DIR		4
#define	M_USER		8
#define	M_STATIC	16


#define	R_NONE		0
#define	R_STATUS	1
#define	R_MULTI		2
#define	C_STATUS	3
#define	C_TEMP_DATA	8

#define	P_VERBOSE	8
#define	P_EXTRA		9


typedef struct _buffer {
    int		mode;

    int		here, end;
    int		size;
    char	*buffer;
    } buf_t;


typedef struct _file {
    char	path[FILENAME_MAX];
    int		mode;
    time_t	mtime;

    int		inode;
    int		used;
    int		deleted;
    buf_t	*buf;		/* M_USER files store the data. */
    } file_t;

typedef struct _dir {

    /* file[0 .. max] has [0 .. len] valid entries. */
    file_t	**file;
    int		len, max;
    } dir_t;

static int add_file(dir_t *d, const char *path, const int mode);



typedef struct _uxfs {
    int		debug;
    int		verbose;
    int		foreground;
    int		single_thread;
    int		other_users;

    char	*mountpoint;
    uid_t	uid;
    gid_t	gid;

    struct {
	int	fd0, fd1;
	buf_t	buf;

	int	argc;
	char	*argv[MAX_ARGS];
	pid_t	pid;
	} co;

    int		n_open, n_close;
    int		inode_count;

    dir_t	dir;	/* Everything is stored in one directory list. */
    } uxfs_t;

static int add_file_from_definition(char *line);

static file_t *f_alloc();

static int do_open(const char *path, struct fuse_file_info *fi);

static uxfs_t uxfs;
static pthread_mutex_t lock;



#define UXFS_OPT(t, p, v) { t, offsetof(uxfs_t, p), v }

#define	OPT_FOREGROUND		1
#define	OPT_DEBUG		2
#define	OPT_VERBOSE		3
#define	OPT_OTHER_USERS		4
#define	OPT_SINGLE_THREAD	5

static struct fuse_opt uxfs_opts[] = {
    UXFS_OPT("dbg=%u",		debug, 0),

    FUSE_OPT_KEY("-f",		OPT_FOREGROUND),
    FUSE_OPT_KEY("-d",		OPT_DEBUG),
    FUSE_OPT_KEY("-v",		OPT_VERBOSE),
    FUSE_OPT_KEY("-o",		OPT_OTHER_USERS),
    FUSE_OPT_KEY("-s",		OPT_SINGLE_THREAD),

    FUSE_OPT_END
    };



static void __exit()
{
	struct fuse_context *ctx = fuse_get_context();
	struct fuse *fo = ctx->fuse;

	fuse_exit (fo);
}

static int printerror(int rc, char *type, char *format, ...)
{
	int	verbose = 0, print = 0;
	char	tag[30], error[400];
	va_list	ap;
	static char program[] = "uxfs";

	va_start(ap, format);
	vsnprintf (error, sizeof(error) - 2, format, ap);
	va_end(ap);

	if (rc == P_VERBOSE) {
		verbose = 1;
		rc = 0;
		}
	else if (rc == P_EXTRA) {
		verbose = 2;
		rc = 0;
		}

	*tag = '\0';
	if (*type != '\0')
		snprintf (tag, sizeof(tag) - 2, "%s: ", type);

	print = (rc != 0)  ||  (*type == '-')  ||
			(uxfs.verbose != 0  &&  verbose == 1)  ||
			(uxfs.verbose == 2  &&  verbose == 2);

	if (print != 0)
		fprintf (stderr, "%s: %s%s\n", program, tag, error);

	if (rc != 0)
		__exit ();

	return (0);
}



static char *m_trim(char *s, int mode)
{
	if (mode == 0  ||  (mode & T_END) != 0) {
		int	i, len = strlen(s);

		for (i = len - 1; i >= 0  &&  s[i] <= ' '; i++)
			s[i] = '\0';
		}

	if (mode == 0  ||  (mode & T_START) != 0) {
		while (*s != '\0'  &&  *s <= ' ')
			s++;
		}

	return (s);
}

static char *m_copy(char *d, const char *s, int len)
{
	char	c;
	int	k;

	for (k = 0; k < len - MIN_FREE  &&  (c = s[k]) != '\0'; k++)
		d[k] = c;

	d[k] = '\0';
	return (d);
}

static char *m_getword(char **from, int delim, char *to, int max)
{
	char	c;
	int	k;

	/*
	 * Jump over leading white space.
	 */

	while ((c = **from) == ' '  ||  c == '\t')
		*from += 1;

	to[k = 0] = '\0';
	max -= 2;

	/*
	 * Copy the text until the delimeter or EOL to
	 * the buffer.
	 */

	while ((c = **from) != '\0') {
		*from += 1;
		if (c == delim)
			break;

		if (k < max)
			to[k++] = c;
		}

	to[k] = '\0';
	return (to);
}


/*
 * Buffer spaces
 */

static buf_t *b_alloc()
{
	buf_t *b = malloc(sizeof(buf_t));
	memset(b, 0, sizeof(buf_t));
	return (b);
}

static void b_free(buf_t *b)
{
	if (b != NULL) {
		if (b->buffer != NULL)
			free (b->buffer);
		
		free (b);
		}
}

static buf_t *b_clear(buf_t *b)
{
	b->here = b->end = 0;
	if (b->buffer == NULL) {
		b->size = LINE_MAX;
		b->buffer = malloc(b->size);
		}

	return (b);
}

static void b_append_line(buf_t *b, const char *line)
{
	int len = strlen(line);
	if (b->end + len + MIN_FREE > b->size) {
		b->size += len + MIN_FREE + LINE_MAX;
		b->buffer = realloc(b->buffer, b->size);
		}

	strcpy(&b->buffer[b->end], line);
	b->end += len;
	b->buffer[b->end++] = '\n';
	b->buffer[b->end]   = '\0';
}

static buf_t *b_from_strings(int count, ...)
{
	int	i;
	char	*line;
	buf_t	*b = b_alloc();
	va_list	ap;

	va_start(ap, count);
	for (i = 0; i < count; i++) {
		line = va_arg(ap, char *);
		b_append_line(b, line);
		}

	va_end(ap);
	return (b);
}

static char *b_getline(buf_t *b, char *line, int size)
{
	int	c, k;

	/*
	 * Look for a line terminator between b->here and b->end,
	 * copy the data to line and keep the buffer intact.
	 *
	 * Q: Merge with b_gets()?
	 */

	size -= MIN_FREE;
	if (b->here >= b->end)
		return (NULL);

	k = 0;
	while (b->here < b->end) {
		if ((c = b->buffer[b->here++]) == '\n') {
			line[k] = '\0';
			return (line);
			}

		if (k < size)
			line[k++] = c;
		}

	line[k] = '\0';
	return (line);
}

static int b_gets(buf_t *b, char *line, int size)
{
	int	c, have_line = 0, len;

	/*
	 * Look for a line terminator between b->here and b->end.
	 */

	while (b->here < b->end) {
		c = b->buffer[b->here++];

		if (c == '\n') {

			/*
			 * Found one.
			 */

			have_line = 1;
			b->buffer[b->here - 1] = '\0';

			m_copy(line, b->buffer, size);

			/*
			 * Remove the line from the buffer and move
			 * the remaining data to the begin of the
			 * buffer.
			 */

			len = b->end - b->here;
			memmove(b->buffer, &b->buffer[b->here], len);
			b->here = 0;
			b->end  = len;

			break;
			}
		}

	return (have_line);
}

static buf_t *b_copy(buf_t *d, buf_t *s)
{
	d->mode = s->mode;
	d->here = s->here;
	d->end  = s->end;
	d->size = s->size;
	d->buffer = malloc(d->size);
	memmove(d->buffer, s->buffer, d->size);

	return (d);
}

static buf_t *b_buffer_to_file(file_t *f, buf_t *b)
{
	if (f->buf != NULL)
		free(f->buf);

	f->buf = b;
	return (NULL);
}


/*
 * I/O with the controller.
 */

static int c_start_server()
{
	int	pfd0[2], pfd1[2];
	pid_t	pid = -1;

	if (pipe(pfd0) != 0  ||  pipe(pfd1) != 0)
		printerror(1, "-ERR", "can't create pipe: %s", strerror(errno));

	if ((pid = fork()) < 0)
		printerror(1, "-ERR", "can't fork(): %s", strerror(errno));
	else if (pid == 0) {

		/*
		 * Connect the process' stdin to our stdout ...
		 */

		close(pfd0[1]);
		dup2(pfd0[0], 0);
		close(pfd0[0]);

		/*
		 * ... and it's stdout to our stdin.
		 */

		close(pfd1[0]);
		dup2(pfd1[1], 1);
		close (pfd1[1]);

		if (1) {
			char	pid[20];

			/*
			 * Set some environment variables.
			 */

			setenv("UXFS_MOUNT_POINT", uxfs.mountpoint, 1);
			snprintf (pid, sizeof(pid) - 2, "%d", getppid());
			setenv("UXFS_PID", pid, 1);
			}

		execvp(uxfs.co.argv[0], uxfs.co.argv);
		printerror(1, "-ERR", "can't exec %s, error= %s",
				uxfs.co.argv[0], strerror(errno));
		exit (1);
		}

	uxfs.co.pid = pid;


	/*
	 * Save the writing side to the process' stdin ...
	 */

	uxfs.co.fd0 = pfd1[0];
	close(pfd0[0]);

	/*
	 * ... and the reading side of its stdout.
	 */

	uxfs.co.fd1 = pfd0[1];
	close(pfd0[0]);

	return (pid);
}

static int c_readinput(int fd, buf_t *b)
{
	int	n;

	if (b->buffer == NULL) {
		b->here = b->end = 0;
		b->size = LINE_MAX;
		b->buffer = malloc(b->size);
		}

	/*
	 * Resize buffer if necessary.
	 */

	if (b->size - b->end < LINE_MAX) {
		b->size += LINE_MAX;
		b->buffer = realloc(b->buffer, b->size);
		}

	/*
	 * Read the input ...
	 */

	n = read(fd, &b->buffer[b->end], b->size - b->end - 2);
	b->end += n;

	return (n);
}

static char *c_gets(char *line, const int size, const int debug)
{
	while (b_gets(&uxfs.co.buf, line, size) == 0) {
		if (c_readinput(uxfs.co.fd0, &uxfs.co.buf) <= 0) {
			printerror(1, "-ERR", "controller closed connecction");
			return (NULL);
			}
		}

	if (debug != 0  &&  uxfs.debug != 0)
		fprintf (stderr, "<< %s\n", line);

	return (line);
}

static int c_puts(int debug, const char *format, ...)
{
	int	n, m;
	char	line[LINE_MAX];
	va_list	ap;

	va_start(ap, format);
	n = vsnprintf (line, sizeof(line) - 2, format, ap);
	va_end(ap);

	if (uxfs.debug != 0  &&  debug != 0)
		fprintf (stderr, ">> %s\n", line);

	if ((m = write(uxfs.co.fd1, line, n)) != strlen(line)) {
		printerror(1, "-ERR", "server closed connection");
		n = -1;
		}

	return (n);
}


  /*
   * c_putc() sends `cmd` with optional arguments (`formmat`
   * parameter) to the controller and read the response
   * inndicated by `resp` into `b`.
   */

static int c_putc(const char *cmd, const char *par,
			const int flags, buf_t *data, buf_t *reply) {
	int	rc = 0;
	char	*p, line[LINE_MAX];

	if (cmd != NULL) {

		/*
		 * Send the command and parameter.
		 */

		if (par != NULL  &&  *par != '\0')
			rc = c_puts(1, "%s %s\n", cmd, par);
		else
			rc = c_puts(1, "%s\n", cmd);

		if (rc <= 0)
			return (1);


		/*
		 * Send data to the controller.
		 */

		if (data != NULL) {
			data->here = 0;
			while ((p = b_getline(data, line, sizeof(line))) != NULL) {
				if (*p == '.') {
					c_puts(0, ".%s\n", p);
					continue;
					}

				c_puts(0, "%s\n", p);
				}

			c_puts(0, ".\n");
			}

		if ((flags & C_TEMP_DATA) != 0)
			b_free(data);
		}

	if ((flags & C_STATUS) != R_NONE) {

		/*
		 * Read the controller's response.
		 */

		char	*p, *s, token[40], response[200];
		char	data[LINE_MAX], line[LINE_MAX];

		if ((p = c_gets(line, sizeof(line), 1)) == NULL)
			return (1);

		/*
		 * Read the first line and get the status token.
		 */

		s = m_getword(&p, ';', response, sizeof(response));
		m_getword(&s, ' ', token, sizeof(token));
		if (strcmp(token, "+OK") == 0)
			rc = 0;
		else if (strcmp(token, "-ERR") == 0)
			rc = 1;
		else {
			/* This initiates termination of uxfs */
			printerror(1, "-ERR", "protocol error: %s", line);
			return (1);
			}

		if (rc == 0  &&  (flags & C_STATUS) == R_MULTI) {
			char	rbuf[LINE_MAX];

			/*
			 * Read the data response for the REQUEST
			 * command.
			 */

			b_clear(reply);
			while (c_gets(rbuf, sizeof(rbuf), 0) != NULL) {
				if (strcmp(rbuf, ".") == 0)
					break;
				else if (rbuf[0] == '.') {
					b_append_line(reply, &rbuf[1]);
					continue;
					}

				b_append_line(reply, rbuf);
				}
			}

		/*
		 * Read further responses from the first line
		 * and interpret the commands.
		 */

		while (*(s = m_getword(&p, ';', response, sizeof(response))) != '\0') {
			m_getword(&s, ' ', token, sizeof(token));
			if (strcmp(token, "QUIT") == 0) {
				__exit();
				return (0);
				}
			else if (strcmp(token, "DIR") == 0) {
				while (c_gets(data, sizeof(data), 0) != NULL) {
					if (strcmp(data, ".") == 0)
						break;

					add_file_from_definition(data);
					}
				}
			else {
				/* Again, terminate. */
				printerror(1, "-ERR", "protocol error: %s", response);
				return (1);
				}
			}
		}

	return (rc);
}




/*
 * Directory operations.
 */

static int d_search_file(const dir_t *d, const char *path, int *pos)
{
	int	r = -2, k, left = 0, right = d->len - 1;
	file_t	*f;

	if (d->len == 0) {
		*pos = 0;
		return (1);
		}

	while (left <= right) {
		*pos = k = (left + right) / 2;
		f = d->file[k];

		if ((r = strcmp(path, f->path)) == 0)
			return (0);
		else if (r < 0)
			right = k - 1;
		else
			left = k + 1;

		f = NULL;
		}

	return (r);
}

static int d_get_parent(const dir_t *d, const char *path, int *pos)
{
	char	*p, dn[FILENAME_MAX];

	printerror(P_VERBOSE, "", "d_get_parent(%s)", path);
	m_copy(dn, path, sizeof(dn));
	//if ((p = strrchr(dn, '/')) == NULL  ||  p == dn)
	//	return (-1);
	if ((p = strrchr(dn, '/')) == NULL)
		return (-1);
	else if (p == dn)
		p++;

	*p = '\0';
	printerror(P_VERBOSE, "", "d_search_file(%s)", dn);
	return (d_search_file(d, dn, pos));
}

static int d_getattr(const file_t *f, struct stat *st)
{
	int	perm = 0;

	/*
	 * Set constant values.
	 */

	st->st_uid     = getuid();
	st->st_gid     = getgid();
	st->st_mtime   = f->mtime;
	st->st_blksize = 512;
	st->st_blocks  = 0;

	if (f == NULL)
		return (0);

	/*
	 * Set the file attributes.
	 */

	perm |= (f->mode & M_WRITE? S_IWUSR: 0);
	perm |= (f->mode & M_READ?  S_IRUSR: 0);
	st->st_ino = f->inode;

	if (strcmp(f->path, "/") == 0) {
		st->st_mode = S_IFDIR | 0775;
		st->st_nlink = 2;
		st->st_blocks = 2;
		st->st_size = 4096;
		}
	else if ((f->mode & M_DIR) != 0) {
		st->st_mode = S_IFDIR | perm | S_IXUSR;
		st->st_nlink = 2;
		st->st_blocks = 2;
		st->st_size = 4096;
		}
	else {
		st->st_mode = S_IFREG | perm;
		st->st_nlink = 1;

		if (f->mode & (M_STATIC | M_USER)) {
			if (f->buf != NULL)
				st->st_size = f->buf->end;
			}
		}

	if (uxfs.other_users) {
		if (st->st_mode & S_IRUSR)
			st->st_mode |= (S_IRGRP | S_IROTH);

		if (st->st_mode & S_IWUSR)
			st->st_mode |= (S_IWGRP | S_IWOTH);

		if (st->st_mode & S_IXUSR)
			st->st_mode |= (S_IXGRP | S_IXOTH);
		}

	return (0);
}

static int d_get_modebits(const char *path, char *par)
{
	char	c;
	int	i, mode = 0;

	for (i = 0; (c = par[i]) != '\0'; i++) {
		if (c == 'r')
			mode |= M_READ;
		else if (c == 'w')
			mode |= M_WRITE;
		else if (c == 'd')
			mode |= M_DIR;
		else if (c == 's')
			mode |= (M_READ | M_WRITE | M_STATIC);
		else {
			printerror(0, "-INFO", "bad mode \"%s\" for %s; assuming \"r\"",
					par, path);
			mode |= M_READ;
			}
		}

	return (mode);
}

static char *d_get_mode(char *par, int size, int mode)
{
	int	k = 0;

	par[k++] = mode & M_DIR?	'd': '-';
	par[k++] = mode & M_READ?	'r': '-';
	par[k++] = mode & M_WRITE?	'w': '-';
	par[k++] = mode & M_STATIC?	's': '-';
	par[k++] = mode & M_USER?	'u': '-';
	par[k]   = '\0';

	return (par);
}

static int add_file(dir_t *d, const char *path, int mode)
{
	int	rc, k;

	if (strlen(path) > FILENAME_MAX)
		return (-1);

	pthread_mutex_lock(&lock);

	/*
	 * Correct some abvious mistakes.
	 */

	if ((mode & (M_WRITE | M_READ)) == 0)
		mode |= M_READ;

	if (mode & M_DIR)
		mode |= M_READ;

	/*
	 * Initialize some space if not already done ...
	 */

	if (d->max == 0) {
		d->max = 10;
		d->file = malloc(d->max * sizeof(file_t *));
		}

	/*
	 * ... and check if the file entry already exists.
	 */

	if ((rc = d_search_file(d, path, &k)) == 0) {
		/* File exists already. */
		d->file[k]->mode = mode;
		d->file[k]->deleted = 0;
		}
	else {
		/*
		 * README: All file entries in the filesystem are stored
		 * in an array of pointers.  The array is sorted by the
		 * file's path to have binary search in d_search_file().
		 *
		 * New elements are inserted into the array at their
		 * correct location and consequently indices of following
		 * file elements change.  In other words: a file's index
		 * into the array is not constant but the pointer to
		 * the file_t structure is.
		 */

		/* Insert the new file. */
		if (d->len == d->max) {
			d->max += 20;
			d->file = realloc(d->file, d->max * sizeof(file_t *));
			}

		if (d->len > 0) {
			if (rc > 0)
				k++;

			if (k < d->len) {
				memmove(&d->file[k+1], &d->file[k],
						(d->len - k) * sizeof(file_t *));
				}
			}

		file_t *f = f_alloc();
		strcpy(f->path, path);
		f->mode  = mode;
		f->mtime = time(NULL);
		f->inode = ++uxfs.inode_count;
		f->used  = 0;
		f->deleted = 0;

		d->file[k] = f;
		d->len++;
		}

	printerror(P_VERBOSE, "", "add_file(): %s %d %d (%d/%d)", path, mode,
				d->file[k]->inode, k, d->len);

	pthread_mutex_unlock(&lock);
	return (k);
}

static int add_file_from_definition(char *line)
{
	int	mode;
	char	*p, *s, path[FILENAME_MAX], mode_par[20];

	p = line;
	m_getword(&p, ' ', path, sizeof(path));
	m_getword(&p, ' ', mode_par, sizeof(mode_par));

	if (*(s = m_trim(path, T_BOTH)) == '\0')
		return (-1);
	else if (*s != '/') {
		printerror(0, "-ERR", "bad path: %s", path);
		return (-1);
		}

	mode = d_get_modebits(path, mode_par);

	/*
	 * Check for directory.
	 */

	if (strcmp(path, "/") == 0)
		mode |= M_DIR;
	else if (path[ strlen(path) - 1 ] == '/') {
		path[ strlen(path) - 1 ] = '\0';
		mode |= M_DIR;
		}
	
	return (add_file(&uxfs.dir, path, mode));
}

static file_t *getfile(dir_t *d, const char *path, int deleted)
{
	file_t	*f = NULL;

	if (d->len == 0)
		;
	else if (d->len == 1) {
		if (strcmp(d->file[0]->path, path) == 0)
			f = d->file[0];
		}
	else {
		int	k;

		if (d_search_file(d, path, &k) == 0)
			f = d->file[k];
		}

	if (f != NULL  &&  deleted == 0  &&  f->deleted != 0)
		f = NULL;

	return (f);
}		




	/*
	 * Function called from libfuse.
	 */

static file_t *f_alloc()
{
	file_t *f = malloc(sizeof(file_t));
	memset(f, 0, sizeof(file_t));
	return (f);
}

static void f_clear(file_t *f)
{
	if (f->buf != NULL) {
		b_free(f->buf);
		f->buf = NULL;
		}
}

static int f_create(const char *path, file_t **f)
{
	int	k;
	file_t	*d;

	printerror(P_VERBOSE, "", "f_create(%s)", path);
	*f = NULL;

	/*
	 * Check if the parent directory exists and allows
	 * write access.
	 */

	if (d_get_parent(&uxfs.dir, path, &k) != 0)
		return (-ENOENT);
		
	d = uxfs.dir.file[k];
	if ((d->mode & M_WRITE) == 0)
		return (-EACCES);

	k = add_file(&uxfs.dir, path, M_READ | M_WRITE | M_USER);
	*f = uxfs.dir.file[k];

	return (0);
}

static int f_open(file_t *f, int mode, struct fuse_file_info *fi)
{
	int	errno, m = 0;
	buf_t	*b;

	printerror(P_VERBOSE, "", "f_open(%s, %d)", f->path, mode & O_ACCMODE);
	errno = EACCES;
	if ((mode & O_ACCMODE) == O_RDWR) {
		/* allow opening a file for both */ ;
		if ((f->mode & (M_WRITE | M_READ)) != (M_WRITE | M_READ))
			return (-errno);

		m = M_WRITE | M_READ;
		}
	else if ((mode & O_ACCMODE) == O_WRONLY) {
		if ((f->mode & M_WRITE) == 0)
			return (-errno);

		m = M_WRITE;
		}
	else if ((mode & O_ACCMODE) == O_RDONLY) {
		if ((f->mode & M_READ) == 0)
			return (-errno);

		m = M_READ;
		}

	pthread_mutex_lock(&lock);

	b = b_alloc(sizeof(buf_t));

	/*
	 * M_USER files have different properties: they can be
	 * removed, "renamed" and their permission may change.
	 *
	 * They also have static content: Data that is written to
	 * the file is send to the controller and following read()
	 * operations read the file's content directly from the
	 * buffer in file_t.
	 */

	b->mode = m | (f->mode & M_USER);

	if ((b->mode & M_READ) != 0) {
		if ((f->mode & M_USER) != 0  &&  f->buf != NULL) {
			b_copy(b, f->buf);
			b->mode = m | (f->mode & M_USER);
			}
		else
			b->buffer = malloc(b->size = 512);

		if ((mode & O_ACCMODE) == O_RDONLY  &&
		    (f->mode & M_USER) == 0) {
			c_putc("READ", f->path, R_MULTI, NULL, b);
			}
		}
	else if ((b->mode & M_WRITE) != 0)
		b->buffer = malloc(b->size = 512);

	fi->fh = (unsigned long) b;
	fi->direct_io = 1;
	f->used++;
	uxfs.n_open++;

	pthread_mutex_unlock(&lock);
	return (0);
}


static inline buf_t *get_file_ptr(struct fuse_file_info *fi)
{
	return (buf_t *) (uintptr_t) fi->fh;
}


static void *do_init(struct fuse_conn_info *conn)
{
	uxfs.uid = getuid();
	uxfs.gid = getgid();

	c_putc("INIT", "", R_STATUS, NULL, NULL);
	return (NULL);
}

static int do_getattr(const char *path, struct stat *st)
{
	file_t	*f;

	printerror(P_EXTRA, "", "do_getattr(%s)", path);
	if ((f = getfile(&uxfs.dir, path, 0)) == NULL)
		return (-ENOENT);

	d_getattr(f, st);
	return (0);
}

static int do_create(const char *path, const mode_t mode,
			struct fuse_file_info *fi)
{
	int	rc;
	file_t	*f = NULL;

	if ((rc = f_create(path, &f)) != 0)
		return (rc);

	return ( f_open(f, O_WRONLY, fi));
}

static int do_readdir(const char *path, void *buf,
			fuse_fill_dir_t filler, off_t offset,
			struct fuse_file_info *fi)
{
	int	k, len, sp;
	char	*p;
	struct stat sbuf;
	file_t	*f;

	printerror(P_EXTRA, "", "do_readdir(\"%s\")", path);

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	if (*path == '\0'  ||  d_search_file(&uxfs.dir, path, &k) != 0) {
		errno = ENOENT;
		return (-1);
		}
	else if ((uxfs.dir.file[k]->mode & M_DIR) == 0) {
		errno = ENOTDIR;
		return (-1);
		}

	len = strlen(path);
	sp  = len;		/* Char at position sp must be `/' */
	if (strcmp(path, "/") == 0)
		sp = 0;

	k++;
	while (k < uxfs.dir.len) {
		f = uxfs.dir.file[k++];
		memset(&sbuf, 0, sizeof(sbuf));

		/*
		 * Skip the item if it is deleted.
		 */

		if (f->deleted != 0)
			continue;

		/*
		 * Check if the item is inside the requested directory.
		 */

		else if (f->path[sp] != '/'  ||  strncmp(f->path, path, len) != 0)
			break;


		/*
		 * There may be no other slash ...
		 */

		else if ((p = strchr(&f->path[sp+1], '/')) == NULL) {
			/* TODO: Use __getattr(file_t *f, struct stat *) */
			do_getattr(path, &sbuf);
			if (filler(buf, &f->path[sp+1], &sbuf, 0) != 0)
				break;
			}

		/*
		 * ... or it must be the last character (then the
		 * item is a directory).
		 */

		else if (p[1] == '\0') {
			char	name[FILENAME_MAX];

			d_getattr(f, &sbuf);
			m_copy(name, &f->path[sp+1], sizeof(name));
			name[ strlen(name) - 1 ] = '\0';
			if (filler(buf, name, &sbuf, 0) != 0)
				break;
			}
		}

	return (0);
}

static int do_open(const char *path, struct fuse_file_info *fi)
{
	file_t	*f;

	printerror(P_VERBOSE, "", "do_open(\"%s\")", path);
	fi->fh = (unsigned long) NULL;
	if ((f = getfile(&uxfs.dir, path, 0)) == NULL) {
		if (do_create(path, 0, NULL) == 0)
			return (0);

		// errno = ENOENT;
		return (-ENOENT);
		}
	else if ((f->mode & M_DIR) != 0) {
		return (-EISDIR);
		}

 	return ( f_open(f, fi->flags & O_ACCMODE, fi));
}

static int do_release(const char *path, struct fuse_file_info *fi)
{
	file_t	*f;
	buf_t	*b;

	printerror(P_VERBOSE, "", "do_release(\"%s\")", path);
	b = get_file_ptr(fi);
	if ((f = getfile(&uxfs.dir, path, 1)) != NULL) {
		f->mtime = time(NULL);
		f->used--;

		b->buffer[b->end] = '\0';
		if (b->mode & M_WRITE) {
			pthread_mutex_lock(&lock);

			c_putc("WRITE", f->path, R_STATUS, b, NULL);
			if (b->mode & M_USER)
				b = b_buffer_to_file(f, b);

			pthread_mutex_unlock(&lock);
			}
		}

	b_free(b);
	uxfs.n_close++;

	return (0);
}

static int do_truncate(const char *path, off_t size)
{
	return (0);
}

static int do_write(const char *path, const char *buf, size_t size,
			off_t offset, struct fuse_file_info *fi)
{
	buf_t	*b;

	printerror(P_EXTRA, "", "do_write(\"%s\", size= %d, offset= %d)", path, size, offset);

	b = get_file_ptr(fi);
	if ((b->mode & M_WRITE) == 0) {
		errno = EBADF;
		return (-1);
		}

	pthread_mutex_lock(&lock);
	if (b->size < offset + size + 4) {
		b->size += offset + size + 4 + 2048;
		b->buffer = realloc(b->buffer, b->size);
		}

	memmove(&b->buffer[offset], buf, size);
	if (b->end < offset + size)
		b->end = offset + size;

	pthread_mutex_unlock(&lock);
	return (size);
}

static int do_read(const char *path, char *buf, size_t size, off_t offset,
                        struct fuse_file_info *fi)
{
	int	n = 0;

	printerror(P_EXTRA, "", "do_read(size= %d, off= %d)", size, offset);
	pthread_mutex_lock(&lock);

	buf_t *b = get_file_ptr(fi);
	if ((n = b->end - offset) <= 0)
		n = 0;
	else {
		if (size < n)
			n = size;

		memmove(buf, &b->buffer[offset], n);
		}

	pthread_mutex_unlock(&lock);
	return (n);
}

static int do_access(const char *path, int mode)
{
	struct stat sbuf;

	printerror(P_VERBOSE, "", "access(%s, mode= %d)", path, mode);
	if (do_getattr(path, &sbuf) != 0)
		return (-ENOENT);

	if ((mode & R_OK)  &&  (sbuf.st_mode & S_IRUSR) == 0)
		return (-EACCES);

	if ((mode & W_OK)  &&  (sbuf.st_mode & S_IWUSR) == 0)
		return (-EACCES);

	if ((mode & X_OK)  &&  (sbuf.st_mode & S_IXUSR) == 0)
		return (-EACCES);

	return (0);
}

static int do_rename(const char *from, const char *to)
{
	int	rc;
	file_t	*src, *dst;

	printerror(P_VERBOSE, "", "rename(from= %s, to= %s)", from, to);

	/*
	 * Renaming a file is difficult.  First, the source file must
	 * have M_USER set.  Second, if the destination file exist it
	 * must be also M_USER and the directory must be writeable if
	 * the file does not exist.
	 *
	 * When all conditions are met the file_t structure of dst
	 * is free()'ed and src's file_t is copied over to dst.  Then
	 * src is marked as deleted.
	 */

	if ((src = getfile(&uxfs.dir, from, 0)) == NULL)
		return (-ENOENT);
	else if (src->mode & M_DIR)
		return (-EISDIR);	/* Only files can be moved. */
	else if ((src->mode & M_USER) == 0)
		return (-EACCES);

	if ((dst = getfile(&uxfs.dir, to, 1)) != NULL) {
		if (dst->mode & M_DIR)
			return (-EISDIR);
		else if ((dst->mode & M_USER) == 0)
			return (-EPERM);
		}
	else if ((rc = f_create(to, &dst)) == 0) {

		/*
		 * Recalculate because the pointer migth have changed.
		 */

		if ((src = getfile(&uxfs.dir, from, 0)) == NULL)
			return (-EACCES);
		}
	else
		return (rc);

	/*
	 * Source and destination meet the requirements.
	 */

	pthread_mutex_lock(&lock);
	c_putc("FILEOP", NULL, C_TEMP_DATA | R_STATUS,
			b_from_strings(3, "rename", from, to), NULL);

	f_clear(dst);
	dst->mode    = src->mode;
	dst->mtime   = time(NULL);
	dst->deleted = 0;
	dst->buf     = src->buf;

	src->buf     = NULL;
	src->deleted = 1;

	pthread_mutex_unlock(&lock);
	return (0);
}

static int do_unlink(const char *path)
{
	file_t	*f;

	printerror(P_VERBOSE, "", "unlink(path= %s)", path);
	if ((f = getfile(&uxfs.dir, path, 0)) == NULL)
		return (-ENOENT);
	else if ((f->mode & M_USER) == 0)
		return (-EPERM);
	else if (f->mode & M_DIR)
		return (-EISDIR);

	c_putc("FILEOP", NULL, C_TEMP_DATA | R_STATUS,
			b_from_strings(2, "unlink", path), NULL);

	f->deleted = 1;
	return (0);
}

static int do_mkdir(const char *path, mode_t mode)
{
	int	rc;
	file_t	*d;

	printerror(P_VERBOSE, "", "mkdir(%s)", path);
	if ((rc = f_create(path, &d)) != 0)
		return (rc);

	d->mode = M_DIR | M_READ | M_WRITE | M_USER;
	if (c_putc("FILEOP", NULL, C_TEMP_DATA | R_STATUS,
			b_from_strings(2, "mkdir", path), NULL) != 0)
		return (-EPERM);

	return (0);
}

static int do_rmdir(const char *path)
{
	int	k, len, sp;
	struct stat sbuf;
	file_t	*f, *d;

	printerror(P_EXTRA, "", "do_rmdir(\"%s\")", path);

	if (*path == '\0'  ||  d_search_file(&uxfs.dir, path, &k) != 0)
		return (-ENOENT);

	d = uxfs.dir.file[k];
	if ((d->mode & M_DIR) == 0)
		return (-ENOTDIR);


	/*
	 * README: do_rmdir() (and do_readdir()) use the volatile
	 * index into the directory array (instead of a pointer)
	 * because they traverse their content.
	 */

	len = strlen(path);
	sp  = len;		/* Char at position sp must be `/' */
	if (strcmp(path, "/") == 0)
		sp = 0;

	k++;
	while (k < uxfs.dir.len) {
		f = uxfs.dir.file[k++];
		memset(&sbuf, 0, sizeof(sbuf));

		/*
		 * Check if the item is in- our outside of our
		 * directory.
		 */

		if (f->path[sp] == '/'  ||  strncmp(f->path, path, len) == 0) {

			/*
			 * If it is not deleted the directory is not empty.
			 */

			if (f->deleted == 0)
				return (-ENOTEMPTY);
			}
		else {
			/*
			 * We reached the first item behind our directory
			 * (tree), which meanss it is empty.
			 */

			break;
			}
		}

	d->deleted = 1;
	if (c_putc("FILEOP", NULL, C_TEMP_DATA | R_STATUS,
			b_from_strings(2, "rmdir", path), NULL) != 0)
		return (-EPERM);

	return (0);
}


static int do_chmod(const char *path, mode_t mode)
{
	printerror(0, "-INFO", "not implemented: chmod(%s)", path);
	return (-1);
}

static int do_chown(const char *path, uid_t uid, gid_t gid)
{
	printerror(0, "-INFO", "not implemented: chown(%s)", path);
	return (-1);
}


static int do_readlink(const char *path, char *buf, size_t size)
{
	printerror(0, "-INFO", "not implemented: readlink(%s)", path);
	return (-1);
}

static int do_mknod(const char *path, const mode_t mode, const dev_t dev)
{
	printerror(0, "-INFO", "not implemented: mknod(%s)", path);
	return (-1);
}

static int do_symlink(const char *from, const char *to)
{
	printerror(0, "-INFO", "not implemented: symlink(%s)", from);
	return (-1);
}

static int do_link(const char *from, const char *to)
{
	printerror(0, "-INFO", "not implemented: link(%s)", from);
	return (-1);
}

static int do_statfs(const char *path, struct statvfs *stbuf)
{
	printerror(0, "-INFO", "not implemented: statfs(%s)", path);
	return (-1);
}

static int do_fsync(const char *path, int isdatasync,
			struct fuse_file_info *fi)
{
	printerror(0, "-INFO", "not implemented: fsync(%s)", path);
	return (-1);
}

#ifdef HAVE_POSIX_FALLOCATE
static int do_fallocate(const char *path, int mode,
				off_t offset, off_t length,
				struct fuse_file_info *fi)
{
	printerror(0, "-INFO", "not implemented: fsync(%s)", path);
	return (-1);
}
#endif



  /*
   * libfuse Interface.
   */

static struct fuse_operations operations = {
    .init		= do_init,
    .getattr		= do_getattr,
//    .fgetattr		= do_fgetattr,
    .readdir		= do_readdir,
    .open		= do_open,
    .release		= do_release,
    .truncate		= do_truncate,
    .write		= do_write,
    .read		= do_read,

    .create		= do_create,
    .access		= do_access,

    .mkdir		= do_mkdir,
    .unlink		= do_unlink,
    .rmdir		= do_rmdir,
    .chmod		= do_chmod,
    .chown		= do_chown,
    .rename		= do_rename,

    .statfs		= do_statfs,
    .fsync		= do_fsync,
    .mknod		= do_mknod,
    .symlink		= do_symlink,
    .link		= do_link,
    .readlink		= do_readlink,

#ifdef HAVE_POSIX_FALLOCATE
    .fallocate		= do_fallocate,
#endif

//    .lseek		= do_lseek,
    };



static int uxfs_opt_proc(void *data, const char *arg, int key,
				struct fuse_args *outargs)
{
	static int have_mount_point = 0;
	(void) data;

	switch (key) {
	case FUSE_OPT_KEY_NONOPT:
		if (have_mount_point == 0) {
			uxfs.mountpoint = strdup(arg);
			have_mount_point = 1;
			return (1);
			}

		if (uxfs.co.argc >= sizeof(uxfs.co.argv) - 2)
			printerror(1, "-ERR", "number of command arguments exceed limit");

		uxfs.co.argv[uxfs.co.argc++] = strdup(arg);
		break;

	case OPT_FOREGROUND:
		uxfs.foreground = 1;
		break;

	case OPT_OTHER_USERS:
		/* allow_root or allow_other */
		uxfs.other_users = uxfs.other_users == 0? 1: 2;
		break;

	case OPT_VERBOSE:
		/* Be verbose or extra verbose */
		uxfs.verbose = uxfs.verbose == 0? 1: 2;
		break;

	case OPT_DEBUG:
		uxfs.debug = 1;
		break;

	case OPT_SINGLE_THREAD:
		uxfs.single_thread = 1;
		break;

	default:
		printerror(1, "-ERR", "internal error");
		abort();
		}

	return (0);
}


int main(int argc, char *argv[])
{
	int	rc = 0, k = 1;


	memset(&uxfs, 0, sizeof(uxfs_t));
	uxfs.inode_count = 1;
	uxfs.co.fd0 = 0;
	uxfs.co.fd1 = 1;

	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	if (fuse_opt_parse(&args, &uxfs, uxfs_opts, uxfs_opt_proc) == -1)
		exit (1);


	fuse_opt_insert_arg(&args, k++, "-f");

	if (uxfs.single_thread != 0)
		fuse_opt_insert_arg(&args, k++, "-s");

	if (uxfs.other_users != 0) {
		fuse_opt_insert_arg(&args, k++, "-o");
		if (uxfs.other_users == 2)
			fuse_opt_insert_arg(&args, k++, "allow_other");
		else
			fuse_opt_insert_arg(&args, k++, "allow_root");
		}

	if (pthread_mutex_init(&lock, NULL) != 0)
		printerror(1, "-ERR", "mutex init failed");

	printerror(0, "+INFO", "starting");

	if (uxfs.co.argc > 0) {
		uxfs.co.argv[uxfs.co.argc] = NULL;
		c_start_server();
		}

	add_file(&uxfs.dir, "/", M_DIR);

#if FUSE_VERSION >= 26
	rc = fuse_main(args.argc, args.argv, &operations, NULL);
#else
	rc = fuse_main(args.argc, args.argv, &operations);
#endif

	fuse_opt_free_args(&args);
	return (rc);
}

