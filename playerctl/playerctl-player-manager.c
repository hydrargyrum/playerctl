/*
 * This file is part of playerctl.
 *
 * playerctl is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * playerctl is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with playerctl If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright © 2014, Tony Crisci and contributors.
 */

#include <gio/gio.h>
#include <glib-object.h>
#include "playerctl/playerctl-common.h"
#include "playerctl/playerctl-player-manager.h"
#include <playerctl/playerctl-player.h>

enum {
    PROP_0,
    PROP_PLAYERS,
    PROP_PLAYER_NAMES,
    N_PROPERTIES,
};

enum {
    NAME_APPEARED,
    NAME_VANISHED,
    PLAYER_APPEARED,
    PLAYER_VANISHED,
    LAST_SIGNAL,
};

static GParamSpec *obj_properties[N_PROPERTIES] = {
    NULL,
};

static guint connection_signals[LAST_SIGNAL] = {0};

struct _PlayerctlPlayerManagerPrivate {
    gboolean initted;
    GError *init_error;
    GDBusProxy *proxy;
    GList *player_names;
    GList *players;
    GCompareDataFunc sort_func;
    gpointer *sort_data;
    GDestroyNotify sort_notify;
};

static void playerctl_player_manager_initable_iface_init(GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE(PlayerctlPlayerManager, playerctl_player_manager, G_TYPE_OBJECT,
                        G_ADD_PRIVATE(PlayerctlPlayerManager) G_IMPLEMENT_INTERFACE(
                            G_TYPE_INITABLE,
                            playerctl_player_manager_initable_iface_init));


static void playerctl_player_manager_set_property(GObject *object, guint property_id,
                                          const GValue *value,
                                          GParamSpec *pspec) {
    //PlayerctlPlayerManager *manager = PLAYERCTL_PLAYER_MANAGER(object);

    switch (property_id) {
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void playerctl_player_manager_get_property(GObject *object, guint property_id,
                                          GValue *value, GParamSpec *pspec) {
    PlayerctlPlayerManager *manager = PLAYERCTL_PLAYER_MANAGER(object);
    switch (property_id) {
    case PROP_PLAYERS:
        g_value_set_pointer(value, manager->priv->players);
        break;
    case PROP_PLAYER_NAMES:
        g_value_set_pointer(value, manager->priv->player_names);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void playerctl_player_manager_constructed(GObject *gobject) {
    PlayerctlPlayerManager *manager = PLAYERCTL_PLAYER_MANAGER(gobject);
    g_initable_init(G_INITABLE(manager), NULL, &manager->priv->init_error);
    G_OBJECT_CLASS(playerctl_player_manager_parent_class)->constructed(gobject);
}

static void playerctl_player_manager_dispose(GObject *gobject) {
    PlayerctlPlayerManager *manager = PLAYERCTL_PLAYER_MANAGER(gobject);

    g_clear_error(&manager->priv->init_error);
    g_clear_object(&manager->priv->proxy);

    G_OBJECT_CLASS(playerctl_player_manager_parent_class)->dispose(gobject);
}

static void playerctl_player_manager_finalize(GObject *gobject) {
    PlayerctlPlayerManager *manager = PLAYERCTL_PLAYER_MANAGER(gobject);

    g_list_free_full(manager->priv->player_names, g_free);
    g_list_free_full(manager->priv->players, g_object_unref);

    G_OBJECT_CLASS(playerctl_player_manager_parent_class)->finalize(gobject);
}


/**
 * playerctl_name_event_copy:
 * @event: a #PlayerctlNameEvent
 *
 * Creates a dynamically allocated name event container as a copy of
 * @event.
 *
 * Returns: (transfer full): a newly-allocated copy of @event
 */
PlayerctlNameEvent *playerctl_name_event_copy(PlayerctlNameEvent *event) {
    PlayerctlNameEvent *retval;

    g_return_val_if_fail(event != NULL, NULL);

    retval = g_slice_new0(PlayerctlNameEvent);
    *retval = *event;

    retval->name = g_strdup(event->name);

    return retval;
}

/**
 * playerctl_name_event_free:
 * @event: (allow-none): a #PlayerctlNameEvent
 *
 * Frees @event. If @event is %NULL, it simply returns.
 */
void playerctl_name_event_free(PlayerctlNameEvent *event) {
    if (event == NULL) {
        return;
    }

    g_free(event->name);
    g_slice_free(PlayerctlNameEvent, event);
}

G_DEFINE_BOXED_TYPE(PlayerctlNameEvent, playerctl_name_event,
    playerctl_name_event_copy, playerctl_name_event_free);


static void playerctl_player_manager_class_init(PlayerctlPlayerManagerClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

    gobject_class->set_property = playerctl_player_manager_set_property;
    gobject_class->get_property = playerctl_player_manager_get_property;
    gobject_class->constructed = playerctl_player_manager_constructed;
    gobject_class->dispose = playerctl_player_manager_dispose;
    gobject_class->finalize = playerctl_player_manager_finalize;

    /**
     * PlayerctlPlayerManager:players: (transfer none) (type GList(PlayerctlPlayer)):
     *
     * A list of players that are currently managed by this class.
     */
    obj_properties[PROP_PLAYERS] =
       g_param_spec_pointer("players",
                            "players",
                            "A list of player objects managed by this manager",
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * PlayerctlPlayerManager:player-names: (transfer none) (type GList(utf8)):
     *
     * A list of player names that are currently available to control.
     */
    obj_properties[PROP_PLAYER_NAMES] =
       g_param_spec_pointer("player-names",
                            "player names",
                            "A list of player names that are currently available to control.",
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(gobject_class, N_PROPERTIES,
                                      obj_properties);

    /**
    * PlayerctlPlayerManager::name-appeared:
    * @self: the #PlayerctlPlayerManager on which the signal was emitted
    * @event: A #PlayerctlNameEvent containing information about the name appearing.
    */
    connection_signals[NAME_APPEARED] =
        g_signal_new("name-appeared",
                     PLAYERCTL_TYPE_PLAYER_MANAGER,
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL,
                     NULL,
                     g_cclosure_marshal_VOID__BOXED,
                     G_TYPE_NONE,
                     1,
                     PLAYERCTL_TYPE_NAME_EVENT);

    connection_signals[NAME_VANISHED] =
        g_signal_new("name-vanished",
                     PLAYERCTL_TYPE_PLAYER_MANAGER,
                     G_SIGNAL_RUN_FIRST,
                     0,
                     NULL,
                     NULL,
                     g_cclosure_marshal_VOID__BOXED,
                     G_TYPE_NONE,
                     1,
                     PLAYERCTL_TYPE_NAME_EVENT);

    connection_signals[PLAYER_APPEARED] =
        g_signal_new("player-appeared",
                     PLAYERCTL_TYPE_PLAYER_MANAGER,
                     G_SIGNAL_RUN_FIRST,
                     0,
                     NULL,
                     NULL,
                     g_cclosure_marshal_VOID__OBJECT,
                     G_TYPE_NONE,
                     1,
                     PLAYERCTL_TYPE_PLAYER);

    connection_signals[PLAYER_VANISHED] =
        g_signal_new("player-vanished",
                     PLAYERCTL_TYPE_PLAYER_MANAGER,
                     G_SIGNAL_RUN_FIRST,
                     0,
                     NULL,
                     NULL,
                     g_cclosure_marshal_VOID__OBJECT,
                     G_TYPE_NONE,
                     1,
                     PLAYERCTL_TYPE_PLAYER);
}

static void playerctl_player_manager_init(PlayerctlPlayerManager *manager) {
    manager->priv = playerctl_player_manager_get_instance_private(manager);
}

static gchar *player_id_from_bus_name(const gchar *bus_name) {
    const size_t prefix_len = strlen(MPRIS_PREFIX);

    if (bus_name == NULL ||
            !g_str_has_prefix(bus_name, MPRIS_PREFIX) ||
            strlen(bus_name) <= prefix_len) {
        return NULL;
    }

    return g_strdup(bus_name + prefix_len);
}

static void manager_remove_managed_player_by_name(PlayerctlPlayerManager *manager,
                                                  gchar *player_name) {
    GList *l = NULL;
    for (l = manager->priv->players; l != NULL; l = l->next) {
        PlayerctlPlayer *player = PLAYERCTL_PLAYER(l->data);
        gchar *id = NULL;
        g_object_get(player, "player-id", &id, NULL);
        if (g_strcmp0(id, player_name) == 0) {
            manager->priv->players = g_list_remove_link(manager->priv->players, l);
            g_signal_emit(manager, connection_signals[PLAYER_VANISHED], 0, player);
            g_list_free_full(l, g_object_unref);
            g_free(id);
            break;
        }
        g_free(id);
    }
}

static void dbus_name_owner_changed_callback(GDBusProxy *proxy, gchar *sender_name,
                                             gchar *signal_name, GVariant *parameters,
                                             gpointer *data) {
    PlayerctlPlayerManager *manager = PLAYERCTL_PLAYER_MANAGER(data);

    if (g_strcmp0(signal_name, "NameOwnerChanged") != 0) {
        return;
    }

    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sss)"))) {
        g_warning("Got unknown parameters on org.freedesktop.DBus "
                  "NameOwnerChange signal: %s",
                  g_variant_get_type_string(parameters));
        return;
    }

    GVariant *name_variant = g_variant_get_child_value(parameters, 0);
    const gchar *name = g_variant_get_string(name_variant, NULL);
    gchar *player_id = player_id_from_bus_name(name);

    if (player_id == NULL) {
        g_variant_unref(name_variant);
        return;
    }

    GVariant *previous_owner_variant = g_variant_get_child_value(parameters, 1);
    const gchar *previous_owner = g_variant_get_string(previous_owner_variant, NULL);

    GVariant *new_owner_variant = g_variant_get_child_value(parameters, 2);
    const gchar *new_owner = g_variant_get_string(new_owner_variant, NULL);

    GList *player_entry = NULL;
    if (strlen(new_owner) == 0 && strlen(previous_owner) != 0) {
        // the name has vanished
        player_entry =
            g_list_find_custom(manager->priv->player_names, player_id,
                               (GCompareFunc)g_strcmp0);
        if (player_entry != NULL) {
            manager->priv->player_names =
                g_list_remove_link(manager->priv->player_names, player_entry);
            manager_remove_managed_player_by_name(manager, player_entry->data);
            PlayerctlNameEvent *event = g_slice_new(PlayerctlNameEvent);
            event->name = g_strdup(player_entry->data);
            g_signal_emit(manager, connection_signals[NAME_VANISHED], 0,
                          event);
            playerctl_name_event_free(event);
            g_list_free_full(player_entry, g_free);
        }
    } else if (strlen(previous_owner) == 0 && strlen(new_owner) != 0) {
        // the name has appeared
        player_entry =
            g_list_find_custom(manager->priv->player_names, player_id,
                               (GCompareFunc)g_strcmp0);
        if (player_entry == NULL) {
            manager->priv->player_names = g_list_prepend(manager->priv->player_names, g_strdup(player_id));
            PlayerctlNameEvent *event = g_slice_new(PlayerctlNameEvent);
            event->name = g_strdup(player_id);
            g_signal_emit(manager, connection_signals[NAME_APPEARED], 0,
                          event);
            playerctl_name_event_free(event);
        }
    }

    g_free(player_id);
    g_variant_unref(name_variant);
    g_variant_unref(previous_owner_variant);
    g_variant_unref(new_owner_variant);
}

static gboolean playerctl_player_manager_initable_init(GInitable *initable,
                                               GCancellable *cancellable,
                                               GError **error) {
    GError *tmp_error = NULL;
    PlayerctlPlayerManager *manager = PLAYERCTL_PLAYER_MANAGER(initable);

    if (manager->priv->initted) {
        return TRUE;
    }

    // TODO system bus
    manager->priv->proxy =
        g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
                                      G_DBUS_PROXY_FLAGS_NONE, NULL,
                                      "org.freedesktop.DBus",
                                      "/org/freedesktop/DBus",
                                      "org.freedesktop.DBus", NULL,
                                      &tmp_error);
    if (tmp_error != NULL) {
        g_propagate_error(error, tmp_error);
        return FALSE;
    }

    manager->priv->player_names = playerctl_list_players(&tmp_error);
    if (tmp_error != NULL) {
        g_propagate_error(error, tmp_error);
        return FALSE;
    }

    g_signal_connect(G_DBUS_PROXY(manager->priv->proxy), "g-signal",
                     G_CALLBACK(dbus_name_owner_changed_callback),
                     manager);

    manager->priv->initted = TRUE;

    return TRUE;
}

static void playerctl_player_manager_initable_iface_init(GInitableIface *iface) {
    iface->init = playerctl_player_manager_initable_init;
}

PlayerctlPlayerManager *playerctl_player_manager_new(GError **err) {
    GError *tmp_error = NULL;

    PlayerctlPlayerManager *manager =
        g_initable_new(PLAYERCTL_TYPE_PLAYER_MANAGER, NULL, &tmp_error, NULL);
    if (tmp_error != NULL) {
        g_propagate_error(err, tmp_error);
        return NULL;
    }

    return manager;
}

void playerctl_player_manager_set_sort_func(PlayerctlPlayerManager *manager,
                                          GCompareDataFunc sort_func,
                                          gpointer *sort_data,
                                          GDestroyNotify notify) {
    // TODO figure out how to make this work with the bindings
    manager->priv->sort_func = sort_func;
    manager->priv->sort_data = sort_data;
    manager->priv->sort_notify = notify;

    manager->priv->players =
        g_list_sort_with_data(manager->priv->players, sort_func, sort_data);
}

void playerctl_player_manager_move_player_to_top(PlayerctlPlayerManager *manager,
                                               PlayerctlPlayer *player) {
    GList *l;
    for (l = manager->priv->players; l != NULL; l = l->next) {
        PlayerctlPlayer *current = PLAYERCTL_PLAYER(l->data);
        if (current == player) {
            manager->priv->players = g_list_remove_link(manager->priv->players, l);
            manager->priv->players = g_list_concat(l, manager->priv->players);

            if (manager->priv->sort_func) {
                manager->priv->players =
                    g_list_sort_with_data(manager->priv->players,
                            manager->priv->sort_func,
                            manager->priv->sort_data);
            }

            break;
        }
    }
}

void playerctl_player_manager_manage_player(PlayerctlPlayerManager *manager,
                                            PlayerctlPlayer *player) {
    if (player == NULL) {
        return;
    }

    GList *l = NULL;
    for (l = manager->priv->players; l != NULL; l = l->next) {
        PlayerctlPlayer *current = PLAYERCTL_PLAYER(l->data);
        if (player == current) {
            return;
        }
    }

    if (manager->priv->sort_func) {
        manager->priv->players =
            g_list_insert_sorted_with_data(manager->priv->players, player,
                                           manager->priv->sort_func,
                                           manager->priv->sort_data);
    } else {
        manager->priv->players = g_list_prepend(manager->priv->players, player);
    }

    g_object_ref(player);
    g_signal_emit(manager, connection_signals[PLAYER_APPEARED], 0, player);
}
