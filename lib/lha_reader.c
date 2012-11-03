/*

Copyright (c) 2011, 2012, Simon Howard

Permission to use, copy, modify, and/or distribute this software
for any purpose with or without fee is hereby granted, provided
that the above copyright notice and this permission notice appear
in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lha_arch.h"
#include "lha_decoder.h"
#include "lha_basic_reader.h"
#include "public/lha_reader.h"
#include "macbinary.h"

typedef enum {

	// Initial state at start of stream:

	CURR_FILE_START,

	// Current file is a "normal" file (or directory) read from
	// the input stream.

	CURR_FILE_NORMAL,

	// Current file is a directory that has been popped from the
	// directory stack.

	CURR_FILE_FAKE_DIR,

	// End of input stream has been reached.

	CURR_FILE_EOF,
} CurrFileType;

struct _LHAReader {
	LHABasicReader *reader;

	// The current file that we are processing (last file returned
	// by lha_reader_next_file).

	LHAFileHeader *curr_file;
	CurrFileType curr_file_type;

	// Pointer to decoder being used to decompress the current file,
	// or NULL if we have not yet started decompression.

	LHADecoder *decoder;

	// Pointer to "inner" decoder. Most of the time,
	// decoder == inner_decoder, but when decoding an archive
	// generated by MacLHA, inner_decoder points to the actual
	// decompressor.

	LHADecoder *inner_decoder;

	// Policy used to extract directories.

	LHAReaderDirPolicy dir_policy;

	// Directories that have been created by lha_reader_extract but
	// have not yet had their metadata set. This is a linked list
	// using the _next field in LHAFileHeader.
	// In the case of LHA_READER_DIR_END_OF_DIR this is a stack;
	// in the case of LHA_READER_DIR_END_OF_FILE it is a list.

	LHAFileHeader *dir_stack;
};

LHAReader *lha_reader_new(LHAInputStream *stream)
{
	LHABasicReader *basic_reader;
	LHAReader *reader;

	basic_reader = lha_basic_reader_new(stream);

	if (basic_reader == NULL) {
		return NULL;
	}

	reader = malloc(sizeof(LHAReader));

	if (reader == NULL) {
		lha_basic_reader_free(basic_reader);
		return NULL;
	}

	reader->reader = basic_reader;
	reader->curr_file = NULL;
	reader->curr_file_type = CURR_FILE_START;
	reader->decoder = NULL;
	reader->inner_decoder = NULL;
	reader->dir_stack = NULL;
	reader->dir_policy = LHA_READER_DIR_END_OF_DIR;

	return reader;
}

void lha_reader_free(LHAReader *reader)
{
	LHAFileHeader *header;

	if (reader->decoder != NULL) {
		if (reader->inner_decoder == reader->decoder) {
			reader->inner_decoder = NULL;
		}
		lha_decoder_free(reader->decoder);
	}

	if (reader->inner_decoder != NULL) {
		lha_decoder_free(reader->inner_decoder);
	}

	// Free any file headers in the stack.

	while (reader->dir_stack != NULL) {
		header = reader->dir_stack;
		reader->dir_stack = header->_next;
		lha_file_header_free(header);
	}

	lha_basic_reader_free(reader->reader);
	free(reader);
}

void lha_reader_set_dir_policy(LHAReader *reader,
                               LHAReaderDirPolicy policy)
{
	reader->dir_policy = policy;
}

// Returns true if the top directory in the stack should be popped off.

static int end_of_top_dir(LHAReader *reader)
{
	LHAFileHeader *input;

	// No directories to pop?

	if (reader->dir_stack == NULL) {
		return 0;
	}

	// Once the end of the input stream is reached, all that is
	// left to do is pop off the remaining directories.

	input = lha_basic_reader_curr_file(reader->reader);

	if (input == NULL) {
		return 1;
	}

	switch (reader->dir_policy) {

		// Shouldn't happen?

		case LHA_READER_DIR_PLAIN:
		default:
			return 1;

		// Don't process directories until we reach the end of
		// the input stream.

		case LHA_READER_DIR_END_OF_FILE:
			return 0;

		// Once we reach a file from the input that is not within
		// the directory at the top of the stack, we have reached
		// the end of that directory, so we can pop it off.

		case LHA_READER_DIR_END_OF_DIR:
			return input->path == NULL
			    || strncmp(input->path,
			               reader->dir_stack->path,
			               strlen(reader->dir_stack->path)) != 0;
	}
}

// Read the next file from the input stream.

LHAFileHeader *lha_reader_next_file(LHAReader *reader)
{
	// Free the current decoder if there is one.

	if (reader->decoder != NULL) {
		if (reader->inner_decoder == reader->decoder) {
			reader->inner_decoder = NULL;
		}

		lha_decoder_free(reader->decoder);
		reader->decoder = NULL;
	}

	if (reader->inner_decoder != NULL) {
		lha_decoder_free(reader->inner_decoder);
		reader->inner_decoder = NULL;
	}

	// No point continuing once the end of the input stream has
	// been reached.

	if (reader->curr_file_type == CURR_FILE_EOF) {
		return NULL;
	}

	// Advance to the next file from the input stream?
	// Don't advance until we've done the fake directories first.

	if (reader->curr_file_type == CURR_FILE_START
	 || reader->curr_file_type == CURR_FILE_NORMAL) {
		lha_basic_reader_next_file(reader->reader);
	}

	// If the last file we returned was a 'fake' directory, we must
	// now unreference it.

	if (reader->curr_file_type == CURR_FILE_FAKE_DIR) {
		lha_file_header_free(reader->curr_file);
	}

	// Pop off all appropriate directories from the stack first.

	if (end_of_top_dir(reader)) {
		reader->curr_file = reader->dir_stack;
		reader->dir_stack = reader->dir_stack->_next;
		reader->curr_file_type = CURR_FILE_FAKE_DIR;
	} else {
		reader->curr_file = lha_basic_reader_curr_file(reader->reader);

		if (reader->curr_file != NULL) {
			reader->curr_file_type = CURR_FILE_NORMAL;
		} else {
			reader->curr_file_type = CURR_FILE_EOF;
		}
	}

	return reader->curr_file;
}

// Create the decoder structure to decompress the data from the
// current file. Returns 1 for success.

static int open_decoder(LHAReader *reader,
                        LHADecoderProgressCallback callback,
                        void *callback_data)
{
	// Can only read from a normal file.

	if (reader->curr_file_type != CURR_FILE_NORMAL) {
		return 0;
	}

	reader->inner_decoder = lha_basic_reader_decode(reader->reader);

	if (reader->inner_decoder == NULL) {
		return 0;
	}

	// Set progress callback for decoder.

	if (callback != NULL) {
		lha_decoder_monitor(reader->inner_decoder,
		                    callback, callback_data);
	}

	// Some archives generated by MacLHA have a MacBinary header
	// attached to the start, which contains MacOS-specific
	// metadata about the compressed file. These are identified
	// and stripped off, using a "passthrough" decoder.

	if (reader->curr_file->os_type == LHA_OS_TYPE_MACOS) {
		reader->decoder = lha_macbinary_passthrough(
		    reader->inner_decoder, reader->curr_file);

		if (reader->decoder == NULL) {
			return 0;
		}
	} else {
		reader->decoder = reader->inner_decoder;
	}

	return 1;
}

size_t lha_reader_read(LHAReader *reader, void *buf, size_t buf_len)
{
	// The first time that we try to read the current file, we
	// must create the decoder to decompress it.

	if (reader->decoder == NULL) {
		if (!open_decoder(reader, NULL, NULL)) {
			return 0;
		}
	}

	// Read from decoder and return the result.

	return lha_decoder_read(reader->decoder, buf, buf_len);
}

// Decompress the current file, invoking the specified callback function
// to monitor progress (if not NULL). Assumes that open_decoder() has
// already been called to initialize decode the file. Decompressed data
// is written to 'output' if it is not NULL.
// Returns true if the file decompressed successfully.

static int do_decode(LHAReader *reader, FILE *output)
{
	uint8_t buf[64];
	unsigned int bytes;

	// Decompress the current file.

	do {
		bytes = lha_reader_read(reader, buf, sizeof(buf));

		if (output != NULL) {
			if (fwrite(buf, 1, bytes, output) < bytes) {
				return 0;
			}
		}

	} while (bytes > 0);

	// Decoder stores output position and performs running CRC.
	// At the end of the stream these should match the header values.

	return lha_decoder_get_length(reader->inner_decoder)
	         == reader->curr_file->length
	    && lha_decoder_get_crc(reader->inner_decoder)
	         == reader->curr_file->crc;
}

int lha_reader_check(LHAReader *reader,
                     LHADecoderProgressCallback callback,
                     void *callback_data)
{
	if (reader->curr_file_type != CURR_FILE_NORMAL) {
		return 0;
	}

	// CRC checking of directories is not necessary.

	if (!strcmp(reader->curr_file->compress_method, LHA_COMPRESS_TYPE_DIR)) {
		return 1;
	}

	// Decode file.

	return open_decoder(reader, callback, callback_data)
	    && do_decode(reader, NULL);
}

// Open an output stream into which to decompress the current file.
// The filename is constructed from the file header of the current file,
// or 'filename' is used if it is not NULL.

static FILE *open_output_file(LHAReader *reader, char *filename)
{
	int unix_uid = -1, unix_gid = -1, unix_perms = -1;

	if (reader->curr_file->extra_flags & LHA_FILE_UNIX_UID_GID) {
		unix_uid = reader->curr_file->unix_uid;
		unix_gid = reader->curr_file->unix_gid;
	}

	if (reader->curr_file->extra_flags & LHA_FILE_UNIX_PERMS) {
		unix_perms = reader->curr_file->unix_perms;
	}

	return lha_arch_fopen(filename, unix_uid, unix_gid, unix_perms);
}

// Set file timestamp(s) for the specified file using values from the
// specified header.

static int set_timestamps_from_header(char *path, LHAFileHeader *header)
{
#if LHA_ARCH == LHA_ARCH_WINDOWS
	if ((header->extra_flags & LHA_FILE_WINDOWS_TIMESTAMPS) != 0) {
		return lha_arch_set_windows_timestamps(
		    path,
		    header->win_creation_time,
		    header->win_modification_time,
		    header->win_access_time
		);
	} else // ....
#endif
	if (header->timestamp != 0) {
		return lha_arch_utime(path, header->timestamp);
	} else {
		return 1;
	}
}

// Second stage of directory extraction: set metadata.

static int set_directory_metadata(LHAFileHeader *header, char *path)
{
	// Set timestamp:

	set_timestamps_from_header(path, header);

	// Set owner and group:

	if (header->extra_flags & LHA_FILE_UNIX_UID_GID) {
		if (!lha_arch_chown(path, header->unix_uid,
		                    header->unix_gid)) {
			// On most Unix systems, only root can change
			// ownership. But if we can't change ownership,
			// it isn't a fatal error. Ignore the failure
			// and continue.

			// TODO: Implement some kind of alternate handling
			// here?
			/* return 0; */
		}
	}

	// Set permissions on directory:

	if (header->extra_flags & LHA_FILE_UNIX_PERMS) {
		if (!lha_arch_chmod(path, header->unix_perms)) {
			return 0;
		}
	}

	return 1;
}

static int extract_directory(LHAReader *reader, char *path)
{
	LHAFileHeader *header;
	unsigned int mode;

	header = reader->curr_file;

	// If path is not specified, use the path from the file header.

	if (path == NULL) {
		path = header->path;
	}

	// Create directory. If there are permissions to be set, create
	// the directory with minimal permissions limited to the running
	// user. Otherwise use the default umask.

	if (header->extra_flags & LHA_FILE_UNIX_PERMS) {
		mode = 0700;
	} else {
		mode = 0777;
	}

	if (!lha_arch_mkdir(path, mode)) {

		// If the attempt to create the directory failed, it may
		// be because the directory already exists. Return success
		// if this is the case; it isn't really an error.

		return lha_arch_exists(path) == LHA_FILE_DIRECTORY;
	}

	// The directory has been created, but the metadata has not yet
	// been applied. It depends on the directory policy how this
	// is handled. If we are using LHA_READER_DIR_PLAIN, set
	// metadata now. Otherwise, save the directory for later.

	if (reader->dir_policy == LHA_READER_DIR_PLAIN) {
		set_directory_metadata(header, path);
	} else {
		lha_file_header_add_ref(header);
		header->_next = reader->dir_stack;
		reader->dir_stack = header;
	}

	return 1;
}

static char *full_path_for_header(LHAFileHeader *header)
{
	char *result;
	size_t filename_len;

	if (header->path != NULL) {
		filename_len = strlen(header->filename)
		             + strlen(header->path)
		             + 1;

		result = malloc(filename_len);

		if (result == NULL) {
			return NULL;
		}

		sprintf(result, "%s%s", header->path, header->filename);

		return result;
	} else {
		return strdup(header->filename);
	}
}

static int extract_file(LHAReader *reader, char *filename,
                        LHADecoderProgressCallback callback,
                        void *callback_data)
{
	FILE *fstream;
	char *tmp_filename = NULL;
	int result;

	// Construct filename?

	if (filename == NULL) {
		tmp_filename = full_path_for_header(reader->curr_file);

		if (tmp_filename == NULL) {
			return 0;
		}

		filename = tmp_filename;
	}

	// Create decoder. If the file cannot be created, there is no
	// need to even create an output file. If successful, open the
	// output file and decode.

	result = 0;

	if (open_decoder(reader, callback, callback_data)) {

		fstream = open_output_file(reader, filename);

		if (fstream != NULL) {
			result = do_decode(reader, fstream);
			fclose(fstream);
		}
	}

	// Set timestamp on file:

	if (result) {
		set_timestamps_from_header(filename, reader->curr_file);
	}

	free(tmp_filename);

	return result;
}

static int extract_symlink(LHAReader *reader, char *filename)
{
	char *tmp_filename = NULL;
	int result;

	// Construct filename?

	if (filename == NULL) {
		tmp_filename = full_path_for_header(reader->curr_file);

		if (tmp_filename == NULL) {
			return 0;
		}

		filename = tmp_filename;
	}

	result = lha_arch_symlink(filename, reader->curr_file->symlink_target);

	// TODO: Set symlink timestamp.

	free(tmp_filename);

	return result;
}

static int extract_normal(LHAReader *reader,
                          char *filename,
                          LHADecoderProgressCallback callback,
                          void *callback_data)
{
	if (strcmp(reader->curr_file->compress_method,
	           LHA_COMPRESS_TYPE_DIR) != 0) {
		return extract_file(reader, filename, callback, callback_data);
	} else if (reader->curr_file->symlink_target != NULL) {
		return extract_symlink(reader, filename);
	} else {
		return extract_directory(reader, filename);
	}
}

int lha_reader_extract(LHAReader *reader,
                       char *filename,
                       LHADecoderProgressCallback callback,
                       void *callback_data)
{
	switch (reader->curr_file_type) {

		case CURR_FILE_NORMAL:
			return extract_normal(reader, filename, callback,
			                      callback_data);

		case CURR_FILE_FAKE_DIR:
			if (filename == NULL) {
				filename = reader->curr_file->path;
			}
			set_directory_metadata(reader->curr_file, filename);
			return 1;

		case CURR_FILE_START:
		case CURR_FILE_EOF:
			break;
	}

	return 0;
}

