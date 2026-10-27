/*----------------------------------------------*/
/* TJpgDec System Configurations R0.03          */
/* pico-os settings                             */
/*----------------------------------------------*/

#define JD_SZBUF        512  /* stream input buffer */
#define JD_FORMAT       0    /* output RGB888 */
#define JD_USE_SCALE    1    /* decode at 1/1, 1/2, 1/4 or 1/8 size */
#define JD_TBLCLIP      1    /* table saturation: faster, +1 KB code */
#define JD_FASTDECODE   1    /* 32-bit barrel shifter */
