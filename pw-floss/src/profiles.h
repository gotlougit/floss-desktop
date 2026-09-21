/* SPDX-License-Identifier: MIT
 * Export a standard PipeWire card so desktop mixers can select profiles.
 * Included after struct bridge; transport changes run on the bridge timer. */
/* Indices and names are persistent WirePlumber state: append, never reorder. */
static const int profile_codecs[] = { -1, 0, 1, -1, 2, 3, 4 };
static const char *profile_names[] = {
    "auto", "a2dp-sink-sbc", "a2dp-sink-aac", "headset-head-unit",
    "a2dp-sink-aptx", "a2dp-sink-aptx-hd", "a2dp-sink-ldac" };
static const char *profile_descriptions[] = {
    "Automatic music / headset", "High Fidelity Playback (SBC)",
    "High Fidelity Playback (AAC)", "Handsfree Headset",
    "High Fidelity Playback (aptX)", "High Fidelity Playback (aptX HD)",
    "High Fidelity Playback (LDAC)" };
static bool profile_available(struct bridge *b, unsigned p)
{
    if (p >= SPA_N_ELEMENTS(profile_codecs)) return false;
    if (p == 0) return true;
    if (p == 3) return b->microphone && !b->hfp_transport_unavailable;
    int codec = profile_codecs[p];
    return b->codec_rates[codec] && b->codec_modes[codec] && b->codec_bits[codec];
}
static uint32_t available_profiles(struct bridge *b)
{
    uint32_t mask = 0;
    for (unsigned p = 0; p < SPA_N_ELEMENTS(profile_codecs); p++)
        if (profile_available(b, p)) mask |= 1u << p;
    return mask;
}
static void profile_info(struct bridge *b)
{
    struct spa_device_info info = SPA_DEVICE_INFO_INIT();
    info.change_mask = SPA_DEVICE_CHANGE_MASK_PROPS | SPA_DEVICE_CHANGE_MASK_PARAMS;
    info.props = &b->card_properties->dict;
    info.params = b->card_params;
    info.n_params = 2;
    spa_hook_list_call(&b->card_listeners, struct spa_device_events, info, 0, &info);
}
static int profile_listener(void *data, struct spa_hook *listener,
        const struct spa_device_events *events, void *user)
{
    struct bridge *b = data;
    struct spa_hook_list save;
    spa_hook_list_isolate(&b->card_listeners, &save, listener, events, user);
    profile_info(b);
    spa_hook_list_join(&b->card_listeners, &save);
    return 0;
}
static int profile_sync(void *data, int seq)
{
    struct bridge *b = data;
    spa_hook_list_call(&b->card_listeners, struct spa_device_events, result, 0, seq, 0, 0, NULL);
    return 0;
}
static int profile_enum(void *data, int seq, uint32_t id, uint32_t start,
        uint32_t max, const struct spa_pod *filter)
{
    struct bridge *b = data;
    if (id != SPA_PARAM_EnumProfile && id != SPA_PARAM_Profile) return -ENOENT;
    for (unsigned i = start, count = 0; i < (id == SPA_PARAM_Profile ? 1u : SPA_N_ELEMENTS(profile_codecs)) && count < max; i++) {
        unsigned p = id == SPA_PARAM_Profile ? (unsigned)b->selected_profile : i;
        if (!profile_available(b, p)) continue;
        uint8_t storage[2048], filtered[2048];
        struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,sizeof(storage));
        struct spa_pod_builder fb = SPA_POD_BUILDER_INIT(filtered,sizeof(filtered));
        struct spa_pod_frame f[2];
        spa_pod_builder_push_object(&builder,&f[0],SPA_TYPE_OBJECT_ParamProfile,id);
        spa_pod_builder_add(&builder,
            SPA_PARAM_PROFILE_index, SPA_POD_Int(p),
            SPA_PARAM_PROFILE_name, SPA_POD_String(profile_names[p]),
            SPA_PARAM_PROFILE_description, SPA_POD_String(profile_descriptions[p]),
            SPA_PARAM_PROFILE_priority, SPA_POD_Int(p == 0 ? 1000 : 100),
            SPA_PARAM_PROFILE_available, SPA_POD_Id(SPA_PARAM_AVAILABILITY_yes),
            SPA_PARAM_PROFILE_save, SPA_POD_Bool(b->profile_save), 0);
        spa_pod_builder_prop(&builder,SPA_PARAM_PROFILE_classes,0);
        spa_pod_builder_push_struct(&builder,&f[1]);
        uint32_t out = 0, in = 1;
        bool mic = b->microphone && (p == 0 || p == 3);
        spa_pod_builder_int(&builder,mic ? 2 : 1);
        spa_pod_builder_add_struct(&builder,SPA_POD_String("Audio/Sink"),SPA_POD_Int(1),
            SPA_POD_String("card.profile.devices"),SPA_POD_Array(sizeof(out),SPA_TYPE_Int,1,&out));
        if (mic) spa_pod_builder_add_struct(&builder,SPA_POD_String("Audio/Source"),SPA_POD_Int(1),
            SPA_POD_String("card.profile.devices"),SPA_POD_Array(sizeof(in),SPA_TYPE_Int,1,&in));
        spa_pod_builder_pop(&builder,&f[1]);
        struct spa_pod *pod = spa_pod_builder_pop(&builder,&f[0]), *result;
        if (spa_pod_filter(&fb,&result,pod,filter) < 0) continue;
        struct spa_result_device_params r = { .id=id, .index=i, .next=i+1, .param=result };
        spa_hook_list_call(&b->card_listeners,struct spa_device_events,result,0,seq,0,SPA_RESULT_TYPE_DEVICE_PARAMS,&r);
        count++;
    }
    return 0;
}
static int profile_set(void *data, uint32_t id, uint32_t flags, const struct spa_pod *param)
{
    struct bridge *b = data;
    int32_t p = -1;
    bool save = false;
    (void)flags;
    if (id != SPA_PARAM_Profile || !param) return -ENOENT;
    if (spa_pod_parse_object(param,SPA_TYPE_OBJECT_ParamProfile,NULL,
        SPA_PARAM_PROFILE_index,SPA_POD_Int(&p),
        SPA_PARAM_PROFILE_save,SPA_POD_OPT_Bool(&save)) < 0 ||
        p < 0 || (unsigned)p >= SPA_N_ELEMENTS(profile_codecs) || !profile_available(b,p)) return -EINVAL;
    if (p != b->requested_profile)
        fprintf(stderr, "pw-floss: requested desktop profile %s\n", profile_names[p]);
    b->requested_profile = p;
    b->profile_save = save;
    return 0;
}
static const struct spa_device_methods profile_methods = {
    SPA_VERSION_DEVICE_METHODS,
    .add_listener=profile_listener, .sync=profile_sync,
    .enum_params=profile_enum, .set_param=profile_set,
};
static void profile_bound(void *data, uint32_t id)
{
    struct bridge *b = data;
    b->card_id = id;
}
static void profile_bound_props(void *data, uint32_t id, const struct spa_dict *props)
{
    profile_bound(data, id);
}
static const struct pw_proxy_events profile_proxy_events = {
    PW_VERSION_PROXY_EVENTS, .bound=profile_bound, .bound_props=profile_bound_props,
};
static bool setup_profiles(struct bridge *b)
{
    char name[64]; snprintf(name,sizeof(name),"floss_card.%s",b->address);
    for (char *p=name; *p; p++) if (*p==':') *p='_';
    b->card_properties = pw_properties_new("device.name",name,
        "device.description",b->device_name, "device.nick",b->device_name,
        "device.api","floss", "device.bus","bluetooth", "media.class","Audio/Device",
        "device.icon-name","audio-headphones-bluetooth", NULL);
    if (!b->card_properties) return false;
    spa_hook_list_init(&b->card_listeners);
    b->card_params[0] = (struct spa_param_info){ .id=SPA_PARAM_EnumProfile, .flags=SPA_PARAM_INFO_READ };
    b->card_params[1] = (struct spa_param_info){ .id=SPA_PARAM_Profile, .flags=SPA_PARAM_INFO_READWRITE };
    b->available_profiles = available_profiles(b);
    b->card.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Device,SPA_VERSION_DEVICE,&profile_methods,b);
    b->card_proxy = pw_core_export(b->core,SPA_TYPE_INTERFACE_Device,
        &b->card_properties->dict,&b->card,0);
    if (!b->card_proxy) return false;
    pw_proxy_add_listener(b->card_proxy,&b->card_proxy_listener,&profile_proxy_events,b);
    return true;
}
static void profile_changed(struct bridge *b)
{
    fprintf(stderr, "pw-floss: active desktop profile %s\n", profile_names[b->selected_profile]);
    /* Re-enumerating unchanged choices makes session policy reselect a saved
     * profile while a new user selection is still being persisted. */
    uint32_t mask = available_profiles(b);
    if (mask != b->available_profiles) {
        b->available_profiles = mask;
        b->card_params[0].flags ^= SPA_PARAM_INFO_SERIAL;
    }
    b->card_params[1].flags ^= SPA_PARAM_INFO_SERIAL;
    if (b->card_proxy) profile_info(b);
}
