#ifndef AUDIO_OUTPUT_TEST_SYS_H
#define AUDIO_OUTPUT_TEST_SYS_H
#define GLAZE_SYS_H
#include <glaze/abi.h>
int gz_audio_status(struct gz_audio_status *);
int gz_audio_start(unsigned,unsigned);
int gz_audio_stop(bool);
int gz_audio_write(const void *,unsigned);
#endif
