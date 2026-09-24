/*
 * iBeacon for Flipper Zero.
 *
 * Broadcasts a standard iBeacon advertisement with a UUID, major, minor and
 * advertising interval you can change on the device itself. Settings survive a
 * reboot.
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
#include <gui/modules/widget.h>
#include <storage/storage.h>

#define UUID_LEN 16
#define ADV_LEN 30
#define SETTINGS_MAGIC 0x4942434Eu /* "IBCN" */
#define SETTINGS_PATH APP_DATA_PATH("settings.bin")

typedef enum {
    ViewIdMenu,
    ViewIdUuid,
    ViewIdNumber,
    ViewIdInfo,
} ViewId;

typedef enum {
    MenuToggle,
    MenuUuid,
    MenuMajor,
    MenuMinor,
    MenuInterval,
    MenuStatus,
} MenuIndex;

typedef enum {
    EditMajor,
    EditMinor,
    EditInterval,
} EditTarget;

typedef struct {
    uint32_t magic;
    uint8_t uuid[UUID_LEN];
    uint16_t major;
    uint16_t minor;
    uint16_t interval;
} Settings;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* menu;
    ByteInput* byte_input;
    NumberInput* number_input;
    Widget* info;

    Settings settings;
    uint8_t uuid_edit[UUID_LEN];
    EditTarget editing;
    bool sending;
    /* This SDK has no view_dispatcher_get_current_view_id, so track it here to
       tell "back to the menu" apart from "leave the app". */
    ViewId current_view;
} App;

/* --- Settings ----------------------------------------------------------- */

static void settings_defaults(Settings* s) {
    /* Apple's own sample UUID. Generate your own with `uuidgen` for real use. */
    static const uint8_t def[UUID_LEN] = {
        0xe2, 0xc5, 0x6d, 0xb5, 0xdf, 0xfb, 0x48, 0xd2,
        0xb0, 0x60, 0xd0, 0xf5, 0xa7, 0x10, 0x96, 0xe0};
    s->magic = SETTINGS_MAGIC;
    memcpy(s->uuid, def, UUID_LEN);
    s->major = 1;
    s->minor = 1;
    s->interval = 100;
}

static void settings_load(Settings* s) {
    settings_defaults(s);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    Settings tmp;
    if(storage_file_open(file, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(storage_file_read(file, &tmp, sizeof(tmp)) == sizeof(tmp) &&
           tmp.magic == SETTINGS_MAGIC) {
            *s = tmp;
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void settings_save(const Settings* s) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, APP_DATA_PATH(""));
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, s, sizeof(*s));
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
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
    furi_string_cat_printf(text, "interval %u ms", app->settings.interval);

    widget_add_string_element(
        app->info,
        64,
        6,
        AlignCenter,
        AlignTop,
        FontPrimary,
        app->sending ? "Broadcasting" : "Stopped");
    widget_add_text_box_element(
        app->info, 0, 22, 128, 42, AlignLeft, AlignTop, furi_string_get_cstr(text), false);

    furi_string_free(text);
    switch_view(app, ViewIdInfo);
}

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
    /* Already broadcasting? Restart with the new values, or the air would carry
       the old frame while the menu claims something else. */
    if(app->sending) beacon_start(app);
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
    if(app->sending) beacon_start(app);
    menu_rebuild(app);
    switch_view(app, ViewIdMenu);
}

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
    case MenuStatus:
        show_info(app);
        break;
    default:
        break;
    }
}

/* The first row shows the current state, so you can see whether it is running
   without drilling in. Submenu items cannot be changed individually, so the
   list is rebuilt; the selection stays on the first row. */
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
    app->byte_input = byte_input_alloc();
    app->number_input = number_input_alloc();
    app->info = widget_alloc();

    view_dispatcher_add_view(app->view_dispatcher, ViewIdMenu, submenu_get_view(app->menu));
    view_dispatcher_add_view(
        app->view_dispatcher, ViewIdUuid, byte_input_get_view(app->byte_input));
    view_dispatcher_add_view(
        app->view_dispatcher, ViewIdNumber, number_input_get_view(app->number_input));
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
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdUuid);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdNumber);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdInfo);
    view_dispatcher_free(app->view_dispatcher);
    submenu_free(app->menu);
    byte_input_free(app->byte_input);
    number_input_free(app->number_input);
    widget_free(app->info);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
