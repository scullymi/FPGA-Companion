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
#include "../ftpd.h"
#include "../telnetd.h"
#include "../xml.h"
#include "../at_wifi.h"
#include "../game20k/core_boot.h"
#include "../game20k/game_switch.h"
#include "../game20k/ra_mac.h"
#include "../game20k/ra_patch.h"
#include "../game20k/ra_task.h"
#include "../game20k/ram_mirror.h"

/*-----------------------------------------------------------*/
/*---            main FPGA communication task            ----*/
/*-----------------------------------------------------------*/

/* game20k: a core switch sets up the next core as the start does, so these two steps of
   com_task are functions, see core_setup(). Their lines keep the indent of com_task for
   a merge with the upstream file. What is game20k's own is in core_boot.c. Returns true
   for a game20k core whose menu this firmware lacks: the menu says so once it runs. */
bool core_config(unsigned char probe) {
  bool mismatch = false;

    // try to load a config .xml from sd card. If the core has identified itself,
    // then e.g. atarist.xml will be read. otherwise config.xml
    FIL fil;
    sdc_lock();    // game20k: after a core switch FTP may be at the card meanwhile
    if(f_open(&fil, sys_get_config_name(), FA_OPEN_EXISTING | FA_READ) == FR_OK) {
      config_init();

      UINT br; char c;
      debugf("Loading XML config from file");
      core_config_file();

      // read byte by byte. Slow but that doesn't hurt ...
      FRESULT r = f_read(&fil, &c, 1, &br);
      while(r == FR_OK && br) {
	xml_parse(c);      
	r = f_read(&fil, &c, 1, &br);
      }    
      f_close(&fil);
      sdc_unlock();

      config_dump();
    } else {
      sdc_unlock();
      // no XML on SD card, try to load from core itself
      // game20k: the firmware's menu goes first, see core_menu()
      char *cfg_str = core_menu(probe, &mismatch);
      if(cfg_str) {
	config_init();
	char *c = cfg_str;
	while(*c) xml_parse(*c++); 
	config_dump();
      } else
	debugf("No valid config found, neither on sd card nor in core");
    }
  return mismatch;
}

void core_start(void) {
    // open disk images, either defaults set in sdc_init or
    // user configure ones from the ini file. This will also
    // start rom image transfers if specified in the ini file
    switch_rom();            /* game20k: a game picked before a core switch goes first */
    sdc_mount_defaults();
    core_images_open();      /* game20k */

    // finally run the ready action. This will usually get the core out of reset
    // On setups not using core configs, just release FPGA from reset
    // But this should actually never be the case nowadays.
    // TODO: An image upload may still be in progress ...
    if(!sdc_image_upload_in_progress()) {
      if(!cfg) sys_set_val('R', 0);
      else     sys_run_action_by_name("ready");
    } else
      debugf("Image upload in progress, delaying ready action");
}

TaskHandle_t com_task_handle = NULL;

static void com_task(__attribute__((unused)) void *p ) {
  debugf("Starting main communication task");
  marker_take();             /* game20k: the marker of a restart, first of all */
  
  // startup FPGA, this will also put the core into reset
  if(sys_wait4fpga()) {
    // FPGA is ready and can be talked to

    // initialitze SD card
    sdc_init();

    // try to load the global config
    inifile_config_read();
    
    // game20k: the board id and the interface tag of the core from the RAM mirror
    // header, before the menu: they pick the menu built into this firmware. The
    // board also decides with the ROM which game this is. After the menu comes the
    // handover of the achievement set. The set itself is read in game_settle()
    // once board and ROM are known, so a set with a valid tag lets the game start in
    // hardcore. rcheevos takes it with the first snapshot.
    unsigned char probe = ram_mirror_probe();
    start_game();             /* game20k: at power-on the start game of Save settings */
    ring_hops(&probe);        /* a game of another board: its core first, see ring_hops() */
    bool mismatch = core_config(probe);   // game20k: the menu, see core_config()

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
    if(mismatch) menu_notify(MENU_EVENT_MISMATCH);

    if(!ra_patch_init()) debugf("RA: set handover could not be set up, no achievements");

    core_start();             /* game20k: the images, the game, the ready action */

    ftpd_init();
    
    // finally prepare for wifi communication
    at_wifi_init();

    debugf("Entering main loop");
  
    // game20k: the RA task starts from the loop once the game is settled, and
    // before that pass's poll, so the first snapshot and any unlock find the
    // task and its handover queue. A stream ejected in the OSD settles on the
    // next pass with "no ROM"; a file picked in the OSD holds the settle back
    // (rom_pending) until its stream ends; a stream that stalls settles after
    // 15 s at the latest, with what is known by then.
    bool ra_started = false;
    TickType_t loop_start = xTaskGetTickCount();
    for(;;) {
      mcu_hw_irq_ack();  // (re-)enable interrupt
      /* wake at least every 20 ms to poll the RAM mirror, not only on an interrupt */
      if(ulTaskNotifyTake( pdTRUE, pdMS_TO_TICKS(20) ))
        sys_handle_interrupts(sys_irq_ctrl(0xff), false);
      if(!game_settled() &&
         ((!sdc_image_upload_in_progress() && !game_rom_pending()) ||
          (xTaskGetTickCount() - loop_start) >= pdMS_TO_TICKS(15000)))
        game_settle();
      if(!ra_started && game_settled()) {
        // RetroAchievements talks to the server in a task of its own,
        // so this loop never waits for it
        ra_task_start();
        ra_started = true;
      }
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
  // the device key for the tags on the card, before anything else runs: making it
  // the first time erases a flash sector with interrupts off, see ra_mac.c
  ra_mac_init();

  mcu_hw_init();
  telnetd_init();
  
  // run FPGA com thread. game20k: 12 KB of stack, about 2.5 times what it used on the Pico
  // with rcheevos running a set
  xTaskCreate( com_task, "FPGA Com", 3072, NULL, CONFIG_MAX_PRIORITY-1, &com_task_handle );

  mcu_hw_main_loop();

#ifndef ESP_PLATFORM
  return 0;
#endif
}
/*-----------------------------------------------------------*/

