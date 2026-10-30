/*
 * slcan.h
 *
 *  Created on: 30 paź 2026
 *      Author: patry
 */
#ifndef SLCAN_H
#define SLCAN_H

/**
 * 1 = receive-only build: the 'O' command and every transmit command are
 *     refused, only 'L' (listen-only) can open the channel. Use it in vehicles.
 */
#ifndef SLCAN_LISTEN_ONLY_BUILD
#define SLCAN_LISTEN_ONLY_BUILD 0
#endif

/** Call once after MX_CAN1_Init() and MX_USART2_UART_Init(). */
void slcan_init(void);

/** Call repeatedly from the main loop. */
void slcan_poll(void);

#endif /* SLCAN_H */
