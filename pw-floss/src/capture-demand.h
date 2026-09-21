/* SPDX-License-Identifier: MIT
 * A source can run for a passive level meter. Only an active, non-monitor
 * consumer linked to our microphone is a request to acquire the SCO transport.
 * Registry callbacks and the transport timer share the main loop. */
struct capture_object {
    struct spa_list link;
    uint32_t id, output, input;
    bool node, monitor, passive, running;
    struct pw_proxy *proxy;
    struct spa_hook listener;
};
static void capture_node_info(void *data, const struct pw_node_info *info)
{
    struct capture_object *o = data;
    if (info->change_mask & PW_NODE_CHANGE_MASK_STATE)
        o->running = info->state == PW_NODE_STATE_RUNNING;
    if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) {
        const char *monitor = spa_dict_lookup(info->props, "stream.monitor");
        const char *passive = spa_dict_lookup(info->props, "node.passive");
        o->monitor = monitor && spa_atob(monitor);
        o->passive = passive && (spa_atob(passive) || !strcmp(passive, "in-follow") ||
                !strcmp(passive, "out-follow"));
    }
}
static void capture_link_info(void *data, const struct pw_link_info *info)
{
    struct capture_object *o = data;
    o->output = info->output_node_id;
    o->input = info->input_node_id;
    if (info->change_mask & PW_LINK_CHANGE_MASK_STATE)
        o->running = info->state == PW_LINK_STATE_ACTIVE;
}
static const struct pw_node_events capture_node_events = {
    PW_VERSION_NODE_EVENTS, .info = capture_node_info,
};
static const struct pw_link_events capture_link_events = {
    PW_VERSION_LINK_EVENTS, .info = capture_link_info,
};
static void capture_global(void *data, uint32_t id, uint32_t permissions,
        const char *type, uint32_t version, const struct spa_dict *props)
{
    struct bridge *b = data;
    bool node = !strcmp(type, PW_TYPE_INTERFACE_Node);
    if (!node && strcmp(type, PW_TYPE_INTERFACE_Link)) return;
    struct capture_object *o = calloc(1, sizeof(*o));
    if (!o) return;
    o->id = id; o->node = node;
    o->output = o->input = SPA_ID_INVALID;
    o->proxy = pw_registry_bind(b->registry, id, type,
        SPA_MIN(version, (uint32_t)(node ? PW_VERSION_NODE : PW_VERSION_LINK)), 0);
    if (!o->proxy) { free(o); return; }
    spa_list_append(&b->graph_objects, &o->link);
    if (node) pw_node_add_listener((struct pw_node *)o->proxy, &o->listener, &capture_node_events, o);
    else pw_link_add_listener((struct pw_link *)o->proxy, &o->listener, &capture_link_events, o);
}
static void capture_remove(void *data, uint32_t id)
{
    struct bridge *b = data;
    struct capture_object *o;
    spa_list_for_each(o, &b->graph_objects, link) {
        if (o->id != id) continue;
        spa_list_remove(&o->link);
        spa_hook_remove(&o->listener);
        pw_proxy_destroy(o->proxy);
        free(o);
        break;
    }
}
static const struct pw_registry_events capture_registry_events = {
    PW_VERSION_REGISTRY_EVENTS, .global = capture_global, .global_remove = capture_remove,
};
static bool setup_capture_watch(struct bridge *b)
{
    spa_list_init(&b->graph_objects);
    b->registry = pw_core_get_registry(pw_stream_get_core(b->stream), PW_VERSION_REGISTRY, 0);
    if (!b->registry) return false;
    pw_registry_add_listener(b->registry, &b->registry_listener, &capture_registry_events, b);
    return true;
}
static void destroy_capture_watch(struct bridge *b)
{
    if (!b->registry) return;
    while (!spa_list_is_empty(&b->graph_objects)) {
        struct capture_object *o = spa_list_first(&b->graph_objects, struct capture_object, link);
        capture_remove(b, o->id);
    }
    spa_hook_remove(&b->registry_listener);
    pw_proxy_destroy((struct pw_proxy *)b->registry);
}
static bool capture_requested(struct bridge *b)
{
    if (!b->capture || !b->registry) return false;
    uint32_t source = pw_stream_get_node_id(b->capture);
    if (source == SPA_ID_INVALID) return false;
    struct capture_object *link, *node;
    spa_list_for_each(link, &b->graph_objects, link) {
        if (link->node || !link->running || link->output != source) continue;
        spa_list_for_each(node, &b->graph_objects, link) {
            if (node->node && node->id == link->input && node->running &&
                    !node->monitor && !node->passive) return true;
        }
    }
    return false;
}
