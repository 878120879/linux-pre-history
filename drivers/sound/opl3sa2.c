/*
 * sound/opl3sa2.c
 *
 * A low level driver for Yamaha OPL3-SA2 and SA3 cards.
 * SAx cards should work, as they are just variants of the SA3.
 *
 * Copyright 1998 Scott Murray <scottm@interlog.com>
 *
 * Originally based on the CS4232 driver (in cs4232.c) by Hannu Savolainen
 * and others.  Now incorporates code/ideas from pss.c, also by Hannu
 * Savolainen.  Both of those files are distributed with the following
 * license:
 *
 * "Copyright (C) by Hannu Savolainen 1993-1997
 *
 *  OSS/Free for Linux is distributed under the GNU GENERAL PUBLIC LICENSE (GPL)
 *  Version 2 (June 1991). See the "COPYING" file distributed with this software
 *  for more info."
 *
 * As such, in accordance with the above license, this file, opl3sa2.c, is
 * distributed under the GNU GENERAL PUBLIC LICENSE (GPL) Version 2 (June 1991).
 * See the "COPYING" file distributed with this software for more information.
 *
 * Change History
 * --------------
 * Scott Murray            Original driver (Jun 14, 1998)
 * Paul J.Y. Lahaie        Changed probing / attach code order
 * Scott Murray            Added mixer support (Dec 03, 1998)
 * Scott Murray            Changed detection code to be more forgiving,
 *                         added force option as last resort,
 *                         fixed ioctl return values. (Dec 30, 1998)
 *
 */

#include <linux/config.h>
#include <linux/module.h>

#include "sound_config.h"
#include "soundmodule.h"

/* Useful control port indexes: */
#define OPL3SA2_MASTER_LEFT  0x07
#define OPL3SA2_MASTER_RIGHT 0x08
#define OPL3SA2_MIC          0x09
#define OPL3SA2_MISC         0x0A

#define OPL3SA3_WIDE         0x14
#define OPL3SA3_BASS         0x15
#define OPL3SA3_TREBLE       0x16

/* Useful constants: */
#define DEFAULT_VOLUME 50
#define DEFAULT_MIC    50
#define DEFAULT_TIMBRE 0

/*
 * NOTE: CHIPSET_UNKNOWN should match the default value of
 *       CONFIG_OPL3SA2_CHIPSET in Config.in to make everything
 *       work right in all situations.
 */
#define CHIPSET_UNKNOWN -1
#define CHIPSET_OPL3SA2  1
#define CHIPSET_OPL3SA3  2
#define CHIPSET_OPL3SAX  4


#ifdef CONFIG_OPL3SA2

/* What's my version? */
#ifdef CONFIG_OPL3SA2_CHIPSET
/* Set chipset if compiled into the kernel */
static int chipset = CONFIG_OPL3SA2_CHIPSET;
#else
static int chipset = CHIPSET_UNKNOWN;
#endif

/* Oh well, let's just cache the name */
static char chipset_name[16];

/* Where's my mixer */
static int opl3sa2_mixer = -1;

/* Bag o' mixer data */
typedef struct opl3sa2_mixerdata {
	unsigned short cfg_port;
	unsigned short padding;
	int            ad_mixer_dev;
	unsigned int   volume_l;
	unsigned int   volume_r;
	unsigned int   mic;
	unsigned int   bass;
	unsigned int   treble;
} opl3sa2_mixerdata;

#ifdef CONFIG_OPL3SA2_CTRL_BASE
/* Set control port if compiled into the kernel */
static opl3sa2_mixerdata opl3sa2_data = { CONFIG_OPL3SA2_CTRL_BASE, };
#else
static opl3sa2_mixerdata opl3sa2_data;
#endif

static opl3sa2_mixerdata *devc = &opl3sa2_data;


/* Standard read and write functions */

static void opl3sa2_write(unsigned short port,
			  unsigned char  index,
			  unsigned char  data)
{
	outb_p(index, port);
	outb(data, port + 1);
}


static void opl3sa2_read(unsigned short port,
			 unsigned char  index,
			 unsigned char* data)
{
	outb_p(index, port);
	*data = inb(port + 1);
}


/* All of the mixer functions... */

static void opl3sa2_set_volume(opl3sa2_mixerdata *devc, int left, int right)
{
	static unsigned char scale[101] = {
		0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0e, 0x0e, 0x0e, 
		0x0e, 0x0e, 0x0e, 0x0e, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 
		0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0b, 0x0b, 0x0b, 
		0x0b, 0x0b, 0x0b, 0x0b, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 
		0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x08, 0x08, 0x08, 
		0x08, 0x08, 0x08, 0x08, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 
		0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x05, 0x05, 0x05, 
		0x05, 0x05, 0x05, 0x05, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 
		0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x02, 0x02, 0x02, 
		0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 
		0x00
	};
	unsigned char vol;

	vol = scale[left];

	/* If level is zero, turn on mute */
	if(!left)
		vol |= 0x80;

	opl3sa2_write(devc->cfg_port, OPL3SA2_MASTER_LEFT, vol);

	vol = scale[right];

	/* If level is zero, turn on mute */
	if(!right)
		vol |= 0x80;

	opl3sa2_write(devc->cfg_port, OPL3SA2_MASTER_RIGHT, vol);
}


static void opl3sa2_set_mic(opl3sa2_mixerdata *devc, int level)
{
	unsigned char vol = 0x1F;

	if((level >= 0) && (level <= 100))
		vol = 0x1F - (unsigned char) (0x1F * level / 100L);

	/* If level is zero, turn on mute */
	if(!level)
		vol |= 0x80;

	opl3sa2_write(devc->cfg_port, OPL3SA2_MIC, vol);
}


static void opl3sa3_set_bass(opl3sa2_mixerdata *devc, int level)
{
	unsigned char bass;

	bass = level ? ((unsigned char) (0x07 * level / 100L)) : 0; 
	bass |= (bass << 4);

	opl3sa2_write(devc->cfg_port, OPL3SA3_BASS, bass);
}


static void opl3sa3_set_treble(opl3sa2_mixerdata *devc, int level)
{	
	unsigned char treble;

	treble = level ? ((unsigned char) (0x07 * level / 100L)) : 0; 
	treble |= (treble << 4);

	opl3sa2_write(devc->cfg_port, OPL3SA3_TREBLE, treble);
}


static void opl3sa2_mixer_reset(opl3sa2_mixerdata *devc)
{
	if(devc)
	{
		opl3sa2_set_volume(devc, DEFAULT_VOLUME, DEFAULT_VOLUME);
		devc->volume_l = devc->volume_r = DEFAULT_VOLUME;

		opl3sa2_set_mic(devc, DEFAULT_MIC);
		devc->mic = DEFAULT_MIC;

		opl3sa3_set_bass(devc, DEFAULT_TIMBRE);
		opl3sa3_set_treble(devc, DEFAULT_TIMBRE);
		devc->bass = devc->treble = DEFAULT_TIMBRE;
	}
}


static void arg_to_volume_mono(unsigned int volume, int *aleft)
{
	int left;
	
	left = volume & 0x00ff;
	if (left > 100)
		left = 100;
	*aleft = left;
}


static void arg_to_volume_stereo(unsigned int volume, int *aleft, int *aright)
{
	arg_to_volume_mono(volume, aleft);
	arg_to_volume_mono(volume >> 8, aright);
}


static int ret_vol_mono(int left)
{
	return ((left << 8) | left);
}


static int ret_vol_stereo(int left, int right)
{
	return ((right << 8) | left);
}


static int call_ad_mixer(opl3sa2_mixerdata *devc, unsigned int cmd, caddr_t arg)
{
	if(devc->ad_mixer_dev != -1) 
		return mixer_devs[devc->ad_mixer_dev]->ioctl(devc->ad_mixer_dev,
							     cmd,
							     arg);
	else 
		return -EINVAL;
}


static int opl3sa2_mixer_ioctl(int dev, unsigned int cmd, caddr_t arg)
{
	int cmdf = cmd & 0xff;

	opl3sa2_mixerdata* devc = (opl3sa2_mixerdata*) mixer_devs[dev]->devc;
	
	switch(cmdf)
	{
		case SOUND_MIXER_VOLUME:
		case SOUND_MIXER_MIC:
		case SOUND_MIXER_BASS:
		case SOUND_MIXER_TREBLE:
		case SOUND_MIXER_DEVMASK:
		case SOUND_MIXER_STEREODEVS: 
		case SOUND_MIXER_RECMASK:
		case SOUND_MIXER_CAPS: 
		case SOUND_MIXER_RECSRC:
			break;

		default:
			return call_ad_mixer(devc, cmd, arg);
	}
	
	if(((cmd >> 8) & 0xff) != 'M')
		return -EINVAL;
		
	if(_SIOC_DIR (cmd) & _SIOC_WRITE)
	{
		switch (cmdf)	
		{
			case SOUND_MIXER_RECSRC:
				if(devc->ad_mixer_dev != -1)
					return call_ad_mixer(devc, cmd, arg);
				else
				{
					if(*(int*)arg != 0)
						return -EINVAL;
					return 0;
				}

			case SOUND_MIXER_VOLUME:
				arg_to_volume_stereo(*(unsigned int*)arg,
						     &devc->volume_l,
						     &devc->volume_r); 
				opl3sa2_set_volume(devc, devc->volume_l,
						   devc->volume_r);
				*(int*)arg = ret_vol_stereo(devc->volume_l,
							     devc->volume_r);
				return 0;
		  
			case SOUND_MIXER_MIC:
				arg_to_volume_mono(*(unsigned int*)arg,
						   &devc->mic);
				opl3sa2_set_mic(devc, devc->mic);
				*(int*)arg = ret_vol_mono(devc->mic);
				return 0;
		  
			case SOUND_MIXER_BASS:
				if(chipset != CHIPSET_OPL3SA2)
				{
					arg_to_volume_mono(*(unsigned int*)arg,
							   &devc->bass);
					opl3sa3_set_bass(devc, devc->bass);
					*(int*)arg = ret_vol_mono(devc->bass);
					return 0;
				}
				return -EINVAL;
		  
			case SOUND_MIXER_TREBLE:
				if(chipset != CHIPSET_OPL3SA2)
				{
					arg_to_volume_mono(*(unsigned int *)arg,
							   &devc->treble);
					opl3sa3_set_treble(devc, devc->treble);
					*(int*)arg = ret_vol_mono(devc->treble);
					return 0;
				}
				return -EINVAL;
		  
			default:
				return -EINVAL;
		}
	}
	else			
	{
		/*
		 * Return parameters
		 */
		switch (cmdf)
		{
			case SOUND_MIXER_DEVMASK:
				if(call_ad_mixer(devc, cmd, arg) == -EINVAL)
					*(int*)arg = 0; /* no mixer devices */

				*(int*)arg |= (SOUND_MASK_VOLUME | SOUND_MASK_MIC);

				/* OPL3-SA2 has no bass and treble mixers */
				if(chipset != CHIPSET_OPL3SA2)
					*(int*)arg |= (SOUND_MASK_BASS |
						       SOUND_MASK_TREBLE);
				return 0;
		  
			case SOUND_MIXER_STEREODEVS:
				if(call_ad_mixer(devc, cmd, arg) == -EINVAL)
					*(int*)arg = 0; /* no stereo devices */
				*(int*)arg |= SOUND_MASK_VOLUME;
				return 0;
		  
			case SOUND_MIXER_RECMASK:
				if(devc->ad_mixer_dev != -1)
				{
					return call_ad_mixer(devc, cmd, arg);
				}
				else
				{
					/* No recording devices */
					return (*(int*)arg = 0);
				}

			case SOUND_MIXER_CAPS:
				if(devc->ad_mixer_dev != -1)
				{
					return call_ad_mixer(devc, cmd, arg);
				}
				else
				{
					*(int*)arg = SOUND_CAP_EXCL_INPUT;
					return 0;
				}

			case SOUND_MIXER_RECSRC:
				if(devc->ad_mixer_dev != -1)
				{
					return call_ad_mixer(devc, cmd, arg);
				}
				else
				{
					/* No recording source */
					return (*(int*)arg = 0);
				}

			case SOUND_MIXER_VOLUME:
				*(int*)arg = ret_vol_stereo(devc->volume_l,
							    devc->volume_r);
				return 0;
			  
			case SOUND_MIXER_MIC:
				*(int*)arg = ret_vol_mono(devc->mic);
				return 0;

			case SOUND_MIXER_BASS:
				if(chipset != CHIPSET_OPL3SA2)
				{
					*(int*)arg = ret_vol_mono(devc->bass);
					return 0;
				}
				else
				{
					return -EINVAL;
				}
			  
			case SOUND_MIXER_TREBLE:
				if(chipset != CHIPSET_OPL3SA2)
				{
					*(int*)arg = ret_vol_mono(devc->treble);
					return 0;
				}
				else
				{
					return -EINVAL;
				}
			  
			default:
				return -EINVAL;
		}
	}
}


static struct mixer_operations opl3sa2_mixer_operations =
{
	"Yamaha",
	"",
	 opl3sa2_mixer_ioctl
};

/* End of mixer-related stuff */


int probe_opl3sa2_mpu(struct address_info *hw_config)
{
#if (defined(CONFIG_MPU401) || defined(CONFIG_MPU_EMU)) && defined(CONFIG_MIDI)
	return probe_mpu401(hw_config);
#else
	return 0;
#endif
}


void attach_opl3sa2_mpu(struct address_info *hw_config)
{
#if (defined(CONFIG_MPU401) || defined(CONFIG_MPU_EMU)) && defined(CONFIG_MIDI)
	attach_mpu401(hw_config);
#endif
}


void unload_opl3sa2_mpu(struct address_info *hw_config)
{
#if (defined(CONFIG_MPU401) || defined(CONFIG_MPU_EMU)) && defined(CONFIG_MIDI)
	unload_mpu401(hw_config);
#endif
}


static int probe_opl3sa2_mss(struct address_info *hw_config)
{
	return probe_ms_sound(hw_config);
}


static void attach_opl3sa2_mss(struct address_info *hw_config)
{
	char mixer_name[64];

	/* Create pretty names for mixer stuff */
	strncpy(mixer_name, chipset_name, 16);
	strncat(mixer_name, " and AD1848 (through MSS)", 64);

	strncpy(opl3sa2_mixer_operations.name, chipset_name, 16);
	strncat(opl3sa2_mixer_operations.name, "-AD1848", 64);

	/* Install master mixer */
	devc->ad_mixer_dev = -1;
	if((opl3sa2_mixer = sound_install_mixer(MIXER_DRIVER_VERSION,
						mixer_name,
						&opl3sa2_mixer_operations,
						sizeof(struct mixer_operations),
						devc)) < 0) 
	{
		printk(KERN_ERR "Could not install %s master mixer\n", chipset_name);
		return;
	}

	opl3sa2_mixer_reset(devc);

	attach_ms_sound(hw_config);	/* Slot 0 */
	if(hw_config->slots[0] != -1)
	{
		/* Did the MSS driver install? */
		if(num_mixers == (opl3sa2_mixer + 2))
		{
			/* The MSS mixer is installed */
			devc->ad_mixer_dev = audio_devs[hw_config->slots[0]]->mixer_dev;

			/* Reroute mixers appropiately */
			AD1848_REROUTE(SOUND_MIXER_LINE1, SOUND_MIXER_CD);
			AD1848_REROUTE(SOUND_MIXER_LINE2, SOUND_MIXER_SYNTH);
			AD1848_REROUTE(SOUND_MIXER_LINE3, SOUND_MIXER_LINE);
		}
	}
}


static void unload_opl3sa2_mss(struct address_info *hw_config)
{
	unload_ms_sound(hw_config);
}


int probe_opl3sa2(struct address_info *hw_config)
{
	unsigned char chipsets[8] = { CHIPSET_UNKNOWN, /* 0 */
				      CHIPSET_OPL3SA2, /* 1 */
				      CHIPSET_OPL3SA3, /* 2 */
				      CHIPSET_UNKNOWN, /* 3 */
				      CHIPSET_OPL3SAX, /* 4 */
				      CHIPSET_OPL3SAX, /* 5 */
				      CHIPSET_UNKNOWN, /* 6 */
				      CHIPSET_OPL3SA3, /* 7 */ };
	unsigned char version = 0;
	char tag;

	/*
	 * Verify that the I/O port range is free.
	 */
	if(check_region(hw_config->io_base, 2))
	{
	    printk(KERN_ERR
		   "%s: Control I/O port 0x%03x not free\n",
		   __FILE__,
		   hw_config->io_base);
	    return 0;
	}

	/*
	 * Determine chipset type (SA2, SA3, or SAx)
	 *
	 * Have to handle two possible override situations:
	 * 1) User compiled driver into the kernel and forced chipset type
	 * 2) User built a module, but wants to override the chipset type
	 */
	if(chipset == CHIPSET_UNKNOWN)
	{
		if(hw_config->card_subtype == CHIPSET_UNKNOWN)
		{
			/*
			 * Look at chipset version in lower 3 bits of index 0x0A, miscellaneous
			 */
			opl3sa2_read(hw_config->io_base,
				     OPL3SA2_MISC,
				     (unsigned char*) &version);
			version &= 0x07;

			/* Match version number to appropiate chipset */
			chipset = chipsets[version];
		}
		else
		{
			/* Use user specified chipset */
			switch(hw_config->card_subtype)
			{
				case 2:
					chipset = CHIPSET_OPL3SA2;
					break;

				case 3:
					chipset = CHIPSET_OPL3SA3;
					break;

				default:
					printk(KERN_ERR "%s: Unknown chipset %d\n",
					       __FILE__,
					       hw_config->card_subtype);
					chipset = CHIPSET_UNKNOWN;
					break;
			}
		}
	}
	else
	{
		/* Use user compiled in chipset */
		switch(chipset)
		{
			case 2:
				chipset = CHIPSET_OPL3SA2;
				break;
				
			case 3:
				chipset = CHIPSET_OPL3SA3;
				break;

			default:
				printk(KERN_ERR "%s: Unknown chipset %d\n",
				       __FILE__,
				       chipset);
				chipset = CHIPSET_UNKNOWN;
				break;
		}
	}

	/* Do chipset specific stuff: */
	switch(chipset)
	{
		case CHIPSET_OPL3SA2:
			printk(KERN_INFO "Found OPL3-SA2 (YMF711)\n");
			tag = '2';
			break;

		case CHIPSET_OPL3SA3:
			printk(KERN_INFO "Found OPL3-SA3 (YMF715)\n");
			tag = '3';
			break;

		case CHIPSET_OPL3SAX:
			printk(KERN_INFO "Found OPL3-SAx (YMF719)\n");
			tag = 'x';
			break;

		default:
			printk(KERN_ERR "No Yamaha audio controller found\n");

			/* If we've actually checked the version, print it out */
			if(version)
			{
				printk(KERN_INFO
				       "%s: chipset version = %x\n",
				       __FILE__,
				       version);
			}
			
			/* Set some sane values */
			chipset = CHIPSET_UNKNOWN;
			tag = '?';
			break;
	}

	if(chipset != CHIPSET_UNKNOWN) {
		/* Generate a pretty name */
		sprintf(chipset_name, "OPL3-SA%c", tag);
		return 1;
	}
	return 0;
}


void attach_opl3sa2(struct address_info *hw_config)
{
   	request_region(hw_config->io_base, 2, chipset_name);

	devc->cfg_port = hw_config->io_base;
}


void unload_opl3sa2(struct address_info *hw_config)
{
        /* Release control ports */
	release_region(hw_config->io_base, 2);

	/* Unload mixer */
	if(opl3sa2_mixer >= 0)
		sound_unload_mixerdev(opl3sa2_mixer);
}


#ifdef MODULE

int io      = -1;
int mss_io  = -1;
int mpu_io  = -1;
int irq     = -1;
int dma     = -1;
int dma2    = -1;
int force   = -1;

MODULE_PARM(io, "i");
MODULE_PARM_DESC(io, "Set i/o base of OPL3-SA2 or SA3 card (usually 0x370)");

MODULE_PARM(mss_io, "i");
MODULE_PARM_DESC(mss_io, "Set MSS (audio) I/O base (0x530, 0xE80, or other. Address must end in 0 or 4 and must be from 0x530 to 0xF48)");

MODULE_PARM(mpu_io, "i");
MODULE_PARM_DESC(mpu_io, "Set MIDI I/O base (0x330 or other. Address must be on 4 location boundaries and must be from 0x300 to 0x334)");

MODULE_PARM(irq, "i");
MODULE_PARM_DESC(mss_irq, "Set MSS (audio) IRQ (5, 7, 9, 10, 11, 12)");

MODULE_PARM(dma, "i");
MODULE_PARM_DESC(dma, "Set MSS (audio) first DMA channel (0, 1, 3)");

MODULE_PARM(dma2, "i");
MODULE_PARM_DESC(dma2, "Set MSS (audio) second DMA channel (0, 1, 3)");

MODULE_PARM(force, "i");
MODULE_PARM_DESC(force, "Force audio controller chipset (2, 3)");

MODULE_DESCRIPTION("Module for OPL3-SA2 and SA3 sound cards (uses AD1848 MSS driver).");
MODULE_AUTHOR("Scott Murray <scottm@interlog.com>");

EXPORT_NO_SYMBOLS;

struct address_info cfg;
struct address_info mss_cfg;
struct address_info mpu_cfg;


/*
 * Install a OPL3SA2 based card.
 *
 * Need to have ad1848 and mpu401 loaded ready.
 */
int init_module(void)
{
        int i;

	if(io == -1 || irq == -1 || dma == -1 || dma2 == -1 || mss_io == -1)
	{
		printk(KERN_ERR "%s: io, mss_io, irq, dma, and dma2 must be set.\n",
		       __FILE__);
		return -EINVAL;
	}
   
        /* Our own config: */
        cfg.io_base = io;
	cfg.irq     = irq;
	cfg.dma     = dma;
	cfg.dma2    = dma2;
	
	/* Does the user want to override the chipset type? */
	if(force != -1)
		cfg.card_subtype = force;
	else
		cfg.card_subtype = CHIPSET_UNKNOWN;

        /* The MSS config: */
	mss_cfg.io_base      = mss_io;
	mss_cfg.irq          = irq;
	mss_cfg.dma          = dma;
	mss_cfg.dma2         = dma2;
	mss_cfg.card_subtype = 1;      /* No IRQ or DMA setup */

	/* Call me paranoid: */
	for(i = 0; i < 6; i++)
	{
		cfg.slots[i] = mss_cfg.slots[i] = mpu_cfg.slots[i] = -1;
	}

	if(probe_opl3sa2(&cfg) == 0)
	{
		return -ENODEV;
	}

	if(probe_opl3sa2_mss(&mss_cfg) == 0)
	{
		return -ENODEV;
	}

	attach_opl3sa2(&cfg);
	attach_opl3sa2_mss(&mss_cfg);

#if (defined(CONFIG_MPU401) || defined(CONFIG_MPU_EMU)) && defined(CONFIG_MIDI)
	if(mpu_io != -1)
	{
            /* MPU config: */
	    mpu_cfg.io_base       = mpu_io;
	    mpu_cfg.irq           = irq;
	    mpu_cfg.dma           = dma;
	    mpu_cfg.always_detect = 1;  /* It's there, so use shared IRQs */

	    if(probe_opl3sa2_mpu(&mpu_cfg))
	    {
		    attach_opl3sa2_mpu(&mpu_cfg);
	    }
	}
#endif
	SOUND_LOCK;
	return 0;
}


void cleanup_module(void)
{
#if (defined(CONFIG_MPU401) || defined(CONFIG_MPU_EMU)) && defined(CONFIG_MIDI)
        if(mpu_cfg.slots[1] != -1)
	{
		unload_opl3sa2_mpu(&mpu_cfg);
	}
#endif
	unload_opl3sa2_mss(&mss_cfg);
	unload_opl3sa2(&cfg);
	SOUND_LOCK_END;
}

#endif /* MODULE */
#endif /* CONFIG_OPL3SA2 */
