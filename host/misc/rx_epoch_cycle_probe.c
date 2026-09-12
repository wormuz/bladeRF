/* In-process RX epoch cycles: sync_config -> enable -> sync_rx -> disable,
 * N times on one open device. This is the shape that exposed the RF-link
 * epoch defects (FX3 resets the fabric inside RF_RX enable; the Nios cfg
 * shadow went stale across that reset), and it is the acceptance check
 * for them: every cycle must deliver samples, and at verbose the status
 * words logged by libbladeRF must show violation=0 and no faults.
 *
 *   cc -o rx_epoch_cycle_probe rx_epoch_cycle_probe.c -lbladeRF
 *   ./rx_epoch_cycle_probe 6
 */
#include <libbladeRF.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
int main(int argc, char**argv){
  struct bladerf *d=NULL; int st; int n=(argc>1)?atoi(argv[1]):4;
  bladerf_set_usb_reset_on_open(false);
  if((st=bladerf_open(&d,NULL))){printf("open %s\n",bladerf_strerror(st));return 1;}
  bladerf_set_frequency(d,BLADERF_CHANNEL_RX(0),925000000);
  bladerf_set_sample_rate(d,BLADERF_CHANNEL_RX(0),61440000,NULL);
  int16_t*b=malloc(50000*2*sizeof(int16_t));
  for(int i=0;i<n;i++){
    bladerf_sync_config(d,BLADERF_RX_X1,BLADERF_FORMAT_SC16_Q11,16,8192,8,1000);
    bladerf_enable_module(d,BLADERF_CHANNEL_RX(0),true);
    st=bladerf_sync_rx(d,b,50000,NULL,1000);
    printf("  cycle %d: %s\n",i+1,st?"FAIL":"ok");
    bladerf_enable_module(d,BLADERF_CHANNEL_RX(0),false);
  }
  free(b); bladerf_close(d); return 0;
}
