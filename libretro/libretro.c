#ifndef _MSC_VER
#include <stdbool.h>
#endif
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

#ifdef _XBOX1
#include <xtl.h>
#endif

#include "shared.h"
#include "libretro.h"
#include "state.h"
#include "genesis.h"
#include "md_ntsc.h"
#include "sms_ntsc.h"

sms_ntsc_t *sms_ntsc;
md_ntsc_t  *md_ntsc;

static int vwidth;
static int vheight;

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_batch_t audio_batch_cb;

void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_variable vars[] = {
      { "blargg_ntsc_filter", "Blargg NTSC filter; disabled|monochrome|composite|svideo|rgb" },
      { "overscan", "Overscan mode; 0|1|2|3" },
      { "gg_extra", "Game Gear extended screen; disabled|enabled" },
      { NULL, NULL },
   };

   environ_cb = cb;
   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

struct bind_conv
{
   int retro;
   int genesis;
};

static struct bind_conv binds[] = {
   { RETRO_DEVICE_ID_JOYPAD_B, INPUT_B },
   { RETRO_DEVICE_ID_JOYPAD_A, INPUT_C },
   { RETRO_DEVICE_ID_JOYPAD_X, INPUT_Y },
   { RETRO_DEVICE_ID_JOYPAD_Y, INPUT_A },
   { RETRO_DEVICE_ID_JOYPAD_START, INPUT_START },
   { RETRO_DEVICE_ID_JOYPAD_L, INPUT_X },
   { RETRO_DEVICE_ID_JOYPAD_R, INPUT_Z },
   { RETRO_DEVICE_ID_JOYPAD_UP, INPUT_UP },
   { RETRO_DEVICE_ID_JOYPAD_DOWN, INPUT_DOWN },
   { RETRO_DEVICE_ID_JOYPAD_LEFT, INPUT_LEFT },
   { RETRO_DEVICE_ID_JOYPAD_RIGHT, INPUT_RIGHT },
   { RETRO_DEVICE_ID_JOYPAD_SELECT, INPUT_MODE },
};


static char g_rom_dir[1024];

char GG_ROM[256];
char AR_ROM[256];
char SK_ROM[256];
char SK_UPMEM[256];
char GG_BIOS[256];
char MS_BIOS_EU[256];
char MS_BIOS_JP[256];
char MS_BIOS_US[256];
char CD_BIOS_EU[256];
char CD_BIOS_US[256];
char CD_BIOS_JP[256];
char DEFAULT_PATH[1024];
char CD_BRAM_JP[256];
char CD_BRAM_US[256];
char CD_BRAM_EU[256];
char CART_BRAM[256];

/* Mega CD backup RAM stuff */
static uint32_t brm_crc[2];
static uint8_t brm_format[0x40] =
{
  0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x00,0x00,0x00,0x00,0x40,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x53,0x45,0x47,0x41,0x5f,0x43,0x44,0x5f,0x52,0x4f,0x4d,0x00,0x01,0x00,0x00,0x00,
  0x52,0x41,0x4d,0x5f,0x43,0x41,0x52,0x54,0x52,0x49,0x44,0x47,0x45,0x5f,0x5f,0x5f
};

/************************************
 * Genesis Plus implementation
 ************************************/
#define CHUNKSIZE   (0x10000)

void error(char * msg, ...)
{
#ifndef _XBOX1
   va_list ap;
   va_start(ap, msg);
   vfprintf(stderr, msg, ap);
   va_end(ap);
#endif
}

int load_archive(char *filename, unsigned char *buffer, int maxsize, char *extension)
{
  int size = 0;
  char in[CHUNKSIZE];
  char msg[64] = "Unable to open file";

  /* Open file */
  FILE *fd = fopen(filename, "rb");

  /* Master System & Game Gear BIOS are optional files */
  if (!strcmp(filename,MS_BIOS_US) || !strcmp(filename,MS_BIOS_EU) || !strcmp(filename,MS_BIOS_JP) || !strcmp(filename,GG_BIOS))
  {
    /* disable all messages */
  }
  
  /* Mega CD BIOS are required files */
  if (!strcmp(filename,CD_BIOS_US) || !strcmp(filename,CD_BIOS_EU) || !strcmp(filename,CD_BIOS_JP)) 
  {
    snprintf(msg, sizeof(msg), "Unable to open CD BIOS: %s", filename);
  }

  if (!fd)
  {
    fprintf(stderr, "ERROR - %s.\n", msg);
    return 0;
  }

  /* Read first chunk */
  fread(in, CHUNKSIZE, 1, fd);

  {
    int left;
    /* Get file size */
    fseek(fd, 0, SEEK_END);
    size = ftell(fd);
    fseek(fd, 0, SEEK_SET);

    /* size limit */
    if(size > maxsize)
    {
      fclose(fd);
      fprintf(stderr, "ERROR - File is too large.\n");
      return 0;
    }

    sprintf((char *)msg,"Loading %d bytes ...", size);
    fprintf(stderr, "INFORMATION - %s\n", msg);

    /* filename extension */
    if (extension)
    {
      memcpy(extension, &filename[strlen(filename) - 3], 3);
      extension[3] = 0;
    }

    /* Read into buffer */
    left = size;
    while (left > CHUNKSIZE)
    {
      fread(buffer, CHUNKSIZE, 1, fd);
      buffer += CHUNKSIZE;
      left -= CHUNKSIZE;
    }

    /* Read remaining bytes */
    fread(buffer, left, 1, fd);
  }

  /* Close file */
  fclose(fd);

  /* Return loaded ROM size */
  return size;
}

static uint16_t bitmap_data_[1024 * 512];

static void init_bitmap(void)
{
   memset(&bitmap, 0, sizeof(bitmap));
   bitmap.width      = 1024;
   bitmap.height     = 512;
   bitmap.pitch      = bitmap.width * sizeof(uint16_t);
   bitmap.data       = (uint8_t *)bitmap_data_;
   bitmap.viewport.w = 0;
   bitmap.viewport.h = 0;
   bitmap.viewport.x = 0;
   bitmap.viewport.y = 0;
}

#define CONFIG_VERSION "GENPLUS-GX 1.7.4"

static void config_default(void)
{
   /* version TAG */
   strncpy(config.version,CONFIG_VERSION,16);

   /* sound options */
   config.psg_preamp     = 150;
   config.fm_preamp      = 100;
   config.hq_fm          = 1;
   config.psgBoostNoise  = 1;
   config.filter         = 0;
   config.lp_range       = 0x9999; /* 0.6 in 16.16 fixed point */
   config.low_freq       = 880;
   config.high_freq      = 5000;
   config.lg             = 1.0;
   config.mg             = 1.0;
   config.hg             = 1.0;
   config.dac_bits 	 = 14;
   config.ym2413         = 2; /* AUTO */
   config.mono           = 0;

   /* system options */
   config.system         = 0; /* AUTO */
   config.region_detect  = 0; /* AUTO */
   config.vdp_mode       = 0; /* AUTO */
   config.master_clock   = 0; /* AUTO */
   config.force_dtack    = 0;
   config.addr_error     = 1;
   config.bios           = 0;
   config.lock_on        = 0;
   config.hot_swap       = 0;

   /* video options */
   config.overscan = 0; /* 3 == FULL */
   config.gg_extra = 0; /* 1 = show extended Game Gear screen (256x192) */
   config.ntsc     = 0;
   config.render   = 0;
}

/* these values are used for libretro reporting too */
static const double pal_fps = 53203424.0 / (3420.0 * 313.0);
static const double ntsc_fps = 53693175.0 / (3420.0 * 262.0);

static void init_audio(void)
{
   audio_init(44100, 0);
}

static void configure_controls(void)
{
   unsigned i;

   switch (system_hw)
   {
      case SYSTEM_MD:
      case SYSTEM_MCD:
         for(i = 0; i < MAX_INPUTS; i++)
         {
            config.input[i].padtype = DEVICE_PAD6B;
         }	
         input.system[0] = SYSTEM_MD_GAMEPAD;
         input.system[1] = SYSTEM_MD_GAMEPAD;
         break;
      case SYSTEM_GG:
      case SYSTEM_SMS:
         input.system[0] = SYSTEM_MS_GAMEPAD;
         input.system[1] = SYSTEM_MS_GAMEPAD;
         break;
      default:
         break;
   }
}


/* Mega CD backup RAM specific */
static void bram_load(void)
{
    FILE *fp;

    /* automatically load internal backup RAM */
    switch (region_code)
    {
       case REGION_JAPAN_NTSC:
          fp = fopen(CD_BRAM_JP, "rb");
          break;
       case REGION_EUROPE:
          fp = fopen(CD_BRAM_EU, "rb");
          break;
       case REGION_USA:
          fp = fopen(CD_BRAM_US, "rb");
          break;
       default:
          return;
    }

    if (fp != NULL)
    {
      fread(scd.bram, 0x2000, 1, fp);
      fclose(fp);

      /* update CRC */
      brm_crc[0] = crc32(0, scd.bram, 0x2000);
    }
    else
    {
      /* force internal backup RAM format (does not use previous region backup RAM) */
      scd.bram[0x1fff] = 0;
    }

    /* check if internal backup RAM is correctly formatted */
    if (memcmp(scd.bram + 0x2000 - 0x20, brm_format + 0x20, 0x20))
    {
      /* clear internal backup RAM */
      memset(scd.bram, 0x00, 0x2000 - 0x40);

      /* internal Backup RAM size fields */
      brm_format[0x10] = brm_format[0x12] = brm_format[0x14] = brm_format[0x16] = 0x00;
      brm_format[0x11] = brm_format[0x13] = brm_format[0x15] = brm_format[0x17] = (sizeof(scd.bram) / 64) - 3;

      /* format internal backup RAM */
      memcpy(scd.bram + 0x2000 - 0x40, brm_format, 0x40);

      /* clear CRC to force file saving (in case previous region backup RAM was also formatted) */
      brm_crc[0] = 0;
    }

    /* automatically load cartridge backup RAM (if enabled) */
    if (scd.cartridge.id)
    {
      fp = fopen(CART_BRAM, "rb");
      if (fp != NULL)
      {
        int filesize = scd.cartridge.mask + 1;
        int done = 0;
        
        /* Read into buffer (2k blocks) */
        while (filesize > CHUNKSIZE)
        {
          fread(scd.cartridge.area + done, CHUNKSIZE, 1, fp);
          done += CHUNKSIZE;
          filesize -= CHUNKSIZE;
        }

        /* Read remaining bytes */
        if (filesize)
        {
          fread(scd.cartridge.area + done, filesize, 1, fp);
        }

        /* close file */
        fclose(fp);

        /* update CRC */
        brm_crc[1] = crc32(0, scd.cartridge.area, scd.cartridge.mask + 1);
      }

      /* check if cartridge backup RAM is correctly formatted */
      if (memcmp(scd.cartridge.area + scd.cartridge.mask + 1 - 0x20, brm_format + 0x20, 0x20))
      {
        /* clear cartridge backup RAM */
        memset(scd.cartridge.area, 0x00, scd.cartridge.mask + 1);

        /* Cartridge Backup RAM size fields */
        brm_format[0x10] = brm_format[0x12] = brm_format[0x14] = brm_format[0x16] = (((scd.cartridge.mask + 1) / 64) - 3) >> 8;
        brm_format[0x11] = brm_format[0x13] = brm_format[0x15] = brm_format[0x17] = (((scd.cartridge.mask + 1) / 64) - 3) & 0xff;

        /* format cartridge backup RAM */
        memcpy(scd.cartridge.area + scd.cartridge.mask + 1 - 0x40, brm_format, 0x40);
      }
    }
}

static void bram_save(void)
{
    FILE *fp;

    /* verify that internal backup RAM has been modified */
    if (crc32(0, scd.bram, 0x2000) != brm_crc[0])
    {
      /* check if it is correctly formatted before saving */
      if (!memcmp(scd.bram + 0x2000 - 0x20, brm_format + 0x20, 0x20))
      {
        switch (region_code)
	{
		case REGION_JAPAN_NTSC:
			fp = fopen(CD_BRAM_JP, "wb");
			break;
		case REGION_EUROPE:
			fp = fopen(CD_BRAM_EU, "wb");
			break;
		case REGION_USA:
			fp = fopen(CD_BRAM_US, "wb");
			break;
		default:
		        return;
	}
        if (fp != NULL)
        {
          fwrite(scd.bram, 0x2000, 1, fp);
          fclose(fp);

          /* update CRC */
          brm_crc[0] = crc32(0, scd.bram, 0x2000);
        }
      }
    }

    /* verify that cartridge backup RAM has been modified */
    if (scd.cartridge.id && (crc32(0, scd.cartridge.area, scd.cartridge.mask + 1) != brm_crc[1]))
    {
      /* check if it is correctly formatted before saving */
      if (!memcmp(scd.cartridge.area + scd.cartridge.mask + 1 - 0x20, brm_format + 0x20, 0x20))
      {
        fp = fopen(CART_BRAM, "wb");
        if (fp != NULL)
        {
          int filesize = scd.cartridge.mask + 1;
          int done = 0;
        
          /* Write to file (2k blocks) */
          while (filesize > CHUNKSIZE)
          {
            fwrite(scd.cartridge.area + done, CHUNKSIZE, 1, fp);
            done += CHUNKSIZE;
            filesize -= CHUNKSIZE;
          }

          /* Write remaining bytes */
          if (filesize)
          {
            fwrite(scd.cartridge.area + done, filesize, 1, fp);
          }

          /* Close file */
          fclose(fp);

          /* update CRC */
          brm_crc[1] = crc32(0, scd.cartridge.area, scd.cartridge.mask + 1);
        }
      }
    }
}

/************************************
 * libretro implementation
 ************************************/

static struct retro_system_av_info g_av_info;

void retro_get_system_info(struct retro_system_info *info)
{
   info->library_name = "Genesis Plus GX";
   info->library_version = "v1.7.4";
   info->valid_extensions = "md|smd|bin|cue|gen|iso|sms|gg|sg";
   info->block_extract = false;
   info->need_fullpath = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   *info = g_av_info;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

size_t retro_serialize_size(void) { return STATE_SIZE; }

bool retro_serialize(void *data, size_t size)
{ 
   if (size != STATE_SIZE)
      return FALSE;

   state_save(data);

   return TRUE;
}

bool retro_unserialize(const void *data, size_t size)
{
   if (size != STATE_SIZE)
      return FALSE;

   state_load((uint8_t*)data);

   return TRUE;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   char *base;
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
      buf[0] = '\0';
}

static void retro_set_viewport_dimensions(void)
{
   unsigned i;
   struct retro_game_geometry geom;
   struct retro_system_timing timing;

   /* HACK: Emulate 10 dummy frames to figure out the real viewport dimensions of the game. */
   if((system_hw & SYSTEM_PBC) == SYSTEM_MD || (system_hw & SYSTEM_PBC) == SYSTEM_MCD)
      for (i = 0; i < 10; i++)
         system_frame_gen(0);
   else
      for (i = 0; i < 10; i++)
         system_frame_sms(0);

   vwidth  = bitmap.viewport.w + (bitmap.viewport.x * 2);
   vheight = bitmap.viewport.h + (bitmap.viewport.y * 2);

   retro_reset();

   if (config.ntsc)
   {
      if (system_hw & SYSTEM_MD)
         vwidth = MD_NTSC_OUT_WIDTH(vwidth);
      else
         vwidth = SMS_NTSC_OUT_WIDTH(vwidth);
   }

   geom.aspect_ratio = 4.0 / 3.0;
   geom.base_width = vwidth;
   geom.base_height = vheight;
   geom.max_width = 1024;
   geom.max_height = 512;

   timing.sample_rate = 44100;

   if (vdp_pal)
      timing.fps = pal_fps;
   else
      timing.fps = ntsc_fps;

   g_av_info.geometry = geom;
   g_av_info.timing   = timing;
}

static bool LoadFile(char * filename)
{
   int size = 0;

   /* check if virtual CD tray was open */
   if ((system_hw == SYSTEM_MCD) && (cdd.status == CD_OPEN))
   {
      /* swap CD image file */
      size = cdd_load(filename, (char *)(cdc.ram));

      /* update CD header information */
      if (!scd.cartridge.boot)
         getrominfo((char *)(cdc.ram));
   }

   /* no CD image file loaded */
   if (!size)
   {
      /* close CD tray to force system reset */
      cdd.status = NO_DISC;
      
      /* load game file */
      size = load_rom(filename);
   }

   return size > 0;
}

static void check_variables(void)
{
   bool update_viewports = false;
   static bool last_ntsc_val_same;
   struct retro_variable var = {0};

   var.key = "blargg_ntsc_filter";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      unsigned orig_value = config.ntsc;

      update_viewports = true;

      if (strcmp(var.value, "disabled") == 0)
         config.ntsc = 0;
      else if (strcmp(var.value, "monochrome") == 0)
      {
         config.ntsc = 1;
         sms_ntsc_init(sms_ntsc, &sms_ntsc_monochrome);
         md_ntsc_init(md_ntsc,   &md_ntsc_monochrome);
      }
      else if (strcmp(var.value, "composite") == 0)
      {
         config.ntsc = 1;
         sms_ntsc_init(sms_ntsc, &sms_ntsc_composite);
         md_ntsc_init(md_ntsc,   &md_ntsc_composite);
      }
      else if (strcmp(var.value, "svideo") == 0)
      {
         config.ntsc = 1;
         sms_ntsc_init(sms_ntsc, &sms_ntsc_svideo);
         md_ntsc_init(md_ntsc,   &md_ntsc_svideo);
      }
      else if (strcmp(var.value, "rgb") == 0)
      {
         config.ntsc = 1;
         sms_ntsc_init(sms_ntsc, &sms_ntsc_rgb);
         md_ntsc_init(md_ntsc,   &md_ntsc_rgb);
      }

      if (orig_value != config.ntsc)
         update_viewports = true;
   }

   var.key = "overscan";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      unsigned orig_value = config.overscan;

      if (strcmp(var.value, "0") == 0)
         config.overscan = 0;
      else if (strcmp(var.value, "1") == 0)
         config.overscan = 1;
      else if (strcmp(var.value, "2") == 0)
         config.overscan = 2;
      else if (strcmp(var.value, "3") == 0)
         config.overscan = 3;

      if (orig_value != config.overscan)
         update_viewports = true;
   }

   var.key = "gg_extra";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      unsigned orig_value = config.gg_extra;

      if (strcmp(var.value, "disabled") == 0)
         config.gg_extra = 0;
      else if (strcmp(var.value, "enabled") == 0)
         config.gg_extra = 1;

      if (orig_value != config.gg_extra)
         update_viewports = true;
   }

   if (update_viewports)
      retro_set_viewport_dimensions();
}

bool retro_load_game(const struct retro_game_info *info)
{
   const char *full_path;
   const char *dir;
   char slash;

   extract_directory(g_rom_dir, info->path, sizeof(g_rom_dir));

   if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) || !dir)
   {
      fprintf(stderr, "[genplus]: Defaulting system directory to %s.\n", g_rom_dir);
      dir = g_rom_dir;
   }
#if defined(_WIN32)
   slash = '\\';
#else
   slash = '/';
#endif

   snprintf(CD_BIOS_EU, sizeof(CD_BIOS_EU), "%s%cbios_CD_E.bin", dir, slash);
   snprintf(CD_BIOS_US, sizeof(CD_BIOS_US), "%s%cbios_CD_U.bin", dir, slash);
   snprintf(CD_BIOS_JP, sizeof(CD_BIOS_JP), "%s%cbios_CD_J.bin", dir, slash);
   snprintf(CD_BRAM_EU, sizeof(CD_BRAM_EU), "%s%cscd_E.brm", dir, slash);
   snprintf(CD_BRAM_US, sizeof(CD_BRAM_US), "%s%cscd_U.brm", dir, slash);
   snprintf(CD_BRAM_JP, sizeof(CD_BRAM_JP), "%s%cscd_J.brm", dir, slash);
   snprintf(CART_BRAM, sizeof(CART_BRAM), "%s%ccart.brm", dir, slash);
   fprintf(stderr, "Sega CD EU BIOS should be located at: %s\n", CD_BIOS_EU);
   fprintf(stderr, "Sega CD US BIOS should be located at: %s\n", CD_BIOS_US);
   fprintf(stderr, "Sega CD JP BIOS should be located at: %s\n", CD_BIOS_JP);
   fprintf(stderr, "Sega CD EU BRAM is located at: %s\n", CD_BRAM_EU);
   fprintf(stderr, "Sega CD US BRAM is located at: %s\n", CD_BRAM_US);
   fprintf(stderr, "Sega CD JP BRAM is located at: %s\n", CD_BRAM_JP);
   fprintf(stderr, "Sega CD RAM CART is located at: %s\n", CART_BRAM);

   snprintf(DEFAULT_PATH, sizeof(DEFAULT_PATH), "%s", g_rom_dir);

   config_default();
   init_bitmap();

   full_path = info->path;

   if (!LoadFile((char *)full_path))
      return false;

   configure_controls();

   init_audio();

   system_init();
   system_reset();

   if (system_hw == SYSTEM_MCD)
      bram_load();

   retro_set_viewport_dimensions();

   check_variables();

   return TRUE;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
   (void)game_type;
   (void)info;
   (void)num_info;
   return FALSE;
}

void retro_unload_game(void) 
{
   if (system_hw == SYSTEM_MCD)
      bram_save();
}

unsigned retro_get_region(void) { return vdp_pal ? RETRO_REGION_PAL : RETRO_REGION_NTSC; }

void *retro_get_memory_data(unsigned id)
{
   if (!sram.on)
      return NULL;

   switch (id)
   {
      case RETRO_MEMORY_SAVE_RAM:
         return sram.sram;

      default:
         return NULL;
   }
}

size_t retro_get_memory_size(unsigned id)
{
   if (!sram.on)
      return 0;

   switch (id)
   {
      case RETRO_MEMORY_SAVE_RAM:
         return 0x10000;

      default:
         return 0;
   }
}

void retro_init(void)
{
   unsigned level, rgb565;
   sms_ntsc = calloc(1, sizeof(sms_ntsc_t));
   md_ntsc  = calloc(1, sizeof(md_ntsc_t));

   level = 1;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);

#ifdef FRONTEND_SUPPORTS_RGB565
   rgb565 = RETRO_PIXEL_FORMAT_RGB565;
   if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb565))
      fprintf(stderr, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");
#endif
}

void retro_deinit(void)
{
   audio_shutdown();
   free(md_ntsc);
   free(sms_ntsc);
}

void retro_reset(void) { system_reset(); }

int16 soundbuffer[3068];

void osd_input_update(void)
{
   unsigned res[MAX_INPUTS], i, j;

   for(i = 0; i < MAX_INPUTS; i++)
      res[i] = 0;

   if (!input_poll_cb)
      return;

   input_poll_cb();

   switch (input.system[0])
   {
      case SYSTEM_MS_GAMEPAD:
      case SYSTEM_MD_GAMEPAD:
         if(input.dev[0] != NO_DEVICE)
         {
         for (j = 0; j < sizeof(binds) / sizeof(binds[0]); j++)
         {
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, binds[j].retro))
               res[0] |= binds[j].genesis;
         }

         input.pad[0] = res[0];
         }
         break;
      case SYSTEM_MOUSE:
         break;
      case SYSTEM_ACTIVATOR:
         break;
      case SYSTEM_XE_A1P:
         break;
      case SYSTEM_WAYPLAY:
         break;
      case SYSTEM_TEAMPLAYER:
         break;
      case SYSTEM_LIGHTPHASER:
         break;
      case SYSTEM_PADDLE:
         break;
      case SYSTEM_SPORTSPAD:
         break;
      default:
         break;
   }


   switch (input.system[1])
   {
      case SYSTEM_MS_GAMEPAD:
      case SYSTEM_MD_GAMEPAD:
         if(input.dev[4] != NO_DEVICE)
         {
            for (j = 0; j < sizeof(binds) / sizeof(binds[0]); j++)
            {
               if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, binds[j].retro))
                  res[1] |= binds[j].genesis;
	       input.pad[4] = res[1];
            }
         }
         break;
      case SYSTEM_MOUSE:
         break;
      case SYSTEM_ACTIVATOR:
         break;
      case SYSTEM_XE_A1P:
         break;
      case SYSTEM_TEAMPLAYER:
         break;
      case SYSTEM_LIGHTPHASER:
         break;
      case SYSTEM_PADDLE:
         break;
      case SYSTEM_SPORTSPAD:
         break;
      default:
         break;
   }

   if (cart.special & HW_J_CART)
   {
      for(i = 5; i < 7; i++)
      {
            for (j = 0; j < sizeof(binds) / sizeof(binds[0]); j++)
            {
               if (input_state_cb(i-3, RETRO_DEVICE_JOYPAD, 0, binds[j].retro))
                  res[i-3] |= binds[j].genesis;
	       input.pad[i] = res[i-3];
            }
      }
   }

}

void retro_run(void) 
{
   int aud;
   bool updated = false;

   if (system_hw == SYSTEM_MCD)
      system_frame_scd(0);
   else if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
      system_frame_gen(0);
   else
      system_frame_sms(0);

   video_cb(bitmap.data, config.ntsc ? vwidth : (bitmap.viewport.w + (bitmap.viewport.x * 2)), bitmap.viewport.h + (bitmap.viewport.y * 2), bitmap.pitch);

   aud = audio_update(soundbuffer) << 1;
   audio_batch_cb(soundbuffer, aud >> 1);

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables();
}

