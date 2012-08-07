/*
 * e-ews-backend.c
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the program; if not, see <http://www.gnu.org/licenses/>
 *
 */

#include "e-ews-backend.h"

#include <config.h>
#include <glib/gi18n-lib.h>

#include "server/e-source-ews-folder.h"

#define E_EWS_BACKEND_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_EWS_BACKEND, EEwsBackendPrivate))

typedef struct _SyncFoldersClosure SyncFoldersClosure;

struct _EEwsBackendPrivate {
	/* Folder ID -> ESource */
	GHashTable *folders;
	GMutex *folders_lock;

	ESource *gal_source;
	gchar *oal_selected;

	gchar *sync_state;
	GMutex *sync_state_lock;

	EEwsConnection *connection;
	GMutex *connection_lock;
};

struct _SyncFoldersClosure {
	EEwsBackend *backend;
	GSList *folders_created;
	GSList *folders_deleted;
	GSList *folders_updated;
};

G_DEFINE_DYNAMIC_TYPE (
	EEwsBackend,
	e_ews_backend,
	E_TYPE_COLLECTION_BACKEND)

static void
sync_folders_closure_free (SyncFoldersClosure *closure)
{
	g_object_unref (closure->backend);

	/* List of EEwsFolder instances. */
	g_slist_free_full (
		closure->folders_created,
		(GDestroyNotify) g_object_unref);

	/* List of folder ID strings. */
	g_slist_free_full (
		closure->folders_deleted,
		(GDestroyNotify) g_free);

	/* List of EEwsFolder instances. */
	g_slist_free_full (
		closure->folders_updated,
		(GDestroyNotify) g_object_unref);

	g_slice_free (SyncFoldersClosure, closure);
}

static gboolean
ews_backend_folders_contains (EEwsBackend *backend,
                              const gchar *folder_id)
{
	gboolean contains;

	g_return_val_if_fail (folder_id != NULL, FALSE);

	g_mutex_lock (backend->priv->folders_lock);

	contains = g_hash_table_contains (backend->priv->folders, folder_id);

	g_mutex_unlock (backend->priv->folders_lock);

	return contains;
}

static void
ews_backend_folders_insert (EEwsBackend *backend,
                            const gchar *folder_id,
                            ESource *source)
{
	g_return_if_fail (folder_id != NULL);
	g_return_if_fail (E_IS_SOURCE (source));

	g_mutex_lock (backend->priv->folders_lock);

	g_hash_table_insert (
		backend->priv->folders,
		g_strdup (folder_id),
		g_object_ref (source));

	g_mutex_unlock (backend->priv->folders_lock);
}

static ESource *
ews_backend_folders_lookup (EEwsBackend *backend,
                            const gchar *folder_id)
{
	ESource *source;

	g_return_val_if_fail (folder_id != NULL, NULL);

	g_mutex_lock (backend->priv->folders_lock);

	source = g_hash_table_lookup (backend->priv->folders, folder_id);

	if (source != NULL)
		g_object_ref (source);

	g_mutex_unlock (backend->priv->folders_lock);

	return source;
}

static gboolean
ews_backend_folders_remove (EEwsBackend *backend,
                            const gchar *folder_id)
{
	gboolean removed;

	g_return_val_if_fail (folder_id != NULL, FALSE);

	g_mutex_lock (backend->priv->folders_lock);

	removed = g_hash_table_remove (backend->priv->folders, folder_id);

	g_mutex_unlock (backend->priv->folders_lock);

	return removed;
}

static CamelEwsSettings *
ews_backend_get_settings (EEwsBackend *backend)
{
	ESource *source;
	ESourceCamel *extension;
	CamelSettings *settings;
	const gchar *extension_name;

	source = e_backend_get_source (E_BACKEND (backend));
	extension_name = e_source_camel_get_extension_name ("ews");
	extension = e_source_get_extension (source, extension_name);
	settings = e_source_camel_get_settings (extension);

	return CAMEL_EWS_SETTINGS (settings);
}

static ESource *
ews_backend_new_child (EEwsBackend *backend,
                       EEwsFolder *folder)
{
	ECollectionBackend *collection_backend;
	ESourceExtension *extension;
	ESource *source;
	const EwsFolderId *fid;
	const gchar *display_name;
	const gchar *extension_name;

	fid = e_ews_folder_get_id (folder);

	collection_backend = E_COLLECTION_BACKEND (backend);
	source = e_collection_backend_new_child (collection_backend, fid->id);

	display_name = e_ews_folder_get_name (folder);
	e_source_set_display_name (source, display_name);

	switch (e_ews_folder_get_folder_type (folder)) {
		case E_EWS_FOLDER_TYPE_CALENDAR:
			extension_name = E_SOURCE_EXTENSION_CALENDAR;
			break;
		case E_EWS_FOLDER_TYPE_TASKS:
			extension_name = E_SOURCE_EXTENSION_TASK_LIST;
			break;
		case E_EWS_FOLDER_TYPE_CONTACTS:
			extension_name = E_SOURCE_EXTENSION_ADDRESS_BOOK;
			break;
		default:
			g_object_unref (source);
			g_return_val_if_reached (NULL);
	}
	extension = e_source_get_extension (source, extension_name);
	e_source_backend_set_backend_name (
		E_SOURCE_BACKEND (extension), "ews");

	extension_name = E_SOURCE_EXTENSION_EWS_FOLDER;
	extension = e_source_get_extension (source, extension_name);
	e_source_ews_folder_set_id (
		E_SOURCE_EWS_FOLDER (extension), fid->id);
	e_source_ews_folder_set_change_key (
		E_SOURCE_EWS_FOLDER (extension), fid->change_key);

	extension_name = E_SOURCE_EXTENSION_OFFLINE;
	extension = e_source_get_extension (source, extension_name);
	e_source_offline_set_stay_synchronized (
		E_SOURCE_OFFLINE (extension), TRUE);

	e_server_side_source_set_remote_deletable (
		E_SERVER_SIDE_SOURCE (source), TRUE);

	return source;
}

static ESource *
ews_backend_new_calendar (EEwsBackend *backend,
                          EEwsFolder *folder)
{
	/* No extra configuration to do. */
	return ews_backend_new_child (backend, folder);
}

static ESource *
ews_backend_new_task_list (EEwsBackend *backend,
                           EEwsFolder *folder)
{
	/* No extra configuration to do. */
	return ews_backend_new_child (backend, folder);
}

static ESource *
ews_backend_new_address_book (EEwsBackend *backend,
                              EEwsFolder *folder)
{
	/* No extra configuration to do. */
	return ews_backend_new_child (backend, folder);
}

static void
ews_backend_sync_created_folders (EEwsBackend *backend,
                                  GSList *list)
{
	ECollectionBackend *collection_backend;
	ESourceRegistryServer *server;
	GSList *link;

	collection_backend = E_COLLECTION_BACKEND (backend);
	server = e_collection_backend_ref_server (collection_backend);

	for (link = list; link != NULL; link = g_slist_next (link)) {
		EEwsFolder *folder = E_EWS_FOLDER (link->data);
		const EwsFolderId *fid;
		ESource *source = NULL;

		/* If we already know about this folder, skip it. */
		fid = e_ews_folder_get_id (folder);
		if (fid->id == NULL)
			continue;  /* not a valid ID anyway */
		if (ews_backend_folders_contains (backend, fid->id))
			continue;

		switch (e_ews_folder_get_folder_type (folder)) {
			case E_EWS_FOLDER_TYPE_CALENDAR:
				source = ews_backend_new_calendar (
					backend, folder);
				break;
			case E_EWS_FOLDER_TYPE_TASKS:
				source = ews_backend_new_task_list (
					backend, folder);
				break;
			case E_EWS_FOLDER_TYPE_CONTACTS:
				source = ews_backend_new_address_book (
					backend, folder);
				break;
			default:
				break;
		}

		if (source != NULL) {
			e_source_registry_server_add_source (server, source);
			g_object_unref (source);
		}
	}

	g_object_unref (server);
}

static void
ews_backend_sync_deleted_folders (EEwsBackend *backend,
                                  GSList *list)
{
	ECollectionBackend *collection_backend;
	ESourceRegistryServer *server;
	GSList *link;

	collection_backend = E_COLLECTION_BACKEND (backend);
	server = e_collection_backend_ref_server (collection_backend);

	for (link = list; link != NULL; link = g_slist_next (link)) {
		const gchar *folder_id = link->data;
		ESource *source = NULL;

		if (folder_id != NULL)
			source = ews_backend_folders_lookup (
				backend, folder_id);

		if (source == NULL)
			continue;

		/* This will trigger a "child-removed" signal and
		 * our handler will remove the hash table entry. */
		e_source_registry_server_remove_source (server, source);

		g_object_unref (source);
	}

	g_object_unref (server);
}

static void
ews_backend_add_gal_source (EEwsBackend *backend)
{
	ECollectionBackend *collection_backend;
	ESourceAutocomplete *autocomplete_extension;
	ESourceBackend *backend_extension;
	ESourceEwsFolder *folder_extension;
	ESourceOffline *offline_extension;
	ESourceRegistryServer *server;
	ESource *source = NULL;
	CamelEwsSettings *settings;
	const gchar *display_name;
	const gchar *extension_name;
	const gchar *gal_uid;
	const gchar *oal_id;
	const gchar *uid;
	gchar *oal_selected;

	settings = ews_backend_get_settings (backend);
	collection_backend = E_COLLECTION_BACKEND (backend);

	gal_uid = camel_ews_settings_get_gal_uid (settings);

	if (gal_uid != NULL) {
		server = e_collection_backend_ref_server (collection_backend);
		source = e_source_registry_server_ref_source (server, gal_uid);
		g_object_unref (server);

		if (source != NULL) {
			g_object_unref (source);
			return;
		}
	}

	oal_selected = camel_ews_settings_dup_oal_selected (settings);

	/* This is supposed to be in the form: ID ':' NAME */
	if (oal_selected != NULL) {
		gchar *cp = strrchr (oal_selected, ':');
		if (cp != NULL) {
			*cp++ = '\0';
			display_name = cp;
			oal_id = oal_selected;
		} else {
			g_free (oal_selected);
			oal_selected = NULL;
		}
	}

	if (oal_selected == NULL) {
		display_name = _("Global Address List");
		oal_id = "global-address-list";
	}

	g_free (backend->priv->oal_selected);
	backend->priv->oal_selected = oal_selected;  /* takes ownership */

	if (oal_id != NULL)
		source = e_collection_backend_new_child (
			collection_backend, oal_id);
	else
		source = e_collection_backend_new_child (
			collection_backend, "Global Address List");

	e_source_set_display_name (source, display_name);

	extension_name = E_SOURCE_EXTENSION_ADDRESS_BOOK;
	backend_extension = e_source_get_extension (source, extension_name);
	e_source_backend_set_backend_name (backend_extension, "ews");

	extension_name = E_SOURCE_EXTENSION_AUTOCOMPLETE;
	autocomplete_extension = e_source_get_extension (source, extension_name);
	e_source_autocomplete_set_include_me (autocomplete_extension, TRUE);

	extension_name = E_SOURCE_EXTENSION_EWS_FOLDER;
	folder_extension = e_source_get_extension (source, extension_name);
	e_source_ews_folder_set_id (folder_extension, oal_id);

	extension_name = E_SOURCE_EXTENSION_OFFLINE;
	offline_extension = e_source_get_extension (source, extension_name);
	e_source_offline_set_stay_synchronized (offline_extension, TRUE);

	server = e_collection_backend_ref_server (collection_backend);
	e_source_registry_server_add_source (server, source);
	g_object_unref (server);

	uid = e_source_get_uid (source);
	camel_ews_settings_set_gal_uid (settings, uid);

	g_object_unref (source);
}

static void
ews_backend_source_changed_cb (ESource *source,
                               EEwsBackend *backend)
{
	CamelEwsSettings *settings;
	const gchar *oal_selected;
	const gchar *gal_uid;

	settings = ews_backend_get_settings (backend);
	gal_uid = camel_ews_settings_get_gal_uid (settings);
	oal_selected = camel_ews_settings_get_oal_selected (settings);

	if (g_strcmp0 (oal_selected, backend->priv->oal_selected) == 0)
		return;

	/* Remove the old Global Address List source if present. */
	if (gal_uid != NULL) {
		ECollectionBackend *collection_backend;
		ESourceRegistryServer *server;

		collection_backend = E_COLLECTION_BACKEND (backend);
		server = e_collection_backend_ref_server (collection_backend);
		source = e_source_registry_server_ref_source (server, gal_uid);

		if (source != NULL) {
			e_source_registry_server_remove_source (server, source);
			g_object_unref (source);
		}

		camel_ews_settings_set_gal_uid (settings, NULL);
	}

	ews_backend_add_gal_source (backend);
}

static gboolean
ews_backend_sync_folders_idle_cb (gpointer user_data)
{
	SyncFoldersClosure *closure = user_data;

	/* FIXME Handle updated folders. */

	ews_backend_sync_deleted_folders (
		closure->backend, closure->folders_deleted);
	ews_backend_sync_created_folders (
		closure->backend, closure->folders_created);

	return FALSE;
}

static void
ews_backend_dispose (GObject *object)
{
	EEwsBackendPrivate *priv;

	priv = E_EWS_BACKEND_GET_PRIVATE (object);

	g_hash_table_remove_all (priv->folders);

	if (priv->connection != NULL) {
		g_object_unref (priv->connection);
		priv->connection = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_ews_backend_parent_class)->dispose (object);
}

static void
ews_backend_finalize (GObject *object)
{
	EEwsBackendPrivate *priv;

	priv = E_EWS_BACKEND_GET_PRIVATE (object);

	g_hash_table_destroy (priv->folders);
	g_mutex_free (priv->folders_lock);

	g_free (priv->oal_selected);

	g_free (priv->sync_state);
	g_mutex_free (priv->sync_state_lock);

	g_mutex_free (priv->connection_lock);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_ews_backend_parent_class)->finalize (object);
}

static void
ews_backend_constructed (GObject *object)
{
	ESource *source;

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (e_ews_backend_parent_class)->constructed (object);

	source = e_backend_get_source (E_BACKEND (object));

	/* XXX Wondering if we ought to delay this until after folders
	 *     are initially populated, just to remove the possibility
	 *     of weird races with clients trying to create folders. */
	e_server_side_source_set_remote_creatable (
		E_SERVER_SIDE_SOURCE (source), TRUE);
}

static void
ews_backend_populate (ECollectionBackend *backend)
{
	ESource *source;

	/* For now at least, we don't need to know the
	 * results, so no callback function is needed. */
	e_ews_backend_sync_folders (
		E_EWS_BACKEND (backend), NULL, NULL, NULL);

	ews_backend_add_gal_source (E_EWS_BACKEND (backend));

	source = e_backend_get_source (E_BACKEND (backend));

	g_signal_connect (
		source, "changed",
		G_CALLBACK (ews_backend_source_changed_cb), backend);
}

static gchar *
ews_backend_dup_resource_id (ECollectionBackend *backend,
                             ESource *child_source)
{
	ESourceEwsFolder *extension;
	const gchar *extension_name;

	extension_name = E_SOURCE_EXTENSION_EWS_FOLDER;
	extension = e_source_get_extension (child_source, extension_name);

	return e_source_ews_folder_dup_id (extension);
}

static void
ews_backend_child_added (ECollectionBackend *backend,
                         ESource *child_source)
{
	ESource *collection_source;
	const gchar *extension_name;
	gboolean is_mail = FALSE;

	collection_source = e_backend_get_source (E_BACKEND (backend));

	extension_name = E_SOURCE_EXTENSION_MAIL_ACCOUNT;
	is_mail |= e_source_has_extension (child_source, extension_name);

	extension_name = E_SOURCE_EXTENSION_MAIL_IDENTITY;
	is_mail |= e_source_has_extension (child_source, extension_name);

	extension_name = E_SOURCE_EXTENSION_MAIL_TRANSPORT;
	is_mail |= e_source_has_extension (child_source, extension_name);

	/* Synchronize mail-related display names with the collection. */
	if (is_mail)
		g_object_bind_property (
			collection_source, "display-name",
			child_source, "display-name",
			G_BINDING_SYNC_CREATE);

	/* Synchronize mail-related user with the collection identity. */
	extension_name = E_SOURCE_EXTENSION_AUTHENTICATION;
	if (is_mail && e_source_has_extension (child_source, extension_name)) {
		ESourceAuthentication *auth_child_extension;
		ESourceCollection *collection_extension;

		extension_name = E_SOURCE_EXTENSION_COLLECTION;
		collection_extension = e_source_get_extension (
			collection_source, extension_name);

		extension_name = E_SOURCE_EXTENSION_AUTHENTICATION;
		auth_child_extension = e_source_get_extension (
			child_source, extension_name);

		g_object_bind_property (
			collection_extension, "identity",
			auth_child_extension, "user",
			G_BINDING_SYNC_CREATE);
	}

	/* We track EWS folders in a hash table by folder ID. */
	extension_name = E_SOURCE_EXTENSION_EWS_FOLDER;
	if (e_source_has_extension (child_source, extension_name)) {
		ESourceEwsFolder *extension;
		gchar *folder_id;

		extension = e_source_get_extension (
			child_source, extension_name);
		folder_id = e_source_ews_folder_dup_id (extension);
		if (folder_id != NULL) {
			ews_backend_folders_insert (
				E_EWS_BACKEND (backend),
				folder_id, child_source);
			g_free (folder_id);
		}
	}

	/* Chain up to parent's child_added() method. */
	E_COLLECTION_BACKEND_CLASS (e_ews_backend_parent_class)->
		child_added (backend, child_source);
}

static void
ews_backend_child_removed (ECollectionBackend *backend,
                           ESource *child_source)
{
	const gchar *extension_name;

	/* We track EWS folders in a hash table by folder ID. */
	extension_name = E_SOURCE_EXTENSION_EWS_FOLDER;
	if (e_source_has_extension (child_source, extension_name)) {
		ESourceEwsFolder *extension;
		const gchar *folder_id;

		extension = e_source_get_extension (
			child_source, extension_name);
		folder_id = e_source_ews_folder_get_id (extension);
		if (folder_id != NULL)
			ews_backend_folders_remove (
				E_EWS_BACKEND (backend), folder_id);
	}

	/* Chain up to parent's child_removed() method. */
	E_COLLECTION_BACKEND_CLASS (e_ews_backend_parent_class)->
		child_removed (backend, child_source);
}

static gboolean
ews_backend_create_resource_sync (ECollectionBackend *backend,
                                  ESource *source,
                                  GCancellable *cancellable,
                                  GError **error)
{
	EEwsConnection *connection;
	EwsFolderId *out_folder_id = NULL;
	EEwsFolderType folder_type = E_EWS_FOLDER_TYPE_UNKNOWN;
	const gchar *extension_name;
	const gchar *parent_folder_id = NULL;
	gchar *folder_name;
	gboolean success = FALSE;

	connection = e_ews_backend_ref_connection_sync (
		E_EWS_BACKEND (backend), cancellable, error);
	if (connection == NULL)
		return FALSE;

	extension_name = E_SOURCE_EXTENSION_ADDRESS_BOOK;
	if (e_source_has_extension (source, extension_name)) {
		folder_type = E_EWS_FOLDER_TYPE_CONTACTS;
		parent_folder_id = "contacts";
	}

	extension_name = E_SOURCE_EXTENSION_CALENDAR;
	if (e_source_has_extension (source, extension_name)) {
		folder_type = E_EWS_FOLDER_TYPE_CALENDAR;
		parent_folder_id = "calendar";
	}

	extension_name = E_SOURCE_EXTENSION_TASK_LIST;
	if (e_source_has_extension (source, extension_name)) {
		folder_type = E_EWS_FOLDER_TYPE_TASKS;
		parent_folder_id = "tasks";
	}

	/* FIXME No support for memo lists. */

	if (parent_folder_id == NULL) {
		g_set_error (
			error, G_IO_ERROR,
			G_IO_ERROR_INVALID_ARGUMENT,
			_("Could not determine a suitable folder "
			"class for a new folder named '%s'"),
			e_source_get_display_name (source));
		goto exit;
	}

	folder_name = e_source_dup_display_name (source);

	success = e_ews_connection_create_folder_sync (
		connection, EWS_PRIORITY_MEDIUM,
		parent_folder_id, TRUE,
		folder_name, folder_type,
		&out_folder_id, cancellable, error);

	g_free (folder_name);

	/* Sanity check */
	g_warn_if_fail (
		(success && out_folder_id != NULL) ||
		(!success && out_folder_id == NULL));

	if (out_folder_id != NULL) {
		ESourceEwsFolder *extension;
		const gchar *extension_name;

		extension_name = E_SOURCE_EXTENSION_EWS_FOLDER;
		extension = e_source_get_extension (source, extension_name);
		e_source_ews_folder_set_id (
			extension, out_folder_id->id);
		e_source_ews_folder_set_change_key (
			extension, out_folder_id->change_key);

		e_ews_folder_id_free (out_folder_id);
	}

	if (success) {
		ESourceRegistryServer *server;
		ESource *parent_source;
		const gchar *cache_dir;
		const gchar *parent_uid;

		/* Configure the source as a collection member. */
		parent_source = e_backend_get_source (E_BACKEND (backend));
		parent_uid = e_source_get_uid (parent_source);
		e_source_set_parent (source, parent_uid);

		/* Changes should be written back to the cache directory. */
		cache_dir = e_collection_backend_get_cache_dir (backend);
		e_server_side_source_set_write_directory (
			E_SERVER_SIDE_SOURCE (source), cache_dir);

		/* Set permissions for clients. */
		e_server_side_source_set_writable (
			E_SERVER_SIDE_SOURCE (source), TRUE);
		e_server_side_source_set_remote_deletable (
			E_SERVER_SIDE_SOURCE (source), TRUE);

		server = e_collection_backend_ref_server (backend);
		e_source_registry_server_add_source (server, source);
		g_object_unref (server);
	}

exit:
	g_object_unref (connection);

	return success;
}

static gboolean
ews_backend_delete_resource_sync (ECollectionBackend *backend,
                                  ESource *source,
                                  GCancellable *cancellable,
                                  GError **error)
{
	EEwsConnection *connection;
	ESourceEwsFolder *extension;
	const gchar *extension_name;
	gchar *folder_id;
	gboolean success = FALSE;

	connection = e_ews_backend_ref_connection_sync (
		E_EWS_BACKEND (backend), cancellable, error);
	if (connection == NULL)
		return FALSE;

	extension_name = E_SOURCE_EXTENSION_EWS_FOLDER;
	if (!e_source_has_extension (source, extension_name)) {
		g_set_error (
			error, G_IO_ERROR,
			G_IO_ERROR_INVALID_ARGUMENT,
			_("Data source '%s' does not represent "
			"an Exchange Web Services folder"),
			e_source_get_display_name (source));
		goto exit;
	}
	extension = e_source_get_extension (source, extension_name);
	folder_id = e_source_ews_folder_dup_id (extension);

	success = e_ews_connection_delete_folder_sync (
		connection, EWS_PRIORITY_MEDIUM, folder_id,
		FALSE, "HardDelete", cancellable, error);

	if (success) {
		ESourceRegistryServer *server;

		server = e_collection_backend_ref_server (backend);
		e_source_registry_server_remove_source (server, source);
		g_object_unref (server);
	}

exit:
	g_object_unref (connection);

	return success;
}

static void
e_ews_backend_class_init (EEwsBackendClass *class)
{
	GObjectClass *object_class;
	ECollectionBackendClass *backend_class;

	g_type_class_add_private (class, sizeof (EEwsBackendPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->dispose = ews_backend_dispose;
	object_class->finalize = ews_backend_finalize;
	object_class->constructed = ews_backend_constructed;

	backend_class = E_COLLECTION_BACKEND_CLASS (class);
	backend_class->populate = ews_backend_populate;
	backend_class->dup_resource_id = ews_backend_dup_resource_id;
	backend_class->child_added = ews_backend_child_added;
	backend_class->child_removed = ews_backend_child_removed;
	backend_class->create_resource_sync = ews_backend_create_resource_sync;
	backend_class->delete_resource_sync = ews_backend_delete_resource_sync;

	/* This generates an ESourceCamel subtype for CamelEwsSettings. */
	e_source_camel_generate_subtype ("ews", CAMEL_TYPE_EWS_SETTINGS);
}

static void
e_ews_backend_class_finalize (EEwsBackendClass *class)
{
}

static void
e_ews_backend_init (EEwsBackend *backend)
{
	backend->priv = E_EWS_BACKEND_GET_PRIVATE (backend);

	backend->priv->folders = g_hash_table_new_full (
		(GHashFunc) g_str_hash,
		(GEqualFunc) g_str_equal,
		(GDestroyNotify) g_free,
		(GDestroyNotify) g_object_unref);

	backend->priv->folders_lock = g_mutex_new ();
	backend->priv->sync_state_lock = g_mutex_new ();
	backend->priv->connection_lock = g_mutex_new ();
}

void
e_ews_backend_type_register (GTypeModule *type_module)
{
	/* XXX G_DEFINE_DYNAMIC_TYPE declares a static type registration
	 *     function, so we have to wrap it with a public function in
	 *     order to register types from a separate compilation unit. */
	e_ews_backend_register_type (type_module);
}

static void
ews_backend_ref_connection_thread (GSimpleAsyncResult *simple,
                                   GObject *object,
                                   GCancellable *cancellable)
{
	EEwsConnection *connection;
	GError *error = NULL;

	connection = e_ews_backend_ref_connection_sync (
		E_EWS_BACKEND (object), cancellable, &error);

	/* Sanity check. */
	g_return_if_fail (
		((connection != NULL) && (error == NULL)) ||
		((connection == NULL) && (error != NULL)));

	if (connection != NULL)
		g_simple_async_result_set_op_res_gpointer (
			simple, connection, (GDestroyNotify) g_object_unref);

	if (error != NULL)
		g_simple_async_result_take_error (simple, error);
}

EEwsConnection *
e_ews_backend_ref_connection_sync (EEwsBackend *backend,
                                   GCancellable *cancellable,
                                   GError **error)
{
	EEwsConnection *connection = NULL;
	CamelEwsSettings *settings;
	gchar *hosturl;
	gboolean success;

	g_return_val_if_fail (E_IS_EWS_BACKEND (backend), NULL);

	g_mutex_lock (backend->priv->connection_lock);
	if (backend->priv->connection != NULL)
		connection = g_object_ref (backend->priv->connection);
	g_mutex_unlock (backend->priv->connection_lock);

	/* If we already have an authenticated
	 * connection object, just return that. */
	if (connection != NULL)
		return connection;

	settings = ews_backend_get_settings (backend);
	hosturl = camel_ews_settings_dup_hosturl (settings);
	connection = e_ews_connection_new (hosturl, settings);
	g_free (hosturl);

	success = e_backend_authenticate_sync (
		E_BACKEND (backend),
		E_SOURCE_AUTHENTICATOR (connection),
		cancellable, error);

	if (success) {
		g_mutex_lock (backend->priv->connection_lock);
		if (backend->priv->connection != NULL)
			g_object_unref (backend->priv->connection);
		backend->priv->connection = g_object_ref (connection);
		g_mutex_unlock (backend->priv->connection_lock);
	} else {
		g_object_unref (connection);
		connection = NULL;
	}

	return connection;
}

void
e_ews_backend_ref_connection (EEwsBackend *backend,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
	GSimpleAsyncResult *simple;

	g_return_if_fail (E_IS_EWS_BACKEND (backend));

	simple = g_simple_async_result_new (
		G_OBJECT (backend), callback,
		user_data, e_ews_backend_ref_connection);

	g_simple_async_result_run_in_thread (
		simple, ews_backend_ref_connection_thread,
		G_PRIORITY_DEFAULT, cancellable);

	g_object_unref (simple);
}

EEwsConnection *
e_ews_backend_ref_connection_finish (EEwsBackend *backend,
                                     GAsyncResult *result,
                                     GError **error)
{
	GSimpleAsyncResult *simple;
	EEwsConnection *connection;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (backend),
		e_ews_backend_ref_connection), NULL);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	if (g_simple_async_result_propagate_error (simple, error))
		return NULL;

	connection = g_simple_async_result_get_op_res_gpointer (simple);
	g_return_val_if_fail (E_IS_EWS_CONNECTION (connection), NULL);

	return g_object_ref (connection);
}

static void
ews_backend_sync_folders_thread (GSimpleAsyncResult *simple,
                                 GObject *object,
                                 GCancellable *cancellable)
{
	GError *error = NULL;

	e_ews_backend_sync_folders_sync (
		E_EWS_BACKEND (object), cancellable, &error);

	if (error != NULL)
		g_simple_async_result_take_error (simple, error);
}

gboolean
e_ews_backend_sync_folders_sync (EEwsBackend *backend,
                                 GCancellable *cancellable,
                                 GError **error)
{
	EEwsConnection *connection;
	GSList *folders_created = NULL;
	GSList *folders_updated = NULL;
	GSList *folders_deleted = NULL;
	gboolean includes_last_folder = FALSE;
	gchar *sync_state;
	gboolean success;

	g_return_val_if_fail (E_IS_EWS_BACKEND (backend), FALSE);

	connection = e_ews_backend_ref_connection_sync (
		backend, cancellable, error);

	if (connection == NULL)
		return FALSE;

	g_mutex_lock (backend->priv->sync_state_lock);
	sync_state = g_strdup (backend->priv->sync_state);
	g_mutex_unlock (backend->priv->sync_state_lock);

	/* XXX I think this leaks the old sync_state value when
	 *     it replaces it with the new sync_state value. */
	success = e_ews_connection_sync_folder_hierarchy_sync (
		connection, EWS_PRIORITY_MEDIUM,
		&sync_state, &includes_last_folder,
		&folders_created, &folders_updated, &folders_deleted,
		cancellable, error);

	if (success) {
		SyncFoldersClosure *closure;

		/* This takes ownership of the folder lists. */
		closure = g_slice_new0 (SyncFoldersClosure);
		closure->backend = g_object_ref (backend);
		closure->folders_created = folders_created;
		closure->folders_deleted = folders_deleted;
		closure->folders_updated = folders_updated;

		/* Process the results from an idle callback. */
		g_idle_add_full (
			G_PRIORITY_DEFAULT_IDLE,
			ews_backend_sync_folders_idle_cb, closure,
			(GDestroyNotify) sync_folders_closure_free);

		g_mutex_lock (backend->priv->sync_state_lock);
		g_free (backend->priv->sync_state);
		backend->priv->sync_state = g_strdup (sync_state);
		g_mutex_unlock (backend->priv->sync_state_lock);

	} else {
		/* Make sure we're not leaking anything. */
		g_warn_if_fail (folders_created == NULL);
		g_warn_if_fail (folders_updated == NULL);
		g_warn_if_fail (folders_deleted == NULL);
	}

	g_free (sync_state);

	g_object_unref (connection);

	return success;
}

void
e_ews_backend_sync_folders (EEwsBackend *backend,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
	GSimpleAsyncResult *simple;

	g_return_if_fail (E_IS_EWS_BACKEND (backend));

	simple = g_simple_async_result_new (
		G_OBJECT (backend), callback,
		user_data, e_ews_backend_sync_folders);

	g_simple_async_result_run_in_thread (
		simple, ews_backend_sync_folders_thread,
		G_PRIORITY_DEFAULT, cancellable);

	g_object_unref (simple);
}

gboolean
e_ews_backend_sync_folders_finish (EEwsBackend *backend,
                                   GAsyncResult *result,
                                   GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (backend),
		e_ews_backend_sync_folders), FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	/* Assume success unless a GError is set. */
	return !g_simple_async_result_propagate_error (simple, error);
}
