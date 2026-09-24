/*
 * iBeacon Emulator for Flipper Zero.
 *
 * Broadcasts a standard iBeacon advertisement with a UUID, major, minor and
 * advertising interval you can change on the device itself, and keeps a list of
 * saved beacon profiles you can switch between.
 *
 * Transmission goes through the firmware's extra-beacon API, which advertises
 * alongside the normal Bluetooth stack, so you keep your regular Flipper
 * connection while it runs.
 */

#include <furi.h>
#include <furi_hal_bt.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/number_input.h>
#include <gui/modules/text_input.h>
#include <gui/modules/widget.h>
#include <storage/storage.h>

#define UUID_LEN 16
#define ADV_LEN 30
#define MAX_PROFILES 8
#define NAME_LEN 17 /* 16 characters plus terminator */
#define SETTINGS_PATH APP_DATA_PATH("settings.conf")
#define SETTINGS_MAX 2048

typedef enum {
    ViewIdMenu,
    ViewIdProfiles,
    ViewIdUuid,
    ViewIdNumber,
    ViewIdName,
    ViewIdInfo,
} ViewId;

typedef enum {
    MenuToggle,
    MenuUuid,
    MenuMajor,
    MenuMinor,
    MenuInterval,
    MenuLoad,
    MenuSave,
    MenuDelete,
    MenuStatus,
} MenuIndex;

typedef enum {
    EditMajor,
    EditMinor,
    EditInterval,
} EditTarget;

/* What the profile list is for at this moment. */
typedef enum {
    ProfileModeLoad,
    ProfileModeDelete,
} ProfileMode;

typedef struct {
    char name[NAME_LEN];
    uint8_t uuid[UUID_LEN];
    uint16_t major;
    uint16_t minor;
} Profile;

typedef struct {
    uint8_t uuid[UUID_LEN];
    uint16_t major;
    uint16_t minor;
    uint16_t interval;
    Profile profiles[MAX_PROFILES];
    uint8_t profile_count;
} Settings;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* menu;
    Submenu* profile_menu;
    ByteInput* byte_input;
    NumberInput* number_input;
    TextInput* text_input;
    Widget* info;

    Settings settings;
    uint8_t uuid_edit[UUID_LEN];
    char name_edit[NAME_LEN];
    EditTarget editing;
    ProfileMode profile_mode;
    bool sending;
    /* This SDK has no view_dispatcher_get_current_view_id, so track it here to
       tell "back to the menu" apart from "leave the app". */
    ViewId current_view;
} App;

/* --- Hex helpers -------------------------------------------------------- */

static int hex_value(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Reads 32 hex characters into 16 bytes. Dashes and spaces are skipped, so both
   "E2C56DB5..." and "e2c56db5-dffb-..." are accepted. */
static bool hex_to_uuid(const char* s, size_t len, uint8_t* out) {
    size_t nibbles = 0;
    for(size_t i = 0; i < len; i++) {
        int v = hex_value(s[i]);
        if(v < 0) continue;
        if(nibbles >= UUID_LEN * 2) return false;
        if(nibbles % 2 == 0) {
            out[nibbles / 2] = (uint8_t)(v << 4);
        } else {
            out[nibbles / 2] |= (uint8_t)v;
        }
        nibbles++;
    }
    return nibbles == UUID_LEN * 2;
}

static void uuid_to_hex(const uint8_t* uuid, FuriString* out) {
    for(size_t i = 0; i < UUID_LEN; i++) {
        furi_string_cat_printf(out, "%02X", uuid[i]);
    }
}

/* --- Settings ----------------------------------------------------------- */

/*
 * Stored as plain text so a script on a computer can write it over the serial
 * CLI, which cannot carry arbitrary binary. Also means you can read and edit it
 * yourself. One key per line:
 *
 *   uuid=E2C56DB5DFFB48D2B060D0F5A71096E0
 *   major=1
 *   minor=1
 *   interval=100
 *   profile=Office|E2C56DB5DFFB48D2B060D0F5A71096E0|1|2
 */

static void settings_defaults(Settings* s) {
    /* Apple's own sample UUID. Generate your own with `uuidgen` for real use. */
    static const uint8_t def[UUID_LEN] = {
        0xe2, 0xc5, 0x6d, 0xb5, 0xdf, 0xfb, 0x48, 0xd2,
        0xb0, 0x60, 0xd0, 0xf5, 0xa7, 0x10, 0x96, 0xe0};
    memset(s, 0, sizeof(*s));
    memcpy(s->uuid, def, UUID_LEN);
    s->major = 1;
    s->minor = 1;
    s->interval = 100;
}

static uint32_t parse_uint(const char* s, size_t len) {
    uint32_t n = 0;
    for(size_t i = 0; i < len; i++) {
        if(s[i] < '0' || s[i] > '9') break;
        n = n * 10 + (uint32_t)(s[i] - '0');
        if(n > 65535) return 65535;
    }
    return n;
}

/* Splits "Name|UUIDHEX|major|minor" into a profile. */
static bool parse_profile(const char* v, size_t len, Profile* p) {
    size_t bar[3];
    size_t found = 0;
    for(size_t i = 0; i < len && found < 3; i++) {
        if(v[i] == '|') bar[found++] = i;
    }
    if(found != 3) return false;

    size_t name_len = bar[0];
    if(name_len >= NAME_LEN) name_len = NAME_LEN - 1;
    memset(p->name, 0, NAME_LEN);
    memcpy(p->name, v, name_len);
    if(p->name[0] == '\0') return false;

    if(!hex_to_uuid(v + bar[0] + 1, bar[1] - bar[0] - 1, p->uuid)) return false;
    p->major = (uint16_t)parse_uint(v + bar[1] + 1, bar[2] - bar[1] - 1);
    p->minor = (uint16_t)parse_uint(v + bar[2] + 1, len - bar[2] - 1);
    return true;
}

static void settings_load(Settings* s) {
    settings_defaults(s);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    char* buf = malloc(SETTINGS_MAX + 1);
    size_t read = 0;

    if(storage_file_open(file, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        read = storage_file_read(file, buf, SETTINGS_MAX);
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    if(read == 0) {
        free(buf);
        return;
    }
    buf[read] = '\0';

    size_t start = 0;
    for(size_t i = 0; i <= read; i++) {
        if(i != read && buf[i] != '\n' && buf[i] != '\r') continue;
        size_t len = i - start;
        if(len > 0) {
            const char* line = buf + start;
            const char* eq = memchr(line, '=', len);
            if(eq) {
                size_t klen = (size_t)(eq - line);
                const char* v = eq + 1;
                size_t vlen = len - klen - 1;
                if(klen == 4 && memcmp(line, "uuid", 4) == 0) {
                    hex_to_uuid(v, vlen, s->uuid);
                } else if(klen == 5 && memcmp(line, "major", 5) == 0) {
                    s->major = (uint16_t)parse_uint(v, vlen);
                } else if(klen == 5 && memcmp(line, "minor", 5) == 0) {
                    s->minor = (uint16_t)parse_uint(v, vlen);
                } else if(klen == 8 && memcmp(line, "interval", 8) == 0) {
                    uint32_t n = parse_uint(v, vlen);
                    if(n >= 20) s->interval = (uint16_t)n;
                } else if(klen == 7 && memcmp(line, "profile", 7) == 0) {
                    if(s->profile_count < MAX_PROFILES) {
                        if(parse_profile(v, vlen, &s->profiles[s->profile_count])) {
                            s->profile_count++;
                        }
                    }
                }
            }
        }
        start = i + 1;
    }
    free(buf);
}

static void settings_save(const Settings* s) {
    FuriString* text = furi_string_alloc();

    furi_string_cat_str(text, "uuid=");
    uuid_to_hex(s->uuid, text);
    furi_string_cat_printf(
        text, "\nmajor=%u\nminor=%u\ninterval=%u\n", s->major, s->minor, s->interval);

    for(uint8_t i = 0; i < s->profile_count; i++) {
        const Profile* p = &s->profiles[i];
        furi_string_cat_printf(text, "profile=%s|", p->name);
        uuid_to_hex(p->uuid, text);
        furi_string_cat_printf(text, "|%u|%u\n", p->major, p->minor);
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, APP_DATA_PATH(""));
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, furi_string_get_cstr(text), furi_string_size(text));
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    furi_string_free(text);
}

/* --- The advertisement -------------------------------------------------- */

/*
 * 02 01 06 | 1A FF 4C 00 02 15 <16 byte uuid> <major> <minor> <power>
 *
 * Apple's company id is 0x004C and goes out little endian, hence 4C 00. Major
 * and minor are big endian. 30 bytes total; a BLE advertisement holds 31.
 */
static void build_adv(const Settings* s, uint8_t* out) {
    uint8_t i = 0;
    out[i++] = 0x02;
    out[i++] = 0x01;
    out[i++] = 0x06;
    out[i++] = 0x1a;
    out[i++] = 0xff;
    out[i++] = 0x4c;
    out[i++] = 0x00;
    out[i++] = 0x02;
    out[i++] = 0x15;
    memcpy(out + i, s->uuid, UUID_LEN);
    i += UUID_LEN;
    out[i++] = (uint8_t)(s->major >> 8);
    out[i++] = (uint8_t)(s->major & 0xff);
    out[i++] = (uint8_t)(s->minor >> 8);
    out[i++] = (uint8_t)(s->minor & 0xff);
    out[i++] = 0xc5; /* measured power at 1 m: -59 dBm */
}

static void beacon_stop(App* app) {
    if(furi_hal_bt_extra_beacon_is_active()) {
        furi_hal_bt_extra_beacon_stop();
    }
    app->sending = false;
}

static bool beacon_start(App* app) {
    /* Reconfiguring is only allowed while stopped. */
    beacon_stop(app);

    GapExtraBeaconConfig config = {
        .min_adv_interval_ms = app->settings.interval,
        .max_adv_interval_ms = (uint16_t)(app->settings.interval + 50),
        .adv_channel_map = GapAdvChannelMapAll,
        .adv_power_level = GapAdvPowerLevel_6dBm,
        .address_type = GapAddressTypeRandom,
        /* Static random address: the top two bits of the most significant byte
           must be 1, hence 0xC3. Keep it stable across sessions, or iOS sees a
           new device every time. */
        .address = {0x4d, 0x56, 0x42, 0x45, 0x41, 0xc3},
    };
    if(!furi_hal_bt_extra_beacon_set_config(&config)) return false;

    uint8_t adv[ADV_LEN];
    build_adv(&app->settings, adv);
    if(!furi_hal_bt_extra_beacon_set_data(adv, ADV_LEN)) return false;
    if(!furi_hal_bt_extra_beacon_start()) return false;

    app->sending = true;
    return true;
}

/* Already broadcasting? Restart with the new values, or the air would carry the
   old frame while the menu claims something else. */
static void reapply(App* app) {
    if(app->sending) beacon_start(app);
}

/* --- Screens ------------------------------------------------------------ */

static void menu_rebuild(App* app);

static void switch_view(App* app, ViewId id) {
    app->current_view = id;
    view_dispatcher_switch_to_view(app->view_dispatcher, id);
}

static void show_info(App* app) {
    widget_reset(app->info);
    FuriString* text = furi_string_alloc();

    furi_string_cat_printf(
        text,
        "%02X%02X%02X%02X-...-%02X%02X%02X%02X\n",
        app->settings.uuid[0],
        app->settings.uuid[1],
        app->settings.uuid[2],
        app->settings.uuid[3],
        app->settings.uuid[12],
        app->settings.uuid[13],
        app->settings.uuid[14],
        app->settings.uuid[15]);
    furi_string_cat_printf(
        text, "major %u   minor %u\n", app->settings.major, app->settings.minor);
    furi_string_cat_printf(
        text,
        "interval %u ms\n%u saved",
        app->settings.interval,
        app->settings.profile_count);

    widget_add_string_element(
        app->info,
        64,
        4,
        AlignCenter,
        AlignTop,
        FontPrimary,
        app->sending ? "Broadcasting" : "Stopped");
    widget_add_text_box_element(
        app->info, 0, 20, 128, 44, AlignLeft, AlignTop, furi_string_get_cstr(text), false);

    furi_string_free(text);
    switch_view(app, ViewIdInfo);
}

/* --- Profiles ----------------------------------------------------------- */

static void profile_chosen(void* context, uint32_t index) {
    App* app = context;
    if(index >= app->settings.profile_count) return;

    if(app->profile_mode == ProfileModeLoad) {
        const Profile* p = &app->settings.profiles[index];
        memcpy(app->settings.uuid, p->uuid, UUID_LEN);
        app->settings.major = p->major;
        app->settings.minor = p->minor;
        settings_save(&app->settings);
        reapply(app);
        show_info(app);
    } else {
        for(uint8_t i = index; i + 1 < app->settings.profile_count; i++) {
            app->settings.profiles[i] = app->settings.profiles[i + 1];
        }
        app->settings.profile_count--;
        settings_save(&app->settings);
        menu_rebuild(app);
        switch_view(app, ViewIdMenu);
    }
}

static void open_profiles(App* app, ProfileMode mode) {
    app->profile_mode = mode;
    submenu_reset(app->profile_menu);
    submenu_set_header(app->profile_menu, mode == ProfileModeLoad ? "Load which?" : "Delete which?");
    for(uint8_t i = 0; i < app->settings.profile_count; i++) {
        submenu_add_item(
            app->profile_menu, app->settings.profiles[i].name, i, profile_chosen, app);
    }
    switch_view(app, ViewIdProfiles);
}

/* The typed text lands in app->name_edit, which was handed to the text input as
   its buffer; the callback itself carries no argument. */
static void name_done(void* context) {
    App* app = context;

    if(app->name_edit[0] != '\0' && app->settings.profile_count < MAX_PROFILES) {
        Profile* p = &app->settings.profiles[app->settings.profile_count];
        memset(p, 0, sizeof(*p));
        strncpy(p->name, app->name_edit, NAME_LEN - 1);
        memcpy(p->uuid, app->settings.uuid, UUID_LEN);
        p->major = app->settings.major;
        p->minor = app->settings.minor;
        app->settings.profile_count++;
        settings_save(&app->settings);
    }
    menu_rebuild(app);
    switch_view(app, ViewIdMenu);
}

/* --- Editing ------------------------------------------------------------ */

static void number_done(void* context, int32_t number) {
    App* app = context;
    switch(app->editing) {
    case EditMajor:
        app->settings.major = (uint16_t)number;
        break;
    case EditMinor:
        app->settings.minor = (uint16_t)number;
        break;
    case EditInterval:
        app->settings.interval = (uint16_t)number;
        break;
    }
    settings_save(&app->settings);
    reapply(app);
    menu_rebuild(app);
    switch_view(app, ViewIdMenu);
}

static void edit_number(App* app, EditTarget target) {
    app->editing = target;
    int32_t current = 0;
    int32_t min = 0;
    int32_t max = 0;
    const char* header = "";

    switch(target) {
    case EditMajor:
        current = app->settings.major;
        min = 0;
        max = 65535;
        header = "Major";
        break;
    case EditMinor:
        current = app->settings.minor;
        min = 0;
        max = 65535;
        header = "Minor";
        break;
    case EditInterval:
        current = app->settings.interval;
        min = 20;
        max = 5000;
        header = "Interval in ms";
        break;
    }

    number_input_set_header_text(app->number_input, header);
    number_input_set_result_callback(app->number_input, number_done, app, current, min, max);
    switch_view(app, ViewIdNumber);
}

static void uuid_done(void* context) {
    App* app = context;
    memcpy(app->settings.uuid, app->uuid_edit, UUID_LEN);
    settings_save(&app->settings);
    reapply(app);
    menu_rebuild(app);
    switch_view(app, ViewIdMenu);
}

/* --- Main menu ---------------------------------------------------------- */

static void menu_chosen(void* context, uint32_t index) {
    App* app = context;
    switch(index) {
    case MenuToggle:
        if(app->sending) {
            beacon_stop(app);
        } else {
            beacon_start(app);
        }
        menu_rebuild(app);
        show_info(app);
        break;
    case MenuUuid:
        memcpy(app->uuid_edit, app->settings.uuid, UUID_LEN);
        byte_input_set_header_text(app->byte_input, "Proximity UUID");
        byte_input_set_result_callback(
            app->byte_input, uuid_done, NULL, app, app->uuid_edit, UUID_LEN);
        switch_view(app, ViewIdUuid);
        break;
    case MenuMajor:
        edit_number(app, EditMajor);
        break;
    case MenuMinor:
        edit_number(app, EditMinor);
        break;
    case MenuInterval:
        edit_number(app, EditInterval);
        break;
    case MenuLoad:
        if(app->settings.profile_count > 0) open_profiles(app, ProfileModeLoad);
        break;
    case MenuSave:
        if(app->settings.profile_count < MAX_PROFILES) {
            memset(app->name_edit, 0, NAME_LEN);
            text_input_set_header_text(app->text_input, "Name this beacon");
            text_input_set_result_callback(
                app->text_input, name_done, app, app->name_edit, NAME_LEN, true);
            switch_view(app, ViewIdName);
        }
        break;
    case MenuDelete:
        if(app->settings.profile_count > 0) open_profiles(app, ProfileModeDelete);
        break;
    case MenuStatus:
        show_info(app);
        break;
    default:
        break;
    }
}

/* The first row shows the current state, so you can see whether it is running
   without drilling in. Submenu items cannot be changed individually, so the
   list is rebuilt. Save and delete only appear when they would do something. */
static void menu_rebuild(App* app) {
    submenu_reset(app->menu);
    submenu_set_header(app->menu, "iBeacon Emulator");
    submenu_add_item(
        app->menu, app->sending ? "Stop broadcasting" : "Start broadcasting", MenuToggle,
        menu_chosen, app);
    submenu_add_item(app->menu, "Set UUID", MenuUuid, menu_chosen, app);
    submenu_add_item(app->menu, "Set major", MenuMajor, menu_chosen, app);
    submenu_add_item(app->menu, "Set minor", MenuMinor, menu_chosen, app);
    submenu_add_item(app->menu, "Set interval", MenuInterval, menu_chosen, app);
    if(app->settings.profile_count > 0) {
        submenu_add_item(app->menu, "Load saved beacon", MenuLoad, menu_chosen, app);
    }
    if(app->settings.profile_count < MAX_PROFILES) {
        submenu_add_item(app->menu, "Save current beacon", MenuSave, menu_chosen, app);
    }
    if(app->settings.profile_count > 0) {
        submenu_add_item(app->menu, "Delete saved beacon", MenuDelete, menu_chosen, app);
    }
    submenu_add_item(app->menu, "Status", MenuStatus, menu_chosen, app);
}

/* Back from a sub-screen returns to the menu; back from the menu exits. */
static bool navigation_callback(void* context) {
    App* app = context;
    if(app->current_view == ViewIdMenu) {
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }
    switch_view(app, ViewIdMenu);
    return true;
}

/* --- Lifecycle ---------------------------------------------------------- */

int32_t ibeacon_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));

    settings_load(&app->settings);
    app->sending = furi_hal_bt_extra_beacon_is_active();

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->menu = submenu_alloc();
    app->profile_menu = submenu_alloc();
    app->byte_input = byte_input_alloc();
    app->number_input = number_input_alloc();
    app->text_input = text_input_alloc();
    app->info = widget_alloc();

    view_dispatcher_add_view(app->view_dispatcher, ViewIdMenu, submenu_get_view(app->menu));
    view_dispatcher_add_view(
        app->view_dispatcher, ViewIdProfiles, submenu_get_view(app->profile_menu));
    view_dispatcher_add_view(
        app->view_dispatcher, ViewIdUuid, byte_input_get_view(app->byte_input));
    view_dispatcher_add_view(
        app->view_dispatcher, ViewIdNumber, number_input_get_view(app->number_input));
    view_dispatcher_add_view(
        app->view_dispatcher, ViewIdName, text_input_get_view(app->text_input));
    view_dispatcher_add_view(app->view_dispatcher, ViewIdInfo, widget_get_view(app->info));

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, navigation_callback);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    menu_rebuild(app);
    switch_view(app, ViewIdMenu);
    view_dispatcher_run(app->view_dispatcher);

    /* Deliberately does NOT stop broadcasting on exit: the extra beacon keeps
       running alongside the stack, which is the point. You want to leave the app
       and walk away with the Flipper. Stop it from the menu. */

    view_dispatcher_remove_view(app->view_dispatcher, ViewIdMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdProfiles);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdUuid);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdNumber);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdName);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdInfo);
    view_dispatcher_free(app->view_dispatcher);
    submenu_free(app->menu);
    submenu_free(app->profile_menu);
    byte_input_free(app->byte_input);
    number_input_free(app->number_input);
    text_input_free(app->text_input);
    widget_free(app->info);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
