#include "linux_gtk.h"
#include "linux_abi.h"
#include "linux_file_picker.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// GTK as the file dialog library uses it: it builds a "file chooser dialog" (title, action, filters, folder), runs it and reads the answer.
// Everything else it calls (windows, displays, signals, types) has nothing to do on a console and is answered with a harmless value.
#define GTK_RESPONSE_ACCEPT (-3)
#define GTK_RESPONSE_CANCEL (-6)
enum { TYPE_WIDGET = 0x1001, TYPE_WINDOW, TYPE_DIALOG, TYPE_FILE_CHOOSER, TYPE_DISPLAY, TYPE_X11_DISPLAY };

typedef struct {
    bool used;
    char name[48];
    char patterns[LINUX_PICKER_PATTERNS][24];
    unsigned count;
} Filter;
typedef struct {
    bool used, has_result, multiple;
    int action;
    char title[96], folder[512], name[128], result[512];
    Filter *filters[LINUX_PICKER_FILTERS];
    unsigned filter_count;
} Dialog;
typedef struct GSList {
    void *data;
    struct GSList *next;
} GSList;

static Dialog dialogs[4];
static Filter filters[16];
static char dummy_display[8], dummy_widget[8];
static atomic_flag pool_lock = ATOMIC_FLAG_INIT;
static void lockPool(void) {
    while (atomic_flag_test_and_set_explicit(&pool_lock, memory_order_acquire)) {}
}
static void unlockPool(void) { atomic_flag_clear_explicit(&pool_lock, memory_order_release); }
static Dialog *dialogOf(void *pointer) {
    for (unsigned i = 0; i < sizeof(dialogs) / sizeof(dialogs[0]); ++i)
        if (pointer == &dialogs[i] && dialogs[i].used) return &dialogs[i];
    return NULL;
}
static Filter *filterOf(void *pointer) {
    for (unsigned i = 0; i < sizeof(filters) / sizeof(filters[0]); ++i)
        if (pointer == &filters[i] && filters[i].used) return &filters[i];
    return NULL;
}
static char *copyString(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = linuxAbiMalloc(length);
    if (copy) memcpy(copy, text, length);
    return copy;
}

static int gtkInitCheck(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    return 1;
}
static void *gtkFileChooserDialogNew(const char *title, void *parent, int action, const char *first_button) {  // more arguments follow: the buttons
    (void)parent;
    (void)first_button;
    Dialog *dialog = NULL;
    lockPool();
    for (unsigned i = 0; i < sizeof(dialogs) / sizeof(dialogs[0]) && !dialog; ++i)
        if (!dialogs[i].used) {
            memset(&dialogs[i], 0, sizeof(dialogs[i]));
            dialogs[i].used = true;
            dialog = &dialogs[i];
        }
    unlockPool();
    if (dialog) {
        dialog->action = action;
        snprintf(dialog->title, sizeof(dialog->title), "%s", title ? title : "");
    }
    return dialog;
}
static void *gtkFileFilterNew(void) {
    Filter *filter = NULL;
    lockPool();
    for (unsigned i = 0; i < sizeof(filters) / sizeof(filters[0]) && !filter; ++i)
        if (!filters[i].used) {
            memset(&filters[i], 0, sizeof(filters[i]));
            filters[i].used = true;
            filter = &filters[i];
        }
    unlockPool();
    return filter;
}
static void gtkFileFilterSetName(void *filter, const char *name) {
    Filter *f = filterOf(filter);
    if (f) snprintf(f->name, sizeof(f->name), "%s", name ? name : "");
}
static void gtkFileFilterAddPattern(void *filter, const char *pattern) {
    Filter *f = filterOf(filter);
    if (f && pattern && f->count < LINUX_PICKER_PATTERNS) snprintf(f->patterns[f->count++], sizeof(f->patterns[0]), "%s", pattern);
}
static void gtkFileChooserAddFilter(void *chooser, void *filter) {
    Dialog *d = dialogOf(chooser);
    Filter *f = filterOf(filter);
    if (d && f && d->filter_count < LINUX_PICKER_FILTERS) d->filters[d->filter_count++] = f;
}
static void *gtkFileChooserGetFilter(void *chooser) {
    Dialog *d = dialogOf(chooser);
    return d && d->filter_count ? (void *)d->filters[0] : NULL;
}
static int gtkFileChooserSetCurrentFolder(void *chooser, const char *folder) {
    Dialog *d = dialogOf(chooser);
    if (d && folder) snprintf(d->folder, sizeof(d->folder), "%s", folder);
    return 1;
}
static void gtkFileChooserSetCurrentName(void *chooser, const char *name) {
    Dialog *d = dialogOf(chooser);
    if (d && name) snprintf(d->name, sizeof(d->name), "%s", name);
}
static void gtkFileChooserSetSelectMultiple(void *chooser, int multiple) {
    Dialog *d = dialogOf(chooser);
    if (d) d->multiple = multiple != 0;
}
static void gtkFileChooserSetDoOverwriteConfirmation(void *chooser, int confirm) {
    (void)chooser;
    (void)confirm;
}
static int gtkDialogRun(void *dialog) {
    Dialog *d = dialogOf(dialog);
    if (!d) return GTK_RESPONSE_CANCEL;
    LinuxPickerRequest request = {0};
    request.action = d->action;
    request.title = d->title;
    request.folder = d->folder[0] ? d->folder : NULL;
    request.name = d->name;
    request.filter_count = d->filter_count;
    for (unsigned f = 0; f < d->filter_count; ++f) {
        request.filter_names[f] = d->filters[f]->name;
        request.pattern_counts[f] = d->filters[f]->count;
        for (unsigned p = 0; p < d->filters[f]->count; ++p) request.patterns[f][p] = d->filters[f]->patterns[p];
    }
    char path[512];
    bool accepted = linuxFilePickerRun(&request, path, sizeof(path));
    if (accepted) {
        snprintf(d->result, sizeof(d->result), "%s", path);
        d->has_result = true;
    }
    return accepted ? GTK_RESPONSE_ACCEPT : GTK_RESPONSE_CANCEL;
}
static char *gtkFileChooserGetFilename(void *chooser) {
    Dialog *d = dialogOf(chooser);
    return d && d->has_result ? copyString(d->result) : NULL;
}
static char *gtkFileChooserGetCurrentName(void *chooser) {
    Dialog *d = dialogOf(chooser);
    if (!d) return NULL;
    const char *slash = d->has_result ? strrchr(d->result, '/') : NULL;
    return copyString(slash ? slash + 1 : d->name);
}
static GSList *gtkFileChooserGetFilenames(void *chooser) {
    Dialog *d = dialogOf(chooser);
    if (!d || !d->has_result) return NULL;
    GSList *node = linuxAbiMalloc(sizeof(GSList));
    if (!node) return NULL;
    node->data = copyString(d->result);
    node->next = NULL;
    return node;
}
static void gtkWidgetDestroy(void *widget) {
    Dialog *d = dialogOf(widget);
    if (!d) return;
    lockPool();
    for (unsigned f = 0; f < d->filter_count; ++f) d->filters[f]->used = false;  // the filters belong to the dialog
    d->used = false;
    unlockPool();
}
static void gtkDoNothing(void) {}
static unsigned gtkZero(void) { return 0; }
static void *gtkNull(void) { return NULL; }
static void *gtkInstance(void *instance) { return instance; }  // g_type_check_instance_cast: a cast always succeeds
static void *gtkDummyWidget(void) { return dummy_widget; }
static void *gtkDummyDisplay(void) { return dummy_display; }
static unsigned long gtkTypeWidget(void) { return TYPE_WIDGET; }
static unsigned long gtkTypeWindow(void) { return TYPE_WINDOW; }
static unsigned long gtkTypeDialog(void) { return TYPE_DIALOG; }
static unsigned long gtkTypeFileChooser(void) { return TYPE_FILE_CHOOSER; }
static unsigned long gtkTypeDisplay(void) { return TYPE_DISPLAY; }
static unsigned long gtkTypeX11Display(void) { return TYPE_X11_DISPLAY; }
static int gtkInstanceIsA(void *instance, unsigned long type) { return instance != NULL && type != TYPE_X11_DISPLAY; }  // never an X11 display
static unsigned long gtkSignalConnect(void) { return 1; }
static void gtkFree(void *memory) { linuxAbiFree(memory); }
static unsigned gtkSlistLength(GSList *list) {
    unsigned count = 0;
    for (; list; list = list->next) ++count;
    return count;
}
static void *gtkSlistNthData(GSList *list, unsigned index) {
    for (; list && index; list = list->next) --index;
    return list ? list->data : NULL;
}
static void gtkSlistFree1(GSList *node) { linuxAbiFree(node); }

typedef struct {
    const char *name;
    uintptr_t address;
} Entry;
#define E(name, function) {name, (uintptr_t)(function)}
static const Entry table[] = {
    E("gtk_init_check", gtkInitCheck),
    E("gtk_file_chooser_dialog_new", gtkFileChooserDialogNew),
    E("gtk_file_filter_new", gtkFileFilterNew),
    E("gtk_file_filter_set_name", gtkFileFilterSetName),
    E("gtk_file_filter_add_pattern", gtkFileFilterAddPattern),
    E("gtk_file_chooser_add_filter", gtkFileChooserAddFilter),
    E("gtk_file_chooser_get_filter", gtkFileChooserGetFilter),
    E("gtk_file_chooser_set_current_folder", gtkFileChooserSetCurrentFolder),
    E("gtk_file_chooser_set_current_name", gtkFileChooserSetCurrentName),
    E("gtk_file_chooser_set_select_multiple", gtkFileChooserSetSelectMultiple),
    E("gtk_file_chooser_set_do_overwrite_confirmation", gtkFileChooserSetDoOverwriteConfirmation),
    E("gtk_dialog_run", gtkDialogRun),
    E("gtk_file_chooser_get_filename", gtkFileChooserGetFilename),
    E("gtk_file_chooser_get_current_name", gtkFileChooserGetCurrentName),
    E("gtk_file_chooser_get_filenames", gtkFileChooserGetFilenames),
    E("gtk_widget_destroy", gtkWidgetDestroy),
    E("gtk_dialog_add_button", gtkDummyWidget),
    E("gtk_widget_show_all", gtkDoNothing),
    E("gtk_widget_get_window", gtkNull),
    E("gtk_widget_get_display", gtkDummyDisplay),
    E("gtk_window_present_with_time", gtkDoNothing),
    E("gtk_window_set_screen", gtkDoNothing),
    E("gtk_events_pending", gtkZero),
    E("gtk_main_iteration", gtkZero),
    E("gtk_widget_get_type", gtkTypeWidget),
    E("gtk_window_get_type", gtkTypeWindow),
    E("gtk_dialog_get_type", gtkTypeDialog),
    E("gtk_file_chooser_get_type", gtkTypeFileChooser),
    E("gdk_display_get_type", gtkTypeDisplay),
    E("gdk_x11_display_get_type", gtkTypeX11Display),
    E("gdk_display_manager_get", gtkDummyWidget),
    E("gdk_display_manager_list_displays", gtkNull),
    E("gdk_display_manager_open_display", gtkNull),
    E("gdk_set_allowed_backends", gtkDoNothing),
    E("gdk_x11_window_foreign_new_for_display", gtkNull),
    E("gdk_window_set_transient_for", gtkDoNothing),
    E("gdk_window_get_events", gtkZero),
    E("gdk_window_set_events", gtkDoNothing),
    E("gdk_window_get_screen", gtkNull),
    E("gdk_x11_get_server_time", gtkZero),
    E("g_type_check_instance_cast", gtkInstance),
    E("g_type_check_instance_is_a", gtkInstanceIsA),
    E("g_signal_connect_data", gtkSignalConnect),
    E("g_signal_handler_disconnect", gtkDoNothing),
    E("g_object_unref", gtkDoNothing),
    E("g_free", gtkFree),
    E("g_slist_length", gtkSlistLength),
    E("g_slist_nth_data", gtkSlistNthData),
    E("g_slist_free_1", gtkSlistFree1),
};
static uintptr_t gtkLookup(const char *name) {
    for (size_t i = 0; name && i < sizeof(table) / sizeof(table[0]); ++i)
        if (!strcmp(table[i].name, name)) return table[i].address;
    return 0;
}
const LinuxVirtualLibrary linuxGtkLibraries[4] = {{"libgtk-3", gtkLookup, NULL, true},
                                                  {"libgdk-3", gtkLookup, NULL, true},
                                                  {"libglib-2.0", gtkLookup, NULL, true},
                                                  {"libgobject-2.0", gtkLookup, NULL, true}};
