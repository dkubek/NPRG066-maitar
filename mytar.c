#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>

#define	LIST_OPT 't'
#define	ARCHIVE_OPT 'f'
#define	VERBOSE_OPT 'v'
#define	EXTRACT_OPT 'x'

#define	LIST_FLAG 0x01
#define	ARCHIVE_FLAG 0x02
#define	EXTRACT_FLAG 0x04
#define	VERBOSE_FLAG 0x08

#define	BLOCK_SIZE 512
#define	HEADER_SIZE 500

#define	REGTYPE '0'	/* regular file */
#define	AREGTYPE '\0'	/* regular file */

#define	TMAGIC "ustar "	/* ustar space and a null */
#define	TMAGLEN 6

const char USAGE_STR[] = "usage: %s -t [ file1 file2 ... ] -f ARCHIVE ";

/*
 * POSIX header.
 * Source: https://www.gnu.org/software/tar/manual/html_node/Standard.html
 */
struct posix_header
{				/* byte offset */
	char name[100];		/*   0 */
	char mode[8];		/* 100 */
	char uid[8];		/* 108 */
	char gid[8];		/* 116 */
	char size[12];		/* 124 */
	char mtime[12];		/* 136 */
	char chksum[8];		/* 148 */
	char typeflag;		/* 156 */
	char linkname[100];	/* 157 */
	char magic[6];		/* 257 */
	char version[2];	/* 263 */
	char uname[32];		/* 265 */
	char gname[32];		/* 297 */
	char devmajor[8];	/* 329 */
	char devminor[8];	/* 337 */
	char prefix[155];	/* 345 */
				/* 500 */
};

typedef struct {
	FILE *fp;
	long record_offset;
	long len;
} Archive;

typedef struct {
	struct posix_header header;
	long data_offset;
} Entry;

/* NOTE: Structure doesn't maintain a copy of the data */
typedef struct
{
	char flags;
	char *archive;		/* Filename of the archive */
	int len;		/* Number of files */
	char **files;		/* Filenames of files */
} Args;

Args * args_new();
void args_free(Args *args);

Archive * archive_open(char *archive_fname);
int is_tar_archive(FILE *fp);
void archive_close(Archive *arch);

void process_args(int argc, char *argv[], Args *args);
void process_archive_arg(char ***argv, Args* args);
void process_list_arg(Args* args);
void process_verbose_arg(Args* args);
void process_extract_arg(Args* args);
void validate_args(Args* args);

int next_entry(Archive *arch, Entry *ent);
int is_empty_block(char *arr, size_t size);

void list(Archive *arch, char *list_files[], int lf_count);
int remove_str(char *arr[], int size, char *str);
int report_missing(char *files[], int size);
void extract(Archive *arch, char *files[], int count, int verbose);


Args *
args_new()
{
	Args *args;
	if ((args = calloc(1, sizeof (Args))) == NULL)
		err(2, "malloc");

	return (args);
}

void
args_free(Args *args)
{
	free(args->files);
	free(args);
}

/*
 * Parse command line arguments.
 */
void
process_args(int argc, char *argv[], Args *args)
{
	if (argc == 1)
		errx(2, USAGE_STR, argv[0]);

	args->len = 0;
	if ((args->files = malloc((argc - 1) * sizeof (char *))) == NULL)
		err(2, "malloc");

	char **p = argv + 1;
	while (*p != NULL) {

		if (*p[0] != '-') {
			args->files[args->len++] = *p;
			p++;
			continue;
		}

		char shortarg = (*p)[1];
		p++;		// Move pointer to parameters
		switch (shortarg) {
			case LIST_OPT:
				process_list_arg(args);
				break;
			case ARCHIVE_OPT:
				process_archive_arg(&p, args);
				break;
			case VERBOSE_OPT:
				process_verbose_arg(args);
				break;
			case EXTRACT_OPT:
				process_extract_arg(args);
				break;
			default:
				errx(2, "invalid option -- '%c'", shortarg);
		}
	}

	validate_args(args);
}

/*
 * Process arguments associated with the -f file flag. Leave pointer on
 * the next unused argument.
 */
void
process_archive_arg(char **pargv[], Args* args)
{
	if (**pargv == NULL)
		errx(64, "option requires an argument -- 'f'");

	if (args->flags & ARCHIVE_FLAG)
		errx(2, "Multiple archive files not supported");

	args->flags = args->flags | ARCHIVE_FLAG;
	args->archive = **pargv;

	// Move pointer to next unused argument
	*pargv = *pargv + 1;
}

/*
 * Process arguments associated with the -t file flag.
 */
void
process_list_arg(Args* args)
{
	if (args->flags & LIST_FLAG)
		errx(2, "Multiple uses of -t not supported");

	args->flags = args->flags | LIST_FLAG;
}

/*
 * Process arguments associated with the -v file flag.
 */
void
process_verbose_arg(Args* args)
{
	if (args->flags & VERBOSE_FLAG)
		errx(2, "Multiple uses of -v not supported");

	args->flags = args->flags | VERBOSE_FLAG;
}

/*
 * Process arguments associated with the -v file flag.
 */
void
process_extract_arg(Args* args)
{
	args->flags = args->flags | EXTRACT_FLAG;
}

/*
 * Validate given command line arguments.
 */
void
validate_args(Args* args)
{
	if (!(args->flags & ARCHIVE_FLAG))
		errx(2, "Refusing to read archive contents from terminal"
			"(missing -f option?)");

	if (!(args->flags & LIST_FLAG) && !(args->flags & EXTRACT_FLAG))
		errx(2, "You must specify one of the -tx options.");

	if ((args->flags & LIST_FLAG) && (args->flags & EXTRACT_FLAG))
		errx(2, "You may not specify more than one -tx options.");
}

/*
 * Crate a new archive object.
 */
Archive *
archive_open(char *archive_fname)
{
	FILE *fp;
	if ((fp = fopen(archive_fname, "r")) == NULL)
		err(2, "%s: Cannot open", archive_fname);

	Archive *arch;
	if ((arch = malloc(sizeof (Archive))) == NULL)
		err(2, "malloc");

	if (!is_tar_archive(fp))
		return (NULL);

	fseek(fp, 0L, SEEK_END);
	arch->len = ftell(fp);
	fseek(fp, 0L, SEEK_SET);

	arch->fp = fp;
	arch->record_offset = 0;

	return (arch);
}

/*
 * Check if given file is a tar archive.
 */
int
is_tar_archive(FILE *fp)
{
	rewind(fp);

	struct posix_header header;

	int items_read = fread(&header, sizeof (struct posix_header), 1, fp);

	if (items_read == 0)
		return (0);

	// Check magic field
	if (strncmp(TMAGIC, header.magic, TMAGLEN)) {
		warnx("This does not look like a tar archive");
		return (0);
	}

	return (1);
}

/*
 * Safely close archive.
 */
void
archive_close(Archive *arch)
{
	if (fclose(arch->fp))
		err(2, "Cannot close");

	free(arch);
}

/*
 * Sets ent to the next entry of the file.
 * On seccessful read return 1, 0 if there are no entries left or 2
 * to indicate unrecoverable error.
 */
int
next_entry(Archive *arch, Entry *ent)
{
	char *buffer;
	if ((buffer = malloc(BLOCK_SIZE)) == NULL)
		err(2, "malloc");

	fseek(arch->fp, arch->record_offset, SEEK_SET);

	int bytes_read;
	bytes_read = fread(buffer, 1, BLOCK_SIZE, arch->fp);

	if (arch->record_offset > arch->len) {
		warnx("Unexpected EOF in archive");
		free(buffer);
		return (2);
	}

	// Already at the end of the archive
	if (bytes_read == 0) {
		free(buffer);
		return (0);
	}

	if (bytes_read != BLOCK_SIZE) {
		warnx("Unexpected EOF in archive");
		free(buffer);
		return (2);
	}

	int status;
	if (buffer[0]) {
		memcpy(&(ent->header), buffer, HEADER_SIZE);
		ent->data_offset = ftell(arch->fp);

		// Set record offset to point to the next record block
		char **endptr = NULL;
		int data_size = strtol(ent->header.size, endptr, 8);
		int data_blocks = (data_size + BLOCK_SIZE - 1) / BLOCK_SIZE;

		arch->record_offset += (1 + data_blocks) * BLOCK_SIZE;
		status = 1;
	}
	else
	{
		// Check for two zero blocks

		if (!is_empty_block(buffer, BLOCK_SIZE)) {
			free(buffer);
			errx(2, "Invalid block");
		}

		// Read next block
		bytes_read  = fread(buffer, 1, BLOCK_SIZE, arch->fp);
		if (bytes_read != BLOCK_SIZE ||
		    !is_empty_block(buffer, BLOCK_SIZE)) {
			int block_ord = arch->record_offset / BLOCK_SIZE + 1;
			warnx("A lone zero block at %d", block_ord);
		}

		arch->record_offset += bytes_read + BLOCK_SIZE;
		status = 0;
	}

	free(buffer);
	return (status);
}

/*
 * Check if given block of memory is zeroed out.
 */
int
is_empty_block(char *arr, size_t size)
{
	for (size_t i = 0; i < size; i++)
		if (arr[i] != '\0')
			return (0);

	return (1);
}

/*
 * List specified files.
 */
void
list(Archive *arch, char *list_files[], int lf_count)
{
	Entry *ent;
	if ((ent = malloc(sizeof (Entry))) == NULL)
		err(2, "malloc");

	int status;
	status = next_entry(arch, ent);
	while (status == 1) {
		if (ent->header.typeflag != REGTYPE &&
		    ent->header.typeflag != AREGTYPE) {
			errx(2, "Unsupported header type: %d",
				ent->header.typeflag);
		}

		if (lf_count == 0) {
			printf("%s\n", ent->header.name);
		} else if (remove_str(list_files, lf_count, ent->header.name)) {
			printf("%s\n", ent->header.name);
		}

		fflush(stdout);

		status = next_entry(arch, ent);
	}

	if (status == 2) {
		free(ent);
		errx(2, "Error is not recoverable: exiting now");
	}

		// List files not found in archive
	if (lf_count != 0)
		if (report_missing(list_files, lf_count)) {
			free(ent);
			errx(2, "Exiting with failure status due to previous "
				"errors");
		}

	free(ent);
}

/*
 * Remove the first occurence of string str form arr.
 */
int
remove_str(char *arr[], int size, char *str)
{
	for (int i = 0; i < size; i++) {
		if (strcmp(arr[i], str) == 0) {
			arr[i][0] = '\0';
			return (1);
		}
	}
	return (0);
}

/*
 * Report files not found in the archive to stderr. Return number of files
 * reported.
 */
int
report_missing(char *files[], int size)
{
	int found = 0;
	for (int i = 0; i < size; i++)
		if (strlen(files[i])) {
			warnx("%s: Not found in archive", files[i]);
			found++;
		}

	return (found);
}

/*
 * Extract file pointed to by ent from archive. Extract as much data as
 * possible. File is created with the filename specified in the archive.
 *
 * Returns number of bytes written.
 */
int
extract_file(Archive *arch, Entry *ent)
{
	char *filename = (ent->header).name;
	char **endptr = NULL;
	int data_size = strtol(ent->header.size, endptr, 8);

	FILE *fout;
	if ((fout = fopen(filename, "w")) == NULL)
		err(2, "%s: Cannot open for writing", filename);

	FILE *fin = arch->fp;
	fseek(fin, ent->data_offset, SEEK_SET);

	char *buffer;
	if ((buffer = malloc(data_size)) == NULL)
		err(2, "malloc");

	int bytes_read = fread(buffer, sizeof (char), data_size, fin);

	int bytes_written = fwrite(buffer, sizeof (char), bytes_read, fout);

	free(buffer);
	fclose(fout);
	return (bytes_written);
}

/*
 * Extract specified files from archive. If verbose is non-zero print
 * currently extracted file to stdout.
 */
void
extract(Archive *arch, char *files[], int count, int verbose)
{
	Entry *ent;
	if ((ent = malloc(sizeof (Entry))) == NULL)
		err(2, "malloc");

	int status;
	status = next_entry(arch, ent);
	while (status == 1) {
		if (ent->header.typeflag != REGTYPE &&
		    ent->header.typeflag != AREGTYPE) {
			errx(2, "Unsupported header type: %d",
				ent->header.typeflag);
		}

		if (count == 0) {
			if (verbose)
				printf("%s\n", ent->header.name);

			extract_file(arch, ent);
		} else if (remove_str(files, count, ent->header.name)) {
			if (verbose)
				printf("%s\n", ent->header.name);

			extract_file(arch, ent);
		}

		fflush(stdout);

		status = next_entry(arch, ent);
	}

	if (status == 2)
		errx(2, "Error is not recoverable: exiting now");

	free(ent);
}

int
main(int argc, char *argv[])
{
	Args *args = args_new();
	process_args(argc, argv, args);

	int verbose = 0;
	if (args->flags & VERBOSE_FLAG)
		verbose = 1;

	Archive* arch = archive_open(args->archive);
	if (arch == NULL)
		errx(2, "Exiting with failure status due to previous errors");

	if (args->flags & LIST_FLAG)
		list(arch, args->files, args->len);
	else if (args->flags & EXTRACT_FLAG)
		extract(arch, args->files, args->len, verbose);

	archive_close(arch);
	args_free(args);

	return (0);
}
