/*
 *  This file is part of the Arduino_10BASE_T1S library.
 *
 *  Copyright (c) 2024 Arduino SA
 *
 *  This Source Code Form is subject to the terms of the Mozilla Public
 *  License, v. 2.0. If a copy of the MPL was not distributed with this
 *  file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/**************************************************************************************
 * INCLUDE
 **************************************************************************************/

#include "TC6_Arduino_10BASE_T1S.h"

#include "lib/libtc6/inc/tc6-regs.h"

#include "lib/liblwip/include/lwip/init.h"
#include "lib/liblwip/include/lwip/timeouts.h"
#include "lib/liblwip/include/netif/etharp.h"

/**************************************************************************************
 * NAMESPACE
 **************************************************************************************/

namespace TC6
{

/**************************************************************************************
 * DEFINE
 **************************************************************************************/

#define TC6LwIP_HOSTNAME            "tc6"
#define TC6LwIP_MTU                 (1536u)

/**************************************************************************************
 * GLOBAL CONSTANTS
 **************************************************************************************/

struct TC6ListNode {
  TC6LwIP_t * inst = nullptr;
  struct TC6ListNode * next = nullptr;
};

static TC6ListNode * tc6_lwip_instance_list_head = nullptr;

/**************************************************************************************
 * MODULE INTERNAL FUNCTION DECLARATION
 **************************************************************************************/

static TC6LwIP_t * GetContextNetif(struct netif *intf)
{
  for (TC6ListNode * ptr = tc6_lwip_instance_list_head;
       ptr != nullptr;
       ptr = ptr->next)
  {
    /* Compare memory address to retrieve the right
     * data structure.
     */
    if (&ptr->inst->ip.netint == intf)
      return ptr->inst;
  }
  return nullptr;
}

static TC6LwIP_t *GetContextTC6(TC6_t *pTC6)
{
  for (TC6ListNode * ptr = tc6_lwip_instance_list_head;
       ptr != nullptr;
       ptr = ptr->next)
  {
    /* Compare memory address to retrieve the right
     * data structure.
     */
    if (ptr->inst->tc.tc6 == pTC6)
      return ptr->inst;
  }
  return nullptr;
}

static void OnPlcaStatus(TC6_t *pInst, bool success, uint32_t addr, uint32_t value, void *tag, void *pGlobalTag)
{
  TC6LwIP_t *lw = (TC6LwIP_t *) tag;
  (void) pInst;
  (void) addr;
  (void) pGlobalTag;
  if ((NULL != lw) && (NULL != lw->tc.pStatusCallback))
  {
    bool status = false;
    if (success)
    {
      status = (0u != ((1u << 15) & value));
    }
    lw->tc.pStatusCallback(success, status);
  }
}

static err_t lwIpInit(struct netif *netif);
static err_t lwIpOut(struct netif *netif, struct pbuf *p);

/**************************************************************************************
 * CTOR/DTOR
 **************************************************************************************/

TC6_Arduino_10BASE_T1S::TC6_Arduino_10BASE_T1S(TC6_Io & tc6_io)
: _tc6_io{tc6_io}
{
  _lw.io = &tc6_io;
}

TC6_Arduino_10BASE_T1S::~TC6_Arduino_10BASE_T1S()
{

}

/**************************************************************************************
 * PUBLIC MEMBER FUNCTIONS
 **************************************************************************************/

bool TC6_Arduino_10BASE_T1S::begin(IPAddress const ip_addr,
                                   IPAddress const network_mask,
                                   IPAddress const gateway,
                                   MacAddress const mac_addr,
                                   T1SPlcaSettings const t1s_plca_settings,
                                   T1SMacSettings const t1s_mac_settings)
{
  /* Initialize LWIP only once. */
  static bool is_lwip_init = false;
  if (!is_lwip_init)
  {
    lwip_init();
    is_lwip_init = true;
  }

  /* Store MAC address to LWIP. */
  memcpy(_lw.ip.mac, mac_addr.data(), sizeof(_lw.ip.mac));

  /* Initialize the TC6 library and pass a global tag. */
  _lw.tc.tc6 = TC6_Init(&_lw);
  if (_lw.tc.tc6 == NULL) {
    return false;
  }

  TC6ListNode * ptr = tc6_lwip_instance_list_head;
  while (ptr != nullptr) ptr = ptr->next;
  ptr = new TC6ListNode;
  ptr->inst = &_lw;
  ptr->next = NULL;
  if (tc6_lwip_instance_list_head == nullptr) {
    tc6_lwip_instance_list_head = ptr;
  }

  /* Initialize TC6 registers. */
  if (!TC6Regs_Init(  _lw.tc.tc6
                    , &_lw
                    , _lw.ip.mac
                    , true /* enable_plca */
                    , t1s_plca_settings.nodeId()
                    , t1s_plca_settings.nodeCount()
                    , t1s_plca_settings.burstCount()
                    , t1s_plca_settings.burstTimer()
                    , t1s_mac_settings.isMacPromiscuousModeEnabled()
                    , t1s_mac_settings.isMacTxCutThroughEnabled()
                    , t1s_mac_settings.isMacRxCutThroughEnabled()))
    return false;

  /* Complete initialization. */
  while (!TC6Regs_GetInitDone(_lw.tc.tc6))
    TC6_Service(_lw.tc.tc6, true);

  /* Assign IP address, network mask and gateway. */
  auto const ip_addr_str = ip_addr.toString();
  auto const network_mask_str = network_mask.toString();
  auto const gateway_str = gateway.toString();

  ip4_addr_t lwip_ip_addr;
  ip4_addr_t lwip_network_mask;
  ip4_addr_t lwip_gateway;

  ipaddr_aton(ip_addr_str.c_str(), &lwip_ip_addr);
  ipaddr_aton(network_mask_str.c_str(), &lwip_network_mask);
  ipaddr_aton(gateway_str.c_str(), &lwip_gateway);

  /* Bring up the interface. */
  if (!netif_add(&_lw.ip.netint, &lwip_ip_addr, &lwip_network_mask, &lwip_gateway, &_lw, lwIpInit, ethernet_input))
    return false;

  netif_set_link_up(&_lw.ip.netint);

  /* Copy the settings for internal usage. */
  _t1s_plca_settings = t1s_plca_settings;

  return true;
}

void TC6_Arduino_10BASE_T1S::digitalWrite(DIO const dio, bool const value)
{
  if (dio == DIO::A0)
    digitalWrite_A0(value);
  else if (dio == DIO::A1)
    digitalWrite_A1(value);
}

void TC6_Arduino_10BASE_T1S::service()
{
  sys_check_timeouts(); /* LWIP timers - ARP, DHCP, TCP, etc. */

  if (_tc6_io.isInterruptActive())
  {
    if (TC6_Service(_lw.tc.tc6, false))
    {
      _tc6_io.releaseInterrupt();
    }
  } else if (_lw.tc.tc6NeedService)
  {
    _lw.tc.tc6NeedService = false;
    TC6_Service(_lw.tc.tc6, true);
  }

  TC6Regs_CheckTimers();
}

bool TC6_Arduino_10BASE_T1S::getPlcaStatus(TC6LwIP_On_PlcaStatus on_plca_status)
{
  _lw.tc.pStatusCallback = on_plca_status;
  return TC6_ReadRegister(_lw.tc.tc6, 0x0004CA03, true, OnPlcaStatus, &_lw); /* PLCA_status_register.plca_status */
}

bool TC6_Arduino_10BASE_T1S::enablePlca()
{
  return TC6Regs_SetPlca(_lw.tc.tc6, true, _t1s_plca_settings.nodeId(), _t1s_plca_settings.nodeCount());
}

bool TC6_Arduino_10BASE_T1S::sendWouldBlock()
{
  TC6_RawTxSegment *dummySeg;
  uint8_t const segCount = TC6_GetRawSegments(_lw.tc.tc6, &dummySeg);
  bool const wouldBlock = (0u == segCount);

  return wouldBlock;
}

void TC6_Arduino_10BASE_T1S::digitalWrite_A0(bool const value)
{
  static bool is_dio_a0_enabled = false;
  if (!is_dio_a0_enabled)
  {
    TC6Regs_EnableDio_A0(_lw.tc.tc6);
    is_dio_a0_enabled = true;
  }

  static bool dio_a0_val = false;
  if (value != dio_a0_val)
  {
    TC6Regs_ToggleDio_A0(_lw.tc.tc6);
    dio_a0_val = value;
  }
}

void TC6_Arduino_10BASE_T1S::digitalWrite_A1(bool const value)
{
  static bool is_dio_a1_enabled = false;
  if (!is_dio_a1_enabled)
  {
    TC6Regs_EnableDio_A1(_lw.tc.tc6);
    is_dio_a1_enabled = true;
  }

  static bool dio_a1_val = false;
  if (value != dio_a1_val)
  {
    TC6Regs_ToggleDio_A1(_lw.tc.tc6);
    dio_a1_val = value;
  }
}

/**************************************************************************************
 * LWIP CALLBACKS
 **************************************************************************************/

static err_t lwIpInit(struct netif *netif)
{
  TC6LwIP_t *lw = GetContextNetif(netif);
  netif->output = etharp_output;
  netif->linkoutput = lwIpOut;
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;
  netif->mtu = TC6LwIP_MTU;
  netif->hwaddr_len = ETHARP_HWADDR_LEN;
  (void) memcpy(netif->name, TC6LwIP_HOSTNAME, 2);
  (void) memcpy(netif->hwaddr, lw->ip.mac, NETIF_MAX_HWADDR_LEN);
  netif_set_up(netif);
  netif_set_default(netif);
  return ERR_OK;
}

static err_t lwIpOut(struct netif *netif, struct pbuf *p)
{
  TC6_RawTxSegment *txSeg = NULL;
  TC6LwIP_t *lw = GetContextNetif(netif);
  struct pbuf *pC = p;
  uint8_t maxSeg;
  uint8_t seg = 0;
  err_t result;
  bool success;
//  TC6_ASSERT(netif && p);
//  TC6_ASSERT(LWIP_TC6_MAGIC == ((TC6LwIP_t*)netif->state)->magic);
  maxSeg = TC6_GetRawSegments(lw->tc.tc6, &txSeg);
  if (maxSeg)
  {
    pbuf_ref(p);
    while (seg < maxSeg)
    {
      txSeg[seg].pEth = (uint8_t *) pC->payload;
      txSeg[seg].segLen = pC->len;
      seg++;
      if (NULL != pC->next)
      {
//        TC6_ASSERT(seg < TC6_TX_ETH_MAX_SEGMENTS);
        pC = pC->next;
      } else
      {
        break;
      }
    }
    success = TC6_SendRawEthernetSegments(
      lw->tc.tc6, txSeg, seg, p->tot_len, 0,
      +[](TC6_t * /* pInst */, const uint8_t * /* pTx */, uint16_t /* len */, void *pTag,
          void * /* pGlobalTag */) -> void
      {
        struct pbuf *p = (struct pbuf *) pTag;
//  TC6_ASSERT(GetContextTC6(pInst));
//  TC6_ASSERT(pTx == p->payload);
//  TC6_ASSERT(len == p->tot_len);
//  TC6_ASSERT(len == p->len);
//  TC6_ASSERT(p->ref);
        pbuf_free(p);
      }, p);
//    TC6_ASSERT(success); /* Must always succeed as TC6_GetRawSegments returned a valid value */
    result = success ? ERR_OK : ERR_IF;
  } else
  {
    result = ERR_WOULDBLOCK;
  }
  return result;
}

/**************************************************************************************
 * NAMESPACE
 **************************************************************************************/

} /* TC6 */

/**************************************************************************************
 * TC6 CALLBACKS
 **************************************************************************************/

static bool FilterRxEthernetPacket(uint16_t ethType)
{
  bool tcpStack = false;
  switch (ethType)
  {
    case 0x0800:
      /* IPv4 */
      tcpStack = true;
      break;
    case 0x0806:
      /* ARP */
      tcpStack = true;
      break;
  }
  return tcpStack;
}

void TC6_CB_OnNeedService(TC6_t *pInst, void *pGlobalTag)
{
  TC6LwIP_t *lw = TC6::GetContextTC6(pInst);
  lw->tc.tc6NeedService = true;
}

uint32_t TC6Regs_CB_GetTicksMs(void)
{
  return millis();
}

bool TC6_CB_OnSpiTransaction(TC6_t *pInst, uint8_t *pTx, uint8_t *pRx, uint16_t len, void *pGlobalTag)
{
  TC6LwIP_t *lw = TC6::GetContextTC6(pInst);
  if (lw == nullptr) {
    return false;
  }
  bool const success = lw->io->spiTransaction(pTx, pRx, len);
  TC6_SpiBufferDone(pInst /* tc6instance */, success /* success */);
  return success;
}

void TC6_CB_OnRxEthernetSlice(TC6_t *pInst, const uint8_t *pRx, uint16_t offset, uint16_t len, void *pGlobalTag)
{
  TC6LwIP_t *lw = TC6::GetContextTC6(pInst);
  bool success = true;
  (void)pInst;
  (void)pGlobalTag;
//  TC6_ASSERT(lw->tc.tc6 == pInst);
  if (lw->tc.rxInvalid) {
    success = false;
  }
  if (success && ((offset + len) > TC6LwIP_MTU)) {
//    PRINT("on_rx_slice:packet greater than MTU", (offset + len));
    lw->tc.rxInvalid = true;
    success = false;
  }
  if (success && (NULL == lw->tc.pbuf)) {
    lw->tc.pbuf = pbuf_alloc(PBUF_RAW, TC6LwIP_MTU, PBUF_RAM);
    if (!lw->tc.pbuf) {
      lw->tc.rxInvalid = true;
      success = false;
    }
    if (success && (NULL != lw->tc.pbuf->next)) {
//      PRINT("rx_slice: could not allocate unsegmented memory diff", (lw->tc.pbuf->tot_len - lw->tc.pbuf->len));
      lw->tc.rxInvalid = true;
      pbuf_free(lw->tc.pbuf);
      lw->tc.pbuf = NULL;
      success = false;
    }
  }
  if (success) {
    (void)memcpy(lw->tc.pbuf->payload + offset, pRx, len);
    lw->tc.rxLen += len;
  }
}

void TC6_CB_OnRxEthernetPacket(TC6_t *pInst, bool success, uint16_t len, uint64_t *rxTimestamp, void *pGlobalTag)
{
#define MIN_HEADER_LEN  (42u)
  TC6LwIP_t *lw = TC6::GetContextTC6(pInst);
  uint16_t ethType;
  struct eth_hdr *ethhdr;
  (void) pInst;
  (void) rxTimestamp;
  (void) pGlobalTag;
//  TC6_ASSERT(lw->tc.tc6 == pInst);
  bool result = true;
  if (!success || lw->tc.rxInvalid || !lw->tc.pbuf || !lw->tc.rxLen)
  {
    result = false;
  }
  if (result && (lw->tc.rxLen != len))
  {
//        PrintRateLimited("on_rx_eth_ready: size mismatch", 0u);
    result = false;
  }
  if (result && (len < MIN_HEADER_LEN))
  {
//        PrintRateLimited("on_rx_eth_ready: received invalid small packet len", len);
    result = false;
  }
  if (result)
  {
//    TC6_ASSERT(lw->tc.pbuf);
//    TC6_ASSERT(lw->tc.pbuf->ref != 0);
    pbuf_realloc(lw->tc.pbuf, len); /* Shrink a pbuf chain to a desired length. */
    ethhdr = (eth_hdr *) lw->tc.pbuf->payload;
    ethType = htons(ethhdr->type);
    if (FilterRxEthernetPacket(ethType))
    {
      /* Integrator decided that TCP/IP stack shall consume the received packet */
      err_t result = lw->ip.netint.input(lw->tc.pbuf, &lw->ip.netint);
      if (ERR_OK == result)
      {
        lw->tc.pbuf = NULL;
        lw->tc.rxLen = 0;
        lw->tc.rxInvalid = false;
      } else
      {
//                PrintRateLimited("on_rx_eth_ready: IP input error", result);
        result = false;
      }
    }
  }
  if (!result)
  {
    if (NULL != lw->tc.pbuf)
    {
//      TC6_ASSERT(NULL != lw->tc.pbuf);
//      pbuf_free(lw->tc.pbuf); // FIXME: SOMETHING FISHY GOING ON !!!
      lw->tc.pbuf = NULL;
    }
    lw->tc.rxLen = 0;
    lw->tc.rxInvalid = false;
  }
}

#define PRINT(...)

void TC6_CB_OnError(TC6_t *pInst, TC6_Error_t err, void *pGlobalTag)
{
  bool reinit = false;
  switch (err)
  {
    case TC6Error_Succeeded:
      PRINT(ESC_GREEN "No error occurred" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Error_NoHardware:
      PRINT(ESC_RED "MISO data implies that there is no MACPHY hardware available" ESC_RESETCOLOR "\r\n");
      reinit = true;
      break;
    case TC6Error_UnexpectedSv:
      PRINT(ESC_RED " Unexpected Start Valid Flag" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Error_UnexpectedDvEv:
      PRINT(ESC_RED "Unexpected Data Valid or End Valid Flag" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Error_BadChecksum:
      PRINT(ESC_RED "Checksum in footer is wrong" ESC_RESETCOLOR "\r\n");
      reinit = true;
      break;
    case TC6Error_UnexpectedCtrl:
      PRINT(ESC_RED "Unexpected control packet received" ESC_RESETCOLOR "\r\n");
      reinit = true;
      break;
    case TC6Error_BadTxData:
      PRINT(ESC_RED "Header Bad Flag received" ESC_RESETCOLOR "\r\n");
      reinit = true;
      break;
    case TC6Error_SyncLost:
      PRINT(ESC_RED "Sync Flag is no longer set" ESC_RESETCOLOR "\r\n");
      reinit = true;
      break;
    case TC6Error_SpiError:
      PRINT(ESC_RED "TC6 SPI Error" ESC_RESETCOLOR "\r\n");
      reinit = true;
      break;
    case TC6Error_ControlTxFail:
      PRINT(ESC_RED "TC6 Control Message Error" ESC_RESETCOLOR "\r\n");
      break;
    default:
      PRINT(ESC_RED "Unknown TC6 error occurred" ESC_RESETCOLOR "\r\n");
      break;
  }
  if (reinit)
  {
    TC6Regs_Reinit(pInst);
  }
}

void TC6Regs_CB_OnEvent(TC6_t *pInst, TC6Regs_Event_t event, void *pTag)
{
  bool reinit = false;
  switch (event)
  {
    case TC6Regs_Event_UnknownError:
      PRINT(ESC_RED "UnknownError" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Transmit_Protocol_Error:
      PRINT(ESC_RED "Transmit_Protocol_Error" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Transmit_Buffer_Overflow_Error:
      PRINT(ESC_RED "Transmit_Buffer_Overflow_Error" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Transmit_Buffer_Underflow_Error:
      PRINT(ESC_RED "Transmit_Buffer_Underflow_Error" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Receive_Buffer_Overflow_Error:
      PRINT(ESC_RED "Receive_Buffer_Overflow_Error" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Loss_of_Framing_Error:
      PRINT(ESC_RED "Loss_of_Framing_Error" ESC_RESETCOLOR "\r\n");
      reinit = true;
      break;
    case TC6Regs_Event_Header_Error:
      PRINT(ESC_RED "Header_Error" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Reset_Complete:
      PRINT(ESC_GREEN "Reset_Complete" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_PHY_Interrupt:
      PRINT(ESC_GREEN "PHY_Interrupt" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Transmit_Timestamp_Capture_Available_A:
      PRINT(ESC_GREEN "Transmit_Timestamp_Capture_Available_A" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Transmit_Timestamp_Capture_Available_B:
      PRINT(ESC_GREEN "Transmit_Timestamp_Capture_Available_B" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Transmit_Timestamp_Capture_Available_C:
      PRINT(ESC_GREEN "Transmit_Timestamp_Capture_Available_C" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Transmit_Frame_Check_Sequence_Error:
      PRINT(ESC_RED "Transmit_Frame_Check_Sequence_Error" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Control_Data_Protection_Error:
      PRINT(ESC_RED "Control_Data_Protection_Error" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_RX_Non_Recoverable_Error:
      PRINT(ESC_RED "RX_Non_Recoverable_Error" ESC_RESETCOLOR "\r\n");
      reinit = true;
      break;
    case TC6Regs_Event_TX_Non_Recoverable_Error:
      PRINT(ESC_RED "TX_Non_Recoverable_Error" ESC_RESETCOLOR "\r\n");
      reinit = true;
      break;
    case TC6Regs_Event_FSM_State_Error:
      PRINT(ESC_RED "FSM_State_Error" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_SRAM_ECC_Error:
      PRINT(ESC_RED "SRAM_ECC_Error" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Undervoltage:
      PRINT(ESC_RED "Undervoltage" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Internal_Bus_Error:
      PRINT(ESC_RED "Internal_Bus_Error" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_TX_Timestamp_Capture_Overflow_A:
      PRINT(ESC_RED "TX_Timestamp_Capture_Overflow_A" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_TX_Timestamp_Capture_Overflow_B:
      PRINT(ESC_RED "TX_Timestamp_Capture_Overflow_B" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_TX_Timestamp_Capture_Overflow_C:
      PRINT(ESC_RED "TX_Timestamp_Capture_Overflow_C" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_TX_Timestamp_Capture_Missed_A:
      PRINT(ESC_RED "TX_Timestamp_Capture_Missed_A" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_TX_Timestamp_Capture_Missed_B:
      PRINT(ESC_RED "TX_Timestamp_Capture_Missed_B" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_TX_Timestamp_Capture_Missed_C:
      PRINT(ESC_RED "TX_Timestamp_Capture_Missed_C" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_MCLK_GEN_Status:
      PRINT(ESC_YELLOW "MCLK_GEN_Status" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_gPTP_PA_TS_EG_Status:
      PRINT(ESC_YELLOW "gPTP_PA_TS_EG_Status" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Extended_Block_Status:
      PRINT(ESC_YELLOW "Extended_Block_Status" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_SPI_Err_Int:
      PRINT(ESC_YELLOW "SPI_Err_Int" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_MAC_BMGR_Int:
      PRINT(ESC_YELLOW "MAC_BMGR_Int" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_MAC_Int:
      PRINT(ESC_YELLOW "MAC_Int" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_HMX_Int:
      PRINT(ESC_YELLOW "HMX_Int" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_GINT_Mask:
      PRINT(ESC_YELLOW "GINT_Mask" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Chip_Error:
      PRINT(ESC_RED "PHY is not trimmed" ESC_RESETCOLOR "\r\n");
      break;
    case TC6Regs_Event_Unsupported_Hardware:
      PRINT(ESC_RED "Unsupported hardware" ESC_RESETCOLOR "\r\n");
      break;
  }
  if (reinit)
  {
    TC6Regs_Reinit(pInst);
  }
}
