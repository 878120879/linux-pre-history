/*  devfs (Device FileSystem) utilities.

    Copyright (C) 1999-2000  Richard Gooch

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    Richard Gooch may be reached by email at  rgooch@atnf.csiro.au
    The postal address is:
      Richard Gooch, c/o ATNF, P. O. Box 76, Epping, N.S.W., 2121, Australia.

    ChangeLog

    19991031   Richard Gooch <rgooch@atnf.csiro.au>
               Created.
    19991103   Richard Gooch <rgooch@atnf.csiro.au>
               Created <_devfs_convert_name> and supported SCSI and IDE CD-ROMs
    20000203   Richard Gooch <rgooch@atnf.csiro.au>
               Changed operations pointer type to void *.
*/
#include <linux/module.h>
#include <linux/init.h>
#include <linux/locks.h>
#include <linux/kdev_t.h>
#include <linux/devfs_fs_kernel.h>


/*  Private functions follow  */

static void __init _devfs_convert_name (char *new, const char *old, int disc)
/*  [SUMMARY] Convert from an old style location-based name to new style.
    <new> The new name will be written here.
    <old> The old name.
    <disc> If true, disc partitioning information should be processed.
    [RETURNS] Nothing.
*/
{
    int host, bus, target, lun;
    char *ptr;
    char part[8];

    /*  Decode "c#b#t#u#"  */
    if (old[0] != 'c') return;
    host = simple_strtol (old + 1, &ptr, 10);
    if (ptr[0] != 'b') return;
    bus = simple_strtol (ptr + 1, &ptr, 10);
    if (ptr[0] != 't') return;
    target = simple_strtol (ptr + 1, &ptr, 10);
    if (ptr[0] != 'u') return;
    lun = simple_strtol (ptr + 1, &ptr, 10);
    if (disc)
    {
	/*  Decode "p#"  */
	if (ptr[0] == 'p') sprintf (part, "part%s", ptr + 1);
	else strcpy (part, "disc");
    }
    else part[0] = '\0';
    sprintf (new, "/host%d/bus%d/target%d/lun%d/%s",
	     host, bus, target, lun, part);
}   /*  End Function _devfs_convert_name  */


/*  Public functions follow  */

/*PUBLIC_FUNCTION*/
void __init devfs_make_root (const char *name)
/*  [SUMMARY] Create the root FS device entry if required.
    <name> The name of the root FS device, as passed by "root=".
    [RETURNS] Nothing.
*/
{
    char dest[64];

    if ( (strncmp (name, "sd/", 3) == 0) || (strncmp (name, "sr/", 3) == 0) )
    {
	strcpy (dest, "../scsi");
	_devfs_convert_name (dest + 7, name + 3, (name[1] == 'd') ? 1 : 0);
    }
    else if ( (strncmp (name, "ide/hd/", 7) == 0) ||
	      (strncmp (name, "ide/cd/", 7) == 0) )
    {
	strcpy (dest, "..");
	_devfs_convert_name (dest + 2, name + 7, (name[4] == 'h') ? 1 : 0);
    }
    else return;
    devfs_mk_symlink (NULL, name, 0, DEVFS_FL_DEFAULT, dest, 0, NULL,NULL);
}   /*  End Function devfs_make_root  */

/*PUBLIC_FUNCTION*/
void devfs_register_tape (devfs_handle_t de)
/*  [SUMMARY] Register a tape device in the "/dev/tapes" hierarchy.
    <de> Any tape device entry in the device directory.
    [RETURNS] Nothing.
*/
{
    int pos;
    devfs_handle_t parent, slave;
    char name[16], dest[64];
    static unsigned int tape_counter = 0;
    static devfs_handle_t tape_dir = NULL;

    if (tape_dir == NULL) tape_dir = devfs_mk_dir (NULL, "tapes", 5, NULL);
    parent = devfs_get_parent (de);
    pos = devfs_generate_path (parent, dest + 3, sizeof dest - 3);
    if (pos < 0) return;
    strncpy (dest + pos, "../", 3);
    sprintf (name, "tape%u", tape_counter++);
    devfs_mk_symlink (tape_dir, name, 0, DEVFS_FL_DEFAULT, dest + pos, 0,
		      &slave, NULL);
    devfs_auto_unregister (de, slave);
}   /*  End Function devfs_register_tape  */
EXPORT_SYMBOL(devfs_register_tape);

/*PUBLIC_FUNCTION*/
void devfs_register_series (devfs_handle_t dir, const char *format,
			    unsigned int num_entries, unsigned int flags,
			    unsigned int major, unsigned int minor_start,
			    umode_t mode, uid_t uid, gid_t gid,
			    void *ops, void *info)
/*  [SUMMARY] Register a sequence of device entries.
    <dir> The handle to the parent devfs directory entry. If this is NULL the
    new names are relative to the root of the devfs.
    <format> The printf-style format string. A single "%u" is allowed.
    <flags> A set of bitwise-ORed flags (DEVFS_FL_*).
    <major> The major number. Not needed for regular files.
    <minor_start> The starting minor number. Not needed for regular files.
    <mode> The default file mode.
    <uid> The default UID of the file.
    <guid> The default GID of the file.
    <ops> The <<file_operations>> or <<block_device_operations>> structure.
    This must not be externally deallocated.
    <info> An arbitrary pointer which will be written to the <<private_data>>
    field of the <<file>> structure passed to the device driver. You can set
    this to whatever you like, and change it once the file is opened (the next
    file opened will not see this change).
    [RETURNS] Nothing.
*/
{
    unsigned int count;
    char devname[128];

    for (count = 0; count < num_entries; ++count)
    {
	sprintf (devname, format, count);
	devfs_register (dir, devname, 0, flags, major, minor_start + count,
			mode, uid, gid, ops, info);
    }
}   /*  End Function devfs_register_series  */
EXPORT_SYMBOL(devfs_register_series);
