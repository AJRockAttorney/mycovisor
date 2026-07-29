#include "lpuart.h"
#include "main.h"
#include "ring_buff.h"


#define LPUART_RX_BUF_LEN 256
#define LPUART_TX_BUF_LEN 256
#define LPUART_RX_STAGING_LEN 512
static uint8_t lpuart_rx_dma_buf[LPUART_RX_BUF_LEN];
static uint8_t tx_rb_buf[LPUART_TX_BUF_LEN];
static uint8_t rx_staging_buf[LPUART_RX_STAGING_LEN];
static LL_DMA_LinkNodeTypeDef lpuart_rx_dma_node;
static lpuart_rx_handler_t rx_handler;


static ring_buf_t tx_rb;
static ring_buf_t rx_staging;

volatile uint32_t rx_overflow;

static volatile size_t tx_dma_len;

static bool lpuart_start_tx(void);
static void rx_stage_handler(const uint8_t *data, size_t len);

volatile uint32_t rx_ore_count;   /* overrun: peripheral RDR overwritten before DMA read it */
volatile uint32_t rx_fe_count;    /* framing error: stop bit not where expected (baud/noise) */
volatile uint32_t rx_ne_count;    /* noise error: line noise detected during a bit sample */
volatile uint32_t rx_dma_err_count; /* GPDMA1 channel 3 data-transfer error */

static void lpuart_rx_error_check(void) {
    if (LL_LPUART_IsActiveFlag_ORE(LPUART1)) {
        LL_LPUART_ClearFlag_ORE(LPUART1);
        rx_ore_count++;
    }
    if (LL_LPUART_IsActiveFlag_FE(LPUART1)) {
        LL_LPUART_ClearFlag_FE(LPUART1);
        rx_fe_count++;
    }
    if (LL_LPUART_IsActiveFlag_NE(LPUART1)) {
        LL_LPUART_ClearFlag_NE(LPUART1);
        rx_ne_count++;
    }
}

static void lpuart_rx_dma_error_check(void) {
    if (LL_DMA_IsActiveFlag_DTE(GPDMA1, LL_DMA_CHANNEL_3)) {
        LL_DMA_ClearFlag_DTE(GPDMA1, LL_DMA_CHANNEL_3);
        rx_dma_err_count++;
    }
}

void lpuart_set_rx_handler(lpuart_rx_handler_t handler) {
    rx_handler = handler;
}

static void lpuart_rx_check(void) {
    static size_t old_pos;
    size_t pos;

    pos=sizeof(lpuart_rx_dma_buf) - LL_DMA_GetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_3);
    if (pos != old_pos) {
        if (pos>old_pos) {
            if (rx_handler) rx_handler(&lpuart_rx_dma_buf[old_pos], pos-old_pos);
        }
        else {
            if (rx_handler) rx_handler(&lpuart_rx_dma_buf[old_pos], sizeof(lpuart_rx_dma_buf)-old_pos);
            if (pos > 0) {
                if (rx_handler) rx_handler(&lpuart_rx_dma_buf[0],pos);
            }
        }
        old_pos=pos;
    }
}

void lpuart_rx_init(void) {
    LL_DMA_InitNodeTypeDef NodeConfig = {0};
    LL_DMA_InitLinkedListTypeDef DMA_InitLinkedListStruct = {0};

    NodeConfig.DestAllocatedPort = LL_DMA_DEST_ALLOCATED_PORT1;
    NodeConfig.DestHWordExchange = LL_DMA_DEST_HALFWORD_PRESERVE;
    NodeConfig.DestByteExchange = LL_DMA_DEST_BYTE_PRESERVE;
    NodeConfig.DestBurstLength = 1;
    NodeConfig.DestIncMode = LL_DMA_DEST_INCREMENT;
    NodeConfig.DestDataWidth = LL_DMA_DEST_DATAWIDTH_BYTE;
    NodeConfig.SrcAllocatedPort = LL_DMA_SRC_ALLOCATED_PORT0;
    NodeConfig.SrcByteExchange = LL_DMA_SRC_BYTE_PRESERVE;
    NodeConfig.DataAlignment = LL_DMA_DATA_ALIGN_ZEROPADD;
    NodeConfig.SrcBurstLength = 1;
    NodeConfig.SrcIncMode = LL_DMA_SRC_FIXED;
    NodeConfig.SrcDataWidth = LL_DMA_SRC_DATAWIDTH_BYTE;
    NodeConfig.TransferEventMode = LL_DMA_TCEM_BLK_TRANSFER;
    NodeConfig.TriggerPolarity = LL_DMA_TRIG_POLARITY_MASKED;
    NodeConfig.BlkHWRequest = LL_DMA_HWREQUEST_SINGLEBURST;
    NodeConfig.Direction = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
    NodeConfig.Request = LL_GPDMA1_REQUEST_LPUART1_RX;
    NodeConfig.UpdateRegisters = (LL_DMA_UPDATE_CTR1 | LL_DMA_UPDATE_CTR2 | LL_DMA_UPDATE_CBR1 | LL_DMA_UPDATE_CSAR
                                  | LL_DMA_UPDATE_CDAR | LL_DMA_UPDATE_CTR3 | LL_DMA_UPDATE_CBR2 | LL_DMA_UPDATE_CLLR);
    NodeConfig.NodeType = LL_DMA_GPDMA_LINEAR_NODE;
    /* Additional settings */
    NodeConfig.SrcAddress = LL_LPUART_DMA_GetRegAddr(LPUART1, LL_LPUART_DMA_REG_DATA_RECEIVE);
    NodeConfig.DestAddress = (uint32_t)lpuart_rx_dma_buf;
    /* Size is always in bytes! Width is determined by source and destination numbers */
    NodeConfig.BlkDataLength = sizeof(lpuart_rx_dma_buf);

    LL_DMA_CreateLinkNode(&NodeConfig, &lpuart_rx_dma_node);

    LL_DMA_ConnectLinkNode(&lpuart_rx_dma_node, LL_DMA_CLLR_OFFSET5,
                           &lpuart_rx_dma_node, LL_DMA_CLLR_OFFSET5);

    /*Connect DMA Node to Channel 3*/
    LL_DMA_SetLinkedListBaseAddr(GPDMA1, LL_DMA_CHANNEL_3, (uint32_t)&lpuart_rx_dma_node);
    LL_DMA_ConfigLinkUpdate(GPDMA1, LL_DMA_CHANNEL_3,
                            (LL_DMA_UPDATE_CTR1 | LL_DMA_UPDATE_CTR2 | LL_DMA_UPDATE_CBR1 | LL_DMA_UPDATE_CSAR
                             | LL_DMA_UPDATE_CDAR | LL_DMA_UPDATE_CTR3 | LL_DMA_UPDATE_CBR2 | LL_DMA_UPDATE_CLLR),
                            (uint32_t)&lpuart_rx_dma_node);

    DMA_InitLinkedListStruct.Priority = LL_DMA_LOW_PRIORITY_LOW_WEIGHT;
    DMA_InitLinkedListStruct.LinkStepMode = LL_DMA_LSM_FULL_EXECUTION;
    DMA_InitLinkedListStruct.LinkAllocatedPort = LL_DMA_LINK_ALLOCATED_PORT0;
    DMA_InitLinkedListStruct.TransferEventMode = LL_DMA_TCEM_LAST_LLITEM_TRANSFER;
    LL_DMA_List_Init(GPDMA1, LL_DMA_CHANNEL_3, &DMA_InitLinkedListStruct);

    LL_DMA_EnableIT_HT(GPDMA1,  LL_DMA_CHANNEL_3);
    LL_DMA_EnableIT_TC(GPDMA1,  LL_DMA_CHANNEL_3);
    LL_DMA_EnableIT_DTE(GPDMA1, LL_DMA_CHANNEL_3);

    NVIC_SetPriority(GPDMA1_Channel3_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 0, 0));
    NVIC_EnableIRQ(GPDMA1_Channel3_IRQn);

    LL_LPUART_EnableIT_IDLE(LPUART1);
    /* CR3.EIE: without this, ORE/FE/NE flags still get set in ISR but never
       raise an interrupt, so LPUART1_IRQHandler would only notice them
       incidentally on the next IDLE -- and ORE left uncleared can stall
       further reception in the meantime. */
    LL_LPUART_EnableIT_ERROR(LPUART1);

    LL_LPUART_EnableDMAReq_RX(LPUART1);
    LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_3);

    rb_init(&rx_staging, rx_staging_buf, sizeof(rx_staging_buf));
}

/* Bumped once per staging event (ISR context only), so callers driving a
   parser off rx_staging can tell whether anything new arrived since they
   last checked, instead of re-polling blind at main-loop speed. */
volatile uint32_t rx_staged_events;

static void rx_stage_handler(const uint8_t *data, size_t len) {
    if (!rb_write(&rx_staging, data, len)) {
        rx_overflow++;
    }
    rx_staged_events++;
}
void lpuart_tx_raw(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        while (!LL_LPUART_IsActiveFlag_TXE(LPUART1)) { }   // wait for room
        LL_LPUART_TransmitData8(LPUART1, data[i]);
    }
}

void lpuart_tx_init(void) {
    LL_DMA_InitTypeDef DMA_InitStruct = {0};
    DMA_InitStruct.Direction = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
    DMA_InitStruct.BlkHWRequest = LL_DMA_HWREQUEST_SINGLEBURST;
    DMA_InitStruct.DataAlignment = LL_DMA_DATA_ALIGN_ZEROPADD;
    DMA_InitStruct.SrcBurstLength = 1;
    DMA_InitStruct.DestBurstLength = 1;
    DMA_InitStruct.SrcDataWidth = LL_DMA_SRC_DATAWIDTH_BYTE;
    DMA_InitStruct.DestDataWidth = LL_DMA_DEST_DATAWIDTH_BYTE;
    DMA_InitStruct.SrcIncMode = LL_DMA_SRC_INCREMENT;
    DMA_InitStruct.DestIncMode = LL_DMA_DEST_FIXED;
    DMA_InitStruct.Priority = LL_DMA_LOW_PRIORITY_MID_WEIGHT;
    DMA_InitStruct.TriggerMode = LL_DMA_TRIGM_BLK_TRANSFER;
    DMA_InitStruct.TriggerPolarity = LL_DMA_TRIG_POLARITY_MASKED;
    DMA_InitStruct.TriggerSelection = 0x00000000U;
    DMA_InitStruct.Request = LL_GPDMA1_REQUEST_LPUART1_TX;
    DMA_InitStruct.TransferEventMode = LL_DMA_TCEM_BLK_TRANSFER;
    DMA_InitStruct.SrcAllocatedPort = LL_DMA_SRC_ALLOCATED_PORT1;
    DMA_InitStruct.DestAllocatedPort = LL_DMA_DEST_ALLOCATED_PORT0;
    DMA_InitStruct.LinkAllocatedPort = LL_DMA_LINK_ALLOCATED_PORT1;
    DMA_InitStruct.LinkStepMode = LL_DMA_LSM_FULL_EXECUTION;
    DMA_InitStruct.LinkedListBaseAddr = 0x00000000U;
    DMA_InitStruct.LinkedListAddrOffset = 0x00000000U;
    DMA_InitStruct.DestAddress = LL_LPUART_DMA_GetRegAddr(LPUART1, LL_LPUART_DMA_REG_DATA_TRANSMIT);
    /* Default settings - updated prior start of transmission */
    DMA_InitStruct.SrcAddress = 0x00000000U;
    DMA_InitStruct.BlkDataLength = 0x00000000U;
    LL_DMA_Init(GPDMA1, LL_DMA_CHANNEL_1, &DMA_InitStruct);

    /* Enable TC interrupt */
    LL_LPUART_EnableDMAReq_TX(LPUART1);
    LL_DMA_EnableIT_TC(GPDMA1, LL_DMA_CHANNEL_1);

    /* GPDMA1 interrupt Init */
    NVIC_SetPriority(GPDMA1_Channel1_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 5, 0));
    NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);

    rb_init(&tx_rb, tx_rb_buf, sizeof(tx_rb_buf));
}
static bool lpuart_start_tx(void) {
    bool started = false;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (tx_dma_len == 0) {
        rb_block_t block = rb_get_linear_block(&tx_rb);
        if (block.len != 0) {
            LL_DMA_ClearFlag_TC(GPDMA1, LL_DMA_CHANNEL_1);
            LL_DMA_SetSrcAddress(GPDMA1, LL_DMA_CHANNEL_1, (uint32_t)block.ptr);
            LL_DMA_SetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_1, block.len);
            tx_dma_len = block.len;
            LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_1);
            started = true;
        }
    }

    __set_PRIMASK(primask);
    return started;
}

bool lpuart_write(const uint8_t *data, size_t len) {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    bool ok = rb_write(&tx_rb, data, len);
    lpuart_start_tx();
    __set_PRIMASK(primask);
    return ok;
}

static void lpuart_tx_complete(void) {
    rb_skip(&tx_rb, tx_dma_len);
    tx_dma_len=0;
    lpuart_start_tx();
}

void lpuart_tx_dma_irq(void) {
    if (LL_DMA_IsEnabledIT_TC(GPDMA1, LL_DMA_CHANNEL_1) &&
        LL_DMA_IsActiveFlag_TC(GPDMA1, LL_DMA_CHANNEL_1)) {
        LL_DMA_ClearFlag_TC(GPDMA1, LL_DMA_CHANNEL_1);
        lpuart_tx_complete();
    }
}

void lpuart_rx_dma_irq(void) {
    if (LL_DMA_IsEnabledIT_HT(GPDMA1, LL_DMA_CHANNEL_3) && LL_DMA_IsActiveFlag_HT(GPDMA1, LL_DMA_CHANNEL_3)) {
        LL_DMA_ClearFlag_HT(GPDMA1, LL_DMA_CHANNEL_3);
        lpuart_rx_check();
    }
    if (LL_DMA_IsEnabledIT_TC(GPDMA1, LL_DMA_CHANNEL_3) && LL_DMA_IsActiveFlag_TC(GPDMA1, LL_DMA_CHANNEL_3)) {
        LL_DMA_ClearFlag_TC(GPDMA1, LL_DMA_CHANNEL_3);
        lpuart_rx_check();
    }
    if (LL_DMA_IsEnabledIT_DTE(GPDMA1, LL_DMA_CHANNEL_3) && LL_DMA_IsActiveFlag_DTE(GPDMA1, LL_DMA_CHANNEL_3)) {
        lpuart_rx_dma_error_check();
    }
}

void lpuart_uart_irq(void) {
    if (LL_LPUART_IsActiveFlag_IDLE(LPUART1)) {
        LL_LPUART_ClearFlag_IDLE(LPUART1);
        lpuart_rx_check();
    }
    lpuart_rx_error_check();
}

void lpuart_init(void) {
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);

    lpuart_rx_init();
    lpuart_tx_init();
    lpuart_set_rx_handler(rx_stage_handler);
}

size_t lpuart_read(uint8_t *dst, size_t max) {
    return rb_read(&rx_staging, dst, max);
}