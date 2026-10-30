/*
 * slcan.c
 *
 *  Created on: 30 paź 2026
 *      Author: patry
 */
/**
  * @file    slcan.c
  * @brief   Lawicel (SLCAN) protocol on STM32L476: bxCAN <-> USART2.
  *
  * Host -> device: ASCII commands terminated by '\r'.
  * Device -> host: '\r' = OK, '\a' (BEL) = error, received frames as text lines.
  *
  * Supported: S0..S8, O, L, C, t, T, r, R, F, V, N, Z0, M, m (M/m are accepted
  * but ignored: all frames pass the hardware filter).
  */
#include "slcan.h"
#include "main.h"
#include <stdbool.h>

extern CAN_HandleTypeDef  hcan1;
extern UART_HandleTypeDef huart2;

#define RXQ_SIZE          512U   /* frames queued between ISR and main loop, power of two */
#define RXQ_MASK          (RXQ_SIZE - 1U)
#define UART_RX_DMA_SIZE  256U   /* circular DMA buffer for host commands */
#define CMD_MAX_LEN       32U    /* longest valid command: 'T' + 8 + 1 + 16 = 26 */

typedef struct {
  uint32_t id;
  uint8_t  dlc;
  uint8_t  ext;
  uint8_t  rtr;
  uint8_t  data[8];
} slcan_msg_t;

static const char HEXCH[] = "0123456789ABCDEF";

/* ---- state shared with interrupts ---- */
static slcan_msg_t rxq[RXQ_SIZE];
static volatile uint16_t rxq_head = 0;        /* written by the CAN ISR     */
static volatile uint16_t rxq_tail = 0;        /* written by the main loop   */
static volatile bool     overflow_flag = false;
static volatile bool     uart_rx_error = false;

/* ---- UART receive and command parser ---- */
static uint8_t  uart_rx_buf[UART_RX_DMA_SIZE];
static uint16_t uart_rx_rd = 0;
static char     cmd_buf[CMD_MAX_LEN];
static uint8_t  cmd_len = 0;
static bool     cmd_overflow = false;

/* ---- channel state ---- */
static bool    channel_open = false;
static bool    channel_listen_only = false;
static uint8_t speed_index = 6;               /* S6 = 500 kbit/s */

/* ===================== helpers ===================== */

static void uart_send(const char *s, uint16_t n)
{
  HAL_UART_Transmit(&huart2, (uint8_t *)s, n, 50);
}

static void reply_ok(void)  { uart_send("\r", 1); }
static void reply_err(void) { uart_send("\a", 1); }

static int8_t hex_val(char c)
{
  if (c >= '0' && c <= '9') return (int8_t)(c - '0');
  if (c >= 'A' && c <= 'F') return (int8_t)(c - 'A' + 10);
  if (c >= 'a' && c <= 'f') return (int8_t)(c - 'a' + 10);
  return -1;
}

static bool parse_hex(const char *s, uint8_t digits, uint32_t *out)
{
  uint32_t v = 0;
  for (uint8_t i = 0; i < digits; i++) {
    int8_t h = hex_val(s[i]);
    if (h < 0) return false;
    v = (v << 4) | (uint32_t)h;
  }
  *out = v;
  return true;
}

static char *put_hex(char *p, uint32_t v, uint8_t digits)
{
  for (int8_t i = (int8_t)digits - 1; i >= 0; i--) {
    *p++ = HEXCH[(v >> (4 * i)) & 0xFU];
  }
  return p;
}

/** Format a received frame: t/T/r/R + ID + DLC + data + '\r'. */
static uint8_t fmt_slcan(char *out, const slcan_msg_t *m)
{
  char *p = out;
  *p++ = m->rtr ? (m->ext ? 'R' : 'r') : (m->ext ? 'T' : 't');
  p = put_hex(p, m->id, m->ext ? 8U : 3U);
  *p++ = (char)('0' + m->dlc);
  if (!m->rtr) {
    for (uint8_t i = 0; i < m->dlc; i++) p = put_hex(p, m->data[i], 2U);
  }
  *p++ = '\r';
  return (uint8_t)(p - out);
}

/* ===================== CAN control ===================== */

/** Accept-all receive filter (bank 0 -> FIFO0). */
static void can_filter_config(void)
{
  CAN_FilterTypeDef f = {0};
  f.FilterBank           = 0;
  f.FilterMode           = CAN_FILTERMODE_IDMASK;
  f.FilterScale          = CAN_FILTERSCALE_32BIT;
  f.FilterMaskIdHigh     = 0x0000;
  f.FilterMaskIdLow      = 0x0000;
  f.FilterFIFOAssignment = CAN_RX_FIFO0;
  f.FilterActivation     = ENABLE;
  if (HAL_CAN_ConfigFilter(&hcan1, &f) != HAL_OK) Error_Handler();
}

/**
  * Compute bit timing for the S0..S8 bit rates from the real APB1 clock.
  * Prefers 16 TQ (sample point 87.5 %), falls back to 20 TQ (85 %, needed for 800k).
  */
static bool compute_timing(uint8_t speed, uint32_t *presc, uint32_t *bs1, uint32_t *bs2)
{
  static const uint32_t bitrate[9] = {
    10000, 20000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000
  };
  static const struct { uint32_t tq, bs1, bs2; } layout[2] = {
    { 16U, CAN_BS1_13TQ, CAN_BS2_2TQ },
    { 20U, CAN_BS1_16TQ, CAN_BS2_3TQ },
  };
  const uint32_t pclk = HAL_RCC_GetPCLK1Freq();

  for (uint8_t i = 0; i < 2U; i++) {
    uint32_t div = bitrate[speed] * layout[i].tq;
    if ((pclk % div) == 0U) {
      uint32_t p = pclk / div;
      if (p >= 1U && p <= 1024U) {
        *presc = p; *bs1 = layout[i].bs1; *bs2 = layout[i].bs2;
        return true;
      }
    }
  }
  return false;
}

static bool channel_start(bool listen_only)
{
  uint32_t presc, bs1, bs2;

  if (channel_open) return false;
#if SLCAN_LISTEN_ONLY_BUILD
  if (!listen_only) return false;
#endif
  if (!compute_timing(speed_index, &presc, &bs1, &bs2)) return false;

  /* While the channel is closed the peripheral is in initialization mode
     (after HAL_CAN_Init() and after HAL_CAN_Stop()), so BTR is writable. */
  hcan1.Instance->BTR = (listen_only ? CAN_MODE_SILENT : CAN_MODE_NORMAL)
                      | CAN_SJW_1TQ | bs1 | bs2 | (presc - 1U);

  rxq_head = 0;
  rxq_tail = 0;
  overflow_flag = false;

  if (HAL_CAN_Start(&hcan1) != HAL_OK) return false;
  if (HAL_CAN_ActivateNotification(&hcan1,
        CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_RX_FIFO0_OVERRUN) != HAL_OK) return false;

  channel_open = true;
  channel_listen_only = listen_only;
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);   /* LED on = channel open */
  return true;
}

static void channel_stop(void)
{
  HAL_CAN_AbortTxRequest(&hcan1, CAN_TX_MAILBOX0 | CAN_TX_MAILBOX1 | CAN_TX_MAILBOX2);
  HAL_CAN_Stop(&hcan1);
  channel_open = false;
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);
}

/** Handle t/T/r/R: parse the line and queue the frame for transmission. */
static bool tx_frame(void)
{
#if SLCAN_LISTEN_ONLY_BUILD
  return false;                                   /* receive-only build: never transmit */
#else
  if (!channel_open || channel_listen_only) return false;

  const bool ext = (cmd_buf[0] == 'T' || cmd_buf[0] == 'R');
  const bool rtr = (cmd_buf[0] == 'r' || cmd_buf[0] == 'R');
  const uint8_t idlen = ext ? 8U : 3U;
  uint32_t id;

  if (cmd_len < (uint8_t)(idlen + 2U)) return false;
  if (!parse_hex(&cmd_buf[1], idlen, &id)) return false;
  if (id > (ext ? 0x1FFFFFFFU : 0x7FFU)) return false;

  const char dc = cmd_buf[1U + idlen];
  if (dc < '0' || dc > '8') return false;
  const uint8_t dlc = (uint8_t)(dc - '0');
  if (cmd_len != (uint8_t)(idlen + 2U + (rtr ? 0U : (2U * dlc)))) return false;

  uint8_t data[8] = {0};
  if (!rtr) {
    for (uint8_t i = 0; i < dlc; i++) {
      uint32_t b;
      if (!parse_hex(&cmd_buf[2U + idlen + (2U * i)], 2U, &b)) return false;
      data[i] = (uint8_t)b;
    }
  }

  CAN_TxHeaderTypeDef h = {0};
  h.StdId = ext ? 0U : id;
  h.ExtId = ext ? id : 0U;
  h.IDE   = ext ? CAN_ID_EXT : CAN_ID_STD;
  h.RTR   = rtr ? CAN_RTR_REMOTE : CAN_RTR_DATA;
  h.DLC   = dlc;
  h.TransmitGlobalTime = DISABLE;

  uint32_t mailbox;
  return HAL_CAN_AddTxMessage(&hcan1, &h, data, &mailbox) == HAL_OK;
#endif
}

/* ===================== command parser ===================== */

static void handle_command(void)
{
  if (cmd_overflow) { reply_err(); return; }
  if (cmd_len == 0U) { reply_ok(); return; }          /* empty line: harmless */

  switch (cmd_buf[0]) {
  case 'S':                                           /* S0..S8: bit rate, channel must be closed */
    if (cmd_len == 2U && cmd_buf[1] >= '0' && cmd_buf[1] <= '8' && !channel_open) {
      speed_index = (uint8_t)(cmd_buf[1] - '0');
      reply_ok();
    } else {
      reply_err();
    }
    break;

  case 'O':                                           /* open, normal mode */
    if (cmd_len == 1U && channel_start(false)) reply_ok(); else reply_err();
    break;

  case 'L':                                           /* open, listen-only (silent) */
    if (cmd_len == 1U && channel_start(true)) reply_ok(); else reply_err();
    break;

  case 'C':                                           /* close; OK even if already closed */
    if (cmd_len == 1U) { if (channel_open) channel_stop(); reply_ok(); } else reply_err();
    break;

  case 't': case 'T': case 'r': case 'R':             /* transmit a frame */
    if (tx_frame()) {
      uart_send((cmd_buf[0] == 'T' || cmd_buf[0] == 'R') ? "Z\r" : "z\r", 2);
    } else {
      reply_err();
    }
    break;

  case 'F': {                                         /* status flags (bits as in the Lawicel spec) */
    if (cmd_len != 1U || !channel_open) { reply_err(); break; }
    uint32_t esr = hcan1.Instance->ESR;
    uint8_t f = 0;
    if ((esr & CAN_ESR_EWGF) != 0U) f |= 0x04U;       /* error warning */
    if ((esr & CAN_ESR_EPVF) != 0U) f |= 0x20U;       /* error passive */
    if ((esr & CAN_ESR_BOFF) != 0U) f |= 0x80U;       /* bus off       */
    if (overflow_flag) { f |= 0x08U; overflow_flag = false; }   /* data overrun */
    char b[4] = { 'F', HEXCH[f >> 4], HEXCH[f & 0xFU], '\r' };
    uart_send(b, 4);
    break;
  }

  case 'V': if (cmd_len == 1U) uart_send("V0101\r", 6); else reply_err(); break;
  case 'N': if (cmd_len == 1U) uart_send("N0001\r", 6); else reply_err(); break;

  case 'Z':                                           /* timestamps are not supported (Z0 only) */
    if (cmd_len == 2U && cmd_buf[1] == '0') reply_ok(); else reply_err();
    break;

  case 'M': case 'm':                                 /* acceptance code/mask: accepted, ignored */
    reply_ok();
    break;

  default:
    reply_err();
    break;
  }
}

static void feed_byte(uint8_t c)
{
  if (c == '\r') {
    handle_command();
    cmd_len = 0;
    cmd_overflow = false;
  } else if (c == '\n') {
    /* ignore LF so that terminals sending CR+LF work */
  } else if (cmd_len < CMD_MAX_LEN) {
    cmd_buf[cmd_len++] = (char)c;
  } else {
    cmd_overflow = true;
  }
}

/* ===================== public API ===================== */

void slcan_init(void)
{
  can_filter_config();
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);
  if (HAL_UART_Receive_DMA(&huart2, uart_rx_buf, UART_RX_DMA_SIZE) != HAL_OK) Error_Handler();
}

void slcan_poll(void)
{
  /* Recover the UART receiver after an overrun/noise/framing error */
  if (uart_rx_error) {
    uart_rx_error = false;
    HAL_UART_AbortReceive(&huart2);
    __HAL_UART_CLEAR_FLAG(&huart2, UART_CLEAR_OREF | UART_CLEAR_NEF | UART_CLEAR_FEF | UART_CLEAR_PEF);
    uart_rx_rd = 0;
    cmd_len = 0;
    cmd_overflow = false;
    HAL_UART_Receive_DMA(&huart2, uart_rx_buf, UART_RX_DMA_SIZE);
  }

  /* 1) Commands from the host (bytes written by the circular DMA) */
  const uint16_t wr = (uint16_t)(UART_RX_DMA_SIZE - __HAL_DMA_GET_COUNTER(huart2.hdmarx)) & (UART_RX_DMA_SIZE - 1U);
  while (uart_rx_rd != wr) {
    feed_byte(uart_rx_buf[uart_rx_rd]);
    uart_rx_rd = (uint16_t)((uart_rx_rd + 1U) & (UART_RX_DMA_SIZE - 1U));
  }

  /* 2) Received CAN frames to the host (a few per call keeps command latency low) */
  for (uint8_t n = 0; n < 8U && rxq_tail != rxq_head; n++) {
    char line[32];
    const uint8_t len = fmt_slcan(line, &rxq[rxq_tail]);
    rxq_tail = (uint16_t)((rxq_tail + 1U) & RXQ_MASK);
    uart_send(line, len);
  }
}

/* ===================== HAL callbacks ===================== */

/** Rx FIFO0 interrupt: copy every pending frame to the software queue. */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
  CAN_RxHeaderTypeDef hdr;
  uint8_t d[8];

  while (HAL_CAN_GetRxFifoFillLevel(hcan, CAN_RX_FIFO0) > 0U) {
    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &hdr, d) != HAL_OK) break;

    const uint16_t next = (uint16_t)((rxq_head + 1U) & RXQ_MASK);
    if (next == rxq_tail) { overflow_flag = true; continue; }   /* queue full */

    slcan_msg_t *m = &rxq[rxq_head];
    m->ext = (hdr.IDE == CAN_ID_EXT) ? 1U : 0U;
    m->id  = m->ext ? hdr.ExtId : hdr.StdId;
    m->rtr = (hdr.RTR == CAN_RTR_REMOTE) ? 1U : 0U;
    m->dlc = (uint8_t)hdr.DLC;
    for (uint8_t i = 0; i < 8U; i++) m->data[i] = d[i];
    rxq_head = next;
  }
}

void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
  if ((hcan->ErrorCode & HAL_CAN_ERROR_RX_FOV0) != 0U) overflow_flag = true;
  HAL_CAN_ResetError(hcan);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART2) uart_rx_error = true;
}

