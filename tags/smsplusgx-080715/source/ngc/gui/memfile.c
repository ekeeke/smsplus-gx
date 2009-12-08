/***************************************************************************
 *   SDCARD/MEMCARD File support
 *
 *
 ***************************************************************************/
#include "shared.h"
#include "dvd.h"
#include "font.h"
#include "saveicon.h"	/*** Nice little icon - thanks brakken! ***/

/* Support for MemCards */
/**
 * libOGC System Work Area
 */
static u8 SysArea[CARD_WORKAREA] ATTRIBUTE_ALIGN (32);
static card_dir CardDir;
static card_file CardFile;
static card_stat CardStatus;

/**
 * DMA Transfer Area.
 * Must be 32-byte aligned.
 * 64k SRAM + 2k Icon
 */
unsigned char savebuffer[0x12000] ATTRIBUTE_ALIGN (32);

int ManageState (u8 direction, u8 device);

/****************************************************************************
 * FILE autoload (SRAM/FreezeState or Config File)
 *
 *
 *****************************************************************************/
void memfile_autoload()
{
  /* this should be transparent to the user */
  SILENT = 1;

  if (option.autofreeze > -1) ManageState(1,option.autofreeze);
  SILENT = 0;
}

void memfile_autosave()
{
  /* this should be transparent to the user */
  SILENT = 1;

  if (option.autofreeze > -1) ManageState(0,option.autofreeze);
  SILENT = 0;
}


/****************************************************************************
 * SDCARD Access functions
 *
 * We use the same buffer as for Memory Card manager
 * Function returns TRUE on success.
 *****************************************************************************/
static int SD_ManageFile(char *filename, int direction)
{
  char pathname[MAXPATHLEN];
  int done = 0;
  int filesize;

  /* first check if directory exist */
  DIR_ITER *dir = diropen("/smsplus/saves");
  if (dir == NULL) mkdir("/smsplus/saves",S_IRWXU);
  else dirclose(dir);

  /* build complete SDCARD filename */
  sprintf (pathname, "/smsplus/saves/%s", filename);
  /* open file */
  FILE *fp = fopen(pathname, direction ? "rb" : "wb");
  if (fp == NULL)
  {
    sprintf (filename, "Error opening %s", pathname);
    WaitPrompt(filename);
    return 0;
  }

  switch (direction)
  {
    case 0: /* SAVING */
      /* save sate into buffer */
      filesize = system_save_state(&savebuffer[0]);

      /* write buffer */
      done = fwrite(savebuffer, 1, filesize, fp);
      if (done < filesize)
      {
        sprintf (filename, "Error writing %s", pathname);
        WaitPrompt(filename);
        return 0;
      }

      fclose(fp);
      sprintf (filename, "Saved %d bytes successfully", done);
      WaitPrompt (filename);
      return 1;
		
    case 1: /* LOADING */
	
      /* read size */
      fseek(fp , 0 , SEEK_END);
      filesize = ftell (fp);
      fseek(fp, 0, SEEK_SET);

      /* read into buffer (32k blocks) */
      done = fread(savebuffer, 1, filesize, fp);
      if (done < filesize)
      {
        sprintf (filename, "Error reading %s", pathname);
        WaitPrompt(filename);
        return 0;
      }
      fclose(fp);
 
      /* load STATE */
			system_load_state(&savebuffer[0]);

			sprintf (filename, "Loaded %d bytes successfully", done);
			WaitPrompt (filename);
			return 1;
	}
	
	return 0; 
}

/****************************************************************************
 * MountTheCard
 *
 * libOGC provides the CARD_Mount function, and it should be all you need.
 * However, experience with previous emulators has taught me that you are
 * better off doing a little bit more than that!
 *
 * Function returns TRUE on success.
 *****************************************************************************/
int MountTheCard (u8 slot)
{
  int tries = 0;
  int CardError;
  *(unsigned long *) (0xcc006800) |= 1 << 13; /*** Disable Encryption ***/
  uselessinquiry ();
  while (tries < 10)
  {
    VIDEO_WaitVSync ();
    CardError = CARD_Mount (slot, SysArea, NULL); /*** Don't need or want a callback ***/
    if (CardError == 0) return 1;
 	  else EXI_ProbeReset ();
    tries++;
  }
  return 0;
}

/****************************************************************************
 * CardFileExists
 *
 * Wrapper to search through the files on the card.
 * Returns TRUE if found.
 ****************************************************************************/
int CardFileExists (char *filename, u8 slot)
{
  int CardError = CARD_FindFirst (slot, &CardDir, TRUE);
  while (CardError != CARD_ERROR_NOFILE)
  {
      CardError = CARD_FindNext (&CardDir);
      if (strcmp ((char *) CardDir.filename, filename) == 0) return 1;
  }
  return 0;
}

/****************************************************************************
 * ManageState
 *
 * Here is the main Freeze File Management stuff.
 * The output file contains an icon (2K), 64 bytes comment and the STATE (~128k)
 *
 * direction == 0 save, 1 load.
 ****************************************************************************/
int ManageState (u8 direction, u8 device)
{
  char filename[128];
  char action[80];
  int CardError;
  unsigned int SectorSize;
  int blocks;
  char comment[2][32] = { {"SMS Plus 1.3"}, {"Freeze State"} };
  int outbytes = 0;
  int sbo;
  int state_size = 0;

  if (!smsromsize) return 0;

	/* clean buffer */
  memset(savebuffer, 0, 0x12000);

  if (direction) ShowAction ("Loading State ...");
  else ShowAction ("Saving State ...");

  /*** No idea what the damn ROM is, so just use CRC32 ***/
  sprintf (filename, "%08X.spz", (u32)crc32 (0, &cart.rom[0], smsromsize));
  strcpy (comment[1], filename);

  /* device is SDCARD, let's go */
	if (device == 0) return SD_ManageFile(filename,direction);

  /* set MCARD slot nr. */
  u8 CARDSLOT = device - 1;

  /* device is MCARD, we continue */
  if (direction == 0) /* Saving */
  {	
	  /* Build the output buffer */
    memcpy (&savebuffer, &icon, 2048);
    memcpy (&savebuffer[2048], &comment[0], 64);
	  state_size = system_save_state(&savebuffer[2112]);
  }

  outbytes = 2048 + 64 + state_size;

  /*** Initialise the CARD system ***/
  memset (&SysArea, 0, CARD_WORKAREA);
  CARD_Init ("SMSP", "00");

  /*** Attempt to mount the card ***/
  CardError = MountTheCard (CARDSLOT);

  if (CardError)
  {
	  /*** Retrieve the sector size ***/
    CardError = CARD_GetSectorSize (CARDSLOT, &SectorSize);

    switch (direction)
	  {
      case 0: /*** Saving ***/
			  /*** Determine number of blocks on this card ***/
	  		blocks = (outbytes / SectorSize) * SectorSize;
	  		if (outbytes % SectorSize) blocks += SectorSize;

			  /*** Check if a previous save exists ***/
	  		if (CardFileExists (filename,CARDSLOT))
	    	{
          CardError = CARD_Open (CARDSLOT, filename, &CardFile);
          if (CardError)
          {
		  			sprintf (action, "Error Open : %d", CardError);
		  			WaitPrompt (action);
            CARD_Unmount (CARDSLOT);
            return 0;
          }

          int size = CardFile.len;
          CARD_Close (&CardFile);

          if (size < blocks)
          {
            /* new size is bigger: check if there is enough space left */
				    CardError = CARD_Create (CARDSLOT, "TEMP", blocks-size, &CardFile);
            if (CardError)
            {
              sprintf (action, "Error Update : %d", CardError);
              WaitPrompt (action);
              CARD_Unmount (CARDSLOT);
              return 0;
            }
            CARD_Close (&CardFile);
            CARD_Delete(CARDSLOT, "TEMP");
          }

          /* always delete existing slot */
          CARD_Delete(CARDSLOT, filename);
		    }
			
        /*** Create a new slot ***/
        CardError = CARD_Create (CARDSLOT, filename, blocks, &CardFile);
        if (CardError)
        {
          sprintf (action, "Error create : %d %d", CardError, CARDSLOT);
          WaitPrompt (action);
          CARD_Unmount (CARDSLOT);
          return 0;
        }
	  		
        /*** Continue and save ***/
        CARD_GetStatus (CARDSLOT, CardFile.filenum, &CardStatus);
        CardStatus.icon_addr = 0x0;
        CardStatus.icon_fmt = 2;
        CardStatus.icon_speed = 1;
        CardStatus.comment_addr = 2048;
        CARD_SetStatus (CARDSLOT, CardFile.filenum, &CardStatus);

        /*** And write the blocks out ***/
        sbo = 0;
        while (outbytes > 0)
        {
          CardError = CARD_Write (&CardFile, &savebuffer[sbo], SectorSize, sbo);
          outbytes -= SectorSize;
          sbo += SectorSize;
        }

	  		CARD_Close (&CardFile);
	  		CARD_Unmount (CARDSLOT);
			  sprintf (action, "Saved %d bytes successfully", blocks);
	  		WaitPrompt (action);
	  		return 1;

		default: /*** Loading ***/
      if (!CardFileExists (filename, CARDSLOT))
	    {
				WaitPrompt ("No Savestate Found");
				CARD_Unmount (CARDSLOT);
				return 0;
			}

	  	memset (&CardFile, 0, sizeof (CardFile));
	  	CardError = CARD_Open (CARDSLOT, filename, &CardFile);
			if (CardError)
			{
				sprintf (action, "Error Open : %d", CardError);
				WaitPrompt (action);
				CARD_Unmount (CARDSLOT);
				return 0;
			}

			blocks = CardFile.len;
			if (blocks < SectorSize) blocks = SectorSize;
			if (blocks % SectorSize) blocks++;

			/*** Just read the file back in ***/
	  	sbo = 0;
	  	int size = blocks;
	  	while (blocks > 0)
			{
				CARD_Read (&CardFile, &savebuffer[sbo], SectorSize, sbo);
	  		sbo += SectorSize;
				blocks -= SectorSize;
			}
			CARD_Close (&CardFile);
			CARD_Unmount (CARDSLOT);

			/*** Load State ***/
			system_load_state(&savebuffer[2112]);
		
			/*** Inform user ***/
			sprintf (action, "Loaded %d bytes successfully", size);
			WaitPrompt (action);
			return 1;
	  }
  }
  else WaitPrompt ("Unable to mount memory card");
  return 0; /*** Signal failure ***/
}