/*
 * TCP_Server.ino — 10BASE-T1S TCP echo/command server with DHCP
 *
 * Acquires an IP address from a DHCP server via the T1S-to-RJ45 bridge, then
 * opens a TCP server on TCP_SERVER_PORT.  Up to MAX_CONNECTIONS clients may
 * be connected at the same time.  Every line received is:
 *   - printed on the Serial monitor, and
 *   - echoed back to the sender with a "> " prefix.
 *
 * lwIP options required in lwipopts.h  (already set by this project):
 *   LWIP_TCP            1
 *   LWIP_DHCP           1
 *   MEMP_NUM_TCP_PCB    4   <- this is MAX_CONNECTIONS
 *   MEMP_NUM_TCP_PCB_LISTEN 1
 *   MEMP_NUM_TCP_SEG    16
 *   MEM_SIZE            8192
 *
 * Simultaneous connection limit:
 *   The LAN8651 is a Layer-1 PHY only — it imposes no TCP connection limit.
 *   The limit is set entirely by MEMP_NUM_TCP_PCB in lwipopts.h.
 *   With 4 PCBs and 8 KB heap on the RP2040 (264 KB SRAM total), 4 concurrent
 *   clients is a practical ceiling given 10 Mbit/s shared-bus bandwidth.
 *   Increase MEMP_NUM_TCP_PCB (and MEM_SIZE proportionally, ~2 KB per client)
 *   to raise the limit.
 *
 * Test from a PC on the same LAN:
 *   Linux/macOS:  nc <pico-ip> 5000
 *   Windows:      Test-NetConnection <pico-ip> -Port 5000
 *                 or: tnc <pico-ip> -Port 5000
 *
 * Author: GitHub Copilot
 */

/**************************************************************************************
 * INCLUDES
 **************************************************************************************/

#include <Arduino_10BASE_T1S.h>
#include <SPI.h>

/* lwIP raw API — required when NO_SYS == 1 */
extern "C" {
#include "lib/liblwip/include/lwip/tcp.h"
#include "lib/liblwip/include/lwip/dhcp.h"
#include "lib/liblwip/include/lwip/netif.h"
}

/**************************************************************************************
 * CONFIGURATION
 **************************************************************************************/

static int const LAN8651_CS_PIN    = 17;
static int const LAN8651_RESET_PIN = 20;
static int const LAN8651_IRQ_PIN   = 21;

static uint8_t const T1S_PLCA_NODE_ID = 1;

static uint16_t const TCP_SERVER_PORT = 5000;
static uint8_t  const MAX_CONNECTIONS = 4;   /* must match MEMP_NUM_TCP_PCB */

/**************************************************************************************
 * LWIP OBJECTS
 **************************************************************************************/

static IPAddress const ip_addr    {0, 0, 0, 0};
static IPAddress const network_mask{0, 0, 0, 0};
static IPAddress const gateway    {0, 0, 0, 0};

static T1SPlcaSettings const t1s_plca_settings{T1S_PLCA_NODE_ID};
static T1SMacSettings  const t1s_default_mac_settings;

static TC6::TC6_Io                 t1s_io (SPI, LAN8651_CS_PIN, LAN8651_RESET_PIN, LAN8651_IRQ_PIN);
static TC6::TC6_Arduino_10BASE_T1S t1s_phy(t1s_io);

/**************************************************************************************
 * PER-CONNECTION STATE
 **************************************************************************************/

struct TcpClient {
  struct tcp_pcb *pcb = nullptr;
  char    rx_buf[256];        /* line accumulation buffer */
  uint16_t rx_len = 0;
  bool    active  = false;
};

static TcpClient clients[MAX_CONNECTIONS];

static TcpClient* allocClient(struct tcp_pcb *pcb)
{
  for (auto &c : clients)
  {
    if (!c.active)
    {
      c.pcb    = pcb;
      c.rx_len = 0;
      c.active = true;
      return &c;
    }
  }
  return nullptr;
}

static void freeClient(TcpClient *c)
{
  if (c)
  {
    c->pcb    = nullptr;
    c->rx_len = 0;
    c->active = false;
  }
}

/**************************************************************************************
 * lwIP CALLBACKS (raw API, NO_SYS=1)
 **************************************************************************************/

/* Forward declaration */
static err_t tcp_server_accept(void *arg, struct tcp_pcb *new_pcb, err_t err);

/* Called by lwIP when a connected client closes or errors */
static void closeClient(TcpClient *c, bool abort_it)
{
  if (!c || !c->pcb) return;

  tcp_arg(c->pcb,  nullptr);
  tcp_recv(c->pcb, nullptr);
  tcp_err(c->pcb,  nullptr);

  if (abort_it)
    tcp_abort(c->pcb);
  else
    tcp_close(c->pcb);

  Serial.print("[TCP] Client disconnected, slot freed\n");
  freeClient(c);
}

/* Error callback — connection reset by peer or internal lwIP error */
static void tcp_client_err(void *arg, err_t err)
{
  TcpClient *c = static_cast<TcpClient *>(arg);
  (void)err;
  Serial.print("[TCP] Connection error, err=");
  Serial.println(static_cast<int>(err));
  freeClient(c);   /* pcb is already freed by lwIP on error path */
}

/* Receive callback — called whenever data arrives on a client connection */
static err_t tcp_client_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
  TcpClient *c = static_cast<TcpClient *>(arg);

  /* p == NULL means the remote side closed the connection */
  if (p == nullptr || err != ERR_OK)
  {
    if (p) pbuf_free(p);
    closeClient(c, false);
    return ERR_OK;
  }

  /* Acknowledge received bytes immediately */
  tcp_recved(pcb, p->tot_len);

  /* Copy payload into our line buffer, handle line breaks */
  struct pbuf *q = p;
  while (q)
  {
    const char *data = static_cast<const char *>(q->payload);
    for (uint16_t i = 0; i < q->len; ++i)
    {
      char ch = data[i];

      if (ch == '\r') continue;   /* skip CR */

      if (ch == '\n' || c->rx_len >= sizeof(c->rx_buf) - 1)
      {
        /* Complete line — null-terminate and process */
        c->rx_buf[c->rx_len] = '\0';

        Serial.print("[TCP RX] ");
        Serial.println(c->rx_buf);

        /* Echo back with "> " prefix */
        char reply[264];
        int  reply_len = snprintf(reply, sizeof(reply), "> %s\r\n", c->rx_buf);
        err_t wr_err   = tcp_write(pcb, reply, (u16_t)reply_len, TCP_WRITE_FLAG_COPY);
        if (wr_err == ERR_OK)
          tcp_output(pcb);
        else
        {
          Serial.print("[TCP] tcp_write failed, err=");
          Serial.println(static_cast<int>(wr_err));
        }

        c->rx_len = 0;
      }
      else
      {
        c->rx_buf[c->rx_len++] = ch;
      }
    }
    q = q->next;
  }

  pbuf_free(p);
  return ERR_OK;
}

/* Accept callback — called when a new TCP client connects */
static err_t tcp_server_accept(void *arg, struct tcp_pcb *new_pcb, err_t err)
{
  (void)arg;
  if (err != ERR_OK || new_pcb == nullptr) return ERR_VAL;

  /* Set higher priority so accepted PCBs aren't dropped */
  tcp_setprio(new_pcb, TCP_PRIO_MIN);

  TcpClient *c = allocClient(new_pcb);
  if (!c)
  {
    Serial.println("[TCP] No free client slots — rejecting connection");
    tcp_abort(new_pcb);
    return ERR_ABRT;
  }

  ip4_addr_t const *remote = &new_pcb->remote_ip;
  Serial.print("[TCP] Client connected: ");
  Serial.print(ip4addr_ntoa(remote));
  Serial.print(":");
  Serial.println(new_pcb->remote_port);

  /* Greet the client */
  const char *banner = "10BASE-T1S TCP Server ready. Type a message and press Enter.\r\n";
  tcp_write(new_pcb, banner, (u16_t)strlen(banner), TCP_WRITE_FLAG_COPY);
  tcp_output(new_pcb);

  /* Register callbacks for this connection */
  tcp_arg (new_pcb, c);
  tcp_recv(new_pcb, tcp_client_recv);
  tcp_err (new_pcb, tcp_client_err);

  return ERR_OK;
}

/**************************************************************************************
 * TCP SERVER START
 **************************************************************************************/

static bool startTcpServer()
{
  struct tcp_pcb *listen_pcb = tcp_new();
  if (!listen_pcb)
  {
    Serial.println("[TCP] tcp_new() failed");
    return false;
  }

  /* Bind to all interfaces on the chosen port */
  err_t err = tcp_bind(listen_pcb, IP_ADDR_ANY, TCP_SERVER_PORT);
  if (err != ERR_OK)
  {
    Serial.print("[TCP] tcp_bind() failed, err=");
    Serial.println(static_cast<int>(err));
    tcp_abort(listen_pcb);
    return false;
  }

  /* Switch the PCB into listening state */
  struct tcp_pcb *server_pcb = tcp_listen_with_backlog(listen_pcb, MAX_CONNECTIONS);
  if (!server_pcb)
  {
    Serial.println("[TCP] tcp_listen() failed (out of memory?)");
    tcp_abort(listen_pcb);
    return false;
  }

  tcp_accept(server_pcb, tcp_server_accept);

  Serial.print("[TCP] Server listening on port ");
  Serial.println(TCP_SERVER_PORT);
  return true;
}

/**************************************************************************************
 * PLCA STATUS CALLBACK
 **************************************************************************************/

static void OnPlcaStatus(bool success, bool plcaStatus)
{
  if (!success)  { Serial.println("PLCA status register read failed"); return; }
  if (plcaStatus) Serial.println("PLCA Mode active");
  else            { Serial.println("CSMA/CD fallback"); t1s_phy.enablePlca(); }
}

/**************************************************************************************
 * SETUP
 **************************************************************************************/

void setup()
{
  Serial.begin(115200);
  while (!Serial) { }
  delay(1000);

  Serial.println("=== 10BASE-T1S TCP Server ===");
  Serial.print("Max simultaneous connections: ");
  Serial.println(MAX_CONNECTIONS);

  pinMode(LAN8651_IRQ_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(LAN8651_IRQ_PIN),
                  []() { t1s_io.onInterrupt(); },
                  FALLING);

  if (!t1s_io.begin())
  { Serial.println("TC6_Io::begin() failed"); for (;;) {} }

  MacAddress const mac_addr = MacAddress::create_from_uid();
  Serial.println(mac_addr);

  if (!t1s_phy.begin(ip_addr, network_mask, gateway,
                     mac_addr, t1s_plca_settings, t1s_default_mac_settings))
  { Serial.println("TC6::begin() failed"); for (;;) {} }

  /* Bring netif up and start DHCP */
  struct netif *netif = t1s_phy.getNetif();
  netif_set_default(netif);
  netif_set_up(netif);

  if (dhcp_start(netif) != ERR_OK)
  { Serial.println("dhcp_start() failed"); for (;;) {} }

  Serial.println("DHCP started — waiting for lease (30 s)...");

  unsigned long const t0 = millis();
  while (!dhcp_supplied_address(netif))
  {
    t1s_phy.service();
    if (millis() - t0 > 30000UL)
    {
      Serial.println("DHCP timed out — check T1S bridge and router.");
      for (;;) { t1s_phy.service(); }
    }
    delay(10);
  }

  Serial.print("IP      : "); Serial.println(ip4addr_ntoa(netif_ip4_addr(netif)));
  Serial.print("Subnet  : "); Serial.println(ip4addr_ntoa(netif_ip4_netmask(netif)));
  Serial.print("Gateway : "); Serial.println(ip4addr_ntoa(netif_ip4_gw(netif)));

  if (!startTcpServer())
  { Serial.println("TCP server failed to start"); for (;;) {} }
}

/**************************************************************************************
 * LOOP
 **************************************************************************************/

void loop()
{
  /* Must be called continuously — drives the MAC/PHY and lwIP timers (ARP/DHCP/TCP) */
  t1s_phy.service();

  /* Periodic PLCA health check */
  static unsigned long prev_plca_check = 0;
  unsigned long const now = millis();
  if (now - prev_plca_check > 1000)
  {
    prev_plca_check = now;
    if (!t1s_phy.getPlcaStatus(OnPlcaStatus))
      Serial.println("getPlcaStatus() failed");
  }
}
