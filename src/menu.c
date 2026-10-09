/*
  menu.c - MiSTeryNano menu based in u8g2

  This version includes the old static MiSTeryNano type of menu
  as well as the new config driven one.

*/
  
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ff.h>
#include <diskio.h>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#else
#include <FreeRTOS.h>
#include <timers.h>
#include <task.h>
#include <queue.h>
#endif

#include "sdc.h"
#include "osd.h"
#include "inifile.h"
#include "menu.h"
#include "sysctrl.h"
#include "debug.h"
#include "ra_patch.h"
#include "ra_state.h"
#include "ra_queue.h"
#include "ra_task.h"
#include "games.h"
#include "rc_version.h"
#include "mcu_hw.h"

#ifdef ENABLE_BLUETOOTH
#include "bluetooth.h"
#endif

// this is the u8g2_font_helvR08_te with any trailing
// spaces removed
#include "font_helvR08_te.c"

// some constants for arrangement
// The OSD (currently) is 64 pixel high. To allow for a proper
// box around a text line, it needs to be 12 pixels high. A total
// of five lines is 5*12 = 60 + title seperation line
#define MENU_LINE_Y      13   // y pos of seperator line
#define MENU_ENTRY_H     12   // height of regular menu entries
#define MENU_ENTRY_BASE   9   // font baseline offset


#define MENU_FORM_FSEL           -1

#define MENU_ENTRY_INDEX_ID       0
#define MENU_ENTRY_INDEX_LABEL    1
#define MENU_ENTRY_INDEX_FORM     2
#define MENU_ENTRY_INDEX_OPTIONS  2
#define MENU_ENTRY_INDEX_VARIABLE 3

// menu types
#define MENU_TYPE_MENU          0
#define MENU_TYPE_FILESELECTOR  1
#define MENU_TYPE_CUSTOM        2  // only internally used

typedef struct config_custom_S {
  char *label;

  // Pointer to function the returns the number of active entries in
  // this custom dialog. This is the number of "lines" that can be
  // selected with the up and down keys
  int (*length)(void); 
  
  // Pointer to function that draws the custom contents
  void (*draw)(void); 

  // Pointer to function called with the 0-based line when a line is
  // selected, NULL when the lines select nothing
  void (*select)(int line);
  
} config_custom_t;

/* new menu state */
typedef struct menu_state {
  int type;
  int selected; 
  int scroll;
  union {
    const config_menu_t *menu;  
    const char *label;             // used for file/image selector, only
    const config_custom_t *custom;  
  };
  
  // file selector related
  sdc_dir_entry_t *dir;

  struct menu_state *prev;
} menu_state_t;

typedef struct {
  char index;
  char **ext;
  config_action_t *action;  // action to be run afterwards

  // string and icon to be used if nothing is selected
  // by default this is "[X] No Disk"
  char *none_str;
  unsigned char *none_icn;
} fsel_state_t;

// menu state is a stack since as the menu is hierarchical
static menu_state_t *menu_state = NULL;
static fsel_state_t fsel_state;

/* =========== handling of variables ============= */
static menu_variable_t *variables = NULL;

menu_variable_t *menu_get_variables(void) {
  return variables;
}

static int menu_variable_get(char id) {
  menu_variable_t *v = variables;

  while(v) {
    if(v->id == id) return v->value;
    v = v->next;
  }
  return 0;  
}

static bool menu_variable_exists(char id) {
  menu_variable_t *v = variables;

  while(v) {
    if(v->id == id) return true;
    v = v->next;
  }
  return false;  
}

static void menu_variable_set(char id, int value) {
  menu_variable_t *v = variables;
  
  while(v) {
    if(v->id == id) {
      if(v->value != value) {
	v->value = value;
	// also set this in the core
	sys_set_val(id, value);
      }
    }
    v = v->next;
  }
}

static void menu_setup_variable(char id, int value) {
  // menu_debugf("setup variable '%c' = %d", id, value);

  // allocate new entry
  menu_variable_t *variable = pvPortMalloc(sizeof(menu_variable_t));
  variable->id = id;
  variable->value = value;
  variable->next = NULL;

  if(!variables)
    variables = variable;
  else {
    menu_variable_t *v = variables;
    while(v->next) v = v->next;
    v->next = variable;
  }

  // also set the value in the core
  sys_set_val(id, value);
}

static void menu_setup_menu_variables(const config_menu_t *menu) {
  config_menu_entry_t *me = menu->entries;
  while(me) {
    if(me->type == CONFIG_MENU_ENTRY_MENU)
      menu_setup_menu_variables(me->menu);
    
    if(me->type == CONFIG_MENU_ENTRY_LIST) {
      // setup variable ...
      menu_setup_variable(me->list->id, me->list->def);
    }
    
    if(me->type == CONFIG_MENU_ENTRY_TOGGLE) {
      // setup variable ...
      menu_setup_variable(me->toggle->id, me->toggle->def);
    }
    
    if(me->type == CONFIG_MENU_ENTRY_RANGE) {
      // setup variable ...
      menu_setup_variable(me->range->id, me->range->def);
    }
    
    me = me->next;
  }
}

static void menu_setup_variables(void) {
  // variables occur in two places:
  // in the set command used in actions
  // in menu items (currently only in lists as buttons use actions)

  // Only the variables from menu items are actually stored permanently.
  // Action variables are sent into the core and processed there.
  
  // search through menu tree for lists
  menu_setup_menu_variables(cfg->menu);  
}


void menu_set_value(unsigned char id, int8_t value) {
  // this is called when reading the ini file
  // We allow values in the ini file which are actually not in the
  // menu at all. This can be used for "static" changes which are made
  // once by manually manipulating the ini file e.g. with a text
  // editor

  if(!menu_variable_exists(id)) {
    // previous companion versions were writing the action related variables
    // into the ini file as well. This mainly affected the 'R' variable often
    // used for reset. We ignore this particular variable by now as this was
    // not intentinal. This should be removed one day.
    if(id == 'R') {
      menu_debugf("suppressing 'R' reset variable");
      return;
    }
    menu_setup_variable(id, value);
  } else
    menu_variable_set(id, value);
}

// various 8x8 icons
static const unsigned char icn_right_bits[]  = { 0x00,0x04,0x0c,0x1c,0x3c,0x1c,0x0c,0x04 };
static const unsigned char icn_left_bits[]   = { 0x00,0x20,0x30,0x38,0x3c,0x38,0x30,0x20 };
static const unsigned char icn_floppy_bits[] = { 0xff,0x81,0x83,0x81,0xbd,0xad,0x6d,0x3f };
static const unsigned char icn_empty_bits[] =  { 0xc3,0xe7,0x7e,0x3c,0x3c,0x7e,0xe7,0xc3 };
static const unsigned char icn_on_bits[] =     { 0x3c,0x42,0x99,0xbd,0xbd,0x99,0x42,0x3c };
static const unsigned char icn_off_bits[] =    { 0x3c,0x42,0x81,0x81,0x81,0x81,0x42,0x3c };

void u8g2_DrawStrT(u8g2_t *u8g2, u8g2_uint_t x, u8g2_uint_t y, const char *s) {
  // get length of string
  int n = 0;
  while(s[n] && s[n] != ';' && s[n] != ',' && s[n] != '|') n++;

  // create a 0 terminated copy in the stack
  char buffer[n+1];
  strncpy(buffer, s, n);
  buffer[n] = '\0';
  
  u8g2_DrawStr(u8g2, x, y, buffer);
}

#define FS_ICON_WIDTH 10
static int fs_scroll_cur = -1;

static int dir_len(sdc_dir_entry_t *d) {
  int len = 0;
  while(d) {
    d = d->next;
    len++;
  }
  return len;
}

static sdc_dir_entry_t *dir_entry(sdc_dir_entry_t *d, int n) {
  while(n && d) {
    d = d->next;
    n--;
  }
  return d;
}

static void menu_fs_scroll_entry(void) {
  // no scrolling
  if(fs_scroll_cur < 0) return;
  
  // don't scroll anything else
  if(menu_state->type != MENU_TYPE_FILESELECTOR) return;
  
  int row = menu_state->selected - 1;
  int y =  MENU_LINE_Y + MENU_ENTRY_H * (row-menu_state->scroll+1);
  int width = u8g2_GetDisplayWidth(&u8g2);

  // jump to the row'th entry in the dir listing
  int swid = u8g2_GetStrWidth(&u8g2, dir_entry(menu_state->dir, row)->name) + 1;

  // fill the area where the scrolling entry would show
  u8g2_SetClipWindow(&u8g2, FS_ICON_WIDTH, y-MENU_ENTRY_BASE, width, y+MENU_ENTRY_H-MENU_ENTRY_BASE);  
  u8g2_DrawBox(&u8g2, FS_ICON_WIDTH, y-MENU_ENTRY_BASE, width-FS_ICON_WIDTH, MENU_ENTRY_H);
  u8g2_SetDrawColor(&u8g2, 0);

  int scroll = fs_scroll_cur++ - 25;   // 25 means 1 sec delay
  if(fs_scroll_cur > swid-width+FS_ICON_WIDTH+50) fs_scroll_cur = 0;
  if(scroll < 0) scroll = 0;
  if(scroll > swid-width+FS_ICON_WIDTH) scroll = swid-width+FS_ICON_WIDTH;
  
  u8g2_DrawStr(&u8g2, FS_ICON_WIDTH-scroll, y, dir_entry(menu_state->dir, row)->name);
  
  // restore previous draw mode
  u8g2_SetDrawColor(&u8g2, 1);
  u8g2_SetMaxClipWindow(&u8g2);
  u8g2_SendBuffer(&u8g2);
}

void menu_timer_enable(bool on);

static void menu_fs_draw_entry(int row, sdc_dir_entry_t *entry) {      
  static const unsigned char folder_icon[] = { 0x70,0x8e,0xff,0x81,0x81,0x81,0x81,0x7e };
  static const unsigned char up_icon[] =     { 0x04,0x0e,0x1f,0x0e,0xfe,0xfe,0xfe,0x00 };
  static const unsigned char empty_icon[] =  { 0xc3,0xe7,0x7e,0x3c,0x3c,0x7e,0xe7,0xc3 };

  int y =  MENU_LINE_Y + MENU_ENTRY_H * (row+1);
  int width = u8g2_GetDisplayWidth(&u8g2);
  
  if(entry->name[0] == '/') {
    if(fsel_state.none_str) u8g2_DrawStr(&u8g2, FS_ICON_WIDTH, y, fsel_state.none_str);
    else                    u8g2_DrawStr(&u8g2, FS_ICON_WIDTH, y, "No Disk");
  } else {
    char str[strlen(entry->name)+1];
    strcpy(str, entry->name);
  
    // properly ellipsize string
    int dotlen = u8g2_GetStrWidth(&u8g2, "...");
    if(u8g2_GetStrWidth(&u8g2, str) > width-FS_ICON_WIDTH) {
      // the entry is too long to fit the menu.    
      // check if this is the selected file and then enable scrolling
      if(row == menu_state->selected - menu_state->scroll - 1)
	fs_scroll_cur = 0;
      
      // enable timer, to allow animations
      menu_timer_enable(true);
      
      while(u8g2_GetStrWidth(&u8g2, str) > width-FS_ICON_WIDTH-dotlen) str[strlen(str)-1] = 0;
      if(strlen(str) < sizeof(str)-4) strcat(str, "...");
    }
  
    u8g2_DrawStr(&u8g2, FS_ICON_WIDTH, y, str);
  }
  
  // draw folder icon in front of directories
  if(entry->is_dir)
    u8g2_DrawXBM(&u8g2, 1, y-8, 8, 8,
		 (entry->name[0] == '/')?(fsel_state.none_icn?fsel_state.none_icn:empty_icon):
		 strcmp(entry->name, "..")?folder_icon:
		 up_icon);

  if(menu_state->selected == row+menu_state->scroll+1)
    u8g2_DrawButtonFrame(&u8g2, 0, y, U8G2_BTN_INV, width, 1, 1);
}

static void menu_push(void) {
  menu_debugf("menu_push()");

  // allocate a new menu entry
  menu_state_t *state = pvPortMalloc(sizeof(menu_state_t));

  // insert the new entry at begin of chain
  state->prev = menu_state;
  menu_state = state;
}

static int menu_len(const config_menu_t *menu) {
  int entries = 0;
  config_menu_entry_t *me = menu->entries;
  while(me) {
    entries++;
    me = me->next;
  }
  return entries;
}

static int menu_count_entries(void) {
  int entries = 0;

  if(menu_state->type == MENU_TYPE_MENU)
    entries = menu_len(menu_state->menu);
  else if(menu_state->type == MENU_TYPE_FILESELECTOR)
    entries = dir_len(menu_state->dir);
  else if(menu_state->type == MENU_TYPE_CUSTOM)
    if(menu_state->custom->length)
      entries = menu_state->custom->length();
    
  return entries+1;  // title is also an entry
}

static bool menu_is_root(const config_menu_t *menu) {
  return menu == cfg->menu;
}

static int menu_entry_is_usable(void) {
  // not root menu? Then all entries are usable. Root menu
  // title is only usable if no hid keyboard or gamepad is present
  if(!menu_is_root(menu_state->menu) || !mcu_hw_hid_present()) return 1;

  // in root menu only the title is unusable
  return menu_state->selected != 0;
}

static config_menu_entry_t *menu_get_selected_entry(void) {
  // first check if there's a menu at all  
  if(menu_state->type != MENU_TYPE_MENU)
    return NULL;
  
  config_menu_entry_t *entry = menu_state->menu->entries;
  for(int i=0;i<menu_state->selected - 1;i++) entry=entry->next;
  return entry;
}
  
static void menu_entry_go(int step) {

  // the up/down events change the a range value in edit mode
  config_menu_entry_t *entry = menu_get_selected_entry();
  if(entry && entry->type == CONFIG_MENU_ENTRY_RANGE && entry->range->edit) {
    int value = menu_variable_get(entry->range->id);

    value -= step;
    if(value > entry->range->max) value = entry->range->max;
    if(value < entry->range->min) value = entry->range->min;          

    menu_variable_set(entry->range->id, value);
    return;
  }							  
  
  int entries = menu_count_entries();
  // a custom page can lose entries while it is open, e.g. when a new set arrives
  if(menu_state->selected >= entries) menu_state->selected = entries - 1;
  if(menu_state->scroll > (entries > 5 ? entries - 5 : 0)) menu_state->scroll = entries > 5 ? entries - 5 : 0;
  do {
    menu_state->selected += step;
    
    // single step wraps top/bottom, paging does not
    if(abs(step) == 1) {    
      if(menu_state->selected < 0) menu_state->selected = entries + menu_state->selected;
      if(menu_state->selected >= entries) menu_state->selected = menu_state->selected - entries;
    } else {
      // limit to top/bottom. Afterwards step 1 in opposite
      // direction to skip unusable entries
      if(menu_state->selected < 1) { menu_state->selected = 1; step = 1; }	
      if(menu_state->selected >= entries) { menu_state->selected = entries - 1; step = -1; }
    }

    // scrolling needed?
    if(step > 0) {
      if(entries <= 5)                            menu_state->scroll = 0;
      else {
	if(menu_state->selected <= 3)             menu_state->scroll = 0;
	else if(menu_state->selected < entries-2) menu_state->scroll = menu_state->selected - 3;
	else                                      menu_state->scroll = entries-5;
      }
    }

    if(step < 0) {
      if(entries <= 5)                            menu_state->scroll = 0;
      else {
	if(menu_state->selected <= 2)             menu_state->scroll = 0;
	else if(menu_state->selected < entries-3) menu_state->scroll = menu_state->selected - 2;
	else                                      menu_state->scroll = entries-5;
      }
    }    
  } while(!menu_entry_is_usable());
}

static void menu_draw_title(const char *s, bool arrow, bool selected) {
  int x = 1;

  // draw left arrow for submenus
  if(arrow) {
    u8g2_DrawXBM(&u8g2, 0, 1, 8, 8, icn_left_bits);    
    x = 8;
  } else if(!mcu_hw_hid_present()) {
    // without keyboard or joystick connected, the menu runs in
    // "single button" mode
  
    // no arrow? Then this is the root menu. In that case
    // a cross is drawn in "single button mode"
    u8g2_DrawXBM(&u8g2, 0, 1, 8, 8, icn_empty_bits);    
    x = 10;
  }
  
  // draw title in bold and seperator line
  u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
  u8g2_DrawStr(&u8g2, x, MENU_ENTRY_BASE, s);
  u8g2_DrawHLine(&u8g2, 0, MENU_LINE_Y, u8g2_GetDisplayWidth(&u8g2));

  if(selected)
    u8g2_DrawButtonFrame(&u8g2, 0, MENU_ENTRY_BASE, U8G2_BTN_INV,
	 u8g2_GetDisplayWidth(&u8g2), 1, 1);
  
  // draw the rest with normal font
  u8g2_SetFont(&u8g2, font_helvR08_te);
}

static char *menuentry_get_label(config_menu_entry_t *entry) {
  if(entry->type == CONFIG_MENU_ENTRY_MENU)
    return entry->menu->label;
  if(entry->type == CONFIG_MENU_ENTRY_FILESELECTOR)
    return entry->fsel->label;
  if(entry->type == CONFIG_MENU_ENTRY_LIST)
    return entry->list->label;
  if(entry->type == CONFIG_MENU_ENTRY_BUTTON)
    return entry->button->label;
  if(entry->type == CONFIG_MENU_ENTRY_IMAGE)
    return entry->image->label;
  if(entry->type == CONFIG_MENU_ENTRY_TOGGLE)
    return entry->toggle->label;
  if(entry->type == CONFIG_MENU_ENTRY_RANGE)
    return entry->range->label;
  
  return NULL;
}

static int menu_get_list_length(config_menu_entry_t *entry) {
  int len = 0;
  config_listentry_t *le = entry->list->listentries;
  while(le) {
    len++;
    le = le->next;
  }
  return len;
}
  
static char *menu_get_listentry(config_menu_entry_t *entry, int value) {
  if(!entry || entry->type != CONFIG_MENU_ENTRY_LIST) return NULL;

  config_listentry_t *le = entry->list->listentries;
  while(le) {
    if(le->value == value)
      return le->label;
    le = le->next;
  }

  return NULL;
}

static void menu_draw_entry(config_menu_entry_t *entry, int row, bool selected) {
  menu_debugf("row %d: %s '%s'", row,
	      config_menuentry_get_type_str(entry),
	      menuentry_get_label(entry));

  // all menu entries use some kind of label
  char *s = menuentry_get_label(entry);
  int ypos = MENU_LINE_Y+MENU_ENTRY_H + MENU_ENTRY_H * row;
  int width = u8g2_GetDisplayWidth(&u8g2);
  
  // all menu entries are a plain text
  u8g2_DrawStr(&u8g2, 1, ypos, s);
    
  // prepare highlight
  int hl_w = width;

  // handle second string for list entries
  if(entry->type == CONFIG_MENU_ENTRY_LIST) {
    // get matching variable
    int value = menu_variable_get(entry->list->id);
    char *str = menu_get_listentry(entry, value);

    if(str) {
      // right align entry
      int sw = u8g2_GetStrWidth(&u8g2, str) + 1;
      if(sw > width/2) sw = width/2;
      u8g2_DrawStr(&u8g2, width-sw, ypos, str);
    }
		  
    hl_w = width/2;
  }
  
  // some entries have a small icon to the right    
  else if(entry->type == CONFIG_MENU_ENTRY_MENU)
    u8g2_DrawXBM(&u8g2, hl_w-8, ypos-8, 8, 8, icn_right_bits);

  else if(entry->type == CONFIG_MENU_ENTRY_FILESELECTOR) {
    // icon depends if floppy is inserted
    u8g2_DrawXBM(&u8g2, hl_w-MENU_ENTRY_BASE, ypos-8, 8, 8,
		 sdc_get_image_name(entry->fsel->index)?icn_floppy_bits:icn_empty_bits);
  }
  
  else if(entry->type == CONFIG_MENU_ENTRY_TOGGLE) 
    u8g2_DrawXBM(&u8g2, hl_w-MENU_ENTRY_BASE, ypos-8, 8, 8,
		 menu_variable_get(entry->toggle->id)?icn_on_bits:icn_off_bits);
  
  if(entry->type == CONFIG_MENU_ENTRY_IMAGE) {
    const unsigned char *icon = sdc_get_image_name(entry->image->index+MAX_DRIVES)?icn_floppy_bits:
      entry->image->none_icn?entry->image->none_icn:
      icn_empty_bits;
    
    u8g2_DrawXBM(&u8g2, hl_w-MENU_ENTRY_BASE, ypos-8, 8, 8, icon);
  }
    
  else if(entry->type == CONFIG_MENU_ENTRY_RANGE) {
    char str[5];  // max length 5 for "-100\0"
    
    // draw range value
    int value = menu_variable_get(entry->range->id);
    sprintf(str, "%d", value);

    // right align entry
    int sw = u8g2_GetStrWidth(&u8g2, str) + 1;
    if(sw > width/2) sw = width/2;
    u8g2_DrawStr(&u8g2, width-sw, ypos, str);

    // in "normal" menu mode, the entire range is highlighted when
    // selected. In "edit" mode only the value is highlighted
    if(entry->range->edit) {    
      // only highlight the value
      hl_w = u8g2_GetStrWidth(&u8g2, str) + 1;      
    }
  }
    
  if(selected)
    u8g2_DrawButtonFrame(&u8g2, width-hl_w, ypos, U8G2_BTN_INV, hl_w, 1, 1);
}

// Draws msg centered in lines no wider than the display, from y_in down, and
// returns the y below the last line. With y_in 0 it only measures. A '\n' ends
// a line, the rest breaks at spaces, and a word wider than the display is split.
static int menu_wrap_text(int y_in, const char *msg) {
  const int width = u8g2_GetDisplayWidth(&u8g2);
  char line[80];
  int y = y_in;

  u8g2_SetFont(&u8g2, font_helvR08_te);
  if(!*msg) return y + 11;   // an empty message still takes one line

  while(*msg) {
    size_t take = 0;   // characters that fit for sure
    size_t gap  = 0;   // last space among them, 0 = none
    size_t i;

    while(*msg == ' ') msg++;                        // a line never starts with a space
    if(*msg == '\n') { y += 11; msg++; continue; }   // an empty line
    if(!*msg) break;

    // take characters until the line is too wide, the buffer is full or the text ends
    for(i = 0; msg[i] && msg[i] != '\n' && i < sizeof(line) - 1; i++) {
      line[i]     = msg[i];
      line[i + 1] = '\0';
      if(u8g2_GetStrWidth(&u8g2, line) > width) break;
      if(msg[i] == ' ') gap = i;
      take = i + 1;
    }

    // stopped inside a word with a space before it: break at the space instead.
    // Stopped at a space: everything before it fits, the line ends right there.
    if(msg[i] && msg[i] != '\n' && msg[i] != ' ' && gap) take = gap;
    if(take == 0) take = 1;   // even one character is too wide, it goes out anyway
    while(take > 1 && msg[take - 1] == ' ') take--;   // spaces at the end would shift the centering

    line[take] = '\0';
    if(y_in) u8g2_DrawStr(&u8g2, (width - u8g2_GetStrWidth(&u8g2, line)) / 2, y, line);
    y += 11;

    // the next line starts after the '\n' that ended this one
    msg += take;
    while(*msg == ' ') msg++;
    if(*msg == '\n') msg++;
  }

  return y;
}

// draw a dialog box
static bool dialog_opened_osd = false;
static TimerHandle_t dialog_disappear_timer = NULL;  
static char network_ipaddr[16];
static bool network_was_connected = false;

static bool menu_dialog_is_open(void) {
  if(!dialog_disappear_timer) return false;  // no timer -> no dialog
  return xTimerIsTimerActive(dialog_disappear_timer) != pdFALSE;
}

static void menu_dialog_timeout(__attribute__((unused)) TimerHandle_t arg) {
  menu_debugf("Close dialog");
  if(dialog_opened_osd) osd_enable(OSD_INVISIBLE);    
  else                  menu_do(0);
}

static void menu_dialog_close(void) {
  if(!dialog_disappear_timer) return;
  xTimerStop(dialog_disappear_timer, 0);
  menu_dialog_timeout(NULL);
}
  
static void menu_draw_dialog_for(const char *title, const char *msg,
                                  TickType_t duration) {
  // check if osd is already visible
  if(!menu_dialog_is_open()) {
    dialog_opened_osd = !osd_is_visible();
    if(dialog_opened_osd) osd_enable(OSD_VISIBLE);
  }

  if(!dialog_disappear_timer)
    dialog_disappear_timer = xTimerCreate("Dialog timer", duration, pdFALSE,
					   NULL, menu_dialog_timeout);
  else
    xTimerChangePeriod(dialog_disappear_timer, duration, 0);
  xTimerStart(dialog_disappear_timer, 0);
  
  u8g2_ClearBuffer(&u8g2);

  // MENU_LINE_Y is the height of the title incl line
  int y = (64 - MENU_LINE_Y - menu_wrap_text(0, msg))/2;
  
  u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
  
  int width = u8g2_GetDisplayWidth(&u8g2);
  int swid = u8g2_GetStrWidth(&u8g2, title);
 
  // draw title in bold and seperator line
  u8g2_DrawStr(&u8g2, (width-swid)/2, y+MENU_ENTRY_BASE, title);
  u8g2_DrawHLine(&u8g2, (width-swid)/2, y+MENU_ENTRY_H, swid);

  u8g2_SetFont(&u8g2, font_helvR08_te);

  menu_wrap_text(y+23, msg);
  
  u8g2_SendBuffer(&u8g2);
}

void menu_draw_dialog(const char *title, const char *msg) {
  menu_draw_dialog_for(title, msg, pdMS_TO_TICKS(2000));
}

// The Network dialog: the address, or that there is none, and the WiFi name
// under it, since a name is often longer than what fits beside the address.
static void menu_net_status(void) {
  char t[80];
  const char *ssid = inifile_config_has("wifi", "ssid") ? inifile_config_get_str("wifi", "ssid") : "-";
  if(network_was_connected) snprintf(t, sizeof(t), "IP %s\n%s", network_ipaddr, ssid);
  else                      snprintf(t, sizeof(t), "not connected\n%s", ssid);
  menu_draw_dialog_for("Network", t, pdMS_TO_TICKS(8000));
}

// The Version dialog: what this firmware tells RetroAchievements, split over four
// lines. The version is the git describe of the game20k repository, handed in by its
// build script, so between releases it carries the distance to the last tag. The
// fourth line is the core's board id and what the ROM file is (game20k).
static void menu_version_status(void) {
  char t[128];
  // a build with the fork's example table knows no game, the platform line says so
  snprintf(t, sizeof(t), "game20k v%s\n%s%s\nrcheevos %s\nboard %u, %s", GAME20K_VERSION, GAME20K_PLATFORM,
           strcmp(ra_games_origin, "generated") ? ", example" : "",
           RCHEEVOS_VERSION_STRING, ra_game_board(), ra_game_rom_label());
  menu_draw_dialog_for("Version", t, pdMS_TO_TICKS(8000));
}

/* ================ the achievement list ================
   One line per achievement of the active set, in the set's order: a mark, the
   title and, on the right, the progress while it runs, else the points. The
   mark is a disk when the account has it in hardcore, a ring when only in
   softcore, an exclamation mark while its challenge is on. OK on a line shows
   the details. RetroAchievements asks that progress and challenges show here. */
static const unsigned char icn_hard_bits[]   = { 0x3c,0x7e,0xff,0xff,0xff,0xff,0x7e,0x3c };
static const unsigned char icn_primed_bits[] = { 0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x18 };

/* Unlocked in the mode that counts: hardcore counts in both modes, softcore only in softcore. */
static bool ra_list_done(unsigned id) {
  return ra_state_known(id) || (!ra_task_hardcore() && ra_state_softcore_only(id));
}

static int ra_list_length(void) {
  return (int)ra_patch_count();
}

static void ra_list_draw(void) {
  const int width = u8g2_GetDisplayWidth(&u8g2);
  int dots;

  // progress and challenges change during play: the menu timer redraws the list
  // about once a second while it shows, see menu_do()
  menu_timer_enable(true);
  u8g2_SetFont(&u8g2, font_helvR08_te);
  dots = u8g2_GetStrWidth(&u8g2, "...");
  if(!ra_patch_count()) {
    u8g2_DrawStr(&u8g2, 1, MENU_LINE_Y + MENU_ENTRY_H, "No set loaded");
    return;
  }
  // the four lines from the scroll position on, like the file selector
  for(int row = 0; row < 4; row++) {
    ra_patch_item_t it;
    int i = row + menu_state->scroll;
    if(!ra_patch_item((unsigned)i, &it)) break;
    int y = MENU_LINE_Y + MENU_ENTRY_H * (row + 1);
    bool done = ra_list_done(it.id);

    // right column: the progress while the achievement is open, else its points
    char right[RA_PATCH_PROGRESS_MAX + 4];
    if(it.progress[0] && !done) snprintf(right, sizeof(right), "%s", it.progress);
    else                        snprintf(right, sizeof(right), "%u", it.points);
    int rw = u8g2_GetStrWidth(&u8g2, right);

    // the title between mark and right column, cut with "..." when too long
    char title[RA_PATCH_TITLE_MAX + 3];
    int room = width - FS_ICON_WIDTH - rw - 3;
    snprintf(title, sizeof(title), "%s", it.title);
    if(u8g2_GetStrWidth(&u8g2, title) > room) {
      while(title[0] && u8g2_GetStrWidth(&u8g2, title) > room - dots) title[strlen(title) - 1] = 0;
      strcat(title, "...");
    }

    if(it.primed && !done)             u8g2_DrawXBM(&u8g2, 1, y - 8, 8, 8, icn_primed_bits);
    else if(ra_state_known(it.id))     u8g2_DrawXBM(&u8g2, 1, y - 8, 8, 8, icn_hard_bits);
    else if(ra_state_softcore_only(it.id)) u8g2_DrawXBM(&u8g2, 1, y - 8, 8, 8, icn_off_bits);
    u8g2_DrawStr(&u8g2, FS_ICON_WIDTH, y, title);
    u8g2_DrawStr(&u8g2, width - rw - 1, y, right);

    if(menu_state->selected == i + 1)
      u8g2_DrawButtonFrame(&u8g2, 0, y, U8G2_BTN_INV, width, 1, 1);
  }
}

/* Cuts s so that it plus "..." fits width in the current font, and adds "...".
   s needs room for three more characters. */
static void ra_list_ellipsize(char *s, int width) {
  int dots = u8g2_GetStrWidth(&u8g2, "...");
  if(u8g2_GetStrWidth(&u8g2, s) <= width) return;
  while(s[0] && u8g2_GetStrWidth(&u8g2, s) > width - dots) s[strlen(s) - 1] = 0;
  while(strlen(s) && s[strlen(s) - 1] == ' ') s[strlen(s) - 1] = 0;
  strcat(s, "...");
}

/* Breaks text into at most max lines of the display width at spaces, into out,
   the way menu_wrap_text() breaks a dialog. When text remains, the last line ends
   with "...". Returns the number of lines. */
static int ra_list_wrap(const char *text, char out[][80], int max) {
  const int width = u8g2_GetDisplayWidth(&u8g2);
  int n = 0;
  u8g2_SetFont(&u8g2, font_helvR08_te);
  while(*text && n < max) {
    size_t take = 0, gap = 0, i;
    char *line = out[n];
    while(*text == ' ') text++;
    if(!*text) break;
    for(i = 0; text[i] && i < 80 - 4; i++) {
      line[i] = text[i];
      line[i + 1] = 0;
      if(u8g2_GetStrWidth(&u8g2, line) > width) break;
      if(text[i] == ' ') gap = i;
      take = i + 1;
    }
    if(text[i] && text[i] != ' ' && gap) take = gap;
    if(!take) take = 1;
    line[take] = 0;
    text += take;
    n++;
    while(*text == ' ') text++;
    if(n == max && *text) ra_list_ellipsize(line, width);   // more text than lines
  }
  return n;
}

/* The details of one achievement: its title as the heading, the description in
   up to three lines, then points and where it stands. Four lines fit the display
   under a heading. */
static void ra_list_select(int line) {
  const int width = u8g2_GetDisplayWidth(&u8g2);
  ra_patch_item_t it;
  char head[RA_PATCH_TITLE_MAX + 3], stat[48], desc[3][80], t[4 * 80];
  const char *state;
  int n, i;

  if(!ra_patch_item((unsigned)line, &it)) return;
  // where it stands: done in this mode first, else what is going on
  if(ra_state_known(it.id))                                state = "hardcore";
  else if(ra_state_softcore_only(it.id) && !ra_task_hardcore()) state = "softcore";
  else if(it.primed)                                       state = "challenge on";
  else if(it.progress[0])                                  state = it.progress;
  else if(ra_state_softcore_only(it.id))                   state = "softcore only";
  else                                                     state = "open";

  // the heading in the dialog's bold font, cut to the display
  u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
  snprintf(head, sizeof(head), "%s", it.title);
  ra_list_ellipsize(head, width);

  u8g2_SetFont(&u8g2, font_helvR08_te);
  snprintf(stat, sizeof(stat), "%u points, %s", it.points, state);
  if(u8g2_GetStrWidth(&u8g2, stat) > width) snprintf(stat, sizeof(stat), "%u pts, %s", it.points, state);
  n = ra_list_wrap(it.desc, desc, 3);
  t[0] = 0;
  for(i = 0; i < n; i++) { strcat(t, desc[i]); strcat(t, "\n"); }
  strcat(t, stat);
  menu_draw_dialog_for(head, t, pdMS_TO_TICKS(10000));
}

static const config_custom_t ra_list = {
  .label  = "Achievements",
  .length = ra_list_length,
  .draw   = ra_list_draw,
  .select = ra_list_select
};

/* Opens the list with its first achievement selected. The draw starts the timer
   that keeps it current. */
static void menu_ra_list_open(void) {
  menu_push();
  menu_state->type     = MENU_TYPE_CUSTOM;
  menu_state->custom   = &ra_list;
  menu_state->selected = ra_patch_count() ? 1 : 0;
  menu_state->scroll   = 0;
}

static void menu_pop(void);

/* ================ the Games page ================
   One line per game on the card, by title (games.c). OK on a line starts it: the
   Pico restarts into its file, after a core switch when it runs on another board,
   see restart_step() in main.c. The list is read when the page opens and again
   when the card changed while it shows. */
static int games_list_length(void) {
  return (int)games_count();
}

static void games_list_draw(void) {
  const int width = u8g2_GetDisplayWidth(&u8g2);
  // an upload over FTP may change the card: the menu timer looks for it, see menu_do()
  menu_timer_enable(true);
  u8g2_SetFont(&u8g2, font_helvR08_te);
  if(!games_count()) {
    u8g2_DrawStr(&u8g2, 1, MENU_LINE_Y + MENU_ENTRY_H, "No games on the card");
    return;
  }
  for(int row = 0; row < 4; row++) {
    games_item_t it;
    char title[GAMES_TITLE_MAX + 4];
    int i = row + menu_state->scroll;
    if(!games_item((unsigned)i, &it)) break;
    int y = MENU_LINE_Y + MENU_ENTRY_H * (row + 1);
    snprintf(title, sizeof(title), "%s", it.title);
    ra_list_ellipsize(title, width - 2);
    u8g2_DrawStr(&u8g2, 1, y, title);
    if(menu_state->selected == i + 1)
      u8g2_DrawButtonFrame(&u8g2, 0, y, U8G2_BTN_INV, width, 1, 1);
  }
}

static void games_list_select(int line) {
  games_item_t it;
  games_pick_t pk;
  const char *cwd = sdc_get_cwd(MAX_DRIVES + RA_PATCH_ROM_IMAGE);
  const char *img = sdc_get_image_name(MAX_DRIVES + RA_PATCH_ROM_IMAGE);
  if(!games_item((unsigned)line, &it)) return;
  // the file that runs already, with no restart pending: nothing to start, the menu closes
  if(img && cwd && !strcmp(cwd, CARD_MOUNTPOINT) && games_name_crc(img) == it.crc &&
     !ra_patch_restart_to() && !games_picked(&pk)) {
    menu_timer_enable(false);
    osd_enable(OSD_INVISIBLE);
    return;
  }
  // main.c takes it from here and says so, see MENU_EVENT_RA_RESTART
  games_pick(&it);
  menu_timer_enable(false);
  menu_pop();
}

static const config_custom_t games_list = {
  .label  = "Games",
  .length = games_list_length,
  .draw   = games_list_draw,
  .select = games_list_select
};

/* Reads the card and opens the page with the game that runs selected, else the
   first one. */
static void menu_games_open(void) {
  const char *cwd = sdc_get_cwd(MAX_DRIVES + RA_PATCH_ROM_IMAGE);
  const char *img = sdc_get_image_name(MAX_DRIVES + RA_PATCH_ROM_IMAGE);
  int sel = 0;
  games_read();
  // the running file's line, as games_list_select() tells it; lines count from 1
  if(img && cwd && !strcmp(cwd, CARD_MOUNTPOINT)) {
    uint32_t crc = games_name_crc(img);
    games_item_t it;
    for(unsigned i = 0; !sel && games_item(i, &it); i++)
      if(it.crc == crc) sel = (int)i + 1;
  }
  if(!sel && games_count()) sel = 1;
  // scrolled as stepping down to that line would scroll it, see menu_entry_go()
  int entries = (int)games_count() + 1;
  menu_push();
  menu_state->type     = MENU_TYPE_CUSTOM;
  menu_state->custom   = &games_list;
  menu_state->selected = sel;
  menu_state->scroll   = (entries <= 5 || sel <= 3) ? 0 : (sel < entries - 2) ? sel - 3 : entries - 5;
}

/* The page on screen without a key: no ROM runs. A message that shows closes onto
   the page, not into a hidden OSD. */
static void menu_games_show(void) {
  if(!cfg || !menu_state) return;
  if(!(menu_state->type == MENU_TYPE_CUSTOM && menu_state->custom == &games_list))
    menu_games_open();
  if(!osd_is_visible()) osd_enable(OSD_VISIBLE);
  dialog_opened_osd = false;
  menu_do(0);
}

// The Account dialog: the account and where it stands, the count, and the
// most urgent note. Four lines fit the display.
static void menu_ra_status(void) {
  char t[100], who[24], count[32], note[24];
  const char *user = inifile_config_has("ra", "user") ? inifile_config_get_str("ra", "user") : "-";
  const char *state;
  const int width = u8g2_GetDisplayWidth(&u8g2);
  unsigned n, i;

  // the name on one line: a wide one is cut with "..", so the note below stays
  // on the display
  snprintf(who, sizeof(who), "%s", user);
  u8g2_SetFont(&u8g2, font_helvR08_te);
  for(size_t len = strlen(who); len > 2 && u8g2_GetStrWidth(&u8g2, who) > width; len--) {
    who[len - 1] = '\0';
    who[len - 2] = '.';
    who[len - 3] = '.';
  }

  switch(ra_task_state()) {
  case RA_TASK_LOGGED_IN:  state = "logged in";      break;
  case RA_TASK_REJECTED:   state = "login rejected"; break;
  case RA_TASK_NO_ACCOUNT: state = "no account";     break;
  case RA_TASK_NO_TIME:    state = "no time server"; break;
  case RA_TASK_RETRYING:   state = "offline";        break;
  case RA_TASK_NO_GAME:    state = "no game";        break;
  default:                 state = "connecting";     break;
  }
  // the mode in front, it decides how the unlocks count. Softcore although the
  // menu asks for hardcore names the reason instead of the login state. A wrong
  // game comes before an unknown ROM, it also explains one (game20k).
  char line[40];
  unsigned why = ra_task_hardcore_blocked();
  // SET without a login: the login state says more than "till online"
  if((why & ~RA_HC_BLOCK_SET) == 0 && (ra_task_state() == RA_TASK_REJECTED || ra_task_state() == RA_TASK_NO_ACCOUNT))
    why = 0;
  if(!ra_task_hardcore() && ra_task_hardcore_wanted() && why)
    snprintf(line, sizeof(line), "softcore: %s",
             (why & RA_HC_BLOCK_CORE) ? "test core" : (why & RA_HC_BLOCK_XML) ? "config.xml" :
             (why & RA_HC_BLOCK_KEY)  ? "key error" : (why & RA_HC_BLOCK_SIZE) ? "set too big" :
             (why & RA_HC_BLOCK_GAME) ? "wrong game" :
             (why & RA_HC_BLOCK_ROM)  ? "ROM unknown" : "till online");
  else
    snprintf(line, sizeof(line), "%s, %s", ra_task_hardcore() ? "hardcore" : "softcore", state);
  // the counts refer to the active set, without one there is nothing to count
  if(!ra_patch_count())
    snprintf(count, sizeof(count), "no set loaded");
  else if(ra_state_softcore_count())
    snprintf(count, sizeof(count), "%u of %u, %u softcore", ra_state_count(), ra_patch_count(), ra_state_softcore_count());
  else
    snprintf(count, sizeof(count), "%u of %u unlocked", ra_state_count(), ra_patch_count());

  // the note: a DIP switch the set does not expect comes first, its conditions
  // would never fire, then what still waits for the server, then in hardcore
  // the server's warning, e.g. "Unknown Emulator": until RetroAchievements
  // approves this client, it keeps the unlocks as casual. A switch this core
  // does not have cannot be wrong.
  const ra_dip_t *dip = ra_game_dips(&n);
  for(i = 0; dip && i < n; i++)
    if(menu_variable_exists(dip[i].id) && menu_variable_get(dip[i].id) != dip[i].value) break;
  if(dip && i < n)            snprintf(note, sizeof(note), "DIP not default!");
  else if(ra_queue_pending()) snprintf(note, sizeof(note), "%u to send", ra_queue_pending());
  else if(!ra_task_hardcore() || !ra_patch_warning(note, sizeof(note))) note[0] = 0;

  snprintf(t, sizeof(t), "%s\n%s\n%s\n%s", who, line, count, note);
  // the game's title as the heading, the four lines are taken (game20k).
  // "Account" while no table game is known.
  menu_draw_dialog_for(ra_game_title() ? ra_game_title() : "Account", t, pdMS_TO_TICKS(8000));
}

static void menu_draw(const config_menu_t *menu, int selected, int scroll) {
  u8g2_ClearBuffer(&u8g2);
 
  // =============== draw a regular menu =================
  menu_debugf("drawing '%s'", menu->label);  
    
  // draw the title. game20k: the root menu is the core's, one per board, so it
  // names the board's first game; the game the ROM was found to be names it
  // instead, e.g. Puck Man on the Pac-Man board
  const char *title = menu->label;
  if(menu_is_root(menu) && ra_game_title()) title = ra_game_title();
  menu_draw_title(title, !menu_is_root(menu), selected == 0);

  config_menu_entry_t *entry = menu->entries;
  for(int i=0;i<scroll;i++) entry=entry->next;  // skip first "scroll" entries
  for(int i=0;i<4 && entry;i++,entry=entry->next)           // then draw up to four entries
    menu_draw_entry(entry, i, selected == scroll+i+1);    
  
  u8g2_SendBuffer(&u8g2);
}

static void menu_fsel_draw(const char *label, sdc_dir_entry_t *dir, int selected, int scroll) {
  u8g2_ClearBuffer(&u8g2);

  // =============== draw a fileselector =================    
  menu_debugf("drawing '%s'", label);
  
  menu_draw_title(label, true, selected == 0);
  menu_timer_enable(false);
  fs_scroll_cur = -1;

  // draw up to four entries
  for(int i=0;i<4 && i<dir_len(dir)-scroll;i++) {            
    menu_debugf("file %s", dir_entry(dir, i+scroll)->name);
    menu_fs_draw_entry(i, dir_entry(dir, i+scroll));
  }
  u8g2_SendBuffer(&u8g2);
}

static void menu_custom_draw(config_custom_t *custom, int selected, int scroll) {
  u8g2_ClearBuffer(&u8g2);

  // =============== draw a custom dialog =================    
  menu_debugf("drawing custom '%s' (%d)", custom->label, selected);

  // title is a regular entry
  menu_draw_title(custom->label, true, selected == 0);

  if(custom->draw)
    custom->draw();
  
  u8g2_SendBuffer(&u8g2);
}

void menu_goto(const config_menu_t *menu) {
  menu_push();
  
  // prepare menu state ...
  menu_state->menu = menu;
  menu_state->selected = 1;
  menu_state->scroll = 0;
  menu_state->type = MENU_TYPE_MENU;
}

static void menu_file_selector_open(config_menu_entry_t *entry) {
  menu_debugf("menu_file_selector_open()");

  // the file selector can either be opened from disk image file
  // selectors or from rom image selectors  
  if(entry->type == CONFIG_MENU_ENTRY_IMAGE) {
    menu_debugf("opening rom image selector");
    fsel_state.ext = entry->image->ext;
    fsel_state.index = entry->image->index + MAX_DRIVES;  // the images are stored after the drives
    fsel_state.none_str = entry->image->none_str;
    fsel_state.none_icn = entry->image->none_icn;
    fsel_state.action = entry->image->action;
  } else {
    menu_debugf("opening drive image selector");
    fsel_state.ext = entry->fsel->ext;
    fsel_state.index = entry->fsel->index;
    fsel_state.none_str = NULL;
    fsel_state.none_icn = NULL;
    fsel_state.action = entry->fsel->action;
  }
    
  // The file selector usually works on the sd card as that is what
  // the cores are reading data from. But cores may be loaded from
  // USB as well. The file selectors default path will begin with
  // "/usb" in that case

  // Initialize current working directory if needed
  if(!sdc_get_cwd(fsel_state.index)) {
    bool is_usb = strncasecmp(entry->fsel->def, "/usb", 4) == 0;
    
    // check if USB is to be used but isn't present
    if(is_usb && (disk_status(1) != RES_OK)) {
      menu_draw_dialog("USB Error", "No USB mass storage device connected!");
      return;
    }

    sdc_set_cwd(fsel_state.index, is_usb?"/usb":"/sd");
  }

  // IMAGE and file selectors are both drawn by the fileselector routines
  menu_push();
  menu_state->label = (entry->type == CONFIG_MENU_ENTRY_IMAGE)?entry->image->label:entry->fsel->label;
  menu_state->selected = 1;
  menu_state->scroll = 0;
  menu_state->type = MENU_TYPE_FILESELECTOR;
  
  // scan file system
  menu_state->dir = sdc_readdir(fsel_state.index, NULL, (void*)fsel_state.ext);
  // try to jump to current file. Get the current image name and path
  char *name = sdc_get_image_name(fsel_state.index);
  if(name) {
    debugf("trying to jump to %s", name);
    // try to find name in file list
    for(int i=0;i<dir_len(menu_state->dir);i++) {
      if(strcmp(dir_entry(menu_state->dir, i)->name, name) == 0) {
	debugf("found preset entry %d", i);
	
	// file found, adjust entry and offset
	menu_state->selected = i+1;
	
	if(dir_len(menu_state->dir) > 4 && menu_state->selected > 3) {
	  debugf("more than 4 files and selected is > 3");
	  if(menu_state->selected < dir_len(menu_state->dir)-1) menu_state->scroll = menu_state->selected - 3;
	  else                                                  menu_state->scroll = dir_len(menu_state->dir)-4;
	}
      }
    }
  }  
}

// all other entries in step down
static void menu_pop(void) {
  menu_debugf("menu_pop()");

  // "pop"ing the root menu means to hide it. This will only
  // happen in single button mode with the roots title
  if(!menu_state->prev) {
    osd_enable(OSD_INVISIBLE);
    return;
  }

  // neither should this as we never really close the
  // root menu
  if(menu_state->menu == cfg->menu) {
    vPortFree(menu_state);
    menu_state = NULL;
    return;
  }

  // de-chain first entry
  menu_state_t *m = menu_state;
  menu_state = menu_state->prev;
  vPortFree(m);
}

static void menu_fileselector_select(sdc_dir_entry_t *entry) {
  int drive = fsel_state.index;
  menu_debugf("%s %d: file selected '%s'", drive>=MAX_DRIVES?"IMG":"DRV",
	      drive>=MAX_DRIVES?drive-MAX_DRIVES:drive, entry->name);
    
  // stop any scroll timer that might be running
  menu_timer_enable(false);
    
  if(entry->is_dir) {
    if(entry->name[0] == '/') {
      // User selected the "No Disk" entry
      // return to parent form
      menu_pop();
      // Eject
      sdc_image_open(drive, NULL);
    } else {	
      // check if we are going up one dir and try to select the
      // directory we are coming from
      char *prev = NULL; 
      if(strcmp(entry->name, "..") == 0) {
	prev = strrchr(sdc_get_cwd(drive), '/');
	if(prev) prev++;
      }

      menu_state->selected = 1;   // start by highlighting '..'
      menu_state->scroll = 0;
      menu_state->dir = sdc_readdir(drive, entry->name, (void*)fsel_state.ext);	
      
      // prev is still valid, since sdc_readdir doesn't free the old string when going
      // up one directory. Instead it just terminates it in the middle	
      if(prev) {
	menu_debugf("up to %s", prev);
	
	// try to find previous dir entry in current dir	  
	for(int i=0;i<dir_len(menu_state->dir);i++) {
	  if(dir_entry(menu_state->dir, i)->is_dir && strcmp(dir_entry(menu_state->dir, i)->name, prev) == 0) {
	    // file found, adjust entry and offset
	    menu_state->selected = i+1;

	    if(dir_len(menu_state->dir) > 4 && menu_state->selected > 3) {
	      if(menu_state->selected < dir_len(menu_state->dir) - 1) menu_state->scroll = menu_state->selected - 3;
	      else                                                    menu_state->scroll = menu_state->selected - 5;
	    }
	  }
	}
      }
    }
  } else {
    // game20k: the ROM file of a known game of another board, e.g. galaga.rom in
    // the Pac-Man core, needs that board's core. Nothing streams to this one:
    // main.c switches the core and starts the game there, see restart_step()
    if(drive == MAX_DRIVES + RA_PATCH_ROM_IMAGE) {
      // a file without a footer is an old one, it neither switches nor streams
      games_footer_t ft;
      char path[strlen(sdc_get_cwd(drive)) + strlen(entry->name) + 2];
      sprintf(path, "%s/%s", sdc_get_cwd(drive), entry->name);
      if(!games_footer_of(path, &ft)) {
        debugf("IMG %d: %s has no valid footer, an old ROM file", drive - MAX_DRIVES, path);
        menu_pop();
        menu_notify(MENU_EVENT_ROM_OLD);
        return;
      }
    }
    if(drive == MAX_DRIVES + RA_PATCH_ROM_IMAGE && ra_patch_pick_other_board(entry->name)) {
      menu_pop();
      return;
    }

    // request insertion of this image
    int r = sdc_image_open(drive, entry->name);

    // return to parent form
    menu_pop();

    // game20k: a ROM file the core rejected never loads, the old game goes on. Say
    // so, else the player waits for a game that does not come
    if(r == SDC_IMAGE_REJECTED)
      menu_draw_dialog_for("ROM rejected", "Not a ROM file for this core:\nwrong size. The game goes on.",
                           pdMS_TO_TICKS(5000));

    // check if this is a drive image selection and run action if yes. Image selectors
    // work differently and do an IRQ driven transfer in the background. The action is there executed
    // ocne the transfer itself is done.
    if(drive<MAX_DRIVES && fsel_state.action)
      sys_run_action(fsel_state.action);
  }
}

void menu_run_current_image_action(void) {
  // Note: The download may also have been triggered when loading config from
  // ini file. We may need to find the right action in that case as well.
  // The same is true for mounted disk images
  
  if(!menu_state) return;

  // the current menu state will be a open file selector if the image transfer
  // has been started through the OSD by the user
  if(menu_state->type != MENU_TYPE_FILESELECTOR) {
    menu_debugf("finished download was ini file triggered during boot");

    if(!sdc_check_for_pending_image_uploads()) {
      menu_debugf("no more image uploads pending");

      sys_run_action_by_name("ready");
    }
      
    return;
  }

  // fsel index should be >= MAX_DRIVES and < MAX_DRIVES+MAX_IMAGES as it
  // should point to an image selector
  if(fsel_state.index < MAX_DRIVES || fsel_state.index >= MAX_DRIVES+MAX_IMAGES) return;

  // run the file selector action if one is present
  if(fsel_state.action)
    sys_run_action(fsel_state.action);

}

// user has pressed esc to go back one level
static void menu_back(void) {
  // when in range edit mode, back leaves edit mode
  config_menu_entry_t *entry = menu_get_selected_entry();
  if(entry && entry->type == CONFIG_MENU_ENTRY_RANGE && entry->range->edit) {
    entry->range->edit = false;
    return;
  }							  

  // stop doing the scroll timer
  menu_timer_enable(false);

  // are we in the root menu?
  if(menu_state->menu == cfg->menu)
    osd_enable(OSD_INVISIBLE);
  else {
    // are we in fileselector?
    if(menu_state->type == MENU_TYPE_FILESELECTOR) {
      // search for ".." in current dir
      sdc_dir_entry_t *entry = NULL;
      for(int i=0;i<dir_len(menu_state->dir);i++)
	if(!strcmp(dir_entry(menu_state->dir, i)->name, ".."))
	  entry = dir_entry(menu_state->dir, i);
      
      // if there was one, go up. Else quit the file selector
      if(entry) menu_fileselector_select(entry);
      else      menu_pop();
    } else
      menu_pop();
  }
}

// user has selected a menu entry
static void menu_select(void) {
  // when in range edit mode, select leaves edit mode
  config_menu_entry_t *entry = menu_get_selected_entry();
  if(entry && entry->type == CONFIG_MENU_ENTRY_RANGE && entry->range->edit) {
    entry->range->edit = false;
    return;
  }							  

  // if the title was selected, then goto parent form
  if(menu_state->selected == 0) {
    menu_pop();
    return;
  }

  // in fileselector
  if(menu_state->type == MENU_TYPE_FILESELECTOR) {
    menu_fileselector_select(dir_entry(menu_state->dir, menu_state->selected-1));
    return;
  }

  // a line of a custom page is no menu entry: the page selects it itself, if at all
  if(menu_state->type == MENU_TYPE_CUSTOM) {
    if(menu_state->custom->select) menu_state->custom->select(menu_state->selected - 1);
    return;
  }
  if(!entry) return;
  
  menu_debugf("Selected: %s '%s'", config_menuentry_get_type_str(entry), menuentry_get_label(entry));

  switch(entry->type) {
  case CONFIG_MENU_ENTRY_FILESELECTOR:
    // user has choosen a file selector
    menu_file_selector_open(entry);
    break;
    
  case CONFIG_MENU_ENTRY_MENU:
    menu_goto(entry->menu);
    break;

  case CONFIG_MENU_ENTRY_LIST: {
    // user has choosen a selection list
    int value = menu_variable_get(entry->list->id) + 1;
    int list_length = menu_get_list_length(entry);
    if(value >= list_length) value = 0;    
    menu_variable_set(entry->list->id, value);

    // check if there's an action connected to changing this
    // list. This e.g. happens when changing system settings is
    // meant to trigger a (cold) boot
    if(entry->list->action)
      sys_run_action(entry->list->action);

  } break;

  case CONFIG_MENU_ENTRY_BUTTON:
    // buttons the companion answers itself, the action is told by its name.
    // The rest goes to the core.
    if(entry->button->action && entry->button->action->name &&
       !strcmp(entry->button->action->name, "netinfo"))
      menu_net_status();
    else if(entry->button->action && entry->button->action->name &&
            !strcmp(entry->button->action->name, "rainfo"))
      menu_ra_status();
    else if(entry->button->action && entry->button->action->name &&
            !strcmp(entry->button->action->name, "verinfo"))
      menu_version_status();
    else if(entry->button->action && entry->button->action->name &&
            !strcmp(entry->button->action->name, "ralist"))
      menu_ra_list_open();
    else if(entry->button->action && entry->button->action->name &&
            !strcmp(entry->button->action->name, "games"))
      menu_games_open();
    else if(entry->button->action)
      sys_run_action(entry->button->action);
    break;
	
  case CONFIG_MENU_ENTRY_IMAGE:
    // user has choosen an image selector
    menu_file_selector_open(entry);
    break;
    
  case CONFIG_MENU_ENTRY_TOGGLE:
    menu_variable_set(entry->toggle->id, !menu_variable_get(entry->toggle->id));
    if(entry->toggle->action)
      sys_run_action(entry->toggle->action);
    break;

  case CONFIG_MENU_ENTRY_RANGE:
    // selecting a range entry enables changing its value
    entry->range->edit = true;
    break;
    
  default:
    menu_debugf("unknown %s", config_menuentry_get_type_str(entry));    
  }
}

// timer implementing key repeat
static TimerHandle_t menu_key_repeat_timer = NULL;  
static int menu_key_last_event = -1;

static void menu_key_repeat(__attribute__((unused)) TimerHandle_t arg) { 
  if(menu_key_last_event >= 0) {
  
    if(menu_key_last_event == MENU_EVENT_UP)     menu_entry_go(-1);
    if(menu_key_last_event == MENU_EVENT_DOWN)   menu_entry_go( 1);

    if(menu_key_last_event == MENU_EVENT_PGUP)   menu_entry_go(-4);
    if(menu_key_last_event == MENU_EVENT_PGDOWN) menu_entry_go( 4);

    if(menu_state->type == MENU_TYPE_MENU)
      menu_draw(menu_state->menu, menu_state->selected, menu_state->scroll);
    else if(menu_state->type == MENU_TYPE_FILESELECTOR)
      menu_fsel_draw(menu_state->label, menu_state->dir, menu_state->selected, menu_state->scroll);
    else if(menu_state->type == MENU_TYPE_CUSTOM)
      menu_custom_draw(menu_state->custom, menu_state->selected, menu_state->scroll);
  
    xTimerChangePeriod( menu_key_repeat_timer, pdMS_TO_TICKS(100), 0);
    xTimerStart( menu_key_repeat_timer, 0 );
  }
}
  
void menu_stop_repeat(void) {
  xTimerStop(menu_key_repeat_timer, 0);
  menu_key_last_event = -1;
}

void menu_do(int event) {
  //
  if(menu_dialog_is_open()) {
    // if the dialog is open, then any key event will close it
    if((event >= MENU_EVENT_UP) && (event <= MENU_EVENT_BACK))
      menu_dialog_close();

    return;
  }

  // -1 is a timer event used to scroll the current file name if it's to long
  // for the OSD
  if(event < 0) {
    if(cfg) {
      if(menu_state->type == MENU_TYPE_FILESELECTOR)
	menu_fs_scroll_entry();
      else if(menu_state->type == MENU_TYPE_CUSTOM && menu_state->custom == &ra_list) {
	// the achievement list shows live progress: redraw it every 25 ticks, 1 s
	static int ticks;
	if(++ticks >= 25) {
	  ticks = 0;
	  menu_custom_draw((config_custom_t *)menu_state->custom, menu_state->selected, menu_state->scroll);
	}
      } else if(menu_state->type == MENU_TYPE_CUSTOM && menu_state->custom == &games_list) {
	// the card changed while the page shows: read it again, the selection stays in range
	if(games_stale() && !menu_dialog_is_open()) {
	  games_read();
	  int n = (int)games_count();
	  if(menu_state->selected > n) menu_state->selected = n;
	  if(menu_state->scroll > (n > 4 ? n - 4 : 0)) menu_state->scroll = n > 4 ? n - 4 : 0;
	  menu_custom_draw((config_custom_t *)menu_state->custom, menu_state->selected, menu_state->scroll);
	}
      } else
	menu_timer_enable(false);   // nothing here animates, the timer rests
    }
      
    return;
  }
  
  menu_debugf("do %d", event);
  
  if(event)  {
    if(event == MENU_EVENT_TOGGLE) {
      if(!osd_is_visible())
	osd_enable(OSD_VISIBLE);
      else {
	menu_timer_enable(false);
	osd_enable(OSD_INVISIBLE);
	return;  // return now to prevent OSD from being drawn, again
      }
    }

    // a key release event just stops any repeat timer
    if(event == MENU_EVENT_KEY_RELEASE) {
      menu_stop_repeat();
      return;
    }

    // UP/DOWN PGUP and PGDOWN have a repeat
    if(event == MENU_EVENT_UP || event == MENU_EVENT_DOWN ||
       event == MENU_EVENT_PGUP || event == MENU_EVENT_PGDOWN) {
      
      if(menu_key_repeat_timer) {
	menu_key_last_event = event;
	xTimerChangePeriod( menu_key_repeat_timer, pdMS_TO_TICKS(500), 0);
	xTimerStart( menu_key_repeat_timer, 0 );
      }
    }
    
    if(event == MENU_EVENT_UP)     menu_entry_go(-1);
    if(event == MENU_EVENT_DOWN)   menu_entry_go( 1);

    if(event == MENU_EVENT_PGUP)   menu_entry_go(-4);
    if(event == MENU_EVENT_PGDOWN) menu_entry_go( 4);

    if(event == MENU_EVENT_SELECT) menu_select();
    if(event == MENU_EVENT_BACK)   menu_back();
  }

  // if no dialog is open, then draw menu/fsel
  if(!menu_dialog_is_open()) {  
    if(menu_state->type == MENU_TYPE_MENU)
      menu_draw(menu_state->menu, menu_state->selected, menu_state->scroll);
    else if(menu_state->type == MENU_TYPE_FILESELECTOR)
      menu_fsel_draw(menu_state->label, menu_state->dir, menu_state->selected, menu_state->scroll);  
    else if(menu_state->type == MENU_TYPE_CUSTOM)
      menu_custom_draw(menu_state->custom, menu_state->selected, menu_state->scroll);
  }
}

TimerHandle_t menu_timer_handle;
// queue to forward key press events from USB to MENU
QueueHandle_t menu_queue = NULL;

void menu_timer_enable(bool on) {
  if(on) xTimerStart(menu_timer_handle, 0);
  else   xTimerStop(menu_timer_handle, 0);
}

// a 25Hz timer that can be activated by the menu whenever animations
// are displayed and which should be updated constantly
static void menu_timer(__attribute__((unused)) TimerHandle_t pxTimer) {
  static long msg = -1;
  xQueueSendToBack(menu_queue, &msg,  ( TickType_t ) 0);
}

static const config_menu_t system_menu_main;

// check if menu is the system menu or a submenu of it
static bool menu_is_systemmenu(void) {
  menu_state_t *ms = menu_state;
  
  while(ms) {
    if((ms->type == CONFIG_MENU_ENTRY_MENU) &&
       (ms->menu == &system_menu_main))
      return true;

    ms = ms->prev;
  }
  return false;
}  

static void menu_handle_latin1(uint8_t code) {
  // this function receives lagtin1 encoded keyboard events and
  // is meant to be used for complex text input in custom dialogs  
  if(code > 31) menu_debugf("menu_handle_latin1(%d/%c)", code, code);
  else          menu_debugf("menu_handle_latin1(%d)", code);
}

static void menu_task(__attribute__((unused)) void *parms) {
  menu_debugf("task running");

  // wait for user events
  while(1) {
    // receive events from usb    
    long cmd;
    xQueueReceive(menu_queue, &cmd, 0xffffffffUL);
    menu_debugf("command %04lx", cmd);

    if(cmd == MENU_EVENT_SYSTEM) {
      menu_debugf("system menu requested");
      menu_dialog_close();
      
      // open osd if it's not open, yet
      if(!osd_is_visible()) osd_enable(OSD_VISIBLE);

      // check if we are already in system menu
      if(!menu_is_systemmenu()) {
	menu_goto(&system_menu_main);
	menu_do(0);   // (re)draw menu
      } else
	menu_debugf("Already in system menu");
    } else

    // catch some non-user controlled events here

#ifdef ENABLE_BLUETOOTH
    if(cmd == MENU_EVENT_BLUETOOTH_CONNECTED) {      
      menu_draw_dialog("Bluetooth", "A device has been connected!");
    } else if(cmd == MENU_EVENT_BLUETOOTH_DISCONNECTED) {
      menu_draw_dialog("Bluetooth", "A device has been disconnected!");
    } else if(cmd == MENU_EVENT_BLUETOOTH_SCAN) {
      menu_draw_dialog("Bluetooth", "Searching for 10 seconds for devices ...");
    } else if(cmd == MENU_EVENT_BLUETOOTH_PIN_CODE_REQUEST) {
      menu_draw_dialog("Bluetooth", "Please enter pin '123456'");
    } else
#endif

    if(cmd == MENU_EVENT_RA_RESTART) {
      // game20k: the ROM picked is another game of this board, main.c restarts
      // the Pico into it for its achievements in a few seconds. A game of another
      // board loads its core first. A game picked on the Games page goes first
      const ra_game_t *g = ra_patch_restart_to();
      games_pick_t pk;
      char message[80];
      if(games_picked(&pk)) {
        snprintf(message, sizeof(message), "%s: %s", pk.title,
                 pk.board != ra_game_board() ? "loading its core" : "starting");
        menu_draw_dialog_for("Games", message, pdMS_TO_TICKS(4000));
      } else if(g && g->board != ra_game_board()) {
        snprintf(message, sizeof(message), "%s: loading its core", g->title);
        menu_draw_dialog_for("Core switch", message, pdMS_TO_TICKS(4000));
      } else {
        snprintf(message, sizeof(message), "%s: restart for its achievements", g ? g->title : "Game");
        menu_draw_dialog_for("RetroAchievements", message, pdMS_TO_TICKS(4000));
      }
    } else
    if(cmd == MENU_EVENT_ROM_OLD) {
      // game20k: a ROM file from before the footer, nothing went to the core
      menu_draw_dialog_for("Old ROM file", "Rebuild the card\nwith make_sdcard.sh", pdMS_TO_TICKS(6000));
    } else
    if(cmd == MENU_EVENT_ROM_DAMAGED) {
      // game20k: the core stays in reset, the player picks another game
      menu_draw_dialog_for("ROM file damaged", "Copy the file again.\nThe game does not start.", pdMS_TO_TICKS(6000));
      menu_games_show();
    } else
    if(cmd == MENU_EVENT_GAMES) {
      // game20k: the core's ROM file is missing, a game is picked instead
      menu_games_show();
    } else
    if(cmd == MENU_EVENT_CORE_SWITCH_FAILED) {
      // game20k: the running bitstream was built before the core switch
      menu_draw_dialog_for("Core switch", "This bitstream cannot load\nanother core. The game goes on.",
                           pdMS_TO_TICKS(5000));
    } else
    if(cmd == MENU_EVENT_NETWORK_GOT_IP || cmd == MENU_EVENT_NETWORK_DISCONNECTED) {
      // game20k: no popup. Address and WiFi name are under Status, and the popup
      // came after every restart of the Pico, a game switch among them
    } else
  if(cmd == MENU_EVENT_USB_MOUNTED) {
      menu_debugf("USB mount event");

      vTaskDelay(100);

      static FATFS usb_fs;
      FRESULT fres;
      if ( (fres = f_mount(&usb_fs, "/usb", 1)) != FR_OK ) menu_debugf("/usb mount failed: %d", fres);
      else {
	menu_debugf("/usb mounted");
	menu_draw_dialog("USB", "A USB mass storage device has been detected!");
      }
    } else if(cmd == MENU_EVENT_USB_UMOUNTED) {
      if (f_unmount("/usb") != FR_OK )	menu_debugf("/usb unmount failed");
      else	                        menu_debugf("/usb unmounted");
      menu_draw_dialog("USB", "A USB mass storage device has been removed!");
    }

    else if((cmd & 0xffffff00) == MENU_EVENT_KEY_LATIN1)
      menu_handle_latin1(cmd & 0xff);

    else
      menu_do(cmd);
  }
}

TaskHandle_t menu_handle = NULL;

void menu_init(void) {
  menu_debugf("Initializing");

  // check if a config was loaded. If no, use the legacy menu
  if(!cfg) {  
    menu_debugf("Warning: No core config found. Is this an old legacy core?");
    return;
  }

  // a config was loaded, use that
  menu_debugf("Using configured menu");

  menu_debugf("Setting up variables");
  menu_setup_variables();
    
  menu_debugf("Processing init action");
  sys_run_action_by_name("init");

  menu_goto(cfg->menu);    

  // create a one shot timer for key repeat
  menu_key_repeat_timer = xTimerCreate( "Key repeat timer", pdMS_TO_TICKS(500), pdFALSE,
					NULL, menu_key_repeat);
    
  // switch MCU controlled leds off
  sys_set_leds(0x00);
  
  // create a 25 Hz timer that frequently wakes the OSD thread
  // allowing for animations
  menu_timer_handle = xTimerCreate("Menu scroll timer", pdMS_TO_TICKS(40), pdTRUE,
				   NULL, menu_timer);
  
  // message queue from USB to OSD
  menu_queue = xQueueCreate(10, sizeof( long ) );
  
  // start a thread for the on screen display. game20k: 6 KB of stack, about three times
  // what the menu used on the Pico with every page open
  xTaskCreate(menu_task, (char *)"menu_task", 1536, NULL, configMAX_PRIORITIES-3, &menu_handle);

  // At this point, the USB may already be ready. But since the
  // menu task wasn't ready by now, it never had a chance to be mounted properly
  if(mcu_hw_usb_msc_present()) {
    menu_debugf("triggering delayed USB init");
    menu_notify(MENU_EVENT_USB_MOUNTED);
  }
}

// queue an event for the menu task
void menu_notify(unsigned long msg) {
  if(menu_queue) 
    xQueueSendToBackFromISR(menu_queue, &msg,  ( TickType_t ) 0);
  else
    menu_debugf("menu_notify(): queue/menu task not ready!");
}

void menu_notify_ip(const char *ipaddr) {
  snprintf(network_ipaddr, sizeof(network_ipaddr), "%s", ipaddr);
  network_was_connected = true;
  menu_notify(MENU_EVENT_NETWORK_GOT_IP);
}

void menu_notify_network_disconnected(void) {
  // only report a disconnect if we had reported an ip address before
  if(!network_was_connected) return;
  network_was_connected = false;
  menu_notify(MENU_EVENT_NETWORK_DISCONNECTED);
}

void menu_joystick_state(unsigned char state) {
  static unsigned char prev_state = 0;
  static TickType_t last_event = 0;   // when the last event was sent

  if(state != prev_state) {
    // Use the newly pressed bits, not state: holding up and touching right
    // (08 -> 09) must not send up again.
    unsigned char pressed = state & ~prev_state;
    unsigned long msg = 0;
    TickType_t now = xTaskGetTickCount();
    menu_debugf("Joystick state change to %02x (new %02x)", state, pressed);

    if(pressed & 0x08) msg = MENU_EVENT_UP;
    if(pressed & 0x04) msg = MENU_EVENT_DOWN;
    if(pressed & 0x02) msg = MENU_EVENT_BACK;
    if(pressed & 0x10) msg = MENU_EVENT_SELECT;
    if(pressed & 0x20) msg = MENU_EVENT_BACK;

    // at most one event per 150 ms against a bouncing stick
    if(msg && (now - last_event) < pdMS_TO_TICKS(150)) msg = 0;
    if(msg) last_event = now;

    // all event bits released: stop the key repeat
    if(!msg && !(state & 0x3e)) msg = MENU_EVENT_KEY_RELEASE;  // 0x3e = 0x02|0x04|0x08|0x10|0x20, the bits used above

    if(msg) menu_notify(msg);
    prev_state = state;
  }
}

// ===========  "single button" menu control ================

// The menu is only controlled through the single OSD hw button
// whenever no keyboard or gamepad is connected.

// three events are supported:
// <200ms: next menu entry
// >200ms and < 3s: select menu entry
// >3s: up one menu level

static TickType_t menu_button_last_event = 0;
static TimerHandle_t menu_button_timer = NULL;

static void menu_button_timer_handler(__attribute__((unused)) TimerHandle_t arg) {
  menu_debugf("button timer event");

  menu_notify(MENU_EVENT_BACK);
  menu_notify(MENU_EVENT_KEY_RELEASE);
  menu_button_last_event = 0;
}

void menu_button_state(unsigned char state) {
  menu_debugf("Button state %02x", state);

  if(mcu_hw_hid_present()) {
    if(state & 0x01) {
      // with keyboard and/or gamepad detected implement the same behaviour as
      // previous versions had
      menu_notify(MENU_EVENT_TOGGLE);
    }
  } else {
    static unsigned char prev_state = 0;

    if(state != prev_state) {
      menu_debugf("Button state change to %02x", state);

      // button has been pressed?
      if(state & 1) {
	// show the osd if it's closed
	if(!osd_is_visible())
	  menu_notify(MENU_EVENT_TOGGLE);
	else {	
	  menu_button_last_event = xTaskGetTickCount();
	  
	  if(!menu_button_timer)
	    menu_button_timer = xTimerCreate( "Button timer", pdMS_TO_TICKS(3000), pdFALSE,
					      NULL, menu_button_timer_handler);
	  
	  xTimerStart(menu_button_timer, 0);
	}
      } else if(menu_button_last_event) {
	xTimerStop(menu_button_timer, 0);

	// button action depends on press duration
	TickType_t len = xTaskGetTickCount() - menu_button_last_event;
	if(len > pdMS_TO_TICKS(200)) {
	  menu_notify(MENU_EVENT_SELECT);
	  menu_notify(MENU_EVENT_KEY_RELEASE);
	} else {
	  menu_notify(MENU_EVENT_DOWN);
	  menu_notify(MENU_EVENT_KEY_RELEASE);
	}
	menu_button_last_event = 0;
      }
	
      prev_state = state;
    }
  }
}

/* ================= system menu =================== */

/*  For simplicity and flexibility, the system menu is
    not strictly bound to use the menu structures. Instead
    it can use programmatically designed dialogs.

    Its about box is an example for this.
*/

static int about_length(void) {
  // return the number of selectable items inside the about dialog.
  return 0;
}

static void about_draw(void) {
  u8g2_DrawStr(&u8g2, 20, MENU_LINE_Y + 1*MENU_ENTRY_H, "FPGA Companion");
  u8g2_DrawStr(&u8g2, 5, MENU_LINE_Y + 2*MENU_ENTRY_H, "(c) 2026 the MiSTle Team");
  u8g2_DrawStr(&u8g2, 0, MENU_LINE_Y + 3*MENU_ENTRY_H, "http://github.com/mistle-dev");
}

const static config_custom_t about = {
  .label = "About",
  .length = about_length,
  .draw = about_draw
};

static void about_func(void) {
  menu_debugf("About");

  // push a custom menu
  menu_push();
  menu_state->type = MENU_TYPE_CUSTOM;
  menu_state->custom = &about;
  menu_state->selected = 0;
  menu_state->scroll = 0;
}

static const config_action_command_t about_exec = {
  .code = CONFIG_ACTION_COMMAND_EXEC,
  .exec = about_func
};

static const config_action_t about_action = {
  .name = "about",
  .commands = (config_action_command_t*)&about_exec
};

static const config_button_t about_btn = {
  .label = "About",
  .action = (config_action_t*)&about_action
};


// the system menu is hard coded as it doesn't need to
// be modified by the running core

#ifdef ENABLE_BLUETOOTH

void btscan(void) {
  menu_notify(MENU_EVENT_BLUETOOTH_SCAN);
  bluetooth_scan();
}

static const config_action_command_t bluetooth_exec = {
  .code = CONFIG_ACTION_COMMAND_EXEC,
  .exec = btscan
};

static const config_action_t bluetooth_action = {
  .name = "btscan",
  .commands = (config_action_command_t*)&bluetooth_exec
};

static const config_button_t bluetooth_btn = {
  .label = "Scan Bluetooth",
  .action = (config_action_t*)&bluetooth_action
};

static const config_menu_entry_t system_menu_bluetooth = {
  .type = CONFIG_MENU_ENTRY_BUTTON,
  .button = (config_button_t*)&bluetooth_btn
};
#endif

// the usb core file selector
static char *core_exts[] = { "fs", "bin", NULL }; 

#if MAX_CORES < 2
#error "Setup at least 2 cores in config.h"
#endif

#ifdef DIRECT_SDC_SUPPORTED
static const config_fsel_t sdc_core_fsel = {
  .index = MAX_DRIVES+MAX_IMAGES+1,
  .label = "Load Core from SD",
  .def = "/sd/core.bin",
  .ext = (char**)core_exts
};

// second entry in main system menu
static const config_menu_entry_t system_menu_sdc_core_fsel = {
  .type = CONFIG_MENU_ENTRY_FILESELECTOR,
  .fsel = (config_fsel_t*)&sdc_core_fsel,
#ifdef ENABLE_BLUETOOTH
  .next = (config_menu_entry_t*)&system_menu_bluetooth
#endif
};
#endif

static const config_fsel_t usb_core_fsel = {
  .index = MAX_DRIVES+MAX_IMAGES+0,
  .label = "Load Core from USB",
  .def = "/usb/core.bin",
  .ext = (char**)core_exts
};

// second entry in main system menu
static const config_menu_entry_t system_menu_usb_core_fsel = {
  .type = CONFIG_MENU_ENTRY_FILESELECTOR,
  .fsel = (config_fsel_t*)&usb_core_fsel,
#ifdef DIRECT_SDC_SUPPORTED
  .next = (config_menu_entry_t*)&system_menu_sdc_core_fsel
#else
#ifdef ENABLE_BLUETOOTH
  .next = (config_menu_entry_t*)&system_menu_bluetooth
#endif
#endif
};

// first entry in main system menu
static const config_menu_entry_t system_menu_about = {
  .type = CONFIG_MENU_ENTRY_BUTTON,
  .button = (config_button_t*)&about_btn,
  .next = (config_menu_entry_t*)&system_menu_usb_core_fsel
};

// the main system menu
static const config_menu_t system_menu_main = {
  .label = "Companion",
  .entries = (config_menu_entry_t*)&system_menu_about
};

