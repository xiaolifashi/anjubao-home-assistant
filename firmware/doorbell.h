#pragma once
// ============================================================================
// 安居宝可视门禁桥接 —— 协议实现（WT32-ETH01 / ESP32 + LAN8720，ESP-IDF 原生）
//
// 拓扑：以太网口接门禁内网（静态 192.168.105.61，冒充室内主机），
//       WiFi 接家庭内网（ESPHome 原生 API 接入 HAOS）。
//
// 行为：
//   1. 持续应答 4 台白名单门口机的 UDP 6672 寻人查询，并周期广播在线宣告；
//   2. 门口机呼入（TCP 18022 反向连接）后自动 705 应答 + 710 接听，
//      并循环播放抓包静音音频保持会话；
//   3. 等待 HA 侧点“开门解锁”（发 518）或“挂断”（发 708）；
//      60 秒无操作自动挂断；可选“自动解锁”开关恢复全自动。
// ============================================================================

#include <string.h>
#include <errno.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <fcntl.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>         // esp_get_free_heap_size（心跳里监控内存）
#include <esp_eth.h>
#include <esp_eth_netif_glue.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <driver/gpio.h>
#include <esp_rom_gpio.h>       // esp_rom_gpio_connect_out/in_signal（SMI 引脚修复用）
#include <soc/gpio_sig_map.h>   // EMAC_MDC_O_IDX / EMAC_MDI_I_IDX / EMAC_MDO_O_IDX
#include "audio_data.h"

static uint32_t db_millis() {   // 毫秒时钟（替代 Arduino 的 millis()）
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// ---------------- 用户配置 ----------------
#define DB_ETH_IP        "192.168.105.61"   // 室内主机的 IP（本机冒充它）
#define DB_ETH_GW        "192.168.0.1"      // 门禁内网网关（隔离网，填不存在的地址也没影响）
#define DB_ETH_MASK      "255.255.0.0"      // 门禁内网是 192.168.x.x 大网段
#define DB_BROADCAST     "192.168.255.255"  // 门禁内网广播地址
#define DB_GATE_PORT     18022              // 门口机控制端口
#define DB_QUERY_PORT    6672               // 寻人查询/应答端口
#define DB_BEACON_PORT   53119              // 在线宣告源端口
#define DB_DRAIN_PORT1   6668               // 门口机音频端口（双向：收访客语音→转发视频桥；发住户语音→门口机）
#define DB_DRAIN_PORT2   9880               // 门口机视频 RTP 入口（转发给 fnOS 视频桥）
#define DB_CALL_TIMEOUT_MS  60000           // 接听后等待解锁的最长时间
#define DB_BEACON_INTERVAL_MS 30000         // 在线宣告周期
#define DB_UNLOCK_DELAY_MS  1500            // 自动模式下接听后延迟多久发 518
#define DB_VIDEO_FWD_IP    "192.168.1.10" // 视频转发目标（飞牛 fnOS 主机，跑视频桥容器）
                                            // 注意是 fnOS 主机的 IP，不是 HA 的 192.168.1.11
#define DB_VIDEO_FWD_PORT  9880             // 视频桥监听的 UDP 端口
#define DB_AUDIO_FWD_PORT  9990             // 下行音频转发目标（视频桥容器的音频入口）
#define DB_AUDIO_UP_PORT   6670             // 上行音频入口（WiFi 侧，视频桥把住户语音发到这里）
#define DB_AUDIO_PKT       256              // A-law 每包 256 字节 = 32ms @ 8kHz（抓包实测）
#define DB_MONITOR_TIMEOUT_MS 180000        // 监视最长时长（防忘关；按用户要求 3 分钟自动停）
#define DB_MONITOR_NOSTREAM_MS 15000        // 发了 704 后这么久没视频流就放弃

struct DbGate { const char *ip; const char *name; };
static const DbGate DB_GATES[] = {
  {"192.168.105.21", "门口机1"},
  {"192.168.105.22", "门口机2"},
  {"192.168.105.23", "门口机3"},
  {"192.168.105.24", "门口机4"},
};
static const int DB_GATE_COUNT = sizeof(DB_GATES) / sizeof(DB_GATES[0]);

// ---------------- 抓包提取的固定帧 ----------------
static const uint8_t DB_BEACON_FRAME[] = {  // 在线宣告（真实主机周期广播）
  0x00,0x25,0x02,0x00,0x00,0x25,0x02,0x06,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x44,0xb6
};
static const uint8_t DB_FRAME_705[] = {  // 查询应答
  0x07,0xb8,0x18,0x00,0x00,0x00,0x72,0x65,0x71,0x3d,0x37,0x30,0x35,0x26,0x71,0x75,
  0x65,0x72,0x79,0x2a,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x01
};
static const uint8_t DB_FRAME_710[] = {  // 接听
  0x07,0xb8,0x1e,0x00,0x00,0x00,0x72,0x65,0x71,0x3d,0x37,0x31,0x30,0x26,0x71,0x75,
  0x65,0x72,0x79,0x3d,0x25,0x02,0x00,0x00,0x25,0x02,0x06,0x02,0x01,0x00,0x00,0x00,
  0x01,0x00,0x00,0x00
};
static const uint8_t DB_FRAME_518[] = {  // 解锁
  0x07,0xb8,0x18,0x00,0x00,0x00,0x72,0x65,0x71,0x3d,0x35,0x31,0x38,0x26,0x71,0x75,
  0x65,0x72,0x79,0x3d,0x22,0x25,0x02,0x00,0x00,0x25,0x02,0x06,0x02,0x78
};
static const uint8_t DB_FRAME_708[] = {  // 挂断
  0x07,0xb8,0x1e,0x00,0x00,0x00,0x72,0x65,0x71,0x3d,0x37,0x30,0x38,0x26,0x71,0x75,
  0x65,0x72,0x79,0x3d,0x25,0x02,0x00,0x00,0x25,0x02,0x06,0x02,0x01,0x00,0x00,0x00,
  0x01,0x00,0x00,0x00
};
static const uint8_t DB_FRAME_704[] = {  // 请求监视（主机按监视键时发出，字节与抓包一致）
  0x07,0xb8,0x1e,0x00,0x00,0x00,0x72,0x65,0x71,0x3d,0x37,0x30,0x34,0x26,0x71,0x75,
  0x65,0x72,0x79,0x2a,0x25,0x02,0x00,0x00,0x25,0x02,0x06,0x02,0x80,0x00,0x00,0x00,
  0x02,0x00,0x00,0x01
};
static const uint8_t DB_FRAME_709[] = {  // 监视确认（门禁机回连 18022 时回复）
  0x07,0xb8,0x18,0x00,0x00,0x00,0x72,0x65,0x71,0x3d,0x37,0x30,0x39,0x26,0x71,0x75,
  0x65,0x72,0x79,0x2a,0x00,0x00,0x80,0x00,0x00,0x00,0x02,0x00,0x00,0x01
};
static const uint8_t DB_FRAME_564[] = {  // 主机上线注册（真实主机开机即发，发后即忘、无应答）
  0x07,0xb8,0x12,0x00,0x00,0x00,0x72,0x65,0x71,0x3d,0x35,0x36,0x34,0x26,0x71,0x75,
  0x65,0x72,0x79,0x3d,0x25,0x02,0x00,0x00
};
static const uint8_t DB_FRAME_888[] = {  // 主机上线注册 2（同上，req=888）
  0x07,0xb8,0x12,0x00,0x00,0x00,0x72,0x65,0x71,0x3d,0x38,0x38,0x38,0x26,0x71,0x75,
  0x65,0x72,0x79,0x3d,0x25,0x02,0x00,0x00
};
#define DB_REG_INTERVAL_MS 300000          // 注册帧每 5 分钟重发一轮
// 监视期间的 RTCP 保活（RR+SDES，每 ~4.5s 一发，字节与抓包一致）；
// 第 8~11 字节是“被报告流”的 SSRC，运行时替换成门口机视频流的实际 SSRC。
static const uint8_t DB_RTCP_RR_TMPL[] = {
  0x81,0xc9,0x00,0x07, 0x65,0xc6,0xe4,0x42, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
  0x81,0xca,0x00,0x05, 0x65,0xc6,0xe4,0x42, 0x01,0x0a,0x72,0x6f,0x6f,0x74,0x40,0x61,0x6e,0x79,0x6b,0x61,
  0x00,0x00,0x00,0x00
};
static const uint8_t DB_RTCP_BYE_TMPL[] = {  // 监视结束：空 RR + SDES + BYE
  0x80,0xc9,0x00,0x01, 0x65,0xc6,0xe4,0x42,
  0x81,0xca,0x00,0x05, 0x65,0xc6,0xe4,0x42, 0x01,0x0a,0x72,0x6f,0x6f,0x74,0x40,0x61,0x6e,0x79,0x6b,0x61,
  0x00,0x00,0x00,0x00,
  0x81,0xcb,0x00,0x01, 0x65,0xc6,0xe4,0x42
};

static const char *DB_TAG = "doorbell";

// ---------------- 运行状态（供 ESPHome 实体读取/操作） ----------------
static volatile int  db_state = 0;          // 0=空闲 1=呼叫中 2=监视中
static volatile int  db_gate_idx = -1;      // 当前呼叫/监视的门口机
static volatile bool db_unlock_req = false; // HA 点了“开门解锁”
static volatile bool db_hangup_req = false; // HA 点了“挂断”
static volatile bool db_auto_unlock = false;// “自动解锁”开关
static volatile int  db_monitor_req = -1;   // HA 点了“监视”门口机下标
static volatile bool db_monitor_stop = false;   // HA 点了“停止监视”
static volatile bool db_monitor_confirmed = false; // 已应答门禁机的监视回连(709)
static volatile uint32_t db_video_ssrc = 0; // 当前视频流的 SSRC（从转发包里学习）
static volatile uint32_t db_video_last_ms = 0;   // 最近一次收到视频包的时刻
static volatile uint16_t db_video_max_seq = 0;   // 已见最大 RTP 序号（写进 RTCP RR 的 EHSNR）
static int db_vfwd = -1;          // 视频转发专用套接字（绑定 WiFi 网卡，强制走 WiFi 出去）
static volatile uint32_t db_video_dropped = 0;   // 视频转发失败累计（WiFi 发送缓冲爆 → ENOMEM）
static volatile uint32_t db_video_rx = 0;        // 9880 收到的白名单视频包累计
static volatile uint32_t db_video_fwd = 0;       // 成功转发出去的视频包累计
static volatile uint32_t db_video_ringdrop = 0;  // 环形缓冲满导致的丢包累计（WiFi 持续跟不上才会 >0）
static int db_vsock = -1;                        // 9880 视频接收套接字（RX 任务专用）
static uint32_t db_gate_addr[DB_GATE_COUNT];     // 白名单门口机 IP（网络序，RX 任务直接比 32 位整数）
static uint32_t db_bridge_addr = 0;              // 视频桥（fnOS）IP，网络序，上行音频只信它
static volatile uint32_t db_last_uplink_ms = 0;  // 最近一次收到桥接器上行音频的时刻
static uint8_t db_silence[DB_AUDIO_PKT];         // 上行静音包（A-law 静音字节 = 0xD5，抓包实测）

// ---- 视频环形缓冲（单生产者 db_vrx_task / 单消费者 db_vtx_task，免锁）----
// 丢包定位结论（计数器实测）：收≈93 包/秒 < 门口机实发 158 包/秒，且 转=收、发失败=0 ——
// 丢包发生在“网线 → recvfrom”之间。门口机按 100Mbps 线速突发（一阵最多 28 包、间隔 86µs），
// 原来“以太网收包 + WiFi 转发”挤在同一个任务里，sendto 长时间持有 lwIP 核锁，
// 收包路径（EMAC DMA → tcpip → socket 队列）被憋死，缓冲溢出后 MAC 层静默丢包。
// 对策：接收独立成高优先级任务，recvfrom 后只比对白名单 + 一次 memcpy 就立刻再收，
//       把 lwIP 各层队列始终腾空；WiFi 转发慢由环形缓冲兜底（64 槽 ≈ 400ms 视频流）。
//       两个任务都钉 core 1，单生产者单消费者 + 同核运行，读写位置用 volatile 即安全
//       （读到旧值只会让“满/空”判断偏保守，不会出错）。
#define DB_VRING_SLOTS  64     // 64 槽 ≈ 400ms 视频流，兜住 WiFi 发送的偶发停顿
#define DB_VRING_SLOT   1088   // 视频 UDP 载荷最大约 1036 字节
static volatile uint8_t  db_vring[DB_VRING_SLOTS][DB_VRING_SLOT];
static volatile uint16_t db_vring_len[DB_VRING_SLOTS];
static volatile uint8_t  db_vring_head = 0;   // 写位置（仅 RX 任务推进）
static volatile uint8_t  db_vring_tail = 0;   // 读位置（仅 TX 任务推进）
// 监视回连跟踪：我们每发一次 704，门口机约 15 秒后会回连 18022 一次。
// 回连帧和访客振铃帧都是 36 字节的 req=704，只能靠上下文区分 —— 真实主机也是这么干的
// （抓包实测：监视回连主机回 709，访客振铃主机回 705）。迟到的回连若被当成振铃，
// 就会无中生有“幽灵呼叫”，门口机还真的进入通话（实测 710 会收到 711 确认）。
// 注意必须用“每一条 704 的发送时刻”做 FIFO 匹配：监视期间每 12 秒续发一次 704，
// 回连对应的往往不是最近那条（15.1s > 12s 续流间隔），只记最近一次会误判。
static volatile uint32_t db_mon_704_ts[DB_GATE_COUNT][3];  // 各门口机已发未匹配的 704 时刻（FIFO）
static volatile uint8_t  db_mon_704_n[DB_GATE_COUNT] = {0};
static bool db_started = false;

// ---------------- 对外 API（YAML lambda 调用） ----------------
bool doorbell_call_active() { return db_state == 1; }   // 呼叫中（不含监视）
bool doorbell_monitoring()  { return db_state == 2; }
const char *doorbell_gate_name() {
  int i = db_gate_idx;
  return (i >= 0 && i < DB_GATE_COUNT) ? DB_GATES[i].name : "-";
}
const char *doorbell_status_text() {
  if (db_state == 0) return "空闲";
  if (db_state == 2) return "监视中";
  return "呼叫中，等待解锁";
}
void doorbell_request_unlock() { if (db_state == 1) db_unlock_req = true; }   // 仅呼叫中生效，防误触
void doorbell_request_hangup() { if (db_state == 1) db_hangup_req = true; }
bool doorbell_auto_enabled()   { return db_auto_unlock; }
void doorbell_set_auto(bool v) { db_auto_unlock = v; }
// 请求监视某台门口机（传 DB_GATES 下标）；仅空闲时生效，呼叫/监视中忽略
void doorbell_request_monitor(int gi) {
  if (db_state == 0 && gi >= 0 && gi < DB_GATE_COUNT) db_monitor_req = gi;
}
void doorbell_request_monitor_stop() { if (db_state == 2) db_monitor_stop = true; }

// ---------------- 工具函数 ----------------
static int db_gate_index(const char *ip) {
  for (int i = 0; i < DB_GATE_COUNT; i++)
    if (strcmp(DB_GATES[i].ip, ip) == 0) return i;
  return -1;
}

// 陌生 IP 只记录一次日志
static char db_ignored[8][16];
static int  db_ignored_n = 0;
static bool db_ignored_seen(const char *ip) {
  for (int i = 0; i < db_ignored_n; i++)
    if (strcmp(db_ignored[i], ip) == 0) return true;
  if (db_ignored_n < 8) {
    strncpy(db_ignored[db_ignored_n], ip, 15);
    db_ignored[db_ignored_n][15] = 0;
    db_ignored_n++;
  }
  return false;
}

// 构造 6672 查询应答：首字节 00->01，第 5~8 字节替换为本机 IP
static size_t db_build_reply(const uint8_t *q, size_t n, uint8_t *out) {
  static const uint8_t fallback[21] = {
    0x01,0x25,0x02,0x06,0x02,0xc0,0xa8,0x69,0x3d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x00
  };
  if (n == 21 && q[0] == 0x00) {
    memcpy(out, q, 21);
    out[0] = 0x01;
    uint32_t a = inet_addr(DB_ETH_IP);   // 网络字节序，与 inet_aton 一致
    memcpy(out + 5, &a, 4);
    return 21;
  }
  memcpy(out, fallback, 21);
  return 21;
}

static bool db_contains(const uint8_t *buf, size_t n, const char *needle) {
  size_t m = strlen(needle);
  if (m > n) return false;
  for (size_t i = 0; i + m <= n; i++)
    if (memcmp(buf + i, needle, m) == 0) return true;
  return false;
}

// 区分“真实振铃”和“周期性状态探针”（两者都是门口机→主机 18022 的 req=704 帧）：
// 真实振铃帧 "req=704&query*" 后第 1 个二进制字段固定为 25 02 06 02
//   —— 多台门口机的真实振铃抓包均如此；
// 西门每 ~60 秒的状态探针帧该字段为全 0（PC 时代日志 10:11:31/10:12:23，与探针节奏吻合）。
static bool db_is_real_ring(const uint8_t *f, int n) {
  static const char q[] = "req=704&query*";
  const int ql = 14;
  for (int i = 0; i + ql + 4 <= n; i++)
    if (memcmp(f + i, q, ql) == 0)
      return f[i+ql] == 0x25 && f[i+ql+1] == 0x02 && f[i+ql+2] == 0x06 && f[i+ql+3] == 0x02;
  return false;   // 找不到完整特征，谨慎起见不按振铃处理
}

// 绑定到任意地址的 UDP 套接字
static int db_udp_bind(uint16_t port) {
  int s = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return -1;
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = INADDR_ANY;
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); return -1; }
  return s;
}

// 等我们回过数据的连接被【对端】先行关断（最多等 wait_ms）。抓包实测：门口机
// 拿到应答（705/709/711）后立刻主动 FIN。让它做主动关闭方，2×MSL 的 TIME_WAIT
// 就留在它那边，不占本机紧张的 TCP 连接池。对端超时不关则我们自行关（兜底）。
static void db_wait_peer_close(int fd, uint32_t wait_ms) {
  struct timeval tv;
  tv.tv_sec = wait_ms / 1000;
  tv.tv_usec = (wait_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  uint8_t drain[32];
  recv(fd, drain, sizeof(drain), 0);   // 0=对端已 FIN；<0=超时，随后由调用方 close
}

// 对“只收数据、从不回发”的入站连接（状态探针 / 708 结束通知 / 非白名单话痨），
// 用 RST 方式关断（SO_LINGER 超时 0）：立刻释放 PCB，不产生 TIME_WAIT。
// 门口机每 ~60 秒探一次 × 4 台 + 话痨设备，这种连接日积月累会把连接池占满，
// 导致通话中 518 解锁 / 708 挂断的外发连接拿不到资源（实测三次重试全部瞬间失败）。
// 注意：凡是我们回过数据的连接（705/709）绝不能用 RST 关——回包可能被丢弃。
static void db_rst_close(int fd) {
  struct linger ling;
  ling.l_onoff = 1;
  ling.l_linger = 0;
  setsockopt(fd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
  close(fd);
}

// 向门口机发一帧 TCP 并等回复（非阻塞连接，带超时）。what = 用途标签（如"518 解锁"）。
// 失败日志带阶段 + errno，事后能区分两类根因：
//   socket 阶段 errno=23/24（ENFILE/EMFILE）→ 本机 fd 表耗尽；
//   connect 阶段 errno=105（ENOBUFS）→ 本机 TCP 连接池耗尽（PCB 被 TIME_WAIT 占满）；
//   connect 阶段 errno=111（ECONNREFUSED）→ 门口机主动拒收（协议/状态问题）。
static bool db_tcp_xact(const char *what, const char *gate_ip, const uint8_t *payload, size_t len,
                        uint8_t *resp, size_t *resp_len, uint32_t timeout_ms) {
  *resp_len = 0;
  int s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    ESP_LOGW(DB_TAG, "%s → %s 失败：socket() errno=%d（fd 耗尽？）空闲堆=%u",
             what, gate_ip, errno, (unsigned)esp_get_free_heap_size());
    return false;
  }
  struct sockaddr_in la;
  memset(&la, 0, sizeof(la));
  la.sin_family = AF_INET;
  la.sin_port = 0;
  la.sin_addr.s_addr = inet_addr(DB_ETH_IP);
  bind(s, (struct sockaddr *)&la, sizeof(la));

  int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, flags | O_NONBLOCK);
  struct sockaddr_in da;
  memset(&da, 0, sizeof(da));
  da.sin_family = AF_INET;
  da.sin_port = htons(DB_GATE_PORT);
  da.sin_addr.s_addr = inet_addr(gate_ip);
  int r = connect(s, (struct sockaddr *)&da, sizeof(da));
  if (r < 0 && errno == EINPROGRESS) {
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    r = select(s + 1, NULL, &wf, NULL, &tv);
    if (r > 0) {
      int err = 0;
      socklen_t l = sizeof(err);
      getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l);
      if (err != 0) { r = -1; errno = err; }   // SO_ERROR 翻成 errno 一并打出
    } else {
      if (r == 0) errno = ETIMEDOUT;           // select 超时（对端无响应）
      r = -1;
    }
  }
  if (r < 0) {
    ESP_LOGW(DB_TAG, "%s → %s 失败：connect errno=%d"
             "（105=本机连接池耗尽，111=门口机拒收）空闲堆=%u",
             what, gate_ip, errno, (unsigned)esp_get_free_heap_size());
    close(s);
    return false;
  }
  fcntl(s, F_SETFL, flags);   // 恢复阻塞

  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (send(s, payload, len, 0) < 0) {
    ESP_LOGW(DB_TAG, "%s → %s 失败：send errno=%d 空闲堆=%u",
             what, gate_ip, errno, (unsigned)esp_get_free_heap_size());
    close(s);
    return false;
  }
  ssize_t n = recv(s, resp, 256, 0);
  if (n > 0) *resp_len = (size_t)n;
  // 拿到应答后再等门口机先 FIN（抓包实测它一应答完就关，局域网内毫秒级），
  // 让 TIME_WAIT 留在它一侧。n==0 说明它已先关；n<0（无应答帧，如 518）直接关。
  if (n > 0) {
    struct timeval etv;
    etv.tv_sec = 0; etv.tv_usec = 300000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &etv, sizeof(etv));
    uint8_t drain[64];
    recv(s, drain, sizeof(drain), 0);
  }
  close(s);
  return true;
}

// ---------------- 以太网初始化（ESP-IDF 原生 esp_eth，LAN8720，WT32-ETH01 引脚） ----------------
static esp_eth_handle_t db_eth_handle = NULL;

// 门禁任务固定跑在 APP 核（core 1）。
// esp_intr_alloc 只在“调用任务当前所在核”上分配非共享中断输入线；
// core 0 的中断输入池被 WiFi 占用较多，EMAC 中断在那里可能分配失败
// （报 “No free interrupt inputs for ETH_MAC interrupt”）。钉到 core 1 即可避开。
#if CONFIG_FREERTOS_UNICORE
  #define DB_CORE tskNO_AFFINITY   // 单核固件没有 core 1，退回不绑定
#else
  #define DB_CORE 1
#endif

// 统一建任务入口：优先钉到 DB_CORE，失败则不绑定兜底
static bool db_spawn_prio(void (*fn)(void *), const char *name, uint32_t stack, void *arg, uint32_t prio) {
  if (xTaskCreatePinnedToCore(fn, name, stack, arg, prio, NULL, DB_CORE) == pdPASS)
    return true;
  ESP_LOGW(DB_TAG, "%s 钉核创建失败，改为不绑定核", name);
  return xTaskCreate(fn, name, stack, arg, prio, NULL) == pdPASS;
}
static bool db_spawn(void (*fn)(void *), const char *name, uint32_t stack, void *arg) {
  return db_spawn_prio(fn, name, stack, arg, 5);
}

static bool db_eth_init() {
  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  mac_config.sw_reset_timeout_ms = 1000;
  // EMAC 收包任务默认优先级只有 15，比 tcpip 线程（18）还低：转发高峰期 tcpip 一忙就把
  // 它抢占，DMA 收包环溢出 → MAC 层静默丢包（花屏主因之一）。提到 24（最高一档，与 IPC
  // 同级）——它每包只搬几十微秒数据，不会饿死任何任务，但再也没人能压住它收包。
  mac_config.rx_task_prio = 24;
  eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
  emac_config.smi_gpio.mdc_num = 23;        // MDC -> GPIO23（IDF>=5.3 的成员名）
  emac_config.smi_gpio.mdio_num = 18;       // MDIO -> GPIO18
  emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;      // 50MHz 外部时钟
  emac_config.clock_config.rmii.clock_gpio = EMAC_CLK_IN_GPIO;     // 由 GPIO0 输入

  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
  phy_config.phy_addr = 1;
  phy_config.reset_gpio_num = -1;           // WT32-ETH01 的 GPIO16 是 PHY 电源，手动控制

  esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
  if (mac == NULL) { ESP_LOGE(DB_TAG, "创建 EMAC 失败"); return false; }
  esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);   // LAN8720 由 lan87xx 统一驱动覆盖
  if (phy == NULL) {
    ESP_LOGE(DB_TAG, "创建 PHY 失败");
    mac->del(mac);   // 重试循环里不能泄漏 MAC 实例（含已占的中断线）
    return false;
  }

  // WT32-ETH01：GPIO16 给 PHY 上电（须在驱动安装前完成）
  gpio_set_direction(GPIO_NUM_16, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_16, 1);
  vTaskDelay(pdMS_TO_TICKS(50));   // 等 PHY 上电稳定，SMI 才能正常应答

  // ------------------------------------------------------------------
  // ESP-IDF 5.5.1+ 官方 bug 规避（espressif/esp-idf#18756）：
  // 5.5.1 起 emac_esp_gpio_init_smi()（在 esp_eth_mac_new_esp32 内部调用）
  // 漏配 SMI 引脚方向，且矩阵连接收尾后 MDIO 的输出使能(OEN_SEL)状态不对，
  // 导致 SMI 读写不可靠：esp_eth_driver_install 报 "wrong chip OUI"。
  // 这里按 5.4.2（验证正常的版本）的原始顺序完整重做一遍 MDC/MDIO 配置：
  //   1) 先设引脚方向；
  //   2) 再重发矩阵连接 —— connect_out 会把输出使能交还给 EMAC 外设
  //      自动控制（MDIO 读周期必须释放总线，只设方向会让它一直被驱动）；
  //   3) 最后关闭上下拉并选回 GPIO 矩阵功能。
  // 等官方修复进入 ESPHome 默认 IDF 版本后，可删除本段。
  gpio_set_direction(GPIO_NUM_23, GPIO_MODE_OUTPUT);                 // MDC
  esp_rom_gpio_connect_out_signal(GPIO_NUM_23, EMAC_MDC_O_IDX, false, false);
  gpio_set_pull_mode(GPIO_NUM_23, GPIO_FLOATING);
  esp_rom_gpio_pad_select_gpio(GPIO_NUM_23);
  gpio_set_direction(GPIO_NUM_18, GPIO_MODE_INPUT_OUTPUT);           // MDIO
  esp_rom_gpio_connect_out_signal(GPIO_NUM_18, EMAC_MDO_O_IDX, false, false);
  esp_rom_gpio_connect_in_signal(GPIO_NUM_18, EMAC_MDI_I_IDX, false);
  gpio_set_pull_mode(GPIO_NUM_18, GPIO_FLOATING);
  esp_rom_gpio_pad_select_gpio(GPIO_NUM_18);
  // ------------------------------------------------------------------

  esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
  if (esp_eth_driver_install(&eth_config, &db_eth_handle) != ESP_OK) {
    ESP_LOGE(DB_TAG, "以太网驱动安装失败（wrong chip OUI=读不到 PHY 芯片；power up timeout=PHY 无响应，检查供电/接线）");
    phy->del(phy);
    mac->del(mac);
    return false;
  }

  esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
  esp_netif_t *eth_netif = esp_netif_new(&netif_config);
  if (eth_netif == NULL) {
    ESP_LOGE(DB_TAG, "创建 netif 失败");
    esp_eth_driver_uninstall(db_eth_handle);
    phy->del(phy);
    mac->del(mac);
    return false;
  }
  if (esp_netif_attach(eth_netif, esp_eth_new_netif_glue(db_eth_handle)) != ESP_OK) {
    ESP_LOGE(DB_TAG, "netif 绑定失败");
    esp_netif_destroy(eth_netif);
    esp_eth_driver_uninstall(db_eth_handle);
    phy->del(phy);
    mac->del(mac);
    return false;
  }

  // 静态 IP：192.168.105.61 / 255.255.0.0（冒充室内主机）
  esp_netif_dhcpc_stop(eth_netif);
  esp_netif_ip_info_t ip_info = {};
  esp_netif_str_to_ip4(DB_ETH_IP, &ip_info.ip);
  esp_netif_str_to_ip4(DB_ETH_GW, &ip_info.gw);
  esp_netif_str_to_ip4(DB_ETH_MASK, &ip_info.netmask);
  if (esp_netif_set_ip_info(eth_netif, &ip_info) != ESP_OK) {
    ESP_LOGE(DB_TAG, "设置静态 IP 失败"); return false;
  }

  if (esp_eth_start(db_eth_handle) != ESP_OK) {
    ESP_LOGE(DB_TAG, "以太网启动失败"); return false;
  }
  vTaskDelay(pdMS_TO_TICKS(1500));   // 等链路起来
  ESP_LOGI(DB_TAG, "以太网已启动，门禁侧 IP: %s", DB_ETH_IP);
  return true;
}

// ---------------- 呼叫会话任务 ----------------
struct DbSession {
  int fd;
  int gate_idx;
  char ip[16];
};

static void db_session_task(void *pv) {
  DbSession *ss = (DbSession *)pv;
  const char *name = DB_GATES[ss->gate_idx].name;
  ESP_LOGI(DB_TAG, "%s（%s）呼入，开始自动接听", name, ss->ip);

  // 1. 振铃帧（req=704，36 字节）已由值守循环读取并校验过（拦截监视回连/708 通知），
  //    这里直接回 705 并断开。等门口机先 FIN（抓包实测它收到 705 立刻就关），
  //    TIME_WAIT 留给它；300ms 没关我们再关。
  send(ss->fd, DB_FRAME_705, sizeof(DB_FRAME_705), 0);
  db_wait_peer_close(ss->fd, 300);
  close(ss->fd);
  ESP_LOGI(DB_TAG, "已应答 req=705");
  vTaskDelay(pdMS_TO_TICKS(800));   // 模拟“等人按接听”

  // 2. 发 710 接听
  uint8_t resp[256];
  size_t rlen = 0;
  if (db_tcp_xact("710 接听", ss->ip, DB_FRAME_710, sizeof(DB_FRAME_710), resp, &rlen, 3000)) {
    if (db_contains(resp, rlen, "req=711"))
      ESP_LOGI(DB_TAG, "门口机确认接听（711），呼叫模式已建立");
    else
      ESP_LOGW(DB_TAG, "710 已发出，但未收到 711 确认");
  } else {
    ESP_LOGW(DB_TAG, "710 发送失败（门口机不可达？）");
  }

  // 3. 循环播放静音音频保活，等待解锁/挂断/超时
  int au = db_udp_bind(DB_AUDIO_SRC_PORT);
  int rc = db_udp_bind(DB_RTCP_SRC_PORT);
  struct sockaddr_in dst_a, dst_r;
  memset(&dst_a, 0, sizeof(dst_a));
  dst_a.sin_family = AF_INET;
  dst_a.sin_port = htons(DB_AUDIO_DST_PORT);
  dst_a.sin_addr.s_addr = inet_addr(ss->ip);
  dst_r = dst_a;
  dst_r.sin_port = htons(DB_RTCP_DST_PORT);

  uint32_t t0 = db_millis();
  uint32_t loop_base = db_millis();
  uint32_t next_at = 0;
  size_t ai = 0, ri = 0;
  bool unlocked = false;
  ESP_LOGI(DB_TAG, "会话保持中（%d 秒内等待解锁指令）…", DB_CALL_TIMEOUT_MS / 1000);
  while (db_millis() - t0 < DB_CALL_TIMEOUT_MS) {
    if (db_hangup_req) {
      db_hangup_req = false;
      ESP_LOGI(DB_TAG, "收到手动挂断");
      break;
    }
    if (db_unlock_req || (db_auto_unlock && db_millis() - t0 > DB_UNLOCK_DELAY_MS)) {
      unlocked = true;
      db_unlock_req = false;
      break;
    }
    uint32_t el = db_millis() - loop_base;
    while (ai < DB_AUDIO_COUNT && el >= next_at) {
      const DbPkt &p = DB_AUDIO[ai];
      if (au >= 0) sendto(au, DB_AUDIO_BLOB + p.off, p.len, 0, (struct sockaddr *)&dst_a, sizeof(dst_a));
      next_at += p.dt_ms;
      ai++;
    }
    while (ri < DB_RTCP_COUNT && el >= DB_RTCP[ri].at_ms) {
      const DbAbsPkt &p = DB_RTCP[ri];
      if (rc >= 0) sendto(rc, DB_AUDIO_BLOB + p.off, p.len, 0, (struct sockaddr *)&dst_r, sizeof(dst_r));
      ri++;
    }
    if (ai >= DB_AUDIO_COUNT) {   // 一轮播完，循环
      ai = 0; ri = 0; next_at = 0; loop_base = db_millis();
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }

  // 4. 解锁
  // 518 末字节按门口机区分（抓包实测：多数门口机=0x78，个别机型=0x79，需逐台抓包确认）。
  // B 系门口机收到后通常不应答、直接断开——“无应答”属正常，真实主机抓包亦如此；
  // 只有 TCP 连接/发送本身失败才重试，最多 3 次。各次失败的阶段和 errno 见上方日志。
  if (unlocked) {
    uint8_t f518[sizeof(DB_FRAME_518)];
    memcpy(f518, DB_FRAME_518, sizeof(f518));
    if (!strcmp(ss->ip, "192.168.105.24")) f518[sizeof(f518) - 1] = 0x79;   // ⚠️ 这台门口机的 518 校验字节抓包实测为 0x79；换门口机要重新抓包确认
    bool ok518 = false;
    for (int i = 0; i < 3 && !ok518; i++) {
      if (i) { ESP_LOGW(DB_TAG, "518 第 %d 次重试…", i + 1); vTaskDelay(pdMS_TO_TICKS(500)); }
      rlen = 0;
      ok518 = db_tcp_xact("518 解锁", ss->ip, f518, sizeof(f518), resp, &rlen, 3000);
    }
    char hex[48];
    int hn = 0;
    for (size_t i = 0; i < rlen && hn < (int)sizeof(hex) - 3; i++)
      hn += snprintf(hex + hn, sizeof(hex) - hn, "%02x", resp[i]);
    hex[hn] = 0;
    ESP_LOGI(DB_TAG, ">>> 已发送 req=518 解锁（连接%s，应答 %u 字节%s%s）<<<",
             ok518 ? "成功" : "失败", (unsigned)rlen, rlen ? "：" : "", hex);
    vTaskDelay(pdMS_TO_TICKS(5000));
  } else {
    ESP_LOGI(DB_TAG, "未解锁（超时或手动挂断）");
  }

  // 5. 挂断收尾
  rlen = 0;
  bool ok708 = db_tcp_xact("708 挂断", ss->ip, DB_FRAME_708, sizeof(DB_FRAME_708), resp, &rlen, 3000);
  if (ok708)
    ESP_LOGI(DB_TAG, "已挂断（708），恢复值守");
  else
    ESP_LOGW(DB_TAG, "708 挂断发送失败（原因见上方 errno）——门口机会在约 60 秒后自行结束");

  if (au >= 0) close(au);
  if (rc >= 0) close(rc);
  db_state = 0;
  db_gate_idx = -1;
  delete ss;
  vTaskDelete(NULL);
}

// ---------------- 视频转发套接字 ----------------
// 必须绑定 WiFi 网卡的 IP 作为源地址：家里的 192.168.1.0/24 被对讲网的
// 192.168.0.0/16 包含，lwIP 路由按 netif 链表顺序匹配（后挂载的以太网排在
// 前面），不绑定的话发往视频桥所在网段（例如 192.168.1.x）的包会被错发进对讲网，fnOS 永远收不到。
static int db_make_vfwd_socket() {
  int s = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return -1;
  esp_netif_t *w = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ipi;
  bool ok = false;
  if (w && esp_netif_get_ip_info(w, &ipi) == ESP_OK && ipi.ip.addr != 0) {
    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = ipi.ip.addr;
    if (bind(s, (struct sockaddr *)&la, sizeof(la)) == 0) {
      uint32_t a = ipi.ip.addr;   // 不用 inet_ntoa：它的静态缓冲会和主任务争用
      ESP_LOGI(DB_TAG, "视频转发绑定 WiFi 网卡（源地址 %u.%u.%u.%u）",
               (unsigned)(a & 0xff), (unsigned)((a >> 8) & 0xff),
               (unsigned)((a >> 16) & 0xff), (unsigned)(a >> 24));
      ok = true;
    }
  }
  if (!ok) {   // 没绑上宁可不用，心跳里会重试；免得悄悄走错网卡
    ESP_LOGW(DB_TAG, "WiFi 网卡未就绪，视频转发套接字稍后重试");
    close(s);
    return -1;
  }
  return s;
}

// 发过一条 704 且门禁机已回应：记录时刻，等它约 15 秒后的回连
static void db_mon_note_704(int gi) {
  if (db_mon_704_n[gi] >= 3) {   // 满了就丢最旧的一条（它的回连早已来过或已丢失）
    for (int i = 0; i < 2; i++) db_mon_704_ts[gi][i] = db_mon_704_ts[gi][i + 1];
    db_mon_704_n[gi] = 2;
  }
  db_mon_704_ts[gi][db_mon_704_n[gi]++] = db_millis();
}

// ---------------- 监视任务（不呼叫直接看门口机画面） ----------------
// 流程（对照抓包）：主机发 704 请求监视 → 门禁机立即推视频到 9880 →
// 期间主机每 ~4.5s 发 RTCP RR+SDES 保活；门禁机回连 18022 时回 709；
// 结束时发 RTCP BYE。视频转发由主任务的 9880 转发路径完成，这里只管控制面。
static void db_monitor_task(void *pv) {
  int gi = (int)(uintptr_t)pv;
  const char *gip = DB_GATES[gi].ip;
  ESP_LOGI(DB_TAG, "请求监视 %s（%s）", DB_GATES[gi].name, gip);

  // 1. 发 704
  uint8_t resp[256];
  size_t rlen = 0;
  if (db_tcp_xact("704 监视", gip, DB_FRAME_704, sizeof(DB_FRAME_704), resp, &rlen, 3000)) {
    ESP_LOGI(DB_TAG, "704 已发出%s", rlen > 0 ? "，门禁机有回应" : "");
    db_mon_note_704(gi);   // 这次 704 约 15 秒后会有一次回连，登记等它
  } else {
    ESP_LOGW(DB_TAG, "704 发送失败（门口机不可达？），继续等视频流");
  }

  // 2. RTCP 保活 socket（源端口 9881，与真实主机一致）
  int rc = db_udp_bind(9881);
  struct sockaddr_in rtcp_dst;
  memset(&rtcp_dst, 0, sizeof(rtcp_dst));
  rtcp_dst.sin_family = AF_INET;
  rtcp_dst.sin_port = htons(6671);
  rtcp_dst.sin_addr.s_addr = inet_addr(gip);

  uint32_t t0 = db_millis();
  uint32_t last_rtcp = 0;
  uint32_t last_retry = t0;   // 初始 704 刚发出，从它开始算 12 秒续流节奏
  bool any_video = false;
  ESP_LOGI(DB_TAG, "监视中（最长 %d 秒，可随时手动停止）…", DB_MONITOR_TIMEOUT_MS / 1000);
  while (db_millis() - t0 < DB_MONITOR_TIMEOUT_MS && !db_monitor_stop) {
    // 学到视频流 SSRC 后，每 4.5 秒发一次 RTCP RR+SDES
    if (db_video_ssrc != 0 && rc >= 0 && db_millis() - last_rtcp >= 4500) {
      last_rtcp = db_millis();
      uint8_t rr[sizeof(DB_RTCP_RR_TMPL)];
      memcpy(rr, DB_RTCP_RR_TMPL, sizeof(rr));
      memcpy(rr + 8, (const void *)&db_video_ssrc, 4);   // 报告块里的源 SSRC
      uint16_t sq = db_video_max_seq;                    // EHSNR 诚实汇报已收最大序号
      rr[16] = 0; rr[17] = 0;
      rr[18] = (uint8_t)(sq >> 8); rr[19] = (uint8_t)(sq & 0xFF);
      sendto(rc, rr, sizeof(rr), 0, (struct sockaddr *)&rtcp_dst, sizeof(rtcp_dst));
    }
    if (db_video_last_ms >= t0) any_video = true;
    // 抓包实测：真实主机监视期间每约 12 秒就续发一次 704 —— 赶在约 15 秒的预览流
    // 结束之前续上，视频流因此不间断。以前我们等断流 4 秒才续，每个周期（约 19 秒）
    // 黑屏约 4 秒，HA 里就表现为周期性转圈。改为按 12 秒节奏固定续发。
    if (db_millis() - last_retry >= 12000) {
      last_retry = db_millis();
      ESP_LOGI(DB_TAG, "按 12 秒节奏续发 704，保持预览流不断");
      uint8_t rb[64]; size_t rl = 0;
      if (db_tcp_xact("704 续流", gip, DB_FRAME_704, sizeof(DB_FRAME_704), rb, &rl, 2000))
        db_mon_note_704(gi);
    }
    // 视频流彻底断了超过 30 秒（续发也救不回来）→ 结束监视
    if (any_video && db_millis() - db_video_last_ms > 30000) {
      ESP_LOGI(DB_TAG, "视频流长时间中断，结束监视");
      break;
    }
    // 发了 704 一直没视频 → 门禁机忙/不在线，放弃
    if (!any_video && db_millis() - t0 > DB_MONITOR_NOSTREAM_MS) {
      ESP_LOGW(DB_TAG, "未收到视频流，监视失败");
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  // 3. 收尾：RTCP BYE ×2（抓包里主机就是这么发的）
  if (rc >= 0) {
    sendto(rc, DB_RTCP_BYE_TMPL, sizeof(DB_RTCP_BYE_TMPL), 0, (struct sockaddr *)&rtcp_dst, sizeof(rtcp_dst));
    vTaskDelay(pdMS_TO_TICKS(200));
    sendto(rc, DB_RTCP_BYE_TMPL, sizeof(DB_RTCP_BYE_TMPL), 0, (struct sockaddr *)&rtcp_dst, sizeof(rtcp_dst));
    close(rc);
  }
  ESP_LOGI(DB_TAG, "监视结束，恢复值守");
  // 只在仍是自己占用状态时复位：监视期间若来了真实振铃，值守循环会停掉本任务
  // 并直接切到呼叫会话（state=1），这里不能把它又清回 0。
  if (db_state == 2) {
    db_state = 0;
    db_gate_idx = -1;
  }
  vTaskDelete(NULL);
}

// 判断门口机这次连上 18022 是不是“监视回连”（我们发 704 后约 15 秒的固定回访）。
// 是 → 回 709 确认并关闭，返回 true；不是 → 返回 false（交给呼叫流程）。
// 注意：回连帧与访客振铃帧同为 36 字节的 req=704，单看内容无法区分，
//       只能用上下文（与某条 704 相隔约 15 秒）判断，与真实主机行为一致。
static bool db_handle_monitor_callback(int cfd, int gi) {
  uint32_t now = db_millis();
  // 过期清理：超过 30 秒还没等到回连的 704 记录作废（防脏数据吞掉以后的真振铃）
  while (db_mon_704_n[gi] > 0 && now - db_mon_704_ts[gi][0] > 30000) {
    for (int j = 0; j + 1 < db_mon_704_n[gi]; j++) db_mon_704_ts[gi][j] = db_mon_704_ts[gi][j + 1];
    db_mon_704_n[gi]--;
  }
  for (int i = 0; i < db_mon_704_n[gi]; i++) {
    uint32_t since = now - db_mon_704_ts[gi][i];
    if (since >= 8000 && since <= 30000) {   // 回连固定在对应 704 的约 15 秒后
      for (int j = i; j + 1 < db_mon_704_n[gi]; j++) db_mon_704_ts[gi][j] = db_mon_704_ts[gi][j + 1];
      db_mon_704_n[gi]--;
      send(cfd, DB_FRAME_709, sizeof(DB_FRAME_709), 0);
      db_wait_peer_close(cfd, 250);   // 等门口机先关（抓包实测它收到应答立刻 FIN）
      close(cfd);
      db_monitor_confirmed = true;
      ESP_LOGI(DB_TAG, "已应答 %s 的监视回连（709，距对应 704 约 %lu 秒）",
               DB_GATES[gi].name, (unsigned long)(since / 1000));
      return true;
    }
  }
  return false;
}

// ---------------- 视频 RX 任务：以最快速度把 9880 的包搬进环形缓冲 ----------------
// 这个任务绝不碰 WiFi：recvfrom 之后只做白名单比对 + 一次 memcpy 就立刻再收，
// 把 lwIP 各层队列（DMA 环 → tcpip → socket 接收队列）始终腾空，不给线速突发憋死的机会。
static void db_vrx_task(void *pv) {
  uint8_t mbuf[DB_VRING_SLOT];
  for (;;) {
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    int n = recvfrom(db_vsock, mbuf, sizeof(mbuf), 0, (struct sockaddr *)&from, &fl);
    if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
    int gi = -1;   // 白名单：比 32 位地址，不走 inet_ntoa（更快，也不与主任务争它的静态缓冲）
    for (int i = 0; i < DB_GATE_COUNT; i++)
      if (from.sin_addr.s_addr == db_gate_addr[i]) { gi = i; break; }
    if (gi < 0) continue;
    db_video_rx++;
    // 学习视频流 SSRC 与最大序号（RTP 头 8~11 / 2~3 字节），监视模式的 RTCP 保活要用
    if (n > 12 && (mbuf[0] >> 6) == 2) {
      if (db_millis() - db_video_last_ms > 2000)
        ESP_LOGI(DB_TAG, "视频流到达（来自 %s），转发 → %s:%d",
                 DB_GATES[gi].name, DB_VIDEO_FWD_IP, DB_VIDEO_FWD_PORT);
      memcpy((void *)&db_video_ssrc, mbuf + 8, 4);
      uint16_t sq = ((uint16_t)mbuf[2] << 8) | mbuf[3];
      if ((uint16_t)(sq - db_video_max_seq) < 0x8000) db_video_max_seq = sq;
      db_video_last_ms = db_millis();
    }
    uint8_t h = db_vring_head;
    uint8_t nx = (uint8_t)((h + 1) % DB_VRING_SLOTS);
    if (nx == db_vring_tail) {   // 环满：WiFi 持续跟不上才会发生，丢包并计数（心跳里上报）
      db_video_ringdrop++;
      continue;
    }
    memcpy((void *)db_vring[h], mbuf, n);
    db_vring_len[h] = (uint16_t)n;
    db_vring_head = nx;   // 数据落槽后再发布写位置
  }
}

// ---------------- 视频 TX 任务：从环形缓冲取包，走 WiFi 网卡转发给 fnOS 视频桥 ----------------
// WiFi 发送慢 / 偶发 ENOMEM 都只堵在这个任务里，由环形缓冲兜底，绝不再拖死以太网收包。
static void db_vtx_task(void *pv) {
  uint8_t tbuf[DB_VRING_SLOT];
  struct sockaddr_in dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_port = htons(DB_VIDEO_FWD_PORT);
  dst.sin_addr.s_addr = inet_addr(DB_VIDEO_FWD_IP);
  uint32_t last_try = 0;
  for (;;) {
    if (db_vring_tail == db_vring_head) {   // 环空：歇 1ms（视频平均 6.3ms 才一包，毫无影响）
      if (db_vfwd < 0 && db_millis() - last_try >= 2000) {   // 空闲时提前把转发套接字备好
        last_try = db_millis();
        db_vfwd = db_make_vfwd_socket();
      }
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    uint8_t t = db_vring_tail;
    uint16_t n = db_vring_len[t];
    memcpy(tbuf, (const void *)db_vring[t], n);
    db_vring_tail = (uint8_t)((t + 1) % DB_VRING_SLOTS);
    if (n == 0) continue;
    if (db_vfwd < 0) {
      if (db_millis() - last_try >= 2000) { last_try = db_millis(); db_vfwd = db_make_vfwd_socket(); }
      if (db_vfwd < 0) { db_video_dropped++; continue; }
    }
    // WiFi 发送缓冲偶发打满会让 sendto 返回 errno=12（ENOMEM）：缓 1ms 补发，最多 3 次
    int rc = sendto(db_vfwd, tbuf, n, 0, (struct sockaddr *)&dst, sizeof(dst));
    for (int r2 = 0; rc < 0 && errno == ENOMEM && r2 < 3; r2++) {
      vTaskDelay(pdMS_TO_TICKS(1));
      rc = sendto(db_vfwd, tbuf, n, 0, (struct sockaddr *)&dst, sizeof(dst));
    }
    if (rc < 0) {
      db_video_dropped++;
      static uint32_t last_fwd_err = 0;
      if (db_millis() - last_fwd_err > 10000) {   // 10 秒最多报一次
        last_fwd_err = db_millis();
        ESP_LOGW(DB_TAG, "视频转发失败 errno=%d 累计丢包=%u 空闲堆=%u（fnOS 离线或内存不足）",
                 errno, (unsigned)db_video_dropped, (unsigned)esp_get_free_heap_size());
      }
    } else {
      db_video_fwd++;
    }
  }
}

// ---------------- 主任务：6672 应答 + 在线宣告 + 18022 值守 ----------------
static void db_main_task(void *pv) {
  while (!db_eth_init()) {
    ESP_LOGE(DB_TAG, "以太网初始化失败，2 秒后重试（检查网线）");
    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  int q = db_udp_bind(DB_QUERY_PORT);
  int d1 = db_udp_bind(DB_DRAIN_PORT1);
  int d2 = db_udp_bind(DB_DRAIN_PORT2);
  db_vsock = d2;   // 9880 视频入口交给 db_vrx_task 专职处理
  int au = db_udp_bind(DB_AUDIO_UP_PORT);   // 6670 上行音频入口（视频桥 → 这里 → 门口机）
  if (q < 0) ESP_LOGE(DB_TAG, "无法绑定 UDP %d", DB_QUERY_PORT);
  if (d2 < 0) ESP_LOGE(DB_TAG, "无法绑定 UDP %d（视频转发不可用）", DB_DRAIN_PORT2);
  if (au < 0) ESP_LOGE(DB_TAG, "无法绑定 UDP %d（对讲上行不可用）", DB_AUDIO_UP_PORT);
  db_bridge_addr = inet_addr(DB_VIDEO_FWD_IP);   // 上行音频只信视频桥一个来源
  memset(db_silence, 0xD5, sizeof(db_silence));  // A-law 静音

  // 下行音频转发目标（fnOS 视频桥，走绑了 WiFi 的 db_vfwd 套接字发出去）
  struct sockaddr_in audio_fwd;
  memset(&audio_fwd, 0, sizeof(audio_fwd));
  audio_fwd.sin_family = AF_INET;
  audio_fwd.sin_port = htons(DB_AUDIO_FWD_PORT);
  audio_fwd.sin_addr.s_addr = db_bridge_addr;

  // 在线宣告套接字：源端口 53119，允许广播
  int bc = ::socket(AF_INET, SOCK_DGRAM, 0);
  int one = 1;
  setsockopt(bc, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
  struct sockaddr_in bla;
  memset(&bla, 0, sizeof(bla));
  bla.sin_family = AF_INET;
  bla.sin_port = htons(DB_BEACON_PORT);
  bla.sin_addr.s_addr = inet_addr(DB_ETH_IP);
  bind(bc, (struct sockaddr *)&bla, sizeof(bla));
  struct sockaddr_in bc_dst;
  memset(&bc_dst, 0, sizeof(bc_dst));
  bc_dst.sin_family = AF_INET;
  bc_dst.sin_port = htons(DB_QUERY_PORT);
  bc_dst.sin_addr.s_addr = inet_addr(DB_BROADCAST);

  // 视频转发目标与转发套接字已挪进 db_vtx_task（空闲时自动建好、坏了自动重建）

  // TCP 值守 18022
  int srv = ::socket(AF_INET, SOCK_STREAM, 0);
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(DB_GATE_PORT);
  sa.sin_addr.s_addr = INADDR_ANY;
  if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(srv, 4) < 0) {
    ESP_LOGE(DB_TAG, "无法监听 TCP %d", DB_GATE_PORT);
    close(srv);
    srv = -1;
  }

  ESP_LOGI(DB_TAG, "门禁值守已启动：只响应 %d 台白名单门口机", DB_GATE_COUNT);
  for (int i = 0; i < DB_GATE_COUNT; i++) {
    ESP_LOGI(DB_TAG, "  白名单: %s（%s）", DB_GATES[i].ip, DB_GATES[i].name);
    db_gate_addr[i] = inet_addr(DB_GATES[i].ip);   // RX 任务比对用（网络序整数）
  }

  // 视频接力双任务：RX（优先级 12）高速收货进环形缓冲，TX（11）从缓冲取包走 WiFi 发出
  if (d2 < 0 ||
      !db_spawn_prio(db_vrx_task, "db_vrx", 4096, NULL, 12) ||
      !db_spawn_prio(db_vtx_task, "db_vtx", 5120, NULL, 11))
    ESP_LOGE(DB_TAG, "视频接力任务创建失败，视频转发不可用");

  uint32_t last_beacon = db_millis() - DB_BEACON_INTERVAL_MS + 5000;  // 启动 5 秒后首发
  uint32_t last_reg = db_millis() - DB_REG_INTERVAL_MS + 8000;        // 注册帧启动 8 秒后首发
  uint32_t last_reply[DB_GATE_COUNT] = {0};

  for (;;) {
    fd_set rf;
    FD_ZERO(&rf);
    int maxfd = -1;
    int fds[4] = {q, d1, au, srv};   // 9880 视频已由 db_vrx_task 专职处理，不参与 select
    for (int i = 0; i < 4; i++) {
      if (fds[i] >= 0) {
        FD_SET(fds[i], &rf);
        if (fds[i] > maxfd) maxfd = fds[i];
      }
    }
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200000;   // 200ms 轮询
    int r = select(maxfd + 1, &rf, NULL, NULL, &tv);

    if (r > 0) {
      // --- 6672 寻人查询 ---
      if (q >= 0 && FD_ISSET(q, &rf)) {
        uint8_t buf[64];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(q, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n > 0) {
          char ipbuf[16];
          strncpy(ipbuf, inet_ntoa(from.sin_addr), 15);
          ipbuf[15] = 0;
          int gi = db_gate_index(ipbuf);
          if (gi < 0) {
            if (!db_ignored_seen(ipbuf))
              ESP_LOGW(DB_TAG, "%s 不在白名单，已忽略（不再提示）", ipbuf);
          } else if (buf[0] == 0x00) {   // 0x00 才是查询帧
            uint32_t now = db_millis();
            if (now - last_reply[gi] >= 1000) {   // 同设备 1 秒防抖
              last_reply[gi] = now;
              uint8_t reply[32];
              size_t rl = db_build_reply(buf, n, reply);
              struct sockaddr_in gd;
              memset(&gd, 0, sizeof(gd));
              gd.sin_family = AF_INET;
              gd.sin_port = htons(DB_QUERY_PORT);
              gd.sin_addr.s_addr = inet_addr(ipbuf);
              sendto(q, reply, rl, 0, (struct sockaddr *)&gd, sizeof(gd));
              sendto(q, reply, rl, 0, (struct sockaddr *)&from, fl);   // 双保险
              ESP_LOGI(DB_TAG, "已应答 %s（%s）的查询", DB_GATES[gi].name, ipbuf);
            }
          }
        }
      }
      // --- 6668 门口机音频入口：访客语音（裸 A-law，256B/32ms，约 31 包/秒）转发给视频桥 ---
      // 9880 视频由独立的 RX/TX 任务对处理，不在此。音频量小，随收随转即可。
      if (d1 >= 0 && FD_ISSET(d1, &rf)) {
        for (int k = 0; k < 8; k++) {
          uint8_t abuf[300];
          struct sockaddr_in afrom;
          socklen_t afl = sizeof(afrom);
          int an = recvfrom(d1, abuf, sizeof(abuf), MSG_DONTWAIT, (struct sockaddr *)&afrom, &afl);
          if (an <= 0) break;   // 队列已空
          bool wl = false;   // 只收白名单门口机的音频
          for (int i = 0; i < DB_GATE_COUNT; i++)
            if (afrom.sin_addr.s_addr == db_gate_addr[i]) { wl = true; break; }
          if (wl && db_vfwd >= 0)
            sendto(db_vfwd, abuf, an, 0, (struct sockaddr *)&audio_fwd, sizeof(audio_fwd));
        }
      }
      // --- 6670 上行音频入口：视频桥（fnOS）发来的住户语音，通话/监视期间中继给门口机 ---
      if (au >= 0 && FD_ISSET(au, &rf)) {
        for (int k = 0; k < 8; k++) {
          uint8_t ubuf[300];
          struct sockaddr_in ufrom;
          socklen_t ufl = sizeof(ufrom);
          int un = recvfrom(au, ubuf, sizeof(ubuf), MSG_DONTWAIT, (struct sockaddr *)&ufrom, &ufl);
          if (un <= 0) break;
          if (ufrom.sin_addr.s_addr != db_bridge_addr) continue;   // 只信视频桥一个来源
          if (db_state != 0 && db_gate_idx >= 0 && d1 >= 0) {
            struct sockaddr_in gd;
            memset(&gd, 0, sizeof(gd));
            gd.sin_family = AF_INET;
            gd.sin_port = htons(6668);
            gd.sin_addr.s_addr = db_gate_addr[db_gate_idx];
            sendto(d1, ubuf, un, 0, (struct sockaddr *)&gd, sizeof(gd));   // 源 105.61:6668，出对讲网卡
            db_last_uplink_ms = db_millis();
          }
        }
      }
      // --- 18022 门口机呼入 ---
      if (srv >= 0 && FD_ISSET(srv, &rf)) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int cfd = accept(srv, (struct sockaddr *)&ca, &cl);
        if (cfd >= 0) {
          char ipbuf[16];
          strncpy(ipbuf, inet_ntoa(ca.sin_addr), 15);
          ipbuf[15] = 0;
          int gi = db_gate_index(ipbuf);
          if (gi < 0) {
            db_rst_close(cfd);   // 话痨设备：不回发任何数据，RST 关断不留 TIME_WAIT
            if (!db_ignored_seen(ipbuf))
              ESP_LOGW(DB_TAG, "%s 呼入，不在白名单，已断开（不再提示）", ipbuf);
          } else {
            // 先收首帧再定性 —— 门口机连上后会立即发一帧。抓包实测的帧型：
            //   req=704(36B)：访客振铃（"query*"后字段=25 02 06 02），或监视回连
            //   req=704(36B) 但字段全 0：门口机的周期性状态探针（西门 ~60 秒一次）
            //   req=708(36B)：通话结束/取消通知（真实主机收到后不回复直接关）
            // 以前见连接就当振铃自动接听，结果把监视回连、708 通知和状态探针全部接成“幽灵呼叫”。
            struct timeval ftv;
            ftv.tv_sec = 0; ftv.tv_usec = 300000;   // 门口机会立即发帧，300ms 足够
            setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &ftv, sizeof(ftv));
            uint8_t fb[64];
            int fn = recv(cfd, fb, sizeof(fb), 0);
            if (fn <= 0) {
              db_rst_close(cfd);
              ESP_LOGW(DB_TAG, "%s 连上 18022 但没发数据，已断开", DB_GATES[gi].name);
            } else if (db_contains(fb, fn, "req=708")) {
              db_rst_close(cfd);   // 结束/取消通知：与真实主机一样不回复
              ESP_LOGI(DB_TAG, "%s 发来通话结束通知（708），已关闭（非振铃）", DB_GATES[gi].name);
            } else if (!db_contains(fb, fn, "req=704")) {
              db_rst_close(cfd);   // 探针类帧：不回发，RST 关断不留 TIME_WAIT
              ESP_LOGW(DB_TAG, "%s 发来未知帧（%d 字节），已忽略", DB_GATES[gi].name, fn);
              ESP_LOG_BUFFER_HEX(DB_TAG, fb, fn > 48 ? 48 : fn);
            } else if (db_handle_monitor_callback(cfd, gi)) {
              // 监视回连，已回 709 并关闭（函数内处理）
            } else if (!db_is_real_ring(fb, fn)) {
              // 周期性状态探针（西门每 ~60 秒一次，帧型 req=704 但二进制字段为 0）。
              // 与负一楼/南门的 24 字节探针一样：不回复直接关——多天实测无副作用。
              // 以前把它当振铃自动接听，于是西门每小时准时“幽灵呼叫”一次。
              // 不回发任何数据的连接一律 RST 关断，不留 TIME_WAIT 占连接池。
              db_rst_close(cfd);
              ESP_LOGI(DB_TAG, "%s 发来状态探针（req=704 但字段为 0，非振铃），已忽略", DB_GATES[gi].name);
              ESP_LOG_BUFFER_HEX(DB_TAG, fb, fn > 48 ? 48 : fn);
            } else if (db_state == 2 && gi == db_gate_idx) {
              // 正在监视这台、但计数对不上（重启等异常）：按回连处理，别掐断监视
              send(cfd, DB_FRAME_709, sizeof(DB_FRAME_709), 0);
              db_wait_peer_close(cfd, 250);
              close(cfd);
              ESP_LOGI(DB_TAG, "已应答 %s 的监视回连（709，兜底）", DB_GATES[gi].name);
            } else {
              // 确认是真实振铃。若正在监视其他门口机：呼叫优先，停监视转呼叫。
              if (db_state == 2) {
                ESP_LOGI(DB_TAG, "%s 振铃，停止当前监视转为呼叫", DB_GATES[gi].name);
                db_monitor_stop = true;
                uint32_t w = db_millis();
                while (db_state == 2 && db_millis() - w < 2500) vTaskDelay(pdMS_TO_TICKS(50));
              }
              if (db_state != 0) {
                db_rst_close(cfd);   // 忙时拒接：不回发数据，RST 关断
                ESP_LOGI(DB_TAG, "%s 呼入但正在%s，忽略",
                         DB_GATES[gi].name, db_state == 2 ? "监视" : "通话");
              } else {
                ESP_LOGI(DB_TAG, "收到振铃帧 %d 字节（req=704）", fn);
                ESP_LOG_BUFFER_HEX(DB_TAG, fb, fn > 48 ? 48 : fn);
                db_state = 1;
                db_gate_idx = gi;
                db_unlock_req = false;
                db_hangup_req = false;
                DbSession *ss = new DbSession();
                ss->fd = cfd;
                ss->gate_idx = gi;
                strncpy(ss->ip, ipbuf, 15);
                ss->ip[15] = 0;
                if (!db_spawn(db_session_task, "db_sess", 6144, ss)) {
                  close(cfd);
                  delete ss;
                  db_state = 0;
                  db_gate_idx = -1;
                  ESP_LOGE(DB_TAG, "会话任务创建失败");
                }
              }
            }
          }
        }
      }
    }

    // --- 周期在线宣告 ---
    if (db_millis() - last_beacon >= DB_BEACON_INTERVAL_MS) {
      last_beacon = db_millis();
      if (bc >= 0)
        sendto(bc, DB_BEACON_FRAME, sizeof(DB_BEACON_FRAME), 0, (struct sockaddr *)&bc_dst, sizeof(bc_dst));
    }

    // --- 主机注册（req=564 + req=888，真实主机开机即向门口机发，发后即忘） ---
    // 怀疑门口机要先见过"主机注册"才肯执行 518 解锁——真实主机抓包里开机就发了这两帧。
    // 统计成功条数：有失败时打警告（单条失败的阶段/errno 在 xact 里已逐条打印）。
    if (db_millis() - last_reg >= DB_REG_INTERVAL_MS) {
      last_reg = db_millis();
      uint8_t rr[256];
      int ok_n = 0;
      for (int gi = 0; gi < DB_GATE_COUNT; gi++) {
        size_t rl = 0;
        if (db_tcp_xact("564 注册", DB_GATES[gi].ip, DB_FRAME_564, sizeof(DB_FRAME_564), rr, &rl, 300)) ok_n++;
        rl = 0;
        if (db_tcp_xact("888 注册", DB_GATES[gi].ip, DB_FRAME_888, sizeof(DB_FRAME_888), rr, &rl, 300)) ok_n++;
      }
      if (ok_n == DB_GATE_COUNT * 2)
        ESP_LOGI(DB_TAG, "已向 %d 台门口机发送主机注册（564/888）", DB_GATE_COUNT);
      else
        ESP_LOGW(DB_TAG, "主机注册（564/888）只成功 %d/%d 条，失败原因见上方 errno", ok_n, DB_GATE_COUNT * 2);
    }

    // --- 通话期间的上行静音保活 ---
    // 抓包实测：真实主机通话中持续向门口机 6668 发 256×0xD5（A-law 静音，约 31 包/秒），
    // 照做以保持门口机放音通道常热。住户按住说话时桥接器的真实语音到达（db_last_uplink_ms
    // 刷新），200ms 内有真实上行就暂停静音，避免与真实语音抢节奏。
    if (db_state == 1 && db_gate_idx >= 0 && d1 >= 0) {
      static uint32_t last_sil = 0;
      if (db_millis() - last_sil >= 32 && db_millis() - db_last_uplink_ms >= 200) {
        last_sil = db_millis();
        struct sockaddr_in gd;
        memset(&gd, 0, sizeof(gd));
        gd.sin_family = AF_INET;
        gd.sin_port = htons(6668);
        gd.sin_addr.s_addr = db_gate_addr[db_gate_idx];
        sendto(d1, db_silence, sizeof(db_silence), 0, (struct sockaddr *)&gd, sizeof(gd));
      }
    }

    // （视频转发套接字的创建/重建已由 db_vtx_task 接管：空闲时每 2 秒重试）

    // --- 每 60 秒一条值守心跳 ---
    // 开机阶段的日志在 API 日志客户端连接前就发出去了（看不到），
    // 靠这条心跳可以随时确认门禁任务和以太网都在正常运行。
    static uint32_t last_hb = 0;
    static uint32_t hb_rx = 0, hb_fwd = 0, hb_drop = 0, hb_ring = 0;
    if (db_millis() - last_hb >= 60000) {
      last_hb = db_millis();
      // 报本周期增量，四栏一对比即可定位丢包在哪一段：
      //   收 < 门口机实发（约 158 包/秒 × 视频秒数）→ 板子前（以太网输入侧没接住）
      //   环满 > 0 → 环形缓冲都兜不住（WiFi 发送持续跟不上，不该出现）
      //   发失败 > 0 → WiFi 发送缓冲打满，补发 3 次仍失败（fnOS 离线也会计这里）
      //   收 ≈ 转 且发失败/环满都是 0，而 fnOS 收得少 → 板子后（WiFi 空口/路由器/fnOS）
      uint32_t drx = db_video_rx - hb_rx, dfwd = db_video_fwd - hb_fwd;
      uint32_t ddrp = db_video_dropped - hb_drop, dring = db_video_ringdrop - hb_ring;
      hb_rx = db_video_rx; hb_fwd = db_video_fwd; hb_drop = db_video_dropped; hb_ring = db_video_ringdrop;
      ESP_LOGI(DB_TAG, "值守心跳：以太网运行中（%s），状态=%s，空闲堆=%u，视频 收%u/转%u/发失败%u/环满%u（近60秒）",
               DB_ETH_IP, db_state == 2 ? "监视中" : (db_state ? "呼叫中" : "空闲"),
               (unsigned)esp_get_free_heap_size(), drx, dfwd, ddrp, dring);
    }

    // --- HA 请求监视：空闲时启动监视任务 ---
    if (db_monitor_req >= 0) {
      int mgi = db_monitor_req;
      db_monitor_req = -1;
      if (db_state == 0) {
        db_state = 2;
        db_gate_idx = mgi;
        db_monitor_stop = false;
        db_monitor_confirmed = false;
        db_mon_704_n[mgi] = 0;   // 清掉历史残留的回连登记（正常早已过期，纯保险）
        db_video_ssrc = 0;   // 清掉上一次会话的 SSRC，等新流来了再学习
        db_video_max_seq = 0;
        if (!db_spawn(db_monitor_task, "db_mon", 6144, (void *)(uintptr_t)mgi)) {
          db_state = 0;
          db_gate_idx = -1;
          ESP_LOGE(DB_TAG, "监视任务创建失败");
        }
      }
    }
  }
}

// ESPHome on_boot 调用
void doorbell_start() {
  if (db_started) return;
  db_started = true;
  if (!db_spawn(db_main_task, "db_main", 6144, NULL))
    ESP_LOGE(DB_TAG, "主任务创建失败");
}
