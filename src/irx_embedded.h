#ifndef PS2_IRX_EMBEDDED_H
#define PS2_IRX_EMBEDDED_H

/*
	Declarations for the IOP modules embedded into the ELF by the
	Makefile.  bin2c already emits a 16 byte aligned array.
*/

#define PS2_DECL_IRX(name) \
	extern unsigned char name##_irx[]; \
	extern unsigned int size_##name##_irx

PS2_DECL_IRX(iomanX);
PS2_DECL_IRX(fileio);
PS2_DECL_IRX(usbd);
PS2_DECL_IRX(bdm);
PS2_DECL_IRX(bdmfs_fatfs);
PS2_DECL_IRX(usbmass_bd);
PS2_DECL_IRX(sio2man);
PS2_DECL_IRX(padman);
PS2_DECL_IRX(ps2kbd);
PS2_DECL_IRX(ps2mouse);
PS2_DECL_IRX(audsrv);

#endif /* PS2_IRX_EMBEDDED_H */
