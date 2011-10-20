/*
 * camel-ews-settings.h
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

#ifndef CAMEL_EWS_SETTINGS_H
#define CAMEL_EWS_SETTINGS_H

#include <camel/camel.h>

/* Standard GObject macros */
#define CAMEL_TYPE_EWS_SETTINGS \
	(camel_ews_settings_get_type ())
#define CAMEL_EWS_SETTINGS(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), CAMEL_TYPE_EWS_SETTINGS, CamelEwsSettings))
#define CAMEL_EWS_SETTINGS_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), CAMEL_TYPE_EWS_SETTINGS, CamelEwsSettingsClass))
#define CAMEL_IS_EWS_SETTINGS(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), CAMEL_TYPE_EWS_SETTINGS))
#define CAMEL_IS_EWS_SETTINGS_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), CAMEL_TYPE_EWS_SETTINGS))
#define CAMEL_EWS_SETTINGS_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), CAMEL_TYPE_EWS_SETTINGS))

G_BEGIN_DECLS

typedef struct _CamelEwsSettings CamelEwsSettings;
typedef struct _CamelEwsSettingsClass CamelEwsSettingsClass;
typedef struct _CamelEwsSettingsPrivate CamelEwsSettingsPrivate;

struct _CamelEwsSettings {
	CamelOfflineSettings parent;
	CamelEwsSettingsPrivate *priv;
};

struct _CamelEwsSettingsClass {
	CamelOfflineSettingsClass parent_class;
};

GType		camel_ews_settings_get_type			(void) G_GNUC_CONST;
gboolean	camel_ews_settings_get_check_all		(CamelEwsSettings *settings);
void		camel_ews_settings_set_check_all		(CamelEwsSettings *settings,
								 gboolean check_all);
gboolean	camel_ews_settings_get_filter_junk		(CamelEwsSettings *settings);
void		camel_ews_settings_set_filter_junk		(CamelEwsSettings *settings,
								 gboolean filter_junk);
gboolean	camel_ews_settings_get_filter_junk_inbox	(CamelEwsSettings *settings);
void		camel_ews_settings_set_filter_junk_inbox	(CamelEwsSettings *settings,
								 gboolean filter_junk_inbox);
G_END_DECLS

#endif /* CAMEL_EWS_SETTINGS_H */