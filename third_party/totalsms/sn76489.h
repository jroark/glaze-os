/* Glaze integer SN76489 backend; compatible with the subset used by TotalSMS. */
#ifndef GLAZE_SMS_SN76489_H
#define GLAZE_SMS_SN76489_H
#include <stdint.h>
typedef struct Sn76489 Sn76489;
enum { Sn76489LfsrTappedBit_SMS=9,Sn76489LfsrFeedBit_SMS=15,Sn76489LfsrTappedBit_SG=3,Sn76489LfsrFeedBit_SG=14 };
Sn76489 *psg_init(uint32_t clock,uint32_t rate);
void psg_quit(Sn76489 *p);
void psg_reset(Sn76489 *p,unsigned taps,unsigned feed);
void psg_write_io(Sn76489 *p,unsigned value,unsigned time);
void psg_gg_write_io(Sn76489 *p,unsigned value,unsigned time);
void psg_set_channel_volume(Sn76489 *p,unsigned channel,unsigned volume);
void psg_set_master_volume(Sn76489 *p,unsigned volume);
void psg_update_timestamp(Sn76489 *p,int delta);
void psg_end_frame(Sn76489 *p,unsigned time);
int psg_samples_avaliable(const Sn76489 *p);
int psg_read_samples(Sn76489 *p,short *out,int count);
void psg_clear_samples(Sn76489 *p);
#endif
