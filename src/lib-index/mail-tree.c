/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "mmap-util.h"
#include "file-set-size.h"
#include "write-full.h"
#include "mail-index.h"
#include "mail-index-util.h"
#include "mail-tree.h"

#include <unistd.h>
#include <fcntl.h>

#define MAIL_TREE_MIN_SIZE \
	(sizeof(struct mail_tree_header) + \
	 INDEX_MIN_RECORDS_COUNT * sizeof(struct mail_tree_node))

static int tree_set_syscall_error(struct mail_tree *tree, const char *function)
{
	i_assert(function != NULL);

	if (ENOSPACE(errno)) {
		tree->index->nodiskspace = TRUE;
		return FALSE;
	}

	index_set_error(tree->index, "%s failed with binary tree file %s: %m",
			function, tree->filepath);
	return FALSE;
}

int _mail_tree_set_corrupted(struct mail_tree *tree, const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	t_push();

	index_set_error(tree->index, "Corrupted binary tree file %s: %s",
			tree->filepath, t_strdup_vprintf(fmt, va));

	t_pop();
	va_end(va);

	/* make sure we don't get back here */
	tree->index->inconsistent = TRUE;
	(void)unlink(tree->filepath);

	return FALSE;
}

static int mmap_update(struct mail_tree *tree)
{
	i_assert(!tree->anon_mmap);

	if (tree->mmap_base != NULL) {
		/* make sure we're synced before munmap() */
		if (!tree->anon_mmap && tree->modified &&
		    msync(tree->mmap_base, tree->mmap_highwater, MS_SYNC) < 0)
			return tree_set_syscall_error(tree, "msync()");
		tree->modified = FALSE;

		if (munmap(tree->mmap_base, tree->mmap_full_length) < 0)
			tree_set_syscall_error(tree, "munmap()");
	}

	tree->mmap_used_length = 0;
	tree->header = NULL;
	tree->node_base = NULL;

	tree->mmap_base = mmap_rw_file(tree->fd, &tree->mmap_full_length);
	if (tree->mmap_base == MAP_FAILED) {
		tree->mmap_base = NULL;
		return tree_set_syscall_error(tree, "mmap()");
	}

	debug_mprotect(tree->mmap_base, tree->mmap_full_length, tree->index);
	return TRUE;
}

static int mmap_verify(struct mail_tree *tree)
{
	struct mail_tree_header *hdr;
	unsigned int extra;

	if (tree->mmap_full_length <
	    sizeof(struct mail_tree_header) + sizeof(struct mail_tree_node)) {
		index_set_error(tree->index, "Too small binary tree file %s",
				tree->filepath);
		(void)unlink(tree->filepath);
		return FALSE;
	}

	extra = (tree->mmap_full_length - sizeof(struct mail_tree_header)) %
		sizeof(struct mail_tree_node);

	if (extra != 0) {
		/* partial write or corrupted -
		   truncate the file to valid length */
		tree->mmap_full_length -= extra;
		if (ftruncate(tree->fd, (off_t)tree->mmap_full_length) < 0)
			tree_set_syscall_error(tree, "ftruncate()");
	}

	hdr = tree->mmap_base;
	if (hdr->used_file_size > tree->mmap_full_length) {
		_mail_tree_set_corrupted(tree,
			"used_file_size larger than real file size "
			"(%"PRIuUOFF_T" vs %"PRIuSIZE_T")",
			hdr->used_file_size, tree->mmap_full_length);
		return FALSE;
	}

	if (hdr->used_file_size < sizeof(struct mail_tree_header) ||
	    (hdr->used_file_size - sizeof(struct mail_tree_header)) %
	    sizeof(struct mail_tree_node) != 0) {
		_mail_tree_set_corrupted(tree,
			"Invalid used_file_size in header (%"PRIuUOFF_T")",
			hdr->used_file_size);
		return FALSE;
	}

	tree->header = tree->mmap_base;
	tree->node_base = (struct mail_tree_node *)
		((char *) tree->mmap_base + sizeof(struct mail_tree_header));
	tree->sync_id = hdr->sync_id;
	tree->mmap_used_length = hdr->used_file_size;
	tree->mmap_highwater = tree->mmap_used_length;
	return TRUE;
}

int _mail_tree_mmap_update(struct mail_tree *tree, int forced)
{
	if (tree->index->mmap_invalidate && tree->mmap_base != NULL) {
		if (msync(tree->mmap_base, tree->mmap_used_length,
			  MS_SYNC | MS_INVALIDATE) < 0)
			return tree_set_syscall_error(tree, "msync()");
	}

	debug_mprotect(tree->mmap_base, tree->mmap_full_length,
		       tree->index);

	if (!forced && tree->header != NULL &&
	    tree->sync_id == tree->header->sync_id) {
		/* make sure file size hasn't changed */
		tree->mmap_used_length = tree->header->used_file_size;
		if (tree->mmap_used_length > tree->mmap_full_length) {
			i_panic("Tree file size was grown without "
				"updating sync_id");
		}

		return TRUE;
	}

	return mmap_update(tree) && mmap_verify(tree);
}

static struct mail_tree *mail_tree_open(struct mail_index *index)
{
	struct mail_tree *tree;
	const char *path;
	int fd;

	path = t_strconcat(index->filepath, ".tree", NULL);
	fd = open(path, O_RDWR | O_CREAT, 0660);
	if (fd == -1) {
		index_file_set_syscall_error(index, path, "open()");
		return NULL;
	}

	tree = i_new(struct mail_tree, 1);
	tree->fd = fd;
	tree->index = index;
	tree->filepath = i_strdup(path);

	index->tree = tree;
	return tree;
}

static struct mail_tree *mail_tree_create_anon(struct mail_index *index)
{
	struct mail_tree *tree;

	tree = i_new(struct mail_tree, 1);
	tree->anon_mmap = TRUE;
	tree->fd = -1;
	tree->index = index;
	tree->filepath = i_strdup_printf("(in-memory tree index for %s)",
					 index->mailbox_path);

	index->tree = tree;
	return tree;
}

int mail_tree_create(struct mail_index *index)
{
	struct mail_tree *tree;

	i_assert(index->lock_type == MAIL_LOCK_EXCLUSIVE);

	tree = !INDEX_IS_IN_MEMORY(index) ? mail_tree_open(index) :
		mail_tree_create_anon(index);
	if (tree == NULL)
		return FALSE;

	if (!mail_tree_rebuild(tree)) {
		mail_tree_free(tree);
		return FALSE;
	}

	return TRUE;
}

static int mail_tree_open_init(struct mail_tree *tree)
{
	if (!mmap_update(tree))
		return FALSE;

	if (tree->mmap_full_length == 0) {
		/* just created it */
		return FALSE;
	}

	if (!mmap_verify(tree)) {
		/* broken header */
		return FALSE;
	}

	if (tree->header->indexid != tree->index->indexid) {
		index_set_error(tree->index,
				"IndexID mismatch for binary tree file %s",
				tree->filepath);

		return FALSE;
	} 

	return TRUE;
}

int mail_tree_open_or_create(struct mail_index *index)
{
	struct mail_tree *tree;

	tree = mail_tree_open(index);
	if (tree == NULL)
		return FALSE;

	if (!mail_tree_open_init(tree)) {
		/* lock and check again, just to avoid rebuilding it twice
		   if two processes notice the error at the same time */
		if (!tree->index->set_lock(tree->index, MAIL_LOCK_EXCLUSIVE)) {
			mail_tree_free(tree);
			return FALSE;
		}

		if (!mail_tree_open_init(tree)) {
			if (!mail_tree_rebuild(tree)) {
				mail_tree_free(tree);
				return FALSE;
			}
		}
	}

	return TRUE;
}

static void mail_tree_close(struct mail_tree *tree)
{
	if (tree->anon_mmap) {
		if (munmap_anon(tree->mmap_base, tree->mmap_full_length) < 0)
			tree_set_syscall_error(tree, "munmap_anon()");
	} else if (tree->mmap_base != NULL) {
		if (munmap(tree->mmap_base, tree->mmap_full_length) < 0)
			tree_set_syscall_error(tree, "munmap()");
	}
	tree->mmap_base = NULL;
	tree->mmap_full_length = 0;
	tree->mmap_used_length = 0;
	tree->header = NULL;

	if (tree->fd != -1) {
		if (close(tree->fd) < 0)
			tree_set_syscall_error(tree, "close()");
		tree->fd = -1;
	}

	i_free(tree->filepath);
}

void mail_tree_free(struct mail_tree *tree)
{
	tree->index->tree = NULL;

	mail_tree_close(tree);
	i_free(tree);
}

static int mail_tree_init(struct mail_tree *tree)
{
        struct mail_tree_header hdr;

	/* first node is always used, and is the RBNULL node */
	memset(&hdr, 0, sizeof(struct mail_tree_header));
	hdr.indexid = tree->index->indexid;
	hdr.used_file_size = sizeof(struct mail_tree_header) +
		sizeof(struct mail_tree_node);

	if (tree->anon_mmap) {
		tree->mmap_full_length = MAIL_TREE_MIN_SIZE;
		tree->mmap_base = mmap_anon(tree->mmap_full_length);
		if (tree->mmap_base == MAP_FAILED)
			return tree_set_syscall_error(tree, "mmap_anon()");

		memcpy(tree->mmap_base, &hdr, sizeof(struct mail_tree_header));
		return mmap_verify(tree);
	}

	if (lseek(tree->fd, 0, SEEK_SET) < 0)
		return tree_set_syscall_error(tree, "lseek()");

	if (write_full(tree->fd, &hdr, sizeof(hdr)) < 0)
		return tree_set_syscall_error(tree, "write_full()");

	if (file_set_size(tree->fd, MAIL_TREE_MIN_SIZE) < 0)
		return tree_set_syscall_error(tree, "file_set_size()");

	return TRUE;
}

int mail_tree_reset(struct mail_tree *tree)
{
	i_assert(tree->index->lock_type == MAIL_LOCK_EXCLUSIVE);

	if (!mail_tree_init(tree) ||
	    (!tree->anon_mmap && !_mail_tree_mmap_update(tree, TRUE))) {
		tree->index->header->flags |= MAIL_INDEX_FLAG_REBUILD_TREE;
		return FALSE;
	}

	return TRUE;
}

int mail_tree_rebuild(struct mail_tree *tree)
{
	struct mail_index_record *rec;

	if (!tree->index->set_lock(tree->index, MAIL_LOCK_EXCLUSIVE))
		return FALSE;

	if (!mail_tree_reset(tree))
		return FALSE;

	rec = tree->index->lookup(tree->index, 1);
	while (rec != NULL) {
		if (!mail_tree_insert(tree, rec->uid,
				      INDEX_RECORD_INDEX(tree->index, rec))) {
			tree->index->header->flags |=
				MAIL_INDEX_FLAG_REBUILD_TREE;
			return FALSE;
		}

		rec = tree->index->next(tree->index, rec);
	}

	return TRUE;
}

int mail_tree_sync_file(struct mail_tree *tree, int *fsync_fd)
{
	*fsync_fd = -1;

	if (!tree->modified || tree->anon_mmap)
		return TRUE;

	i_assert(tree->mmap_base != NULL);

	if (msync(tree->mmap_base, tree->mmap_highwater, MS_SYNC) < 0)
		return tree_set_syscall_error(tree, "msync()");

	tree->mmap_highwater = tree->mmap_used_length;
	tree->modified = FALSE;

	*fsync_fd = tree->fd;
	return TRUE;
}

int _mail_tree_grow(struct mail_tree *tree)
{
	uoff_t new_fsize;
	unsigned int grow_count;
	void *base;

	grow_count = tree->index->header->messages_count *
		INDEX_GROW_PERCENTAGE / 100;
	if (grow_count < 16)
		grow_count = 16;

	new_fsize = (uoff_t)tree->mmap_full_length +
		(grow_count * sizeof(struct mail_tree_node));
	i_assert(new_fsize < OFF_T_MAX);

	if (tree->anon_mmap) {
		i_assert(new_fsize < SSIZE_T_MAX);

		base = mremap_anon(tree->mmap_base, tree->mmap_full_length,
				   (size_t)new_fsize, MREMAP_MAYMOVE);
		if (base == MAP_FAILED)
			return tree_set_syscall_error(tree, "mremap_anon()");

		tree->mmap_base = base;
		tree->mmap_full_length = (size_t)new_fsize;
		return mmap_verify(tree);
	}

	if (file_set_size(tree->fd, (off_t)new_fsize) < 0)
		return tree_set_syscall_error(tree, "file_set_size()");

	/* file size changed, let others know about it too by changing
	   sync_id in header. */
	tree->header->sync_id++;
	tree->modified = TRUE;

	if (!_mail_tree_mmap_update(tree, TRUE))
		return FALSE;

	return TRUE;
}

void _mail_tree_truncate(struct mail_tree *tree)
{
	/* pretty much copy&pasted from mail_index_compress() */
	uoff_t empty_space, truncate_threshold;

	i_assert(tree->index->lock_type == MAIL_LOCK_EXCLUSIVE);

	if (tree->mmap_full_length <= MAIL_TREE_MIN_SIZE || tree->anon_mmap)
		return;

	empty_space = tree->mmap_full_length - tree->mmap_used_length;

	truncate_threshold =
		tree->mmap_full_length / 100 * INDEX_TRUNCATE_PERCENTAGE;

	if (empty_space > truncate_threshold) {
		tree->mmap_full_length = tree->mmap_used_length +
			(empty_space * INDEX_TRUNCATE_KEEP_PERCENTAGE / 100);

		/* keep the size record-aligned */
		tree->mmap_full_length -= (tree->mmap_full_length -
					   sizeof(struct mail_tree_header)) %
			sizeof(struct mail_tree_node);

		if (tree->mmap_full_length < MAIL_TREE_MIN_SIZE)
			tree->mmap_full_length = MAIL_TREE_MIN_SIZE;

		if (ftruncate(tree->fd, (off_t)tree->mmap_full_length) < 0)
			tree_set_syscall_error(tree, "ftruncate()");

		tree->header->sync_id++;
	}
}
