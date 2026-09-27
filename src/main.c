/*
  main.c - MiSTeryNano FPGA Companion Pi Pico variant

*/

#include "../mcu_hw.h"

#include "../config.h"
#include "../sysctrl.h"
#include "../sdc.h"
#include "../osd.h"
#include "../menu.h"
#include "../inifile.h"
#include "../debug.h"
#include "../spi.h"     /* SPI_TARGET_RAM */
#include "rc_runtime.h"
#include "../ra_patch.h"
#include "../ra_queue.h"
#include "../ra_state.h"
#include "../ra_task.h"
#include <string.h>
#include "pico/time.h"

/* RAM mirror on SPI target 5: each poll reads the header and, if there is a new
   snapshot, fetches the game RAM plus the oracle log, checks it, and sends the verdict
   as the first byte of the next transfer. The sizes must match ram_mirror_pkg.sv of
   the game20k FPGA core. */
#define RAM_MIRROR_HEAD  8
#define RAM_MIRROR_DATA  5120                        /* bgram + wram1..3            */
#define RAM_MIRROR_LOG   1536                        /* oracle log, 512 x 3 bytes   */
#define RAM_MIRROR_BODY  (RAM_MIRROR_DATA + RAM_MIRROR_LOG)
#define RAM_MIRROR_FOOT  (RAM_MIRROR_HEAD + RAM_MIRROR_BODY)
#define RAM_MIRROR_BYTES (RAM_MIRROR_FOOT + 8)       /* 6672 */

static unsigned char ram_mirror_buf[RAM_MIRROR_BYTES];
static unsigned char ram_mirror_verdict = 0xA5;
static int ram_mirror_frame = -1;                    /* frame number of the last good snapshot */
static unsigned char ram_mirror_seen[RAM_MIRROR_DATA / 8];

/* rcheevos evaluates the achievement conditions over each good snapshot. The set
   is defined on the flat layout of the mirror (bgram 2048, then wram1..3 of 1024
   each), so an address is the offset in the snapshot. */
static rc_runtime_t   ra_rt;
static bool           ra_ready;
static unsigned char  ra_triggered;         /* achievements triggered, saturating */
static unsigned char  ra_last;              /* last one, 1-based position in the set */
static unsigned short ra_us;                /* evaluation time of the last snapshot */
static unsigned       ra_oob;               /* reads outside the mirror */
static unsigned       ra_rp_frames;         /* frames since the rich presence text was read */
#define RA_RP_EVERY   60                    /* frames between two readings, about a second */

static uint32_t ra_peek(uint32_t address, uint32_t num_bytes, void *ud) {
  (void)ud;
  /* outside the mirror there is nothing, say so once instead of reading air */
  if(address + num_bytes > RAM_MIRROR_DATA) {
    if(!ra_oob++) debugf("RA: condition reads 0x%lx, outside the mirror", (unsigned long)address);
    return 0;
  }
  const unsigned char *p = ram_mirror_buf + RAM_MIRROR_HEAD + address;
  switch(num_bytes) {
  case 1: return p[0];
  case 2: return p[0] | (p[1] << 8);
  case 4: return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
  default: return 0;
  }
}

/* The banner at the bottom of the picture: 24 characters the FPGA shows for a
   while. It carries the title of an unlock, or a message that starts with who
   speaks, "RA: " for the achievements and "SYS: " for the machine itself. One
   character goes over with each mirror poll in header bytes 6 and 7. The show
   flag follows once the whole text is in the FPGA, so it appears at once. Banners
   that arrive while one is shown wait their turn, a few of them. */
#define BANNER_LEN   24
#define BANNER_MS    8000
#define BANNER_QUEUE 4
typedef struct {
  char text[BANNER_LEN];
  bool gold;        /* text gold for hardcore, white for softcore, as on the RA site */
  bool new;         /* mark green: new, goes to the server. Grey: the account has it, or it is queued */
} banner_t;
static banner_t   banner;                  /* the one being sent or shown */
static banner_t   banner_q[BANNER_QUEUE];  /* the ones waiting, oldest first */
static unsigned   banner_n;                /* how many wait */
static unsigned   banner_pos;              /* next character to send */
static bool       banner_pending;          /* text on its way, show follows after the pass */
static bool       banner_live;             /* show flag on */
static TickType_t banner_start;            /* when the show flag went on */

static void banner_show(const char *text, bool gold, bool new) {
  banner_t b;
  unsigned i = 0;
  if(banner_n >= BANNER_QUEUE) {
    debugf("RA: banner '%s' dropped, %u already wait", text, banner_n);
    return;
  }
  // the FPGA font has A-Z, 0-9, space and ! - . : so lower case is raised and
  // anything else becomes a space. The rest of the line is spaces.
  for(; text[i] && i < BANNER_LEN; i++) {
    char c = text[i];
    if(c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    else if(!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(" !-.:", c))) c = ' ';
    b.text[i] = c;
  }
  // a longer text ends at a word, if one ends in the last third of the line,
  // rather than in the middle of one. Titles can have up to 64 characters.
  if(text[i] && text[i] != ' ') {
    unsigned cut = i;
    while(cut > BANNER_LEN * 2 / 3 && b.text[cut - 1] != ' ') cut--;
    if(cut > BANNER_LEN * 2 / 3) i = cut - 1;
  }
  for(; i < BANNER_LEN; i++) b.text[i] = ' ';
  b.gold = gold;
  b.new  = new;
  banner_q[banner_n++] = b;
  debugf("RA: banner '%.*s' for %u s (%s text, %s mark)%s", BANNER_LEN, b.text, BANNER_MS / 1000,
         gold ? "gold" : "white", new ? "green" : "grey", banner_n > 1 ? ", waits" : "");
}

/* One poll's worth of banner work: ends a banner after BANNER_MS, starts the
   next waiting one, and fills header bytes 6 and 7. Bit 7 show, bit 6 gold text,
   bit 5 green mark, bits 4..0 the position, byte 7 the character. */
static void banner_step(unsigned char *hdr) {
  if(banner_live && (xTaskGetTickCount() - banner_start) >= pdMS_TO_TICKS(BANNER_MS))
    banner_live = false;
  if(!banner_live && !banner_pending && banner_n) {
    banner = banner_q[0];
    memmove(banner_q, banner_q + 1, (--banner_n) * sizeof(banner_q[0]));
    banner_pos     = 0;
    banner_pending = true;
  }
  hdr[6] = (unsigned char)((banner_live ? 0x80 : 0) | (banner.gold ? 0x40 : 0) |
                           (banner.new ? 0x20 : 0) | (banner_pos & 0x1f));
  hdr[7] = (unsigned char)banner.text[banner_pos];
  if(++banner_pos >= BANNER_LEN) {
    banner_pos = 0;
    if(banner_pending) {            // the whole text is over, now it may show
      banner_pending = false;
      banner_live    = true;
      banner_start   = xTaskGetTickCount();
    }
  }
}

/* A message when the RA task's state changes: the account is in, or the
   player should know why nothing counts. */
static void banner_login(void) {
  static ra_task_state_t shown = RA_TASK_STARTING;
  ra_task_state_t now = ra_task_state();
  if(now == shown) return;
  shown = now;
  // messages are white, the mark is green when all is well and grey when not
  if(now == RA_TASK_LOGGED_IN) {
    char text[BANNER_LEN + 1];
    snprintf(text, sizeof(text), "RA: %s", inifile_config_get_str("ra", "user"));
    banner_show(text, false, true);
  } else if(now == RA_TASK_REJECTED)
    banner_show("RA: LOGIN REJECTED", false, false);
  else if(now == RA_TASK_NO_TIME)
    banner_show("RA: NO TIME SERVER", false, false);
}

static void ra_event(const rc_runtime_event_t *ev) {
  if(ev->type != RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED) return;
  bool hardcore = ra_task_hardcore();
  if(ra_triggered < 255) ra_triggered++;
  ra_last = (unsigned char)ra_patch_index(ev->id);
  debugf("RA: %s achievement %u triggered: %s", hardcore ? "hardcore" : "softcore",
         (unsigned)ev->id, ra_patch_title(ev->id));
  // gold in hardcore, white in softcore, as on the RA site. The mark is green when
  // the unlock is new in this mode, grey when the account has it already or one
  // is queued. Asked before the queue takes this one.
  bool known = ra_state_known(ev->id) || (!hardcore && ra_state_softcore_only(ev->id));
  banner_show(ra_patch_title(ev->id), hardcore, !known);
  // the RA task keeps it on the card and sends it, this never blocks
  ra_queue_add(ev->id, hardcore);
}

/* Read the header first. Byte 7 set means a harvest was running when the transfer
   started: the shadow is half old, half new, so skip it and retry on the next poll.
   That is normal, the verdict stays. From the verdict byte on, the FPGA starts no
   new harvest until chip select rises, so a clear byte 7 holds to the end. A harvest
   starting in exactly that clock tears the snapshot, the checksum (0xE6) catches it.
   The header read is full duplex: hdr_tx goes to the FPGA meanwhile. */
static void ram_mirror_poll(void) {
  unsigned char hdr_tx[RAM_MIRROR_HEAD] = { 0,0,0,0,0,0,0,0 };
  /* back channel for the diagnostic bars of the FPGA: the rcheevos state */
  hdr_tx[0] = ra_triggered;
  hdr_tx[1] = (unsigned char)ra_patch_count();
  hdr_tx[2] = (unsigned char)(ra_us & 0xff);
  hdr_tx[3] = (unsigned char)(ra_us >> 8);
  hdr_tx[4] = ra_last;

  // the banner, one character per poll in bytes 6 and 7
  banner_login();
  banner_step(hdr_tx);

  mcu_hw_spi_begin();
  mcu_hw_spi_tx_u08(SPI_TARGET_RAM);
  mcu_hw_spi_tx_u08(ram_mirror_verdict);     /* verdict on the previous transfer */
  mcu_hw_spi_txrx_block(hdr_tx, ram_mirror_buf, RAM_MIRROR_HEAD);

  /* Verdict: 0xA5 ok, otherwise the failed check. 0xE1 magic, 0xE2 layout,
     0xE4 frame number, 0xE6 checksum, 0xE7 log overflow, 0xE8 and 0xE9 see below. */
  if(ram_mirror_buf[0] != 'R' || ram_mirror_buf[1] != 'A' ||
     ram_mirror_buf[2] != 'C' || ram_mirror_buf[3] != 'H') {
    mcu_hw_spi_end(); ram_mirror_verdict = 0xE1; return;
  }
  if(ram_mirror_buf[4] != 0x02) {            /* layout 2: game RAM plus oracle log */
    mcu_hw_spi_end(); ram_mirror_verdict = 0xE2; return;
  }
  if(ram_mirror_buf[7] != 0x00) {            /* harvest running, retry next poll */
    mcu_hw_spi_end(); return;
  }
  /* frame number counts harvests: same as the last good snapshot means nothing new */
  int frame = ram_mirror_buf[5] | (ram_mirror_buf[6] << 8);
  if(frame == ram_mirror_frame) {
    mcu_hw_spi_end(); return;
  }

  mcu_hw_spi_rx_block(ram_mirror_buf + RAM_MIRROR_HEAD,
                      RAM_MIRROR_BYTES - RAM_MIRROR_HEAD);
  mcu_hw_spi_end();

  /* Same checksum the FPGA forms when it fills the FIFO, over game RAM and oracle
     log. Rotate before XOR so that the byte order counts. */
  unsigned short sum = 0;
  for(unsigned int i = RAM_MIRROR_HEAD; i < RAM_MIRROR_FOOT; i++)
    sum = (unsigned short)(((sum << 1) | (sum >> 15)) ^ ram_mirror_buf[i]);

  const unsigned char *ftr  = ram_mirror_buf + RAM_MIRROR_FOOT;
  unsigned short       want = (unsigned short)(ftr[4] | (ftr[5] << 8));

  /* Checksum before the underrun bit: the bit only says the FIFO ran empty, the
     checksum says whether data arrived wrong. */
  if(ram_mirror_buf[5] != ftr[0] ||
     ram_mirror_buf[6] != ftr[1])                 { ram_mirror_verdict = 0xE4; return; }
  if(sum != want)                                 { ram_mirror_verdict = 0xE6; return; }
  if(ftr[3] != 0x00)                              { ram_mirror_verdict = 0xE7; return; }

  /* Underrun with a good checksum did no harm: keep the snapshot, report 0xE8. */
  unsigned char note = (ftr[2] != 0x00) ? 0xE8 : 0xA5;

  /* Oracle. The log holds every game write during the harvest, and the snapshot must
     carry the last value written to each of those addresses. Walk the log backwards
     and check only the first hit per address. Other addresses cannot differ. */
  unsigned int n = (unsigned int)ftr[6] | (((unsigned int)ftr[7] & 3u) << 8);
  if(n > RAM_MIRROR_LOG / 3) n = RAM_MIRROR_LOG / 3;

  for(unsigned int i = 0; i < sizeof(ram_mirror_seen); i++) ram_mirror_seen[i] = 0;

  unsigned short bad = 0;
  for(int i = (int)n - 1; i >= 0; i--) {
    const unsigned char *e = ram_mirror_buf + RAM_MIRROR_HEAD + RAM_MIRROR_DATA + 3 * i;
    unsigned short flat = (unsigned short)(e[0] | (e[1] << 8));
    if(flat >= RAM_MIRROR_DATA) continue;                  /* cannot happen, guards the index */
    if(ram_mirror_seen[flat >> 3] & (1u << (flat & 7))) continue;
    ram_mirror_seen[flat >> 3] |= (unsigned char)(1u << (flat & 7));
    if(ram_mirror_buf[RAM_MIRROR_HEAD + flat] != e[2]) bad++;
  }
  if(bad) { ram_mirror_verdict = 0xE9; return; }     /* snapshot does not match the log */

  /* rcheevos only sees a snapshot that passed every check. The set comes from
     the RA task, which reads it from the card, and is activated here once it
     is there. */
  if(!ra_ready) {
    rc_runtime_init(&ra_rt);
    ra_ready = true;
  }
  ra_patch_apply_pending(&ra_rt);
  absolute_time_t t0 = get_absolute_time();
  rc_runtime_do_frame(&ra_rt, ra_event, ra_peek, NULL, NULL);
  int64_t dt = absolute_time_diff_us(t0, get_absolute_time());
  ra_us = dt > 65535 ? 65535 : (unsigned short)dt;

  /* the RA task pings only while frames arrive, and sends the rich presence text,
     read here where the snapshot is valid, about once a second */
  ra_task_frame();
  if(++ra_rp_frames >= RA_RP_EVERY) {
    char rp[RA_RP_MAX];
    ra_rp_frames = 0;
    rc_runtime_get_richpresence(&ra_rt, rp, sizeof(rp), ra_peek, NULL, NULL);
    ra_task_set_richpresence(rp);
  }

  ram_mirror_frame   = frame;
  ram_mirror_verdict = note;
}


#include "../ftpd.h"
#include "../telnetd.h"
#include "../xml.h"
#include "../at_wifi.h"

/*-----------------------------------------------------------*/
/*---            main FPGA communication task            ----*/
/*-----------------------------------------------------------*/

TaskHandle_t com_task_handle = NULL;

static void com_task(__attribute__((unused)) void *p ) {
  debugf("Starting main communication task");
  
  // startup FPGA, this will also put the core into reset
  if(sys_wait4fpga()) {
    // FPGA is ready and can be talked to

    // initialitze SD card
    sdc_init();

    // try to load the global config
    inifile_config_read();
    
    // try to load a config .xml from sd card. If the core has identified itself,
    // then e.g. atarist.xml will be read. otherwise config.xml
    FIL fil;
    if(f_open(&fil, sys_get_config_name(), FA_OPEN_EXISTING | FA_READ) == FR_OK) {
      config_init();

      UINT br; char c;
      debugf("Loading XML config from file");

      // read byte by byte. Slow but that doesn't hurt ...
      FRESULT r = f_read(&fil, &c, 1, &br);
      while(r == FR_OK && br) {
	xml_parse(c);      
	r = f_read(&fil, &c, 1, &br);
      }    
      f_close(&fil);

      config_dump();
    } else {
      // no XML on SD card, try to load from core itself
      char *cfg_str = sys_get_config();
      if(cfg_str) {
	debugf("Loading XML config from core");
	config_init();
	char *c = cfg_str;
	while(*c) xml_parse(*c++); 
	config_dump();
      } else
	debugf("No valid config found, neither on sd card nor in core");
    }

    // process any pending interrupt. Filter out irq 1 which is the
    // FPGA cold boot event which we ignore since we just booted outselves
    sys_handle_interrupts(sys_irq_ctrl(0xff), true);
    
    // by default, DB9 interrupts are disabled. Reading
    // the DB9 state enables them. This is what hid_handle_event
    // does.
    hid_handle_event();

    // initialize on-screen-display and menu system
    osd_init();    
    menu_init();

    // open disk images, either defaults set in sdc_init or
    // user configure ones from the ini file. This will also
    // start rom image transfers if specified in the ini file
    sdc_mount_defaults();

    // finally run the ready action. This will usually get the core out of reset
    // On setups not using core configs, just release FPGA from reset
    // But this should actually never be the case nowadays.
    // TODO: An image upload may still be in progress ...
    if(!sdc_image_upload_in_progress()) {
      if(!cfg) sys_set_val('R', 0);
      else     sys_run_action_by_name("ready");
    } else
      debugf("Image upload in progress, delaying ready action");

    ftpd_init();
    
    // finally prepare for wifi communication
    at_wifi_init();

    // RetroAchievements talks to the server in a task of its own,
    // so this loop never waits for it
    ra_task_start();

    debugf("Entering main loop");
  
    for(;;) {
      mcu_hw_irq_ack();  // (re-)enable interrupt
      /* wake at least every 20 ms to poll the RAM mirror, not only on an interrupt */
      if(ulTaskNotifyTake( pdTRUE, pdMS_TO_TICKS(20) ))
        sys_handle_interrupts(sys_irq_ctrl(0xff), false);
      ram_mirror_poll();
    }
  }

  /* This will only be reached if the FPGA is not ready */
  /* So loop foreever while e.g. USB is still being handled */
  /* e.g. for debugging */
  for(;;) {
    // frequently check for an FPGA to show up and reboot to
    // startup normally if one is detected
    if(sys_status_is_valid()) {
      debugf("FPGA detected!");
      // This may be due to a USB download. So give USB
      // some time to finish. The same happens in sysconfig.c
      vTaskDelay(pdMS_TO_TICKS(2000));
      mcu_hw_reset();
    }
      
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
  mcu_hw_init();
  telnetd_init();
  
  // run FPGA com thread
  xTaskCreate( com_task, "FPGA Com", 4096, NULL, CONFIG_MAX_PRIORITY-1, &com_task_handle );

  mcu_hw_main_loop();

#ifndef ESP_PLATFORM
  return 0;
#endif
}
/*-----------------------------------------------------------*/

