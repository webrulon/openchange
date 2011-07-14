/*
   OpenChange Storage Abstraction Layer library

   OpenChange Project

   Copyright (C) Julien Kerihuel 2009

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mapistore.h"
#include "mapistore_errors.h"
#include "mapistore_private.h"
#include <dlinklist.h>
#include "libmapi/libmapi_private.h"

#include <string.h>

/**
   \details Initialize the mapistore context

   \param mem_ctx pointer to the memory context

   \return allocate mapistore context on success, otherwise NULL
 */
_PUBLIC_ struct mapistore_context *mapistore_init(TALLOC_CTX *mem_ctx, const char *path)
{
	int				retval;
	struct mapistore_context	*mstore_ctx;

	mstore_ctx = talloc_zero(mem_ctx, struct mapistore_context);
	if (!mstore_ctx) {
		return NULL;
	}

	mstore_ctx->processing_ctx = talloc_zero(mstore_ctx, struct processing_context);

	retval = mapistore_init_mapping_context(mstore_ctx->processing_ctx);
	if (retval != MAPISTORE_SUCCESS) {
		DEBUG(5, ("[%s:%d]: %s\n", __FUNCTION__, __LINE__, mapistore_errstr(retval)));
		talloc_free(mstore_ctx);
		return NULL;
	}

	retval = mapistore_backend_init(mstore_ctx, path);
	if (retval != MAPISTORE_SUCCESS) {
		DEBUG(5, ("[%s:%d]: %s\n", __FUNCTION__, __LINE__, mapistore_errstr(retval)));
		talloc_free(mstore_ctx);
		return NULL;
	}

	mstore_ctx->context_list = NULL;
	mstore_ctx->indexing_list = talloc_zero(mstore_ctx, struct indexing_context_list);
	mstore_ctx->replica_mapping_ctx = NULL;
	mstore_ctx->notifications = NULL;
	mstore_ctx->subscriptions = NULL;

	mstore_ctx->nprops_ctx = NULL;
	retval = mapistore_namedprops_init(mstore_ctx, &(mstore_ctx->nprops_ctx));

	DEBUG(5, ("initted mstore_ctx ref: %p\n", mstore_ctx));

	return mstore_ctx;
}


/**
   \details Release the mapistore context and destroy any data
   associated

   \param mstore_ctx pointer to the mapistore context

   \note The function needs to rely on talloc destructors which is not
   implemented in code yet.

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE error
 */
_PUBLIC_ int mapistore_release(struct mapistore_context *mstore_ctx)
{
	if (!mstore_ctx) return MAPISTORE_ERR_NOT_INITIALIZED;

	DEBUG(5, ("freeing up mstore_ctx ref: %p\n", mstore_ctx));

	talloc_free(mstore_ctx->nprops_ctx);
	talloc_free(mstore_ctx->processing_ctx);
	talloc_free(mstore_ctx->context_list);
	talloc_free(mstore_ctx);

	return MAPISTORE_SUCCESS;
}


/**
   \details Add a new connection context to mapistore

   \param mstore_ctx pointer to the mapistore context
   \param uri the connection context URI
   \param pointer to the context identifier the function returns

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE error
 */
_PUBLIC_ int mapistore_add_context(struct mapistore_context *mstore_ctx,
				   const char *uri, uint64_t fid, uint32_t *context_id, void **backend_object)
{
	TALLOC_CTX				*mem_ctx;
	int					retval;
	struct backend_context			*backend_ctx;
	struct backend_context_list    		*backend_list;
	char					*namespace;
	char					*namespace_start;
	char					*backend_uri;

	/* Step 1. Perform Sanity Checks on URI */
	if (!uri || strlen(uri) < 4) {
		return MAPISTORE_ERR_INVALID_NAMESPACE;
	}

	mem_ctx = talloc_named(NULL, 0, "mapistore_add_context");
	namespace = talloc_strdup(mem_ctx, uri);
	namespace_start = namespace;
	namespace = strchr(namespace, ':');
	if (!namespace) {
		DEBUG(0, ("[%s:%d]: Error - Invalid namespace '%s'\n", __FUNCTION__, __LINE__, namespace_start));
		talloc_free(mem_ctx);
		return MAPISTORE_ERR_INVALID_NAMESPACE;
	}

	if (namespace[1] && namespace[1] == '/' &&
	    namespace[2] && namespace[2] == '/' &&
	    namespace[3]) {
		backend_uri = talloc_strdup(mem_ctx, &namespace[3]);
		namespace[3] = '\0';
		backend_ctx = mapistore_backend_create_context(mstore_ctx, mstore_ctx->conn_info, namespace_start, backend_uri, fid);
		if (!backend_ctx) {
			return MAPISTORE_ERR_CONTEXT_FAILED;
		}

		backend_list = talloc_zero((TALLOC_CTX *) mstore_ctx, struct backend_context_list);
		backend_list->ctx = backend_ctx;
		retval = mapistore_get_context_id(mstore_ctx->processing_ctx, &backend_list->ctx->context_id);
		if (retval != MAPISTORE_SUCCESS) {
			talloc_free(mem_ctx);
			return MAPISTORE_ERR_CONTEXT_FAILED;
		}
		*context_id = backend_list->ctx->context_id;
		*backend_object = backend_list->ctx->root_folder_object;
		DLIST_ADD_END(mstore_ctx->context_list, backend_list, struct backend_context_list *);

	} else {
		DEBUG(0, ("[%s:%d]: Error - Invalid URI '%s'\n", __FUNCTION__, __LINE__, uri));
		talloc_free(mem_ctx);
		return MAPISTORE_ERR_INVALID_NAMESPACE;
	}

	talloc_free(mem_ctx);
	return MAPISTORE_SUCCESS;
}


/**
   \details Increase the reference counter of an existing context

   \param mstore_ctx pointer to the mapistore context
   \param contex_id the context identifier referencing the context to
   update

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE error
 */
_PUBLIC_ int mapistore_add_context_ref_count(struct mapistore_context *mstore_ctx,
					     uint32_t context_id)
{
	struct backend_context		*backend_ctx;
	int				retval;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	if (context_id == -1) return MAPISTORE_ERROR;

	/* Step 0. Ensure the context exists */
	DEBUG(0, ("mapistore_add_context_ref_count: context_is to increment is %d\n", context_id));
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 1. Increment the ref count */
	retval = mapistore_backend_add_ref_count(backend_ctx);

	return retval;
}


/**
   \details Search for an existing context given its uri

   \param mstore_ctx pointer to the mapistore context
   \param uri the URI to lookup
   \param context_id pointer to the context identifier to return

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE error
 */
_PUBLIC_ int mapistore_search_context_by_uri(struct mapistore_context *mstore_ctx,
					     const char *uri, uint32_t *context_id, void **backend_object)
{
	struct backend_context		*backend_ctx;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	if (!uri) return MAPISTORE_ERROR;

	backend_ctx = mapistore_backend_lookup_by_uri(mstore_ctx->context_list, uri);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_NOT_FOUND, NULL);

	*context_id = backend_ctx->context_id;
	*backend_object = backend_ctx->root_folder_object;

	return MAPISTORE_SUCCESS;
}


/**
   \details Delete an existing connection context from mapistore

   \param mstore_ctx pointer to the mapistore context
   \param context_id the context identifier referencing the context to
   delete

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE error
 */
_PUBLIC_ int mapistore_del_context(struct mapistore_context *mstore_ctx, 
				   uint32_t context_id)
{
	struct backend_context_list	*backend_list;
	struct backend_context		*backend_ctx;
	int				retval;
	bool				found = false;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	if (context_id == -1) return MAPISTORE_ERROR;

	/* Step 0. Ensure the context exists */
	DEBUG(0, ("mapistore_del_context: context_id to del is %d\n", context_id));
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* search the backend_list item */
	for (backend_list = mstore_ctx->context_list; backend_list; backend_list = backend_list->next) {
		if (backend_list->ctx->context_id == context_id) {
			found = true;
			break;
		}		
	}
	if (found == false) {
		return MAPISTORE_ERROR;
	}

	/* Step 1. Delete the context within backend */
	retval = mapistore_backend_delete_context(backend_ctx);
	switch (retval) {
	case MAPISTORE_ERR_REF_COUNT:
		return MAPISTORE_SUCCESS;
	case MAPISTORE_SUCCESS:
		DLIST_REMOVE(mstore_ctx->context_list, backend_list);
		/* Step 2. Add the free'd context id to the free list */
		retval = mapistore_free_context_id(mstore_ctx->processing_ctx, context_id);
		break;
	default:
		return retval;
	}

	return retval;
}


/**
   \details Associate an indexing context to a mapistore context

   \param mstore_ctx pointer to the mapistore context
   \param username account name referencing the indexing record
   \param context_id the context identifier referencing the context to
   alter

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE error
 */
_PUBLIC_ int mapistore_add_context_indexing(struct mapistore_context *mstore_ctx,
					    const char *username,
					    uint32_t context_id)
{
	struct indexing_context_list	*indexing_ctx;
	struct backend_context		*backend_ctx;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);
	MAPISTORE_RETVAL_IF(!username, MAPISTORE_ERROR, NULL);
	MAPISTORE_RETVAL_IF(context_id == -1, MAPISTORE_ERROR, NULL);

	/* Step 0. Ensure the context exists */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);
	/* If the indexing pointer is already existing, return success */
	MAPISTORE_RETVAL_IF(backend_ctx->indexing, MAPISTORE_SUCCESS, NULL);

	/* Step 1. Search the indexing record */
	indexing_ctx = mapistore_indexing_search(mstore_ctx, username);
	MAPISTORE_RETVAL_IF(!indexing_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Reference the indexing record within backend context */
	backend_ctx->indexing = indexing_ctx;

	/* Step 3. Increment the indexing ref counter */
	mapistore_indexing_add_ref_count(indexing_ctx);

	DEBUG(0, ("mapistore_add_context_indexing username: %s\n", backend_ctx->indexing->username));

	return MAPISTORE_SUCCESS;
}


void mapistore_set_errno(int status)
{
	errno = status;
}


/**
   \details return a string explaining what a mapistore error constant
   means.

   \param mapistore_err the mapistore error constant

   \return constant string
 */
_PUBLIC_ const char *mapistore_errstr(int mapistore_err)
{
	switch (mapistore_err) {
	case MAPISTORE_SUCCESS:
		return "Success";
	case MAPISTORE_ERROR:
		return "Non-specific error";
	case MAPISTORE_ERR_NO_MEMORY:
		return "No memory available";
	case MAPISTORE_ERR_ALREADY_INITIALIZED:
		return "Already initialized";
	case MAPISTORE_ERR_NOT_INITIALIZED:
		return "Not initialized";
	case MAPISTORE_ERR_NO_DIRECTORY:
		return "No such file or directory";
	case MAPISTORE_ERR_DATABASE_INIT:
		return "Database initialization failed";
	case MAPISTORE_ERR_DATABASE_OPS:
		return "database operation failed";
	case MAPISTORE_ERR_BACKEND_REGISTER:
		return "storage backend registration failed";
	case MAPISTORE_ERR_BACKEND_INIT:
		return "storage backend initialization failed";
	}

	return "Unknown error";
}


/**
   \details Open a directory in mapistore

   \param mstore_ctx pointer to the mapistore context
   \param context_id the context identifier referencing the backend
   where the directory will be opened
   \param parent_fid the parent folder identifier
   \param fid folder identifier to open

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE errors
 */
_PUBLIC_ int mapistore_folder_open_folder(struct mapistore_context *mstore_ctx, uint32_t context_id,
					  void *folder, TALLOC_CTX *mem_ctx, uint64_t fid, void **child_folder)
{
	struct backend_context		*backend_ctx;
	int				ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend open_folder */
	ret = mapistore_backend_folder_open_folder(backend_ctx, folder, mem_ctx, fid, child_folder);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

/**
   \details Create a directory in mapistore

   \param mstore_ctx pointer to the mapistore context
   \param context_id the context identifier referencing the backend
   where the directory will be created
   \param parent_fid the parent folder identifier
   \param new_fid the folder identifier for the new folder
   \param aRow pointer to MAPI data structures with properties to be
   added to the new folder

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE errors
 */
_PUBLIC_ int mapistore_folder_create_folder(struct mapistore_context *mstore_ctx, uint32_t context_id,
					    void *folder, TALLOC_CTX *mem_ctx, uint64_t fid, struct SRow *aRow, void **child_folder)
{
	struct backend_context		*backend_ctx;
	int				ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);	
	
	/* Step 2. Call backend create_folder */
	ret = mapistore_backend_folder_create_folder(backend_ctx, folder, mem_ctx, fid, aRow, child_folder);

	return ret;
}


/**
   \details Remove a directory in mapistore

   \param mstore_ctx pointer to the mapistore context
   \param context_id the context identifier referencing the backend
   \param parent_fid the parent folder identifier
   \param fid the folder identifier representing the folder to delete
   \param flags flags that control the behaviour of the operation

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE errors
 */
_PUBLIC_ int mapistore_folder_delete_folder(struct mapistore_context *mstore_ctx, uint32_t context_id,
					    void *folder, uint64_t fid, uint8_t flags)
{
	struct backend_context		*backend_ctx;
	int				ret;
	TALLOC_CTX			*mem_ctx;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	mem_ctx = talloc_zero(NULL, TALLOC_CTX);

	/* Step 1. Find the backend context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);	

	/* Step 2. Handle deletion of child folders / messages */
	if (flags | DEL_FOLDERS) {
		uint64_t	*childFolders;
		uint32_t	childFolderCount;
		int		retval;
		uint32_t	i;

		/* Get subfolders list */
		retval = mapistore_folder_get_child_fids(mstore_ctx, context_id, folder, mem_ctx, &childFolders, &childFolderCount);
		if (retval) {
			DEBUG(4, ("mapistore_delete_folder bad retval: 0x%x", retval));
			return MAPI_E_NOT_FOUND;
		}

		/* Delete each subfolder in mapistore */
		for (i = 0; i < childFolderCount; ++i) {
			retval = mapistore_folder_delete_folder(mstore_ctx, context_id, folder, childFolders[i], flags);
			if (retval) {
				  DEBUG(4, ("mapistore_delete_folder failed to delete fid 0x%"PRIx64" (0x%x)", childFolders[i], retval));
				  talloc_free(mem_ctx);
				  return MAPI_E_NOT_FOUND;
			}
		}
	}
	
	/* Step 3. Call backend delete_folder */
	ret = mapistore_backend_folder_delete_folder(backend_ctx, folder, fid);

	talloc_free(mem_ctx);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

/**
   \details Open a message in mapistore

   \param mstore_ctx pointer to the mapistore context
   \param context_id the context identifier referencing the backend
   where the directory will be opened
   \param parent_fid the parent folder identifier
   \param mid the message identifier to open
   \param pointer to the mapistore_message structure

   \return MAPISTORE SUCCESS on success, otherwise MAPISTORE errors
 */
_PUBLIC_ int mapistore_folder_open_message(struct mapistore_context *mstore_ctx, uint32_t context_id,
					   void *folder, TALLOC_CTX *mem_ctx, uint64_t mid, void **messagep, struct mapistore_message **msg)
{
	struct backend_context		*backend_ctx;
	int				ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend open_message */
	ret = mapistore_backend_folder_open_message(backend_ctx, folder, mem_ctx, mid, messagep, msg);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}


/**
   \details Create a message in mapistore

   \param mstore_ctx pointer to the mapistore context

   \param context_id the context identifier referencing the backend
   where the messagewill be created
   \param parent_fid the parent folder identifier
   \param mid the message identifier to create

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE errors
 */
_PUBLIC_ int mapistore_folder_create_message(struct mapistore_context *mstore_ctx, uint32_t context_id,
					     void *folder, TALLOC_CTX *mem_ctx, uint64_t mid, uint8_t associated, void **messagep)
{
	struct backend_context		*backend_ctx;
	int				ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);
	
	/* Step 2. Call backend create_message */
	ret = mapistore_backend_folder_create_message(backend_ctx, folder, mem_ctx, mid, associated, messagep);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

/**
   \details Delete a message from mapistore

   \param mstore_ctx pointer to the mapistore context
   \param context_id the context identifier referencing the backend
   where the message's to be located is stored
   \param mid the message identifier of the folder to delete
   \param flags flags that control the behaviour of the operation (MAPISTORE_SOFT_DELETE
   or MAPISTORE_PERMANENT_DELETE)

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE errors
 */
_PUBLIC_ int mapistore_folder_delete_message(struct mapistore_context *mstore_ctx, uint32_t context_id,
					     void *folder, uint64_t mid, uint8_t flags)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend operation */
	ret = mapistore_backend_folder_delete_message(backend_ctx, folder, mid, flags);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

/**
   \details Retrieve the number of child folders within a mapistore
   folder

   \param mstore_ctx pointer to the mapistore context
   \param context_id the context identifier referencing the backend
   \param fid the folder identifier
   \param RowCount pointer to the count result to return

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE errors
 */
_PUBLIC_ int mapistore_folder_get_folder_count(struct mapistore_context *mstore_ctx, uint32_t context_id,
					       void *folder, uint32_t *RowCount)
{
	struct backend_context		*backend_ctx;
	int				ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 0. Ensure the context exists */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 1. Call backend readdir */
	ret = mapistore_backend_folder_get_child_count(backend_ctx, folder, MAPISTORE_FOLDER_TABLE, RowCount);

	return ret;
}


/**
   \details Retrieve the number of child messages within a mapistore folder

   \param mstore_ctx pointer to the mapistore context
   \param context_id the context identifier referencing the backend
   \param fid the folder identifier
   \param RowCount pointer to the count result to return

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE errors
 */
_PUBLIC_ int mapistore_folder_get_message_count(struct mapistore_context *mstore_ctx, uint32_t context_id,
						void *folder, uint8_t table_type, uint32_t *RowCount)
{
	struct backend_context		*backend_ctx;
	int				ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 0. Ensure the context exists */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend get_child_count */
	ret = mapistore_backend_folder_get_child_count(backend_ctx, folder, table_type, RowCount);

	return ret;
}

/**
   \details Retrieve the folder IDs of child folders within a mapistore
   folder

   \param mstore_ctx pointer to the mapistore context
   \param context_id the context identifier referencing the backend
   \param fid the folder identifier (for the parent folder)
   \param child_fids pointer to where to return the array of child fids
   \param child_fid_count pointer to the count result to return

   \note The caller is responsible for freeing the \p child_fids array
   when it is no longer required.
   
   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE errors
 */
_PUBLIC_ int mapistore_folder_get_child_fids(struct mapistore_context *mstore_ctx, uint32_t context_id,
					     void *folder, TALLOC_CTX *mem_ctx, uint64_t *child_fids[], uint32_t *child_fid_count)
{
	DEBUG(0, ("unimplemented\n"));
	abort();

	return 0;
}

_PUBLIC_ int mapistore_folder_open_table(struct mapistore_context *mstore_ctx, uint32_t context_id,
					 void *folder, TALLOC_CTX *mem_ctx, uint8_t table_type, uint32_t handle_id, void **table, uint32_t *row_count)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend operation */
	ret = mapistore_backend_folder_open_table(backend_ctx, folder, mem_ctx, table_type, handle_id, table, row_count);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

/**
   \details Modify recipients of a message in mapistore

   \param mstore_ctx pointer to the mapistore context
   \param context_id the context identifier referencing the backend
   where properties will be stored
   \param mid the identifier referencing the message
   \rows the array of recipient rows
   \count the number of elements in the array

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE errors
 */
int mapistore_message_modify_recipients(struct mapistore_context *mstore_ctx, uint32_t context_id, void *message, struct ModifyRecipientRow *rows, uint16_t count)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend modifyrecipients */
	ret = mapistore_backend_message_modify_recipients(backend_ctx, message, rows, count);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

/**
   \details Commit the changes made to a message in mapistore

   \param mstore_ctx pointer to the mapistore context
   \param context_id the context identifier referencing the backend
   where the message's changes will be saved
   \param mid the message identifier to save
   \param flags flags associated to the commit operation

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE errors
 */
_PUBLIC_ int mapistore_message_save(struct mapistore_context *mstore_ctx, uint32_t context_id,
				    void *message)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend savechangesmessage */
	ret = mapistore_backend_message_save(backend_ctx, message);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}


/**
   \details Submits a message for sending.

   \param mstore_ctx pointer to the mapistore context
   \param context_id the context identifier referencing the backend
   where the message will be submitted
   \param mid the message identifier representing the message to submit
   \param flags flags associated to the submit operation

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE errors
 */
_PUBLIC_ int mapistore_message_submit(struct mapistore_context *mstore_ctx, uint32_t context_id,
				      void *message, enum SubmitFlags flags)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend submitmessage */
	ret = mapistore_backend_message_submit(backend_ctx, message, flags);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

_PUBLIC_ int mapistore_message_get_attachment_table(struct mapistore_context *mstore_ctx, uint32_t context_id,
						    void *message, TALLOC_CTX *mem_ctx, void **table, uint32_t *row_count)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend operation */
	ret = mapistore_backend_message_get_attachment_table(backend_ctx, message, mem_ctx, table, row_count);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

_PUBLIC_ int mapistore_message_open_attachment(struct mapistore_context *mstore_ctx, uint32_t context_id,
					       void *message, TALLOC_CTX *mem_ctx, uint32_t aid, void **attachment)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend operation */
	ret = mapistore_backend_message_open_attachment(backend_ctx, message, mem_ctx, aid, attachment);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

_PUBLIC_ int mapistore_message_create_attachment(struct mapistore_context *mstore_ctx, uint32_t context_id,
						 void *message, TALLOC_CTX *mem_ctx, void **attachment, uint32_t *aid)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend operation */
	ret = mapistore_backend_message_create_attachment(backend_ctx, message, mem_ctx, attachment, aid);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

_PUBLIC_ int mapistore_message_attachment_open_embedded_message(struct mapistore_context *mstore_ctx, uint32_t context_id, void *message, TALLOC_CTX *mem_ctx, void **embedded_message, uint64_t *mid, struct mapistore_message **msg)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend operation */
	ret = mapistore_backend_message_attachment_open_embedded_message(backend_ctx, message, mem_ctx, embedded_message, mid, msg);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

_PUBLIC_ int mapistore_table_get_available_properties(struct mapistore_context *mstore_ctx, uint32_t context_id, void *table, TALLOC_CTX *mem_ctx, struct SPropTagArray **propertiesp)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend operation */
	ret = mapistore_backend_table_get_available_properties(backend_ctx, table, mem_ctx, propertiesp);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

_PUBLIC_ int mapistore_table_set_columns(struct mapistore_context *mstore_ctx, uint32_t context_id, void *table, uint16_t count, enum MAPITAGS *properties)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend operation */
	ret = mapistore_backend_table_set_columns(backend_ctx, table, count, properties);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

_PUBLIC_ int mapistore_table_set_restrictions(struct mapistore_context *mstore_ctx, uint32_t context_id, void *table, struct mapi_SRestriction *restrictions, uint8_t *table_status)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend operation */
	ret = mapistore_backend_table_set_restrictions(backend_ctx, table, restrictions, table_status);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

_PUBLIC_ int mapistore_table_set_sort_order(struct mapistore_context *mstore_ctx, uint32_t context_id, void *table, struct SSortOrderSet *sort_order, uint8_t *table_status)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend operation */
	ret = mapistore_backend_table_set_sort_order(backend_ctx, table, sort_order, table_status);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

_PUBLIC_ int mapistore_table_get_row(struct mapistore_context *mstore_ctx, uint32_t context_id, void *table, TALLOC_CTX *mem_ctx,
				     enum table_query_type query_type, uint32_t rowid, struct mapistore_property_data **data)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend operation */
	ret = mapistore_backend_table_get_row(backend_ctx, table, mem_ctx, query_type, rowid, data);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

_PUBLIC_ int mapistore_properties_get_available_properties(struct mapistore_context *mstore_ctx, uint32_t context_id, void *object, TALLOC_CTX *mem_ctx, struct SPropTagArray **propertiesp)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend operation */
	ret = mapistore_backend_properties_get_available_properties(backend_ctx, object, mem_ctx, propertiesp);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}


_PUBLIC_ int mapistore_properties_get_properties(struct mapistore_context *mstore_ctx, uint32_t context_id,
						 void *object, TALLOC_CTX *mem_ctx,
						 uint16_t count, enum MAPITAGS *properties,
						 struct mapistore_property_data *data)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend operation */
	ret = mapistore_backend_properties_get_properties(backend_ctx, object, mem_ctx, count, properties, data);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}

_PUBLIC_ int mapistore_properties_set_properties(struct mapistore_context
						 *mstore_ctx, uint32_t context_id,
						 void *object,
						 struct SRow *aRow)
{
	struct backend_context	*backend_ctx;
	int			ret;

	/* Sanity checks */
	MAPISTORE_SANITY_CHECKS(mstore_ctx, NULL);

	/* Step 1. Search the context */
	backend_ctx = mapistore_backend_lookup(mstore_ctx->context_list, context_id);
	MAPISTORE_RETVAL_IF(!backend_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Step 2. Call backend operation */
	ret = mapistore_backend_properties_set_properties(backend_ctx, object, aRow);

	return !ret ? MAPISTORE_SUCCESS : MAPISTORE_ERROR;
}
