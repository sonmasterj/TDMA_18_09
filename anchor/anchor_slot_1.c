/* Copyright (c) 2015 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */

#include "sdk_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "bsp.h"
#include "boards.h"
#include "nordic_common.h"
#include "nrf_drv_clock.h"
#include "nrf_drv_spi.h"
#include "nrf_uart.h"
#include "app_util_platform.h"
#include "nrf_gpio.h"
#include "nrf_delay.h"
#include "nrf_log.h"
#include "nrf.h"
#include "app_error.h"
#include "app_util_platform.h"
#include "app_error.h"
#include <string.h>
#include "port_platform.h"
#include "deca_types.h"
#include "deca_param_types.h"
#include "deca_regs.h"
#include "deca_device_api.h"
//#include "nrf_drv_timer.h"
#include "SEGGER_RTT.h"
#include "uart.h"
#include "nrf_drv_gpiote.h"
	
//-----------------dw1000----------------------------
static dwt_config_t config = {
    5,                /* Channel number. */
    DWT_PRF_16M,      /* Pulse repetition frequency. */
    DWT_PLEN_256,     /* Preamble length. Used in TX only. */
    DWT_PAC8,         /* Preamble acquisition chunk size. Used in RX only. */
    3,               /* TX preamble code. Used in TX only. */
    3,               /* RX preamble code. Used in RX only. */
    0,                /* 0 to use standard SFD, 1 to use non-standard SFD. */
    DWT_BR_6M8,       /* Data rate. */
    DWT_PHRMODE_STD,  /* PHY header mode. */
    (256+1 + 8 - 8)     /* SFD timeout (preamble length + 1 + SFD length - PAC size). Used in RX only. */
};

/* Preamble timeout, in multiple of PAC size. See NOTE 3 below. */
#define PRE_TIMEOUT 1000

/* Delay between frames, in UWB microseconds. See NOTE 1 below. */
#define POLL_TX_TO_RESP_RX_DLY_UUS 100 

/*Should be accurately calculated during calibration*/
#define TX_ANT_DLY 16300
#define RX_ANT_DLY 16456	

/* beacon frame message for anchor 1*/
#define ALL_MESS_ID 7
#define ALL_MSG_SN_IDX 2
#define BN_SESSION_ID 10
#define BN_CLUSTER_NUM 11
#define BN_CLUSTER_MAP 12
#define BN_DATA_MAP 16
#define BEACON_TYPE 0x10
#define ALL_MSG_COMMON_LEN 7
#define MESS_TYPE 9
#define POLL_TAG_SLOT 7

/*TDMA */
#define ANCHOR_SLOT 2
static uint8_t beacon[]={0x41,0x88,0,0x11,0x22,0xFF,0xFF,0x00,0x00,BEACON_TYPE,0,0,0,0,0,0,0,0,0,0};
/* twr frame */
/* Frames used in the ranging process. See NOTE 2,3 below. */
static uint8 rx_poll_msg[] = {0x41, 0x88, 0, 0x11, 0x22, 0xff, 0xff, 0x00, 0x00, 0xE0, 0, 0};
static uint8 tx_resp_msg[] = {0x41, 0x88, 0, 0x11, 0x22, 0xff, 0xff, ANCHOR_SLOT, ANCHOR_SLOT, 0xE1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
#define POLL_TYPE 0xE0
#define RESP_POLL 0xE1
#define RESP_MSG_POLL_RX_TS_IDX 10
#define RESP_MSG_RESP_TX_TS_IDX 14
#define RESP_MSG_TS_LEN 4	
#define REST_MSG_DES_ADD 5

/* Buffer to store received response message*/
#define RX_BUF_LEN 30
static uint8 rx_buffer[RX_BUF_LEN];
/* Frame count*/
static uint8 frame_seq_nb = 0;
/* system reg status */
static uint32 status_reg = 0;
/* time stamp variable*/
typedef unsigned long long uint64;
static uint64 poll_rx_ts;
static uint64 resp_tx_ts;
#define POLL_RX_TO_RESP_TX_DLY_UUS  1100
#define RESP_TX_TO_FINAL_RX_DLY_UUS 500
#define UUS_TO_DWT_TIME 65536
/* TDMA superframe*/
#define SP_TIME 50

//--------------dw1000---end---------------

//Led
#define RED_LED_PIN 2

// systick config
 #define SYSTICK_COUNT_ENABLE        1
 #define SYSTICK_INTERRUPT_ENABLE    2
 #define SYSTICK_PER 10000 //10 us incr
static volatile uint32_t TICK_MAX=1000000;
static volatile uint32_t tick=0;
 uint32 offset=0;
 uint32_t time_last,time_current;
 uint8 flag=0;
 uint32_t cnt=0;
 uint32_t temp=0;
 uint8 poll_event_flag=0;
//interrupt systick
void beacon_send();
void SysTick_Handler(void)  {
     tick++;
     
 }
 static uint64 get_rx_timestamp_u64(void)
{
  uint8 ts_tab[5];
  uint64 ts = 0;
  int i;
  dwt_readrxtimestamp(ts_tab);
  for (i = 4; i >= 0; i--)
  {
    ts <<= 8;
    ts |= ts_tab[i];
  }
  return ts;
}
static void resp_msg_set_ts(uint8 *ts_field, const uint64 ts, uint8 len_data)
{
  int i;
  for (i = 0; i < len_data; i++)
  {
    ts_field[i] = (ts >> (i * 8)) & 0xFF;
  }
}
static void resp_msg_get_ts(uint8 *ts_field, uint64 *ts, uint8 len_data)
{
  int i;
  *ts = 0;
  for (i = 0; i < len_data; i++)
  {
    *ts += ts_field[i] << (i * 8);
  }
}
//end of insert and take data to/from rf buffer
void beacon_send()
{
    dwt_syncrxbufptrs();
    beacon[ALL_MSG_SN_IDX] = frame_seq_nb;
    dwt_writetxdata(sizeof(beacon), beacon, 0); /* Zero offset in TX buffer. */
    dwt_writetxfctrl(sizeof(beacon), 0, 1); /* Zero offset in TX buffer, ranging. */
    dwt_starttx(DWT_START_TX_IMMEDIATE );
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS);
    frame_seq_nb ++;
    //printf("res twr\n");
}
void dw1000_init()
{
   /* Setup DW1000 IRQ pin */  
  nrf_gpio_cfg_input(DW1000_IRQ, NRF_GPIO_PIN_NOPULL); 		//irq
  //printf("Singled Sided Two Way Ranging Initiator Example \r\n");
  /* Reset DW1000 */
  reset_DW1000(); 

  /* Set SPI clock to 2MHz */
  port_set_dw1000_slowrate();			
  
  /* Init the DW1000 */
  if (dwt_initialise(DWT_LOADUCODE) == DWT_ERROR)
  {
    //Init of DW1000 Failed
    while (1) {};
  }

  // Set SPI to 8MHz clock
  port_set_dw1000_fastrate();

  /* Configure DW1000. */
  dwt_configure(&config);

  /* Apply default antenna delay value. See NOTE 2 below. */
  dwt_setrxantennadelay(RX_ANT_DLY);
  dwt_settxantennadelay(TX_ANT_DLY);
  /* Set preamble timeout for expected frames. See NOTE 3 below. */
 // dwt_setpreambledetecttimeout(8); // PRE_TIMEOUT
  dwt_setrxtimeout(0); // Maximum value timeout with DW1000 is 65ms  
//  dwt_setinterrupt(DWT_INT_RFCG, 1);
//  dwt_setcallbacks(NULL, &rx_ok_cb, NULL, NULL);
  dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS);
  dwt_setrxaftertxdelay(RESP_TX_TO_FINAL_RX_DLY_UUS);
}

int main(void)
{
    /*Initialization UART*/
    boUART_Init ();
    nrf_gpio_cfg_output(RED_LED_PIN);
    dw1000_init();
    SysTick_Config(SystemCoreClock /SYSTICK_PER); 
    while(1)
   {
     dwt_rxenable(DWT_START_RX_IMMEDIATE);
     /* Poll for reception of a frame or error/timeout. See NOTE 5 below. */
     while (!((status_reg = dwt_read32bitreg(SYS_STATUS_ID)) & (SYS_STATUS_RXFCG | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR)))
     {};
     if (status_reg & SYS_STATUS_RXFCG)
     {
       uint32 frame_len;
       /* Clear good RX frame event in the DW1000 status register. */
       dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG);
       /* A frame has been received, read it into the local buffer. */
       frame_len = dwt_read32bitreg(RX_FINFO_ID) & RX_FINFO_RXFL_MASK_1023;
       if (frame_len <= RX_BUFFER_LEN)
       {
         dwt_readrxdata(rx_buffer, frame_len, 0);
       }
       /* Check that the frame is a poll sent by "SS TWR initiator" example*/
       rx_buffer[ALL_MSG_SN_IDX] = 0;
        if(rx_buffer[MESS_TYPE]==BEACON_TYPE && rx_buffer[3]==0x11)
       {
         tick=0; 
         printf("init\n"); 
       // nrf_gpio_pin_toggle(RED_LED_PIN);
       }
       if(rx_buffer[MESS_TYPE]==POLL_TYPE && rx_buffer[3]==0x11)
       {
         uint8 tag_slot=rx_buffer[POLL_TAG_SLOT];
         flag=1;
         
         while(flag==1)
        {
         if(tick>=(tag_slot*6*10+ANCHOR_SLOT*10) && tick<(tag_slot*6*10+ANCHOR_SLOT*10+15))
         {
               uint32 resp_tx_time;
               poll_rx_ts = get_rx_timestamp_u64();
               resp_tx_time = (poll_rx_ts + ((POLL_RX_TO_RESP_TX_DLY_UUS+975*(ANCHOR_SLOT-1)) * UUS_TO_DWT_TIME)) >> 8;
               dwt_setdelayedtrxtime(resp_tx_time);
               resp_tx_ts = (((uint64)(resp_tx_time & 0xFFFFFFFEUL)) << 8) + TX_ANT_DLY;
               /* Write all timestamps in the final message. See NOTE 8 below. */
               resp_msg_set_ts(&tx_resp_msg[RESP_MSG_POLL_RX_TS_IDX], poll_rx_ts,4);
               resp_msg_set_ts(&tx_resp_msg[RESP_MSG_RESP_TX_TS_IDX], resp_tx_ts,4);
               /* Write and send the response message. See NOTE 9 below. */
               tx_resp_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
               tx_resp_msg[5]=tag_slot;
               tx_resp_msg[6]=tag_slot;
               dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0); /* Zero offset in TX buffer. See Note 5 below.*/
               dwt_writetxfctrl(sizeof(tx_resp_msg), 0, 1); /* Zero offset in TX buffer, ranging. */
               int ret=dwt_starttx(DWT_START_TX_DELAYED);
               if (ret == DWT_SUCCESS)
              {
              /* Poll DW1000 until TX frame sent event set. See NOTE 5 below. */
               while (!(dwt_read32bitreg(SYS_STATUS_ID) & SYS_STATUS_TXFRS))
              {};

              /* Clear TXFRS event. */
              dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS);

              /* Increment frame sequence number after transmission of the poll message (modulo 256). */
             frame_seq_nb++;
            // flag=0;
             printf("ds\n");
             }
            else
            {

              /* Reset RX to properly reinitialise LDE operation. */
              dwt_rxreset();
              printf("error\n");
               
             }
            flag=0;
        }
       }
      }
     }
     else
     {
        /* Clear RX error events in the DW1000 status register. */
        dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        /* Reset RX to properly reinitialise LDE operation. */
        dwt_rxreset();
     }
  }
}
